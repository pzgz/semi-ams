#!/usr/bin/env python3
"""SEMI-AMS 控制端内置 MQTT 服务端的自测脚本（只用标准库，不需要 mosquitto / paho）

    python3 tools/mqtt_selftest.py 192.168.5.218

检查这几条：
  1. 匿名 CONNECT 能通过
  2. SUBSCRIBE 通配符、SUBACK
  3. 进料器上报 state（retain）→ 订阅者收得到，网页 /api/state 里能看到这台设备
  4. 后连的客户端能收到保留消息（retain 位为 1）
  5. 网页上点"进料" → 订阅 <前缀>control 的客户端收到 {"mac":...,"start":"on"}
  6. 客户端掉线（带遗嘱）→ 其它订阅者收到 availability offline，网页上变离线
"""
import json
import socket
import struct
import sys
import time
import urllib.parse
import urllib.request

HOST = sys.argv[1] if len(sys.argv) > 1 else "192.168.5.218"
PORT = int(sys.argv[2]) if len(sys.argv) > 2 else 1883
USER = sys.argv[3] if len(sys.argv) > 3 else ""
PWD = sys.argv[4] if len(sys.argv) > 4 else ""
PREFIX = "semiams/"
DEV_MAC = "aa:bb:cc:dd:ee:01"
DEV_ID = "aabbccddee01"
FAILS = []


def ok(cond, what):
    print(("  ok   " if cond else "  FAIL ") + what)
    if not cond:
        FAILS.append(what)


def enc_len(n):
    out = b""
    while True:
        b = n % 128
        n //= 128
        if n:
            b |= 0x80
        out += bytes([b])
        if not n:
            return out


def enc_str(s):
    if isinstance(s, str):
        s = s.encode()
    return struct.pack(">H", len(s)) + s


def recv_exact(sock, n):
    buf = b""
    while len(buf) < n:
        chunk = sock.recv(n - len(buf))
        if not chunk:
            raise ConnectionError("连接被关闭")
        buf += chunk
    return buf


def read_pkt(sock, timeout=3.0):
    sock.settimeout(timeout)
    hdr = recv_exact(sock, 1)[0]
    remain, mult = 0, 1
    while True:
        b = recv_exact(sock, 1)[0]
        remain += (b & 0x7F) * mult
        mult *= 128
        if not (b & 0x80):
            break
    body = recv_exact(sock, remain) if remain else b""
    return hdr, body


def parse_publish(hdr, body):
    tl = struct.unpack(">H", body[:2])[0]
    topic = body[2 : 2 + tl].decode()
    i = 2 + tl
    qos = (hdr >> 1) & 3
    pid = None
    if qos:
        pid = struct.unpack(">H", body[i : i + 2])[0]
        i += 2
    return topic, body[i:], bool(hdr & 1), qos, pid


class Client:
    def __init__(self, cid, will=None, user=None, pwd=None):
        if user is None and USER:
            user, pwd = USER, PWD
        self.s = socket.create_connection((HOST, PORT), timeout=5)
        flags = 0x02
        payload = enc_str(cid)
        if will:
            flags |= 0x04
            if will.get("retain"):
                flags |= 0x20
            payload += enc_str(will["topic"]) + enc_str(will["msg"])
        if user:
            flags |= 0x80
            payload += enc_str(user)
        if pwd:
            flags |= 0x40
            payload += enc_str(pwd)
        body = enc_str("MQTT") + bytes([4, flags]) + struct.pack(">H", 60) + payload
        self.s.sendall(bytes([0x10]) + enc_len(len(body)) + body)
        hdr, ack = read_pkt(self.s)
        self.rc = ack[1] if len(ack) >= 2 else -1

    def subscribe(self, *filters):
        pid = 7
        body = struct.pack(">H", pid)
        for f in filters:
            body += enc_str(f) + bytes([0])
        self.s.sendall(bytes([0x82]) + enc_len(len(body)) + body)
        hdr, ack = read_pkt(self.s)
        return list(ack[2:])

    def publish(self, topic, payload, retain=False, qos=0):
        body = enc_str(topic) + (struct.pack(">H", 1) if qos else b"") + payload.encode()
        hdr = 0x30 | (0x01 if retain else 0) | (qos << 1)
        self.s.sendall(bytes([hdr]) + enc_len(len(body)) + body)

    def drain(self, seconds=1.0):
        """收 seconds 秒内到达的 PUBLISH，返回 (topic, payload, retain) 列表"""
        out = []
        end = time.time() + seconds
        while time.time() < end:
            try:
                hdr, body = read_pkt(self.s, max(0.05, end - time.time()))
            except (socket.timeout, TimeoutError):
                break
            except OSError:
                break
            if hdr >> 4 == 3:
                topic, payload, retain, qos, pid = parse_publish(hdr, body)
                out.append((topic, payload.decode("utf-8", "replace"), retain))
        return out

    def close(self):
        try:
            self.s.close()
        except OSError:
            pass


def api_state():
    with urllib.request.urlopen(f"http://{HOST}/api/state", timeout=5) as r:
        return json.loads(r.read().decode())


def api_control(idx, cmd):
    data = urllib.parse.urlencode({"idx": idx, "cmd": cmd}).encode()
    req = urllib.request.Request(f"http://{HOST}/api/control", data=data)
    with urllib.request.urlopen(req, timeout=5) as r:
        return json.loads(r.read().decode())


def main():
    print(f"目标 {HOST}:{PORT}\n")

    print("1. CONNECT 鉴权")
    if USER:
        anon = Client("selftest-anon", user="")
        ok(anon.rc == 4, f"匿名被拒绝（返回码 {anon.rc}）")
        anon.close()
        wrong = Client("selftest-wrong", user=USER, pwd="wrong-pass")
        ok(wrong.rc == 4, f"密码错被拒绝（返回码 {wrong.rc}）")
        wrong.close()
    sub = Client("selftest-sub")
    ok(sub.rc == 0, f"带账号连接 CONNACK {sub.rc}")

    print("2. SUBSCRIBE 通配符")
    granted = sub.subscribe(f"{PREFIX}+/state", f"{PREFIX}control", f"{PREFIX}+/availability")
    ok(granted == [0, 0, 0], f"SUBACK {granted}")

    print("3. 进料器上报 state（retain）")
    feeder = Client("selftest-feeder", will={
        "topic": f"{PREFIX}{DEV_ID}/availability",
        "msg": json.dumps({"state": "offline", "ip": "192.168.5.9", "mac": DEV_MAC}),
        "retain": True,
    })
    state = {
        "state": "打印中", "key": "PRINTING", "motor": "停止", "duty": 0,
        "in": 1, "ready": 1, "lack": 0, "full": 1,
        "rssi": -61, "heap": 12480, "uptime": 126,
        "ip": "192.168.5.9", "mac": DEV_MAC,
    }
    feeder.publish(f"{PREFIX}{DEV_ID}/state", json.dumps(state, ensure_ascii=False), retain=True)
    got = sub.drain(1.5)
    ok(any(t == f"{PREFIX}{DEV_ID}/state" and '"PRINTING"' in p for t, p, _ in got),
       f"订阅者收到 state：{[t for t, _, _ in got]}")

    st = api_state()
    dev = next((d for d in st["devices"] if d["mac"] == DEV_MAC), None)
    ok(dev is not None, "网页 /api/state 里出现这台设备")
    ok(dev and dev["key"] == "PRINTING" and dev["online"], "网页上状态是 打印中 且在线")

    print("4. 后连的客户端能收到保留消息")
    late = Client("selftest-late")
    late.subscribe(f"{PREFIX}#")
    got = late.drain(1.5)
    ok(any(t.endswith("/state") and retain for t, _, retain in got), "新订阅收到 retain=1 的 state")

    print("5. 网页点“进料” → control")
    r = api_control(dev["i"] if dev else 0, "feed")
    ok(r.get("ok") is True, f"/api/control 返回 {r}")
    got = sub.drain(1.5)
    ctrl = [(t, p) for t, p, _ in got if t == f"{PREFIX}control"]
    ok(bool(ctrl) and '"start"' in ctrl[0][1] and DEV_MAC in ctrl[0][1], f"收到 {ctrl}")

    print("6. 客户端掉线，遗嘱生效")
    feeder.close()
    got = sub.drain(2.5)
    offline = [(t, p) for t, p, _ in got if t.endswith("/availability")]
    ok(bool(offline) and "offline" in offline[0][1], f"收到遗嘱：{offline}")
    time.sleep(0.5)
    st = api_state()
    dev = next((d for d in st["devices"] if d["mac"] == DEV_MAC), None)
    ok(dev is not None and not dev["online"], "网页上这台变离线")

    print("7. 清掉保留消息（空的 retain）")
    feeder2 = Client("selftest-feeder2")
    feeder2.publish(f"{PREFIX}{DEV_ID}/state", "", retain=True)
    time.sleep(0.4)
    late2 = Client("selftest-late2")
    late2.subscribe(f"{PREFIX}{DEV_ID}/state")
    got = late2.drain(1.0)
    ok(not [1 for t, _, _ in got if t.endswith("/state")], "残留的 state 已被清掉")

    feeder2.close()
    late.close()
    late2.close()
    sub.close()

    print("\n%s（失败 %d 项）" % ("全部通过" if not FAILS else "有测试没通过", len(FAILS)))
    return 1 if FAILS else 0


if __name__ == "__main__":
    sys.exit(main())
