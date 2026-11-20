#!/usr/bin/env python3
"""拓竹云端登录 + 打印机状态读取（纯标准库，零依赖）。

参考 ou0826/bambu-monitor 的云端模式，流程与 Bambu Studio 一致：

  1. POST /v1/user-service/user/login      {account, password} 或 {account, code}
  2. accessToken 是 JWT 时可直接解出 u_<uid>；不是 JWT 就查 preference 接口
  3. 云 MQTT：cn.mqtt.bambulab.com:8883  用户名 u_<uid>  密码 accessToken
  4. 订阅 device/<SN>/report，发 pushall 拿全量状态

用法（在仓库根目录执行）：
  python3 controller/tools/pc/bambu_cloud.py login            # 交互登录（手机号/邮箱 + 密码或验证码）
  python3 controller/tools/pc/bambu_cloud.py devices          # 列出账号下已绑定的打印机
  python3 controller/tools/pc/bambu_cloud.py status           # 连云 MQTT 读一次状态
  python3 controller/tools/pc/bambu_cloud.py status --watch 30  # 连续读 30 秒
  python3 controller/tools/pc/bambu_cloud.py --json status    # 机器可读输出
  python3 controller/tools/pc/bambu_cloud.py status --raw     # 打印原始报文

本文件同时是 bambu_bridge.py 的依赖库（MiniMqtt / Cloud 都在这里）。

凭据：密码只在内存里用，绝不落盘；token 存 ~/.bambu_cloud.json（0600，90 天有效）。

接口现状（2026-11 实测）：
  · 密码登录      POST /v1/user-service/user/login       可用
  · 邮箱验证码    POST /v1/user-service/user/sendemail/code  可用
  · 短信验证码    POST /v1/user-service/user/sendsmscode  被极验人机验证拦（HTTP 418），
                  纯程序调不通，只能用密码登录或邮箱验证码
"""
from __future__ import annotations

import argparse
import base64
import getpass
import json
import os
import socket
import ssl
import struct
import sys
import time
import urllib.error
import urllib.request

# ---------------------------------------------------------------------------
# 常量
# ---------------------------------------------------------------------------
API_CN = "https://api.bambulab.cn"   # 中国区（大陆账号必须用）
API_INTL = "https://api.bambulab.com"
UA = "bambu_network_agent/01.09.05.01"

MQTT_CN = "cn.mqtt.bambulab.com"
MQTT_INTL = "us.mqtt.bambulab.com"
MQTT_PORT = 8883

TOKEN_FILE = os.path.expanduser("~/.bambu_cloud.json")
HTTP_TIMEOUT = 25


# ---------------------------------------------------------------------------
# 极简 MQTT 3.1.1 客户端（只做 CONNECT / SUBSCRIBE / PUBLISH qos0 / PING）
# ---------------------------------------------------------------------------
def _enc_varint(n: int) -> bytes:
    """MQTT Remaining Length 变长编码。"""
    out = bytearray()
    while True:
        b = n % 128
        n //= 128
        if n:
            b |= 0x80
        out.append(b)
        if not n:
            return bytes(out)


def _mqtt_str(s: str) -> bytes:
    b = s.encode("utf-8")
    return struct.pack("!H", len(b)) + b


class MiniMqtt:
    """够用就行的 MQTT 客户端：连上、订阅、发布、收报文。

    故意不实现 qos1/qos2/遗嘱/重连，只保留云端读状态需要的最小集合。
    这些字节布局后面可以一比一搬到 ESP32 上。
    """

    def __init__(self, host: str, port: int, username: str, password: str,
                 client_id: str, insecure: bool = False, keepalive: int = 60,
                 secure: bool = True):
        self.host, self.port = host, port
        self.username, self.password = username, password
        self.client_id = client_id
        self.insecure = insecure      # 自签证书（打印机本地 8883）
        self.secure = secure          # False = 裸 TCP（C3 的 1883）
        self.keepalive = keepalive
        self.sock = None
        self._pid = 0
        self._buf = b""

    # ---------- 连接 ----------
    def connect(self, timeout: float = 10.0) -> int:
        raw = socket.create_connection((self.host, self.port), timeout=timeout)
        raw.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        if not self.secure:
            self.sock = raw
            self.sock.settimeout(timeout)
            return self._handshake()

        # 局域网直连是自签证书；云端是正规 CA，必须校验
        if self.insecure:
            ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
            ctx.check_hostname = False
            ctx.verify_mode = ssl.CERT_NONE
            try:
                ctx.set_ciphers("DEFAULT:@SECLEVEL=0")
            except ssl.SSLError:
                pass
        else:
            ctx = ssl.create_default_context()
        self.sock = ctx.wrap_socket(raw, server_hostname=self.host)
        self.sock.settimeout(timeout)
        return self._handshake()

    def _handshake(self) -> int:
        flags = 0x02                      # clean session
        if self.username:
            flags |= 0x80
        if self.password:
            flags |= 0x40
        vh = _mqtt_str("MQTT") + bytes([4, flags]) + struct.pack("!H", self.keepalive)
        pl = _mqtt_str(self.client_id)
        if self.username:
            pl += _mqtt_str(self.username)
        if self.password:
            pl += _mqtt_str(self.password)
        body = vh + pl
        self.sock.sendall(b"\x10" + _enc_varint(len(body)) + body)

        pkt = self._read_packet()
        if pkt is None or pkt[0] >> 4 != 2:
            raise RuntimeError("CONNACK 未收到")
        rc = pkt[1][1] if len(pkt[1]) >= 2 else -1
        if rc != 0:
            raise RuntimeError(f"MQTT 拒绝连接 rc={rc}"
                               + ("（用户名或密码不对）" if rc in (4, 5) else ""))
        return rc

    def subscribe(self, topic: str, qos: int = 0) -> None:
        self._pid = self._pid % 65535 + 1
        body = struct.pack("!H", self._pid) + _mqtt_str(topic) + bytes([qos])
        self._send(0x82, body)

    def publish(self, topic: str, payload: bytes, qos: int = 0) -> None:
        body = _mqtt_str(topic) + payload
        self._send(0x30 | (qos << 1), body)

    def ping(self) -> None:
        self._send(0xC0, b"")

    def _send(self, header: int, body: bytes) -> None:
        assert self.sock is not None
        self.sock.sendall(bytes([header]) + _enc_varint(len(body)) + body)

    # ---------- 收包 ----------
    def _recv_exact(self, n: int) -> bytes:
        assert self.sock is not None
        while len(self._buf) < n:
            chunk = self.sock.recv(4096)
            if not chunk:
                raise ConnectionError("连接被对端关闭")
            self._buf += chunk
        out, self._buf = self._buf[:n], self._buf[n:]
        return out

    def _read_packet(self) -> tuple[int, bytes] | None:
        """返回 (固定头字节, 剩余部分)。超时返回 None。"""
        try:
            h = self._recv_exact(1)[0]
        except socket.timeout:
            return None
        mult, length = 1, 0
        while True:
            b = self._recv_exact(1)[0]
            length += (b & 0x7F) * mult
            if not b & 0x80:
                break
            mult *= 128
            if mult > 128 ** 3:
                raise RuntimeError("Remaining Length 非法")
        return h, (self._recv_exact(length) if length else b"")

    def poll(self, deadline: float) -> dict | None:
        """等到 deadline 之前收到一条 PUBLISH；顺带回 PINGRESP 之外的东西一律忽略。"""
        while time.monotonic() < deadline:
            self.sock.settimeout(max(0.05, min(1.0, deadline - time.monotonic())))
            pkt = self._read_packet()
            if pkt is None:
                continue
            h, body = pkt
            kind = h >> 4
            if kind == 3:                       # PUBLISH
                qos = (h >> 1) & 0x03
                if len(body) < 2:
                    continue
                tlen = struct.unpack("!H", body[:2])[0]
                payload = body[2 + tlen + (2 if qos else 0):]
                return {"topic": body[2:2 + tlen].decode("utf-8", "replace"),
                        "payload": payload}
            if kind == 13:                      # PINGRESP
                continue
            # SUBACK / CONNACK 等直接跳过
        return None

    def close(self) -> None:
        if self.sock:
            try:
                self.sock.close()
            except OSError:
                pass
            self.sock = None


# ---------------------------------------------------------------------------
# 云端 API
# ---------------------------------------------------------------------------
class CloudError(Exception):
    def __init__(self, msg: str, code=None):
        super().__init__(_zh(msg))
        self.code = code


_ERR_ZH = {
    "Incorrect account or password.": "账号或密码不正确",
    "Incorrect code": "验证码不正确",
    "Code does not exist or has expired; please request a new verification code.":
        "验证码不存在或已过期，请重新发送",
    "Please login.": "未登录（token 过期或无效）",
    "The server cannot or will not process the request due to something that is "
    "perceived to be a client error.": "服务器拒绝了这个请求（接口不可用或账号区域不对）",
}


def _zh(msg: str) -> str:
    s = str(msg or "").strip()
    if s in _ERR_ZH:
        return _ERR_ZH[s]
    for k, v in _ERR_ZH.items():
        if s and s[:40] == k[:40]:
            return v
    return s


class Cloud:
    def __init__(self, region: str = "cn"):
        self.region = region
        self.token = ""
        self.base = API_CN if region == "cn" else API_INTL
        self.uid = ""

    @property
    def mqtt_host(self) -> str:
        return MQTT_CN if self.region == "cn" else MQTT_INTL

    def _bases(self) -> list[str]:
        """本区优先，再试另一区（账号可能注册在另一个区）。"""
        return [API_CN, API_INTL] if self.region == "cn" else [API_INTL, API_CN]

    def _req(self, path: str, payload: dict | None = None, auth: bool = False,
             base: str | None = None) -> dict:
        url = (base or self.base) + path
        data = json.dumps(payload).encode() if payload is not None else None
        req = urllib.request.Request(url, data=data, method="POST" if data else "GET")
        req.add_header("User-Agent", UA)
        req.add_header("Content-Type", "application/json")
        if auth and self.token:
            req.add_header("Authorization", "Bearer " + self.token)
        try:
            with urllib.request.urlopen(req, timeout=HTTP_TIMEOUT,
                                        context=ssl.create_default_context()) as r:
                body = r.read().decode("utf-8", "replace")
        except urllib.error.HTTPError as e:
            body = e.read().decode("utf-8", "replace")
            try:
                obj = json.loads(body)
            except Exception:
                raise CloudError(f"HTTP {e.code}: {body[:160]}") from None
            # 极验人机验证：短信接口现在基本都走这条
            if e.code == 418 or obj.get("captchaId"):
                raise CloudError(
                    "这个接口要求人机验证（极验），程序里过不了。"
                    "请改用密码登录，或改用邮箱账号 + 邮箱验证码"
                ) from None
            msg = obj.get("error") or obj.get("message") or f"HTTP {e.code}"
            err = CloudError(str(msg))
            err.code = obj.get("code")          # 1 = 账号/密码不对，2 = 验证码不对
            raise err from None
        except urllib.error.URLError as e:
            raise CloudError(f"连不上拓竹服务器：{e.reason}") from None
        try:
            return json.loads(body or "{}")
        except Exception:
            raise CloudError(f"返回内容无法解析：{body[:160]}") from None

    # ---------- 登录 ----------
    def _login_one(self, base: str, payload: dict) -> str:
        data = self._req("/v1/user-service/user/login", payload, base=base)
        token = data.get("accessToken") or ""
        if token:
            self.token = token
            self.base = base
            return token
        if data.get("loginType") == "verifyCode":
            raise CloudError("此账号需要验证码登录，请用 --code 方式")
        if data.get("loginType") == "tfa":
            raise CloudError("此账号开了二次验证，请用验证码登录")
        raise CloudError(f"登录失败：{data}")

    def _login(self, payload: dict) -> str:
        last: Exception | None = None
        for base in self._bases():
            try:
                return self._login_one(base, payload)
            except CloudError as e:
                last = e
                if getattr(e, "code", None) == 1:
                    continue            # 换区重试
                raise
        raise last or CloudError("登录失败")

    def login_password(self, account: str, password: str) -> str:
        return self._login({"account": account, "password": password, "apiError": ""})

    def login_code(self, account: str, code: str) -> str:
        return self._login({"account": account, "code": code})

    def send_code(self, account: str, tfa: str = "") -> None:
        """发登录验证码：手机走 sendsmscode（大概率被极验拦），邮箱走 sendemail/code。

        注意 type 必须是 codeLogin；账号开了二次验证时还要带 tfaKey/tfaCode。
        """
        is_email = "@" in account
        payload = {"email" if is_email else "phone": account, "type": "codeLogin"}
        if tfa:
            payload["tfaCode"] = tfa
        path = ("/v1/user-service/user/sendemail/code" if is_email
                else "/v1/user-service/user/sendsmscode")
        last: Exception | None = None
        for base in self._bases():
            try:
                self._req(path, payload, base=base)
                self.base = base
                return
            except CloudError as e:
                last = e
        raise last or CloudError("发送验证码失败")

    # ---------- 账号 / 设备 ----------
    def resolve_uid(self) -> str:
        """云 MQTT 用户名要 u_<uid>。JWT 能直接解，解不出就查 preference。"""
        if self.uid:
            return self.uid
        self.uid = _jwt_username(self.token)[2:] if _jwt_username(self.token) else ""
        if not self.uid:
            try:
                self.uid = str(self._req("/v1/design-user-service/my/preference",
                                         auth=True).get("uid") or "")
            except CloudError:
                self.uid = ""
        return self.uid

    @property
    def mqtt_username(self) -> str:
        name = _jwt_username(self.token)
        if name:
            return name
        uid = self.resolve_uid()
        return f"u_{uid}" if uid else ""

    def devices(self) -> list[dict]:
        data = self._req("/v1/iot-service/api/user/bind", auth=True)
        devs = data.get("devices")
        if not isinstance(devs, list):
            raise CloudError(f"取设备列表失败：{data}")
        out = []
        for d in devs:
            if not isinstance(d, dict):
                continue
            out.append({
                "sn": d.get("dev_id") or "",
                "name": d.get("name") or "",
                "model": d.get("dev_product_name") or "",
                "model_id": d.get("dev_model_name") or "",
                "access_code": d.get("dev_access_code") or "",
                "online": bool(d.get("online")),
                "print_status": d.get("print_status") or "",
                "firmware": d.get("firmware_version") or "",
            })
        return out


def _jwt_username(token: str) -> str:
    """从 JWT 的 payload 里取 username（形如 u_2839xxxx）。"""
    try:
        p = token.split(".")[1]
        p += "=" * (-len(p) % 4)
        d = json.loads(base64.urlsafe_b64decode(p).decode("utf-8", "ignore"))
    except Exception:
        return ""
    name = d.get("username") or ""
    if name.startswith("u_"):
        return name
    return f"u_{d['uid']}" if d.get("uid") else ""


# ---------------------------------------------------------------------------
# token 缓存
# ---------------------------------------------------------------------------
def save_token(cloud: Cloud) -> None:
    data = {"region": cloud.region, "base": cloud.base,
            "token": cloud.token, "uid": cloud.resolve_uid(),
            "saved_at": int(time.time())}
    fd = os.open(TOKEN_FILE, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
    with os.fdopen(fd, "w") as f:
        json.dump(data, f)
    print(f"[token] 已保存到 {TOKEN_FILE}（0600）")


def load_token(region: str | None) -> Cloud | None:
    if not os.path.exists(TOKEN_FILE):
        return None
    try:
        with open(TOKEN_FILE) as f:
            d = json.load(f)
    except Exception:
        return None
    cloud = Cloud(d.get("region") or region or "cn")
    if region:
        cloud.region = region
    cloud.token = d.get("token") or ""
    cloud.base = d.get("base") or cloud.base
    cloud.uid = d.get("uid") or ""
    age = (time.time() - int(d.get("saved_at") or 0)) / 86400
    if not cloud.token:
        return None
    print(f"[token] 复用缓存（{age:.0f} 天前保存，约 90 天过期）")
    return cloud


# ---------------------------------------------------------------------------
# 状态解析
# ---------------------------------------------------------------------------
STATE_ZH = {
    "IDLE": "空闲", "PREPARE": "准备中", "SLICING": "切片中", "RUNNING": "打印中",
    "PAUSE": "已暂停", "FINISH": "已完成", "FAILED": "已失败", "INIT": "初始化",
    "OFFLINE": "离线", "SLICING": "切片中",
}


def parse_status(p: dict) -> dict:
    """从 report 的 print 对象里抽 C3 屏上要显示的那些字段。"""
    ams_units = []
    for unit in (p.get("ams") or {}).get("ams") or []:
        trays = []
        for t in unit.get("tray") or []:
            if not isinstance(t, dict):
                continue
            trays.append({
                "id": t.get("id"),
                "type": t.get("tray_type") or "",
                "color": (t.get("tray_color") or "")[:6],
                "remain": t.get("remain"),
            })
        ams_units.append({"id": unit.get("id"), "humidity": unit.get("humidity"),
                          "temp": unit.get("temp"), "trays": trays})

    vt = p.get("vt_tray") or {}
    rem = p.get("mc_remaining_time")
    gs = str(p.get("gcode_state") or "").upper()
    return {
        "state": gs,
        "state_zh": STATE_ZH.get(gs, gs),
        "percent": p.get("mc_percent"),
        "remaining_min": rem,
        "layer": p.get("layer_num"),
        "total_layer": p.get("total_layer_num"),
        "file": p.get("subtask_name") or p.get("gcode_file") or "",
        "nozzle": p.get("nozzle_temper"),
        "nozzle_target": p.get("nozzle_target_temper"),
        "bed": p.get("bed_temper"),
        "bed_target": p.get("bed_target_temper"),
        "chamber": p.get("chamber_temper"),
        "wifi": p.get("wifi_signal") or "",
        "speed_level": p.get("spd_lvl"),
        "stage": p.get("stg_cur"),
        "print_error": p.get("print_error") or 0,
        "hms": [{"attr": h.get("attr"), "code": h.get("code")}
                for h in (p.get("hms") or []) if isinstance(h, dict)],
        "ams": ams_units,
        "vt_tray": {"type": vt.get("tray_type") or "",
                    "color": (vt.get("tray_color") or "")[:6]} if vt else None,
    }


def _fmt_temp(cur, tgt) -> str:
    if cur is None:
        return "—"
    return f"{cur:.0f}°→{tgt:.0f}°" if tgt else f"{cur:.0f}°"


def print_status(s: dict) -> None:
    print("\n" + "=" * 58)
    print(f"  状态      {s['state_zh']} ({s['state']})")
    print(f"  进度      {s['percent']}%   "
          f"层 {s['layer']}/{s['total_layer']}   剩余 {s['remaining_min']} 分钟")
    if s["file"]:
        print(f"  文件      {s['file']}")
    print(f"  温度      喷嘴 {_fmt_temp(s['nozzle'], s['nozzle_target'])}   "
          f"热床 {_fmt_temp(s['bed'], s['bed_target'])}   "
          f"腔体 {_fmt_temp(s['chamber'], None)}")
    print(f"  网络      {s['wifi']}   速度档 {s['speed_level']}   阶段 {s['stage']}")
    if s["print_error"]:
        print(f"  错误码    {s['print_error']}")
    if s["hms"]:
        print("  HMS       " + ", ".join(f"{h['attr']}-{h['code']}" for h in s["hms"]))
    for u in s["ams"]:
        parts = []
        for t in u["trays"]:
            if t["type"]:
                parts.append(f"槽{t['id']} {t['type']} #{t['color']}")
        if parts:
            print(f"  AMS{u['id']}      " + "  ".join(parts))
    print("=" * 58)


# ---------------------------------------------------------------------------
# 读一次状态
# ---------------------------------------------------------------------------
def read_status(host: str, username: str, password: str, sn: str,
                insecure: bool, wait: float = 8.0) -> dict:
    mq = MiniMqtt(host, MQTT_PORT, username, password,
                  client_id=f"c3probe-{sn[-6:]}" if sn else "c3probe",
                  insecure=insecure)
    mq.connect()
    try:
        mq.subscribe(f"device/{sn}/report")
        got = mq.poll(time.monotonic() + 1.5)          # 等 SUBACK 落地
        # 要一份全量：P1/A1 平时只推变化字段
        mq.publish(f"device/{sn}/request", json.dumps(
            {"pushing": {"sequence_id": "0", "command": "pushall", "version": 1,
                         "push_target": 1}}).encode())
        deadline = time.monotonic() + wait
        last_rx = None
        while time.monotonic() < deadline:
            msg = mq.poll(deadline)
            if msg is None:
                break
            last_rx = time.monotonic()
            try:
                data = json.loads(msg["payload"])
            except Exception:
                continue
            if isinstance(data.get("print"), dict):
                return data
        if last_rx is None:
            raise RuntimeError("已连上但没有收到状态报文（SN 对不对？）")
        raise RuntimeError("收到了报文但没有 print 段")
    finally:
        mq.close()


# ---------------------------------------------------------------------------
# 登录交互
# ---------------------------------------------------------------------------
def interactive_login(region: str, use_code: bool) -> Cloud:
    cloud = Cloud(region)
    account = os.environ.get("BAMBU_ACCOUNT") or input("拓竹账号（手机号或邮箱）：").strip()
    if not account:
        sys.exit("账号不能为空")

    if use_code:
        try:
            cloud.send_code(account)
        except CloudError as e:
            sys.exit(f"发验证码失败：{e}")
        print("验证码已发送，请查收短信/邮件")
        code = input("验证码：").strip()
        cloud.login_code(account, code)
    else:
        pwd = os.environ.get("BAMBU_PASSWORD") or getpass.getpass("密码（不回显）：")
        try:
            cloud.login_password(account, pwd)
        except CloudError as e:
            print(f"[!] 密码登录失败：{e}")
            print("[i] 试试验证码登录")
            try:
                cloud.send_code(account)
            except CloudError as e2:
                sys.exit(f"发验证码也失败：{e2}")
            code = input("验证码：").strip()
            cloud.login_code(account, code)

    uid = cloud.resolve_uid()
    print(f"[ok] 登录成功  区域={cloud.region}  接口={cloud.base}")
    print(f"     MQTT 用户名={cloud.mqtt_username}  token 长度={len(cloud.token)}")
    if not uid:
        print("[!] 没拿到 uid，云 MQTT 可能连不上")
    save_token(cloud)
    return cloud


# ---------------------------------------------------------------------------
# main
# ---------------------------------------------------------------------------
def main() -> int:
    ap = argparse.ArgumentParser(description="拓竹云端登录 + 打印机状态读取")
    ap.add_argument("cmd", nargs="?", default="status",
                    choices=["login", "devices", "status"],
                    help="login=登录并缓存 token; devices=设备列表; status=读状态（默认）")
    ap.add_argument("--region", choices=["cn", "intl"], help="账号区域，默认 cn")
    ap.add_argument("--code", action="store_true", help="用验证码登录（不发密码）")
    ap.add_argument("--sn", help="指定序列号，默认取设备列表第一台")
    ap.add_argument("--watch", type=float, default=0, help="持续读取的秒数")
    ap.add_argument("--json", action="store_true", help="输出 JSON")
    ap.add_argument("--raw", action="store_true", help="打印原始 report 报文")
    ap.add_argument("--relogin", action="store_true", help="忽略缓存的 token")
    # 仅本地调试：直接连打印机 8883，用来验证 MQTT 报文层
    ap.add_argument("--local", metavar="IP", help=argparse.SUPPRESS)
    ap.add_argument("--access-code", metavar="CODE", help=argparse.SUPPRESS)
    args = ap.parse_args()

    # ---- 本地直连调试模式：不登录、不走云 ----
    if args.local:
        if not (args.sn and args.access_code):
            sys.exit("--local 需要同时给 --sn 和 --access-code")
        data = read_status(args.local, "bblp", args.access_code, args.sn,
                           insecure=True, wait=8)
        if args.raw:
            print(json.dumps(data, ensure_ascii=False, indent=2))
        if args.json:
            print(json.dumps(parse_status(data["print"]), ensure_ascii=False))
        else:
            print_status(parse_status(data["print"]))
        return 0

    # ---- 拿 token ----
    cloud = None if args.relogin else load_token(args.region)
    if cloud is None:
        if args.cmd == "login" or sys.stdin.isatty():
            cloud = interactive_login(args.region or "cn", args.code)
        else:
            sys.exit("没有可用的 token，请先跑："
                     "python3 controller/tools/pc/bambu_cloud.py login")

    if args.cmd == "login":
        return 0

    # ---- 设备列表 ----
    try:
        devs = cloud.devices()
    except CloudError as e:
        sys.exit(f"取设备列表失败：{e}（token 可能过期，试试 --relogin）")

    if args.cmd == "devices":
        if args.json:
            out = [{**d, "access_code": ("*" * 6 if d["access_code"] else "")}
                   for d in devs]
            print(json.dumps(out, ensure_ascii=False, indent=2))
            return 0
        if not devs:
            print("账号下没有已绑定的打印机")
            return 1
        print(f"\n账号下共 {len(devs)} 台打印机：\n")
        for d in devs:
            code = d["access_code"]
            print(f"  {d['name'] or '(无名)'}   [{d['model']}]")
            print(f"    SN       {d['sn']}")
            print(f"    在线     {'是' if d['online'] else '否'}   "
                  f"任务状态 {d['print_status'] or '—'}")
            print(f"    访问码   {code[:2] + '****' + code[-2:] if len(code) == 8 else code or '—'}")
            if d["firmware"]:
                print(f"    固件     {d['firmware']}")
            print()
        return 0

    # ---- 读状态 ----
    sn = args.sn or (devs[0]["sn"] if devs else "")
    if not sn:
        sys.exit("没有可用序列号，用 --sn 指定")
    if not cloud.mqtt_username:
        sys.exit("拿不到 uid，云 MQTT 用户名拼不出来")

    deadline = time.time() + args.watch
    while True:
        try:
            data = read_status(cloud.mqtt_host, cloud.mqtt_username, cloud.token, sn,
                               insecure=False)
        except (RuntimeError, OSError, ssl.SSLError) as e:
            sys.exit(f"读取失败：{e}")
        if args.raw:
            print(json.dumps(data, ensure_ascii=False, indent=2))
        if args.json:
            print(json.dumps(parse_status(data["print"]), ensure_ascii=False))
        else:
            print_status(parse_status(data["print"]))
        if not args.watch or time.time() >= deadline:
            return 0
        time.sleep(5)


if __name__ == "__main__":
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        sys.exit(130)
