#!/usr/bin/env python3
"""在电脑上直接连接拓竹打印机的 MQTT，确认 IP / 序列号 / 访问码是否正确，并记录打印机发出的消息。

只用 Python 自带的库，不需要安装任何东西。只读取，不控制打印机（只发一次 pushall 请求完整状态）。

用法：
    python3 tools/bambu_listen.py --ip 192.168.1.50 --sn 序列号 --code 访问码
    （可选）--seconds 600   最多运行多少秒，默认一直运行，按 Ctrl+C 停止
    （可选）--out 文件名     原始消息保存位置，默认 bambu_log_日期时间.jsonl
    （可选）--no-pushall     不发送完整状态请求

屏幕上会显示：连接结果、每条命令回报，以及关键字段的变化（打印状态、阶段、耗材传感器、料盘等），
和进料器固件判断时用的字段一致。原始消息逐条存进 .jsonl 文件，发给我可以用来调整判断规则。
"""
import argparse
import json
import socket
import ssl
import struct
import sys
import time

CONNACK_TEXT = {
    0: "成功",
    1: "打印机不支持这个 MQTT 版本",
    2: "客户端 ID 被拒绝",
    3: "打印机 MQTT 服务不可用",
    4: "用户名或访问码错误",
    5: "未授权（访问码错误？）",
}


def enc_len(n):
    out = bytearray()
    while True:
        b = n % 128
        n //= 128
        if n:
            b |= 0x80
        out.append(b)
        if not n:
            return bytes(out)


def enc_str(s):
    b = s.encode()
    return struct.pack(">H", len(b)) + b


def packet(header, body):
    return bytes([header]) + enc_len(len(body)) + body


def connect_packet(client_id, user, password, keepalive=60):
    body = enc_str("MQTT") + bytes([4, 0xC2]) + struct.pack(">H", keepalive)
    body += enc_str(client_id) + enc_str(user) + enc_str(password)
    return packet(0x10, body)


def subscribe_packet(pid, topic):
    return packet(0x82, struct.pack(">H", pid) + enc_str(topic) + b"\x00")


def publish_packet(topic, payload):
    return packet(0x30, enc_str(topic) + payload)


class Conn:
    def __init__(self, sock):
        self.sock = sock
        self.buf = b""

    def _fill(self):
        try:
            data = self.sock.recv(16384)
        except socket.timeout:
            return False
        if not data:
            raise ConnectionError("打印机关闭了连接")
        self.buf += data
        return True

    def read_packet(self):
        """返回 (类型, 标志, 内容)，超时返回 None"""
        while True:
            if len(self.buf) >= 2:
                mult, length, i = 1, 0, 1
                while i < len(self.buf):
                    b = self.buf[i]
                    length += (b & 0x7F) * mult
                    mult *= 128
                    i += 1
                    if not b & 0x80:
                        break
                else:
                    i = None
                if i is not None and len(self.buf) >= i + length and not (self.buf[i - 1] & 0x80):
                    h = self.buf[0]
                    body = self.buf[i:i + length]
                    self.buf = self.buf[i + length:]
                    return h >> 4, h & 0x0F, body
            if not self._fill():
                return None


def get(d, *path):
    for p in path:
        if isinstance(d, dict) and p in d:
            d = d[p]
        else:
            return None
    return d


WATCH = [
    ("gcode_state", ("gcode_state",)),
    ("stg_cur", ("stg_cur",)),
    ("hw_switch_state", ("hw_switch_state",)),
    ("print_error", ("print_error",)),
    ("tray_now", ("ams", "tray_now")),
    ("mc_print_stage", ("mc_print_stage",)),
]


def extruder_summary(p):
    info = get(p, "device", "extruder", "info")
    if not isinstance(info, list):
        return None
    parts = []
    for e in info:
        if not isinstance(e, dict):
            continue
        snow = e.get("snow")
        bits = e.get("info")
        s = "id%s" % e.get("id")
        if isinstance(snow, int):
            ams, slot = snow >> 8, snow & 0xFF
            where = "外置料盘" if ams in (254, 255) and slot != 255 else ("无" if slot == 255 else "AMS%d-%d" % (ams, slot))
            s += " 料盘=%s(snow=%d)" % (where, snow)
        if isinstance(bits, int):
            s += " 挤出机%s耗材(info=%d)" % ("有" if bits & 2 else "无", bits)
        parts.append(s)
    return "; ".join(parts) if parts else None


def main():
    ap = argparse.ArgumentParser(description="连接拓竹打印机 MQTT，显示并记录消息")
    ap.add_argument("--ip", required=True)
    ap.add_argument("--sn", required=True, help="打印机序列号")
    ap.add_argument("--code", required=True, help="打印机访问码（配对码）")
    ap.add_argument("--seconds", type=int, default=0)
    ap.add_argument("--out", default=time.strftime("bambu_log_%Y%m%d_%H%M%S.jsonl"))
    ap.add_argument("--no-pushall", action="store_true")
    a = ap.parse_args()
    sn = a.sn.strip().upper()

    def log(msg):
        print(time.strftime("%H:%M:%S"), msg, flush=True)

    log("连接 %s:8883 ..." % a.ip)
    raw = socket.create_connection((a.ip, 8883), timeout=5)
    ctx = ssl.create_default_context()
    ctx.check_hostname = False
    ctx.verify_mode = ssl.CERT_NONE  # 打印机用自签名证书
    sock = ctx.wrap_socket(raw, server_hostname=a.ip)
    sock.settimeout(1.0)
    log("TLS 已连接（%s）" % sock.version())
    c = Conn(sock)
    sock.sendall(connect_packet("semi-ams-pc-%d" % (int(time.time()) % 100000), "bblp", a.code.strip()))

    deadline = time.time() + 10
    while True:
        p = c.read_packet()
        if p and p[0] == 2:
            rc = p[2][1] if len(p[2]) >= 2 else -1
            log("登录结果：%s（%d）" % (CONNACK_TEXT.get(rc, "未知"), rc))
            if rc != 0:
                return 1
            break
        if time.time() > deadline:
            log("打印机没有回应登录")
            return 1

    topic = "device/%s/report" % sn
    sock.sendall(subscribe_packet(1, topic))
    deadline = time.time() + 10
    while True:
        p = c.read_packet()
        if p and p[0] == 9:
            ok = len(p[2]) >= 3 and p[2][2] != 0x80
            log("订阅 %s：%s" % (topic, "成功" if ok else "被拒绝（序列号是否正确？）"))
            if not ok:
                return 1
            break
        if time.time() > deadline:
            log("打印机没有回应订阅（序列号是否正确？）")
            return 1

    if not a.no_pushall:
        req = {"pushing": {"sequence_id": "0", "command": "pushall", "version": 1, "push_target": 1}}
        sock.sendall(publish_packet("device/%s/request" % sn, json.dumps(req).encode()))
        log("已请求完整状态（pushall）")

    log("开始接收，原始消息保存到 %s，按 Ctrl+C 停止" % a.out)
    state = {}
    last_ping = time.time()
    start = time.time()
    count = 0
    with open(a.out, "a", encoding="utf-8") as f:
        try:
            while not a.seconds or time.time() - start < a.seconds:
                if time.time() - last_ping > 30:
                    sock.sendall(b"\xC0\x00")
                    last_ping = time.time()
                p = c.read_packet()
                if not p or p[0] != 3:
                    continue
                body = p[2]
                tlen = struct.unpack(">H", body[:2])[0]
                payload = body[2 + tlen + (2 if (p[1] >> 1) & 3 else 0):]
                count += 1
                text = payload.decode("utf-8", "replace")
                f.write(json.dumps({"t": time.strftime("%Y-%m-%d %H:%M:%S"), "len": len(payload), "msg": text},
                                   ensure_ascii=False) + "\n")
                f.flush()
                try:
                    d = json.loads(text)
                except ValueError:
                    log("无法解析的消息（%d 字节）" % len(payload))
                    continue
                pr = d.get("print") if isinstance(d, dict) else None
                if not isinstance(pr, dict):
                    keys = ",".join(d.keys()) if isinstance(d, dict) else "?"
                    log("其它消息：%s（%d 字节）" % (keys, len(payload)))
                    continue
                cmd = pr.get("command")
                if cmd and cmd != "push_status":
                    short = {k: v for k, v in pr.items() if k in (
                        "command", "result", "reason", "use_ams", "ams_mapping", "ams_mapping2", "target",
                        "ams_id", "slot_id", "param", "subtask_name", "sequence_id")}
                    log("命令回报：%s" % json.dumps(short, ensure_ascii=False))
                changes = []
                for name, path in WATCH:
                    v = get(pr, *path)
                    if v is not None and state.get(name) != v:
                        changes.append("%s %s→%s" % (name, state.get(name, "?"), v))
                        state[name] = v
                ex = extruder_summary(pr)
                if ex and state.get("extruder") != ex:
                    changes.append("挤出机：%s" % ex)
                    state["extruder"] = ex
                if changes:
                    log("（%d 字节）%s" % (len(payload), "，".join(changes)))
        except KeyboardInterrupt:
            pass
        except (OSError, ConnectionError) as e:
            log("连接断开：%s" % e)
    log("结束，共收到 %d 条消息，保存在 %s" % (count, a.out))
    try:
        sock.sendall(b"\xE0\x00")
        sock.close()
    except OSError:
        pass
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, ConnectionError) as e:
        print(time.strftime("%H:%M:%S"), "连接失败：%s" % e)
        sys.exit(1)
