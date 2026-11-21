#!/usr/bin/env python3
"""拓竹打印机 → SEMI-AMS 控制端（C3）MQTT 桥。

PC 负责跟打印机打交道（TLS / 云端 token 都在这边），把状态压成一条精简 JSON，
明文 ASCII 发到 C3 内置的 broker（默认 1883）。C3 不需要 mbedtls，也不需要联网。

数据流向：

    打印机 8883 (TLS)  ──┐
                          ├─► 本脚本 ─► C3 1883 (明文) ─► /api/state ─► 网页
    拓竹云 8883 (TLS)  ──┘

用法（在仓库根目录执行，--broker 必填）：

  python3 controller/tools/pc/bambu_bridge.py \
      --broker 192.168.1.100:1883 --source local --ip 192.168.1.50

  # 先本地、失败回落云端，其它参数用默认
  python3 controller/tools/pc/bambu_bridge.py --broker 192.168.1.100:1883

  # 只走云端（用 bambu_cloud.py 的 token）
  python3 controller/tools/pc/bambu_bridge.py --broker 192.168.1.100:1883 --source cloud

  # 调试：只打印 JSON 不发送（这种模式不连 broker，--broker 可省）
  python3 controller/tools/pc/bambu_bridge.py --source local --ip 192.168.1.50 --dry-run -v

  # 发一条就退出
  python3 controller/tools/pc/bambu_bridge.py --broker 192.168.1.100:1883 --once

访问码来源（按顺序）：
  1. --code 参数
  2. Bambu Studio 配置里的 access_code（SN → 8 位码，跟着云端同步）

两条都拿不到时，--source auto 会回落到云端 MQTT（那条路只要 token，不要访问码）。
"""
from __future__ import annotations

import argparse
import json
import os
import socket
import ssl
import struct
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from bambu_cloud import Cloud, CloudError, MiniMqtt, load_token  # noqa: E402

MQTT_PORT = 8883
BROKER_EXAMPLE = "192.168.1.100:1883"   # 仅作文案示例，不是默认值
DEFAULT_TOPIC = "semiams/printer"   # 末尾 /printer 是 C3 侧认的钩子

# HMS_0500-0500-0001-0007：打印机拒绝了一条没有签名的 MQTT 命令。
# 打印机绑着云账号时，较新的固件会给每条 print 命令验签名（用的密钥在 Bambu
# Studio 的私有网络插件里，第三方拿不到）；只有开了「仅局域网 + 开发者模式」
# 才会无条件接受。只读的 report 不验签，所以监控一直是好的，只有控制会失败。
AUTH_HMS = (0x05000500, 0x00010007)


# ---------------------------------------------------------------------------
# Bambu Studio 配置里的访问码
# ---------------------------------------------------------------------------
def studio_conf_path() -> str | None:
    cands = []
    appdata = os.environ.get("APPDATA")            # Windows
    if appdata:
        cands.append(os.path.join(appdata, "BambuStudio", "BambuStudio.conf"))
    cands.append(os.path.expanduser(                 # macOS
        "~/Library/Application Support/BambuStudio/BambuStudio.conf"))
    cands.append(os.path.expanduser("~/.config/BambuStudio/BambuStudio.conf"))
    for p in cands:
        if os.path.exists(p):
            return p
    return None


def studio_access_codes() -> dict[str, str]:
    """从 Bambu Studio 配置里读 SN → 访问码（只读，不改原文件）。"""
    p = studio_conf_path()
    if not p:
        return {}
    try:
        with open(p, encoding="utf-8", errors="ignore") as f:
            # 文件末尾会跟一行 "# MD5 checksum ..."，所以只解析第一个 JSON 对象
            data, _ = json.JSONDecoder().raw_decode(f.read().lstrip())
        codes = data.get("access_code")
        return {str(k): str(v) for k, v in codes.items() if k and v} if isinstance(codes, dict) else {}
    except Exception:
        return {}


# ---------------------------------------------------------------------------
# 从打印机自签证书的 CN 拿序列号
# ---------------------------------------------------------------------------
def _der_children(buf: bytes, i: int, end: int):
    out = []
    while i < end:
        tag = buf[i]
        i += 1
        ln = buf[i]
        i += 1
        if ln & 0x80:
            n = ln & 0x7F
            ln = int.from_bytes(buf[i:i + n], "big")
            i += n
        out.append((tag, i, i + ln))
        i += ln
    return out


def cert_cn(der: bytes) -> str:
    """取证书 Subject CN。拓竹打印机用自签证书，CN 就是序列号。"""
    try:
        top = _der_children(der, 0, len(der))
        cert = _der_children(der, top[0][1], top[0][2])
        tbs = _der_children(der, cert[0][1], cert[0][2])
        idx = 1 if tbs and tbs[0][0] == 0xA0 else 0
        # serial, signature, issuer, validity, subject
        s_tag, s_start, s_end = tbs[idx + 4]
        for _t, rdn_s, rdn_e in _der_children(der, s_start, s_end):
            for _t2, a_s, a_e in _der_children(der, rdn_s, rdn_e):
                items = _der_children(der, a_s, a_e)
                if len(items) < 2:
                    continue
                oid = der[items[0][1]:items[0][2]]
                if oid == b"\x55\x04\x03":       # 2.5.4.3 commonName
                    return der[items[1][1]:items[1][2]].decode("utf-8", "replace")
    except Exception:
        pass
    return ""


def discover_sn(ip: str, timeout: float = 4.0) -> str:
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
    ctx.check_hostname = False
    ctx.verify_mode = ssl.CERT_NONE
    try:
        with socket.create_connection((ip, MQTT_PORT), timeout=timeout) as raw:
            with ctx.wrap_socket(raw) as ss:
                return cert_cn(ss.getpeercert(binary_form=True) or b"")
    except OSError:
        return ""


# ---------------------------------------------------------------------------
# 打印机数据源（保持一条长连接，做增量合并）
# ---------------------------------------------------------------------------
def deep_update(dst: dict, src: dict) -> None:
    """P1/A1 系列只推变化字段，必须增量合并，否则状态会漂。"""
    for k, v in src.items():
        if isinstance(v, dict) and isinstance(dst.get(k), dict):
            deep_update(dst[k], v)
        else:
            dst[k] = v


class PrinterLink:
    def __init__(self, host: str, username: str, password: str, sn: str,
                 insecure: bool, label: str, src: str = "local"):
        self.host, self.username, self.password = host, username, password
        self.sn, self.insecure, self.label = sn, insecure, label
        self.src = src                    # "local" / "cloud"，C3 顶栏用它显示「本地 · 在线」
        self.model = ""                   # 从 info.get_version 的 product_name 读，如 "A1"
        self.state: dict = {"print": {}}
        self.mq: MiniMqtt | None = None
        self._last_push = 0.0
        self._last_rx = 0.0
        self._last_tx = 0.0               # 最后一次往打印机发字节（判心跳用）
        self._version_req = 0.0
        self._seq = 0                     # 下发命令的 sequence_id，0-9999 循环
        self._hms_hint = 0.0              # 上次「命令被拒」告警的时间，防刷屏

    def open(self, timeout: float = 8.0) -> None:
        self.close()
        mq = MiniMqtt(self.host, MQTT_PORT, self.username, self.password,
                      client_id=f"c3b-{self.sn[-6:]}" if self.sn else "c3bridge",
                      insecure=self.insecure, keepalive=60)
        mq.connect(timeout=timeout)
        mq.subscribe(f"device/{self.sn}/report")
        self.mq = mq
        self._last_rx = time.monotonic()
        self._last_tx = time.monotonic()
        self.request_pushall()
        self.request_version()
        print(f"[{self.label}] 已连接 {self.host}，订阅 device/{self.sn}/report")

    def close(self) -> None:
        if self.mq:
            self.mq.close()
            self.mq = None

    def request_pushall(self) -> None:
        if not self.mq:
            return
        self.mq.publish(f"device/{self.sn}/request", json.dumps(
            {"pushing": {"sequence_id": "0", "command": "pushall",
                         "version": 1, "push_target": 1}}).encode())
        self._last_push = time.monotonic()
        self._last_tx = self._last_push

    def send_print(self, body: dict) -> None:
        """往 device/{SN}/request 下发一条 print 命令（进料/退料用）。

        MiniMqtt 只实现了 qos0，拓竹在忙着推状态时偶尔会漏收，所以网页那边
        不承诺「已执行」，只承诺「已下发」，真实结果看 gcode_state / tray_now。
        """
        if not self.mq:
            raise RuntimeError("打印机没连上")
        self._seq = (self._seq + 1) % 10000
        body.setdefault("print", {})["sequence_id"] = str(self._seq)
        self.mq.publish(f"device/{self.sn}/request", json.dumps(body).encode(), qos=0)
        self._last_tx = time.monotonic()

    def request_version(self) -> None:
        """问一次机型：info.get_version 的 module[].product_name = "Bambu Lab A1"。

        这是唯一能「本地」拿到型号的路子——状态报文里没有机型字段。
        """
        if not self.mq:
            return
        self.mq.publish(f"device/{self.sn}/request", json.dumps(
            {"info": {"sequence_id": "0", "command": "get_version"}}).encode())
        self._version_req = time.monotonic()
        self._last_tx = self._version_req

    def _take_version(self, info: dict) -> bool:
        """注意报文形状：{"info":{"command":"get_version","module":[…]}}，
        module 直接挂在 info 下，没有嵌套的 get_version 键。"""
        mods = info.get("module")
        if not isinstance(mods, list):
            return False
        for m in mods:
            if not isinstance(m, dict):
                continue
            pn = str(m.get("product_name") or "").strip()
            if pn:
                # "Bambu Lab A1 mini" → "A1 mini"
                self.model = pn[10:] if pn.lower().startswith("bambu lab ") else pn
                print(f"[{self.label}] 机型 {pn} → {self.model}")
                return True
        return False

    def _scan_hms(self, delta: dict) -> None:
        """盯一眼报文里新出现的 HMS，撞上「命令校验失败」就大声喊一次。

        这是第三方控制最容易踩的坑：命令被固件拒了，桥接这边只看到「已发布」，
        网页毫无反应，只有拓竹 App 弹一条 HMS。日志里说清楚，省得去猜是不是
        报文格式写错了。

        只看增量里的 hms（不是 self.state），否则这条 HMS 会一直留在合并后的
        状态里，每 5 分钟重复告警一次。
        """
        hms = delta.get("hms")
        if not isinstance(hms, list):
            return
        for h in hms:
            if not isinstance(h, dict):
                continue
            if (_i(h.get("attr")), _i(h.get("code"))) != AUTH_HMS:
                continue
            if time.monotonic() - self._hms_hint > 300:
                self._hms_hint = time.monotonic()
                print("[!] 打印机拒了上一条命令：HMS_0500-0500-0001-0007"
                      "（MQTT 命令校验失败）\n"
                      "    固件会给 print 命令验签名，第三方签不出来。需要在打印机上开"
                      "「局域网模式 + 开发者模式」\n"
                      "    （设置 → 局域网模式 → 开发者模式），详见 "
                      "controller/docs/printer-mqtt.md 的「控制命令被拒」一节。\n"
                      "    只读的状态上报不走这条校验，所以监控不受影响。")
            return

    def pump(self, budget: float) -> bool:
        """收预算时间内的报文并合并。返回是否拿到了新数据。

        连接被对端重置会抛 OSError，由 main 里的重连逻辑兜住——
        长跑时打印机会主动断（TCP RST / keepalive 超时），不能让它带崩进程。
        """
        got = False
        deadline = time.monotonic() + budget
        while time.monotonic() < deadline:
            if not self.mq:
                return got
            # MQTT keepalive=60，规范要求空闲时也必须发 PINGREQ，
            # 否则打印机会在 ~90 秒后单方面断开（实测 30 分钟才被重置，但根因是这个）
            if time.monotonic() - self._last_tx > 25:
                self.mq.ping()
                self._last_tx = time.monotonic()
            msg = self.mq.poll(min(deadline, time.monotonic() + 0.5))
            if msg is None:
                continue
            try:
                data = json.loads(msg["payload"])
            except Exception:
                continue
            if isinstance(data.get("info"), dict):
                self._take_version(data["info"])
                continue
            if isinstance(data.get("print"), dict):
                deep_update(self.state["print"], data["print"])
                self._scan_hms(data["print"])
                self._last_rx = time.monotonic()
                got = True
        # 空闲 45 秒补一次全量，防止漏字段
        if time.monotonic() - self._last_rx > 45:
            self.request_pushall()
            self._last_rx = time.monotonic()
        # 还没拿到机型就 30 秒重问一次（打印机忙时可能漏回）
        if not self.model and time.monotonic() - self._version_req > 30:
            self.request_version()
        return got


# ---------------------------------------------------------------------------
# 压缩成 C3 能直接吃的精简 JSON（受 MQTT_BODY_MAX = 768 字节限制）
# ---------------------------------------------------------------------------
def _i(v, default=0) -> int:
    try:
        return int(float(v))
    except (TypeError, ValueError):
        return default


def _rssi(v) -> int:
    """'-58dBm' → -58"""
    if isinstance(v, (int, float)):
        return int(v)
    s = str(v or "").replace("dBm", "").strip()
    try:
        return int(float(s))
    except ValueError:
        return 0


def compact(p: dict, ident: dict | None = None) -> dict:
    """字段名尽量短，因为整个 payload 要塞进 768 字节。

    AMS 用 "材料:颜色:余量|…" 这种扁平分号串，不用数组：
    C3 的 jsonmac 只认扁平键，拆字符串比在固件里加数组解析器省事。
    ident 是「设备身份」（型号/SN/来源），顶栏那几个字段。
    """
    slots = []
    unit0 = None
    for unit in (p.get("ams") or {}).get("ams") or []:
        if unit0 is None:
            unit0 = unit
        for t in (unit.get("tray") or []):
            if not isinstance(t, dict) or not t.get("tray_type"):
                continue
            slots.append("{}:{}:{}".format(
                str(t.get("tray_type"))[:6],
                str(t.get("tray_color") or "")[:6].upper(),
                _i(t.get("remain"), -1)))
    # HMS 官方写法是四段 16 位十六进制：attr 拆成前两段，code 拆成后两段，
    # 例如 attr=0x05000500 / code=0x00010007 → "0500-0500-0001-0007"
    hms = ["{:04X}-{:04X}-{:04X}-{:04X}".format(
               _i(h.get("attr")) >> 16, _i(h.get("attr")) & 0xFFFF,
               _i(h.get("code")) >> 16, _i(h.get("code")) & 0xFFFF)
           for h in (p.get("hms") or []) if isinstance(h, dict)]
    vt = p.get("vt_tray") or {}
    out = {
        "st": str(p.get("gcode_state") or ""),
        "pct": _i(p.get("mc_percent")),
        "ly": _i(p.get("layer_num")),
        "tly": _i(p.get("total_layer_num")),
        "rem": _i(p.get("mc_remaining_time")),
        "f": str(p.get("subtask_name") or p.get("gcode_file") or "")[:40],
        "nz": _i(p.get("nozzle_temper")),
        "nzt": _i(p.get("nozzle_target_temper")),
        "bd": _i(p.get("bed_temper")),
        "bdt": _i(p.get("bed_target_temper")),
        "ch": _i(p.get("chamber_temper")),
        "wf": _rssi(p.get("wifi_signal")),
        "spd": _i(p.get("spd_lvl")),
        "er": _i(p.get("print_error")),
        "hn": len(hms),
        "h1": hms[0] if hms else "",
        "ams": "|".join(slots[:4]),
        # AMS 单元号 / 湿度档位（1-5，固件给的是档位不是百分比）/ 温度（Lite 不报，-1）
        "aid": str((unit0 or {}).get("id") if unit0 else ""),
        "amsh": _i((unit0 or {}).get("humidity"), -1),
        "amst": _i(float((unit0 or {}).get("temp") or -1), -1) if (unit0 or {}).get("temp") else -1,
        "tn": _i((p.get("ams") or {}).get("tray_now"), 255),
        "vt": ("{}:{}".format(str(vt.get("tray_type"))[:6],
                              str(vt.get("tray_color") or "")[:6].upper())
               if vt.get("tray_type") else ""),
    }
    if ident:
        out.update({k: v for k, v in ident.items() if v})
    return out


# ---------------------------------------------------------------------------
# 连 C3 的 broker
# ---------------------------------------------------------------------------
class BrokerLink:
    def __init__(self, host: str, port: int, user: str, password: str):
        self.host, self.port, self.user, self.password = host, port, user, password
        self.mq: MiniMqtt | None = None
        self._last_ping = 0.0

    def open(self) -> None:
        self.close()
        mq = MiniMqtt(self.host, self.port, self.user, self.password,
                      client_id="c3bridge", keepalive=60, secure=False)
        mq.connect(timeout=6)
        self.mq = mq
        self._last_ping = time.monotonic()
        print(f"[C3] 已连接 {self.host}:{self.port}")

    def subscribe(self, topic: str) -> None:
        assert self.mq
        self.mq.subscribe(topic)
        print(f"[C3] 已订阅 {topic}（下行命令）")

    def poll(self, budget: float = 0.02) -> dict | None:
        """取一条下行消息；没有就返回 None。"""
        if not self.mq:
            return None
        try:
            return self.mq.poll(time.monotonic() + budget)
        except OSError:
            return None

    def close(self) -> None:
        if self.mq:
            self.mq.close()
            self.mq = None

    def publish(self, topic: str, payload: bytes) -> int:
        assert self.mq
        self.mq.publish(topic, payload, qos=0)
        self.keepalive()
        return len(payload)

    def keepalive(self) -> None:
        if self.mq and time.monotonic() - self._last_ping > 30:
            try:
                self.mq.ping()
            except OSError:
                pass
            self._last_ping = time.monotonic()


# ---------------------------------------------------------------------------
# 下行命令：网页点「进料/退料」→ C3 发布 <topic>/cmd → 这里转给打印机
# ---------------------------------------------------------------------------
def filament_cmd(kind: str) -> dict:
    """进退料都走 ams_change_filament（bambu-printer-manager 的实测写法）：
    load   → slot_id / target = 254（外挂虚拟槽位），温度 -1 = 用耗材档案里的值
    unload → slot_id / target = 255（不指定槽位，退掉当前那一路）
    """
    if kind == "load":
        return {"print": {"command": "ams_change_filament", "ams_id": 0,
                          "slot_id": 254, "target": 254,
                          "curr_temp": -1, "tar_temp": -1, "soft_temp": 0}}
    return {"print": {"command": "ams_change_filament", "ams_id": 0,
                      "slot_id": 255, "target": 255}}


PRINT_ACTS = ("pause", "resume", "stop")


def print_cmd(kind: str) -> dict:
    """暂停/继续/停止：拓竹的统一格式是 {print:{command,param}}。
    和进退料的 ams_change_filament 是两套完全不同的命令，别混用。"""
    return {"print": {"command": kind, "param": ""}}


def run_print_cmd(msg: dict, link: PrinterLink) -> None:
    """白名单执行一条下行命令。宁可什么都不做，也不把未知命令转给打印机。"""
    try:
        req = json.loads((msg.get("payload") or b"{}").decode("utf-8", "replace"))
    except ValueError:
        print("[cmd] 命令不是合法 JSON，丢掉")
        return
    kind = str(req.get("c") or "")
    if kind not in ("load", "unload") + PRINT_ACTS:
        print(f"[cmd] 不认识的动作 {kind!r}，丢掉")
        return
    st = str((link.state.get("print") or {}).get("gcode_state") or "")
    printing = st in ("RUNNING", "PAUSE", "PREPARE")
    # 每个动作只有唯一合法的状态窗口：暂停要打印中、继续要暂停中、停止要有任务
    if kind in PRINT_ACTS:
        if not ((kind == "pause" and st == "RUNNING")
                or (kind == "resume" and st == "PAUSE")
                or (kind == "stop" and printing)):
            print(f"[cmd] 打印机当前 {st or '?'}，不能 {kind}，丢掉")
            return
        link.send_print(print_cmd(kind))
        print(f"[cmd] 已下发 {kind}（打印机 {st}）")
        return
    if printing:
        print(f"[cmd] 打印机正在 {st}，拒绝 {kind}")
        return
    link.send_print(filament_cmd(kind))
    print(f"[cmd] 已下发 {kind}（打印机状态 {st or '?'}）")


# ---------------------------------------------------------------------------
# 取数：本地优先，失败回落云端
# ---------------------------------------------------------------------------
def pick_source(args) -> PrinterLink:
    codes = studio_access_codes()

    if args.source in ("auto", "local"):
        ip, sn, code = args.ip, args.sn, args.code
        if not ip:
            print("[!] 没给 --ip，本地直连跳过")
        else:
            if not sn:
                sn = discover_sn(ip)
                if sn:
                    print(f"[本地] 从证书 CN 拿到序列号 {sn}")
            if not code and sn:
                code = codes.get(sn, "")
                if code:
                    print(f"[本地] 从 Bambu Studio 配置拿到访问码 "
                          f"{code[:2]}****{code[-2:]}")
            if sn and code:
                link = PrinterLink(ip, "bblp", code, sn, insecure=True, label="本地",
                                   src="local")
                try:
                    link.open()
                    return link
                except (OSError, RuntimeError, ssl.SSLError) as e:
                    print(f"[!] 本地直连失败：{e}")
                    if args.source == "local":
                        sys.exit(1)

    # ---- 云端 ----
    cloud = load_token(args.region)
    if cloud is None:
        sys.exit("本地不可用，也没有云端 token。先跑："
                 "python3 controller/tools/pc/bambu_cloud.py login")
    devs = cloud.devices()
    sn = args.sn or (devs[0]["sn"] if devs else "")
    if not sn:
        sys.exit("账号下没有已绑定的打印机")
    link = PrinterLink(cloud.mqtt_host, cloud.mqtt_username, cloud.token, sn,
                       insecure=False, label="云端", src="cloud")
    link.open()
    return link


def cloud_model(sn: str, region: str | None) -> str:
    """云端 bind 拿型号。没 token / 没网就返回空，不影响本地取数。

    只取 model：昵称（bind.name）本地和云端都不值得为它多一次查询，
    顶栏也不显示了。
    """
    try:
        cloud = load_token(region)
        if cloud is None:
            return ""
        for d in cloud.devices():
            if d.get("sn") == sn:
                return d.get("model") or ""
    except Exception:
        pass
    return ""


def main() -> int:
    ap = argparse.ArgumentParser(description="拓竹状态 → SEMI-AMS 控制端 MQTT 桥")
    ap.add_argument("--broker", default="",
                    help=f"必填，C3 地址 IP:端口，如 {BROKER_EXAMPLE}")
    ap.add_argument("--topic", default=DEFAULT_TOPIC, help=f"发布主题，默认 {DEFAULT_TOPIC}")
    ap.add_argument("--user", default="", help="C3 broker 用户名（没设就是匿名）")
    ap.add_argument("--password", default="", help="C3 broker 密码")
    ap.add_argument("--source", choices=["auto", "local", "cloud"], default="auto")
    ap.add_argument("--ip", default="", help="打印机 IP（本地模式）")
    ap.add_argument("--sn", default="", help="打印机序列号，不给就自动探")
    ap.add_argument("--code", default="", help="访问码，不给就读 Bambu Studio 配置")
    ap.add_argument("--model", default="", help="机型（默认自动读 info.get_version）")
    ap.add_argument("--region", choices=["cn", "intl"], help="云端账号区域")
    ap.add_argument("--interval", type=float, default=5.0, help="发布间隔秒，默认 5")
    ap.add_argument("--once", action="store_true", help="发一条就退出")
    ap.add_argument("--cmd", choices=["load", "unload", "pause", "resume", "stop"],
                    help="只发一条命令就退出（调试用）：load/unload 进退料，pause/resume/stop 控制打印")
    ap.add_argument("--dry-run", action="store_true", help="只打印，不发给 C3")
    ap.add_argument("-v", "--verbose", action="store_true", help="打印每次发布的 JSON")
    args = ap.parse_args()

    # --broker 必须显式给出：C3 地址是环境相关的，藏在默认值里容易发到错的机器。
    # 只有 --dry-run / --cmd 豁免——这两种都不需要 C3。
    if not args.dry_run and not args.cmd and not args.broker:
        ap.error(f"必须显式指定 --broker（例如 --broker {BROKER_EXAMPLE}）")

    bh, _, bp = args.broker.partition(":")
    broker = None
    if not args.dry_run and not args.cmd:
        try:
            broker = BrokerLink(bh, int(bp or 1883), args.user, args.password)
            broker.open()
            broker.subscribe(args.topic + "/cmd")
        except (OSError, RuntimeError) as e:
            sys.exit(f"连不上 C3 broker {args.broker}：{e}")

    link = pick_source(args)

    # 调试用：只发一条命令就退出，用来验证协议（不需要 C3）
    if args.cmd:
        body = (print_cmd(args.cmd) if args.cmd in PRINT_ACTS
                else filament_cmd(args.cmd))
        # dry-run 绝不能碰打印机：stop 是不可撤销的，误发就是废一个任务
        if args.dry_run:
            print(f"[dry-run] 只打印不发送：{json.dumps(body, ensure_ascii=False)}")
        else:
            link.send_print(body)
            tip = {"load": "喷嘴会升温", "unload": "喷嘴会升温",
                   "pause": "打印会立刻停在当前层", "resume": "从暂停的那一层接着打",
                   "stop": "打印机会立刻中断当前任务"}[args.cmd]
            print(f"[cmd] 已下发 {args.cmd}，去打印机上看反应（{tip}）")
        link.close()
        return 0

    # 顶栏身份：SN（证书 CN）、型号（get_version，云端可补）、来源。
    # 拿不到也不影响取数，只是顶栏少一段。
    cmodel = "" if args.model else cloud_model(link.sn, args.region)

    def ident() -> dict:
        return {"sn": link.sn, "src": link.src,
                "md": args.model or cmodel or link.model}

    last_sent = 0.0
    last_sig = None
    backoff = 1.0                        # 断线重连退避，成功后复位
    try:
        while True:
            try:
                link.pump(max(1.0, args.interval))
            except (OSError, RuntimeError, ssl.SSLError) as e:
                # 打印机侧断连（TCP RST / keepalive 超时 / 打印机重启）不该带崩桥接
                print(f"[!] 打印机连接中断：{type(e).__name__}: {e}"
                      f"，{backoff:.0f}s 后重连")
                link.close()
                time.sleep(backoff)
                try:
                    link.open()
                    backoff = 1.0
                except (OSError, RuntimeError, ssl.SSLError) as e2:
                    print(f"[!] 重连失败：{e2}")
                    backoff = min(backoff * 2, 30.0)
                continue

            # 下行命令：C3 网页点「进料/退料」→ semiams/printer/cmd
            if broker:
                msg = broker.poll(0.02)
                if msg:
                    try:
                        run_print_cmd(msg, link)
                    except (OSError, RuntimeError) as e:
                        print(f"[cmd] 下发失败：{e}")

            p = link.state.get("print") or {}
            if not p.get("gcode_state"):
                time.sleep(0.5)
                continue

            idt = ident()
            payload = compact(p, idt)
            # 必须 ensure_ascii=False：C3 的 jsonmac 只会按字节抓字符串，
            # 发成 \uXXXX 转义的话它拿到的就是字面量
            body = json.dumps(payload, separators=(",", ":"),
                              ensure_ascii=False).encode("utf-8")

            sig = (payload["st"], payload["pct"], payload["ly"], payload["er"],
                   payload["hn"], payload["ams"], idt["md"], idt["sn"])
            due = time.monotonic() - last_sent >= args.interval
            changed = sig != last_sig
            # 状态变化立刻发，否则按 interval 发（云端有 2 秒粒度，不必更密）
            if changed or due:
                if args.verbose or args.dry_run:
                    print(f"[{payload['st'] or '?'}] {payload['pct']}% "
                          f"{payload['ly']}/{payload['tly']}层 {len(body)}B  {body.decode()}")
                if broker:
                    try:
                        broker.publish(args.topic, body)
                    except (OSError, RuntimeError) as e:
                        print(f"[!] 发布失败：{e}，重连 C3")
                        broker.close()
                        time.sleep(1)
                        try:
                            broker.open()
                            broker.subscribe(args.topic + "/cmd")
                        except (OSError, RuntimeError) as e2:
                            # C3 掉线/重启时不要退出，下一轮还会再试
                            print(f"[!] C3 重连失败：{e2}")
                last_sent = time.monotonic()
                last_sig = sig

            if args.once:
                return 0
    except KeyboardInterrupt:
        print("\n[退出]")
    finally:
        link.close()
        if broker:
            broker.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
