#!/usr/bin/env python3
"""压一下内置 MQTT 服务端，看它到底能同时接几个客户端（只用标准库）。

    python3 tools/mqtt_capacity.py <IP> [端口] [用户名] [密码] [最多试几个]

做法：一条一条建 TCP 连接并完成 CONNECT，保持全部连接不断开，
统计拿到 CONNACK rc=0 的有几个，第一个失败的是第几个。
"""
import socket
import struct
import sys
import time

HOST = sys.argv[1] if len(sys.argv) > 1 else "192.168.5.218"
PORT = int(sys.argv[2]) if len(sys.argv) > 2 else 1883
USER = sys.argv[3] if len(sys.argv) > 3 else ""
PWD = sys.argv[4] if len(sys.argv) > 4 else ""
MAXTRY = int(sys.argv[5]) if len(sys.argv) > 5 else 20


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


def enc_str(x):
    if isinstance(x, str):
        x = x.encode()
    return struct.pack(">H", len(x)) + x


def connect(cid):
    flags = 0x02 | (0x80 if USER else 0) | (0x40 if PWD else 0)
    payload = enc_str(cid)
    if USER:
        payload += enc_str(USER)
    if PWD:
        payload += enc_str(PWD)
    body = enc_str("MQTT") + bytes([4, flags]) + struct.pack(">H", 60) + payload
    s = socket.create_connection((HOST, PORT), timeout=5)
    s.sendall(bytes([0x10]) + enc_len(len(body)) + body)
    ack = s.recv(4)
    rc = ack[3] if len(ack) >= 4 else -1
    return s, rc


def main():
    kept = []
    ok = 0
    first_fail = None
    for i in range(MAXTRY):
        try:
            s, rc = connect("cap-%02d" % i)
        except Exception as e:
            print("第 %d 个：建连接就失败了（%s）" % (i + 1, e))
            first_fail = i + 1
            break
        if rc == 0:
            kept.append(s)
            ok += 1
            print("第 %2d 个：CONNACK 0x00  在线 %d" % (i + 1, ok))
        else:
            print("第 %2d 个：CONNACK 0x%02x  被拒" % (i + 1, rc))
            s.close()
            first_fail = i + 1
            break

    # 保持 3 秒，让网页那边也能看到实时在线数
    time.sleep(3)
    print("\n同时在线：%d 个%s" % (ok, "，第 %d 个开始被拒" % first_fail if first_fail else ""))

    for s in kept:
        try:
            s.sendall(bytes([0xE0, 0x00]))  # DISCONNECT
            s.close()
        except Exception:
            pass
    time.sleep(1)
    print("已全部断开")


if __name__ == "__main__":
    main()
