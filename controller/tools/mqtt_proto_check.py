#!/usr/bin/env python3
"""按 MQTT 3.1 / 3.1.1 / 5.0 三种协议各跑一遍 CONNECT→SUBSCRIBE→PUBLISH→收消息。

用法：python3 tools/mqtt_proto_check.py [host] [port] [user] [pass]

只用标准库。MQTT 5.0 的报文和 3.x 差在：CONNECT 多属性块、CONNACK/SUBACK/UNSUBACK 多属性长度、
PUBLISH 在主题后面多一个属性块——这个脚本就是专门验证这几处。
"""
import socket
import struct
import sys

HOST = sys.argv[1] if len(sys.argv) > 1 else "192.168.5.218"
PORT = int(sys.argv[2]) if len(sys.argv) > 2 else 1883
USER = sys.argv[3] if len(sys.argv) > 3 else ""
PWD = sys.argv[4] if len(sys.argv) > 4 else ""
PREFIX = "semiams/"

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


def s(x):
    if isinstance(x, str):
        x = x.encode()
    return struct.pack(">H", len(x)) + x


def varint(buf, i):
    val, mul = 0, 1
    while True:
        b = buf[i]
        i += 1
        val += (b & 0x7F) * mul
        mul *= 128
        if not b & 0x80:
            return val, i


class Conn:
    def __init__(self, ver, cid):
        self.ver = ver
        flags = 0x02
        payload = s(cid)
        if USER:
            flags |= 0x80
            payload += s(USER)
        if PWD:
            flags |= 0x40
            payload += s(PWD)
        name, level = ("MQIsdp", 3) if ver == 3 else ("MQTT", ver)
        body = s(name) + bytes([level, flags]) + struct.pack(">H", 60)
        if ver == 5:
            body += b"\x00"  # 属性长度 0
        body += payload
        self.sock = socket.create_connection((HOST, PORT), timeout=6)
        self.sock.settimeout(6)
        self.sock.sendall(bytes([0x10]) + enc_len(len(body)) + body)
        ack = self.read_packet()
        # acked = 固定头 + body；3.x 的 body 是 ackFlags+rc，5.0 再多个属性长度
        if not ack:
            self.rc = -1
        else:
            self.rc = ack[3] if (ver == 5 and len(ack) >= 4) else (ack[2] if len(ack) >= 3 else -1)
        self.acked = ack

    last_rem = 0

    def read_packet(self):
        hdr = self.sock.recv(1)
        if not hdr:
            return None
        mul, rem, n = 1, 0, 0
        while True:
            b = self.sock.recv(1)
            if not b:
                return None
            rem += (b[0] & 0x7F) * mul
            mul *= 128
            n += 1
            if not b[0] & 0x80:
                break
        self.last_rem = rem  # 记下剩余长度，后面用它验证 5.0 的 CONNACK 多了属性长度字节
        body = b""
        while len(body) < rem:
            chunk = self.sock.recv(rem - len(body))
            if not chunk:
                break
            body += chunk
        return bytes([hdr[0]]) + body

    def subscribe(self, *filters):
        pid = 1
        body = struct.pack(">H", pid)
        if self.ver == 5:
            body += b"\x00"
        for f in filters:
            body += s(f) + bytes([0])
        self.sock.sendall(bytes([0x82]) + enc_len(len(body)) + body)
        pkt = self.read_packet()
        if pkt is None or pkt[0] >> 4 != 9:
            return None
        i = 3  # 跳过固定头 + packetId 低字节
        if self.ver == 5:
            _, i = varint(pkt, i)
        return list(pkt[i:])

    def publish(self, topic, payload, qos=0):
        pid = 7
        body = s(topic)
        hdr = 0x30 | (qos << 1)
        if qos:
            body += struct.pack(">H", pid)
        if self.ver == 5:
            body += b"\x00"
        body += payload.encode()
        self.sock.sendall(bytes([hdr]) + enc_len(len(body)) + body)
        if qos:
            pkt = self.read_packet()
            return pkt is not None and pkt[0] >> 4 == 4

    def wait_publish(self):
        """等下一条 PUBLISH，返回 (topic, payload, retain)"""
        while True:
            pkt = self.read_packet()
            if pkt is None:
                return None
            if pkt[0] >> 4 != 3:
                continue
            i = 1
            tlen = struct.unpack(">H", pkt[i:i + 2])[0]
            i += 2
            topic = pkt[i:i + tlen].decode()
            i += tlen
            qos = (pkt[0] >> 1) & 3
            if qos:
                i += 2
            if self.ver == 5:
                plen, i = varint(pkt, i)
                i += plen
            return topic, pkt[i:].decode("utf-8", "replace"), bool(pkt[0] & 1)

    def unsubscribe(self, *filters):
        pid = 2
        body = struct.pack(">H", pid)
        if self.ver == 5:
            body += b"\x00"
        for f in filters:
            body += s(f)
        self.sock.sendall(bytes([0xA2]) + enc_len(len(body)) + body)
        pkt = self.read_packet()
        return pkt is not None and pkt[0] >> 4 == 11

    def close(self):
        try:
            self.sock.sendall(b"\xe0\x00")
        except OSError:
            pass
        self.sock.close()


def run(ver, label):
    print(f"\n{label}（协议级别 {ver}）")
    sub = Conn(ver, f"chk-sub-{ver}")
    ok(sub.rc == 0, f"CONNECT 通过，CONNACK rc={sub.rc}")
    if sub.rc != 0:
        sub.close()
        return
    if ver == 5:
        ok(sub.last_rem == 3 and sub.acked[-1] == 0, "5.0 的 CONNACK 带属性长度 0（剩余长度 3）")
    else:
        ok(sub.last_rem == 2, f"3.x 的 CONNACK 剩余长度 2（实际 {sub.last_rem}）")

    granted = sub.subscribe(f"{PREFIX}+/state", f"{PREFIX}control")
    ok(granted is not None and all(g == 0 for g in granted), f"SUBACK {granted}")

    pub = Conn(ver, f"chk-pub-{ver}")
    ok(pub.rc == 0, f"第二个连接 rc={pub.rc}")
    payload = f'{{"mac":"aa:bb:cc:dd:ee:{ver}0","state":"打印中","ip":"10.0.0.{ver}"}}'
    ok(pub.publish(f"{PREFIX}aabbccddee{ver}0/state", payload, qos=1), "QoS1 发布收到 PUBACK")

    got = sub.wait_publish()
    ok(got is not None and got[0] == f"{PREFIX}aabbccddee{ver}0/state", f"订阅者收到主题 {got[0] if got else None}")
    ok(got is not None and got[1] == payload, "payload 一字不差（属性块没被当成正文）")

    ok(sub.unsubscribe(f"{PREFIX}control"), "UNSUBACK")
    sub.close()
    pub.close()


for v, name in ((3, "MQTT 3.1"), (4, "MQTT 3.1.1"), (5, "MQTT 5.0")):
    run(v, name)

print(f"\n{'全部通过' if fails == 0 else str(fails) + ' 项失败'}")
sys.exit(1 if fails else 0)
