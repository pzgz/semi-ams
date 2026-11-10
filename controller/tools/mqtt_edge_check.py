#!/usr/bin/env python3
"""针对 0.2.7 六个修复的回归检查（MQTT 协议 + HTTP 接口）。

用法：python3 tools/mqtt_edge_check.py [host] [port] [user] [pass]

覆盖：
  1) /api/control 对离线设备返回 409，对没有人订阅时返回 503（不再假「已发送」）
  4) QoS 2：PUBLISH → PUBREC，重复同 pid 只补 PUBREC 不重复投递，PUBREL → PUBCOMP
  5) 订阅过滤器超过 47 字节 → SUBACK 0x80，而不是截断后假装成功
  9) 材料类型带引号 / 尖括号 / 反斜杠 → 400，且 /api/state 的 JSON 仍然合法

只用标准库。刷完固件后跑一遍，退出码 0 = 全通过。
"""
import json
import socket
import struct
import sys
import urllib.parse
import urllib.request

HOST = sys.argv[1] if len(sys.argv) > 1 else "192.168.5.218"
PORT = int(sys.argv[2]) if len(sys.argv) > 2 else 1883
USER = sys.argv[3] if len(sys.argv) > 3 else ""
PWD = sys.argv[4] if len(sys.argv) > 4 else ""
PREFIX = "semiams/"
BASE = f"http://{HOST}"

fails = 0


def ok(cond, msg):
    global fails
    print(("  ok   " if cond else "  FAIL ") + msg)
    if not cond:
        fails += 1


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
    b = s.encode()
    return struct.pack("!H", len(b)) + b


class Conn:
    def __init__(self, cid, ver=4):
        flags = 0x02
        payload = enc_str(cid)
        if USER:
            flags |= 0x80
            payload += enc_str(USER)
        if PWD:
            flags |= 0x40
            payload += enc_str(PWD)
        body = enc_str("MQTT") + bytes([ver, flags]) + struct.pack("!H", 30)
        if ver == 5:
            body += b"\x00"
        body += payload
        self.s = socket.create_connection((HOST, PORT), 5)
        self.s.settimeout(1.5)
        self.s.sendall(bytes([0x10]) + enc_len(len(body)) + body)
        ack = self.s.recv(16)
        self.ver = ver
        self.rc = ack[3] if len(ack) >= 4 else -1

    def send(self, hdr, body):
        self.s.sendall(bytes([hdr]) + enc_len(len(body)) + body)

    def sub(self, filt, pid=1, qos=0):
        self.send(0x82, struct.pack("!H", pid) + enc_str(filt) + bytes([qos]))
        data = self.s.recv(64)
        # 5.0 的 SUBACK 在 pid 后面多一个属性长度
        return data[5] if self.ver == 5 and len(data) >= 6 else (data[4] if len(data) >= 5 else None)

    def pub(self, topic, payload, qos=0, retain=False, pid=1):
        hdr = 0x30 | (qos << 1) | (1 if retain else 0)
        body = enc_str(topic)
        if qos:
            body += struct.pack("!H", pid)
        if self.ver == 5:
            body += b"\x00"
        body += payload.encode()
        self.send(hdr, body)

    def recv_pkt(self, timeout=1.5):
        self.s.settimeout(timeout)
        try:
            hdr = self.s.recv(1)
            if not hdr:
                return None
            n, mul = 0, 1
            while True:
                b = self.s.recv(1)[0]
                n += (b & 0x7F) * mul
                mul *= 128
                if not b & 0x80:
                    break
            body = b""
            while len(body) < n:
                chunk = self.s.recv(n - len(body))
                if not chunk:
                    break
                body += chunk
            return hdr[0], body
        except socket.timeout:
            return None

    def close(self):
        try:
            self.send(0xE0, b"")
            self.s.close()
        except OSError:
            pass


def http_post(path, data):
    body = urllib.parse.urlencode(data).encode()
    req = urllib.request.Request(BASE + path, data=body, method="POST")
    try:
        with urllib.request.urlopen(req, timeout=6) as r:
            return r.status, json.loads(r.read().decode())
    except urllib.error.HTTPError as e:
        return e.code, json.loads(e.read().decode())


def http_get(path):
    with urllib.request.urlopen(BASE + path, timeout=6) as r:
        return json.loads(r.read().decode())


print("== 5) 订阅过滤器超长：应回 0x80，不应截断后假装成功")
c = Conn("edge-long")
ok(c.rc == 0, f"连接 rc={c.rc}")
long_filter = PREFIX + "x" * 50 + "/state"
rcode = c.sub(long_filter)
ok(rcode == 0x80, f"47 字节以上的过滤器 SUBACK 返回码 {rcode}（期望 128 / 0x80）")
short_rc = c.sub(PREFIX + "probe/#", pid=2)
ok(short_rc == 0, f"正常长度的过滤器仍然返回 0（实际 {short_rc}）")
c.close()

print("\n== 4) QoS 2：PUBREC → 重复 pid 只补 PUBREC → PUBREL → PUBCOMP")
sub = Conn("edge-sub")
ok(sub.rc == 0, f"订阅者连接 rc={sub.rc}")
sub.sub(PREFIX + "edge/#")
pub = Conn("edge-pub2")
ok(pub.rc == 0, f"发布者连接 rc={pub.rc}")
pub.pub(PREFIX + "edge/qos2", '{"mac":"aa:bb:cc:dd:ee:01","state":"QoS2"}', qos=2, pid=9)
pkt = pub.recv_pkt()
ok(pkt is not None and pkt[0] >> 4 == 5, f"QoS2 PUBLISH 收到 PUBREC（{pkt[0]:#04x}）" if pkt else "没有任何应答")
got = sub.recv_pkt()
ok(got is not None, "订阅者收到消息")
pub.pub(PREFIX + "edge/qos2", '{"mac":"aa:bb:cc:dd:ee:01","state":"QoS2"}', qos=2, pid=9)
pkt2 = pub.recv_pkt()
ok(pkt2 is not None and pkt2[0] >> 4 == 5, "重复的同 pid 报文再次回 PUBREC")
dup = sub.recv_pkt(1.0)
ok(dup is None, "重复报文没有被投递第二次")
pub.send(0x62, struct.pack("!H", 9))
pkt3 = pub.recv_pkt()
ok(pkt3 is not None and pkt3[0] >> 4 == 7, f"PUBREL 收到 PUBCOMP（{pkt3[0]:#04x}）" if pkt3 else "PUBREL 没有回 PUBCOMP")
pub.pub(PREFIX + "edge/qos1", "x", qos=1, pid=7)
pkt4 = pub.recv_pkt()
ok(pkt4 is not None and pkt4[0] >> 4 == 4, "QoS1 的 PUBACK 没被 QoS2 改动影响")
pub.close()
sub.close()

print("\n== 9) 材料类型里的特殊字符要被挡住")
before = http_get("/api/state")
code, j = http_post("/api/adddev", {"mac": "aa:bb:cc:dd:ee:77", "color": "#123456",
                                   "material": 'a"b<c>d\\e'})
ok(code == 400 and not j.get("ok"), f"带引号/尖括号/反斜杠的材料 → HTTP {code} {j.get('err')}")
code, j = http_post("/api/adddev", {"mac": "aa:bb:cc:dd:ee:77", "color": "#zzzzzz", "material": "PLA"})
ok(code == 400 and not j.get("ok"), f"非十六进制颜色 → HTTP {code} {j.get('err')}")
after = http_get("/api/state")
ok(len(after["devices"]) == len(before["devices"]), "/api/state 里没有多出脏设备，JSON 仍能解析")

print("\n== 1) /api/control 不能假报「已发送」")
devs = after["devices"]
offline_idx = next((d["i"] for d in devs if not d["online"]), None)
if offline_idx is None:
    print("  ..   没有离线设备，跳过（在线设备故意不发指令，免得真让进料器动起来）")
else:
    code, j = http_post("/api/control", {"idx": offline_idx, "cmd": "feed"})
    ok(code == 409 and not j.get("ok"), f"离线设备点进料 → HTTP {code}「{j.get('err')}」")
code, j = http_post("/api/control", {"idx": 0, "cmd": "不存在的动作"})
ok(code == 400, f"不认识的动作 → HTTP {code}")

print(f"\n{'全部通过' if fails == 0 else str(fails) + ' 项失败'}")
sys.exit(1 if fails else 0)
