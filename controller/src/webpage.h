// 控制端的网页（全部塞进固件，不依赖 LittleFS，刷完就能用）
#pragma once

// 主页面：MQTT 设置卡片 + 设备表（行数跟着设备数走）
static const char PAGE_INDEX[] PROGMEM = R"HTMLX(
<!DOCTYPE html>
<html lang="zh-CN">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>SEMI-AMS 控制端</title>
<style>
*{box-sizing:border-box}
body{margin:0;background:#f4f5f7;color:#1f2329;
     font:14px/1.5 -apple-system,BlinkMacSystemFont,"PingFang SC","Microsoft YaHei",sans-serif}
.wrap{max-width:1180px;margin:0 auto;padding:18px 16px 40px}
header{display:flex;align-items:center;gap:12px;margin:4px 0 14px}
h1{font-size:18px;margin:0;font-weight:600}
.pill{display:inline-flex;align-items:center;gap:6px;font-size:12px;padding:3px 10px;border-radius:999px;
      background:#eef0f3;color:#5b6472;border:1px solid #e2e5ea}
.pill i{width:7px;height:7px;border-radius:50%;background:#98a1ae;display:inline-block}
.pill.ok{background:#eaf7ee;border-color:#cbe8d5;color:#1e7a3c}
.pill.ok i{background:#2f9e44}
.pill.warn{background:#fdf3e6;border-color:#f6e0bf;color:#8a5a00}
.pill.warn i{background:#e08a1e}
.pill.bad{background:#fdeceb;border-color:#f6cfcc;color:#a12a20}
.pill.bad i{background:#c0392b}
.card{background:#fff;border:1px solid #e6e8ec;border-radius:10px;margin-bottom:16px;
      box-shadow:0 1px 2px rgba(16,24,40,.04)}
.cfg{display:grid;grid-template-columns:1.1fr .7fr 1fr 1fr 1fr;gap:14px;padding:16px 18px 8px}
label{display:block;font-size:12px;color:#5b6472;margin-bottom:5px}
input[type=text],input[type=password],input[type=number]{width:100%;height:34px;padding:0 10px;
      border:1px solid #dcdfe5;border-radius:6px;background:#fff;font-size:13px;color:#1f2329;outline:none}
input[type=text]:focus,input[type=password]:focus{border-color:#3b6fd4}
/* 只读展示项（本机 IP）：长得像输入框，但是纯文本，点不了也改不了 */
.ro{height:34px;line-height:32px;padding:0 10px;border:1px solid #eceef2;border-radius:6px;
    background:#f5f6f8;font-size:13px;color:#1f2329;font-family:ui-monospace,SFMono-Regular,Menlo,monospace;
    white-space:nowrap;overflow:hidden;text-overflow:ellipsis;cursor:default}
.ro.empty{color:#a0a6b0;font-family:inherit}
input:focus{border-color:#3b6fd4;box-shadow:0 0 0 3px rgba(59,111,212,.12)}
.cfgfoot{display:flex;align-items:center;gap:12px;padding:0 18px 16px}
button{font:inherit;cursor:pointer;border:1px solid transparent;border-radius:6px;color:#fff;
       padding:7px 14px;font-size:13px;line-height:1;transition:filter .15s}
button:hover{filter:brightness(1.06)}
button:disabled{opacity:.45;cursor:not-allowed;filter:none}
.primary{background:#2b7fd4}
.green{background:#2f9e44}.orange{background:#e08a1e}.red{background:#c0392b}.blue{background:#2b7fd4}
/* 删除按钮：白底红字，免得和「紧急停止」两个实心红按钮混在一起 */
.del{background:#fff;border:1px solid #f0c6c2;color:#c0392b}
.del:hover{background:#fdeceb;filter:none}
.hint{font-size:12px;color:#8a929e}
tr.emptyrow td{padding:26px 12px;text-align:center;color:#9aa2ae;font-size:13px;cursor:pointer}
tr.emptyrow:hover td{background:#f7f8fa;color:#3b6fd4}
button.plain{background:#fff;border:1px solid #dcdfe5;color:#5b6472;padding:5px 10px;font-size:12px;border-radius:6px}
button.plain:hover{background:#f7f8fa;filter:none}
.head{display:flex;align-items:center;gap:10px;padding:13px 18px;border-bottom:1px solid #eef0f3}
.head b{font-size:14px;font-weight:600}
.head .hint{margin-left:auto}
table{width:100%;border-collapse:collapse;table-layout:fixed}
th{font-size:12.5px;font-weight:500;color:#5b6472;text-align:left;padding:11px 12px;
   background:#fafbfc;border-bottom:1px solid #eef0f3;white-space:nowrap}
td{padding:10px 12px;border-bottom:1px solid #f0f2f5;vertical-align:middle}
tr:last-child td{border-bottom:none}
tbody tr:hover{background:#fafbfd}
.mono{font-family:ui-monospace,SFMono-Regular,Menlo,monospace;font-size:12.5px;color:#3c4453}
.mac{font-family:ui-monospace,SFMono-Regular,Menlo,monospace;font-size:12.5px;color:#3c4453}
.empty{color:#b3b9c2}
.st{display:inline-block;font-size:12.5px}
.st-pill{display:inline-block;font-size:12px;padding:2px 8px;border-radius:999px;background:#eef0f3;color:#5b6472}
.st-PRINTING{background:#eaf7ee;color:#1e7a3c}
.st-EMERGENCY_STOPPED{background:#fdeceb;color:#a12a20}
.st-UNLOADING,.st-MATERIAL_LOADING{background:#eaf1fd;color:#2359b8}
.sub{font-size:11.5px;color:#98a1ae;margin-top:2px}
input[type=color]{-webkit-appearance:none;appearance:none;border:1px solid #dfe2e8;border-radius:6px;
      width:40px;height:28px;padding:2px;background:#fff;cursor:pointer}
input[type=color]::-webkit-color-swatch-wrapper{padding:0}
input[type=color]::-webkit-color-swatch{border:none;border-radius:4px}
.mat{height:30px;width:112px;border:1px solid #dcdfe5;border-radius:6px;padding:0 8px;font-size:13px;outline:none}
.mat:focus{border-color:#3b6fd4}
.ops{display:flex;gap:6px;flex-wrap:nowrap}
.ops button{padding:6px 9px;font-size:12px;white-space:nowrap}
.mask{position:fixed;inset:0;background:rgba(17,24,39,.38);display:none;align-items:center;
      justify-content:center;padding:20px;z-index:20}
.mask.on{display:flex}
.modal{background:#fff;border-radius:12px;max-width:520px;width:100%;max-height:84vh;overflow:auto;
       box-shadow:0 20px 50px rgba(16,24,40,.24)}
.modal .head{border-bottom:1px solid #eef0f3}
.modal .body{padding:14px 18px 18px}
.kv{display:grid;grid-template-columns:120px 1fr;row-gap:8px;font-size:13px}
.kv div:nth-child(odd){color:#6b7280}
/* 添加设备弹窗里的表单 */
.form{display:flex;flex-direction:column;gap:12px}
.form label{margin-bottom:5px}
.form input[type=color]{width:56px;height:32px}
.addfoot{display:flex;align-items:center;gap:12px;margin-top:16px}
#addErr{color:#c0392b}
#toast{position:fixed;left:50%;bottom:26px;transform:translateX(-50%) translateY(20px);opacity:0;
      background:#1f2329;color:#fff;font-size:13px;padding:9px 16px;border-radius:8px;
      transition:all .22s;pointer-events:none;z-index:40;max-width:80vw}
#toast.on{opacity:1;transform:translateX(-50%) translateY(0)}
@media(max-width:900px){
  .cfg{grid-template-columns:1fr 1fr}
  .wrap{padding:12px 10px 30px}
  th,td{padding:8px 8px}
  .ops{flex-wrap:wrap}
}
</style>
</head>
<body>
<div class="wrap">
  <header>
    <h1>SEMI-AMS 控制端</h1>
    <small id="ver">v0.1</small>
    <span class="pill" id="wifiPill"><i></i><span id="wifiText">--</span></span>
  </header>

  <section class="card">
    <div class="cfg">
      <div><label>本机 IP（进料器填这个）</label><div class="ro empty" id="ip">未联网</div></div>
      <div><label>端口</label><input id="port" type="number" placeholder="1883" min="1" max="65535"></div>
      <div><label>登录账号</label><input id="user" type="text" placeholder="留空 = 允许匿名" autocomplete="off"></div>
      <div><label>登录密码</label><input id="pass" type="password" placeholder="留空 = 不修改" autocomplete="new-password"></div>
      <div><label>Topic 前缀</label><input id="prefix" type="text" placeholder="semiams/" autocomplete="off"></div>
    </div>
    <div class="cfgfoot">
      <button class="primary" id="saveBtn">保存并重连</button>
      <span class="hint" id="cfgHint">这台设备就是 MQTT 服务端：进料器的「局域网 MQTT」里填上面的 IP 和端口；账号密码留空表示允许匿名，填了则两边要一致</span>
      <span class="pill" id="mqttPill"><i></i><span id="mqttText">读取中…</span></span>
    </div>
  </section>

  <section class="card">
    <div class="head">
      <b>☰ 设备列表</b>
      <span class="hint" id="devHint">--</span>
      <button class="plain" id="addBtn" title="把一台进料器手动加进表里（填 mac / 颜色 / 材料）">添加设备</button>
    </div>
    <table>
      <colgroup>
        <col style="width:15%"><col style="width:13%"><col style="width:19%">
        <col style="width:8%"><col style="width:13%"><col>
      </colgroup>
      <thead>
        <tr><th>设备 mac</th><th>设备 ip</th><th>设备状态</th><th>颜色</th><th>材料类型</th><th>电机控制</th></tr>
      </thead>
      <tbody id="rows"></tbody>
    </table>
  </section>
</div>

<div class="mask" id="mask">
  <div class="modal">
    <div class="head"><b id="dtTitle">设备详情</b><span class="hint"><button class="blue" id="dtClose">关闭</button></span></div>
    <div class="body"><div class="kv" id="dtBody"></div></div>
  </div>
</div>
<div class="mask" id="addMask">
  <div class="modal">
    <div class="head"><b>添加设备</b><span class="hint"><button class="blue" id="addCancel">取消</button></span></div>
    <div class="body">
      <div class="form">
        <div><label>设备 mac</label>
          <input id="addMac" type="text" placeholder="84:f3:eb:dd:27:00" autocomplete="off" spellcheck="false"></div>
        <div><label>颜色</label>
          <input id="addColor" type="color" value="#3b6fd4"></div>
        <div><label>材料类型</label>
          <input id="addMat" type="text" list="mats" maxlength="15" placeholder="PLA" autocomplete="off"></div>
      </div>
      <div class="addfoot">
        <button class="primary" id="addOk">添加</button>
        <span class="hint" id="addErr"></span>
      </div>
      <p class="hint" style="margin:12px 0 0">
        mac 就是进料器网页「状态」里那串，冒号可以不加；加完等它上报一次消息就会变在线。
      </p>
    </div>
  </div>
</div>
<div id="toast"></div>
<datalist id="mats"><option>PLA</option><option>PETG</option><option>ABS</option><option>ASA</option><option>TPU</option><option>PC</option><option>PA-CF</option></datalist>

<script>
const rowsEl = document.getElementById('rows');
const el = id => document.getElementById(id);
let firstLoad = true;
let focusRow = -1;

const KEY_CLASS = k => k ? 'st st-' + k : 'st';

function makeRow(i) {
  const tr = document.createElement('tr');
  // 这一行现在挂的是哪台设备：颜色/材料/命令都按 mac 提交，
  // 这样两次轮询之间有人删了别的行、行号往前挪了，也不会改到隔壁那台
  tr.dataset.mac = '';
  tr.innerHTML = `
    <td class="mac" data-c="mac">—</td>
    <td class="mono" data-c="ip">—</td>
    <td><span class="st" data-c="state">未接入</span><div class="sub" data-c="motor"></div></td>
    <td><input type="color" data-c="color" value="#3b6fd4" disabled></td>
    <td><input class="mat" type="text" data-c="material" list="mats" maxlength="15" placeholder="PLA" disabled></td>
    <td><div class="ops">
      <button class="green" data-cmd="feed" disabled>进料</button>
      <button class="orange" data-cmd="retract" disabled>退料</button>
      <button class="red" data-cmd="emg" disabled>紧急停止</button>
      <button class="red" data-cmd="emgclear" disabled>解除停止</button>
      <button class="del" data-cmd="del" disabled title="从设备表删掉这一行">删除</button>
      <button class="blue" data-cmd="detail" disabled>详情</button>
    </div></td>`;
  const color = tr.querySelector('[data-c=color]');
  const mat = tr.querySelector('[data-c=material]');
  color.addEventListener('focus', () => focusRow = i);
  mat.addEventListener('focus', () => focusRow = i);
  const blur = () => { if (focusRow === i) focusRow = -1; };
  color.addEventListener('blur', blur);
  mat.addEventListener('blur', blur);
  const save = () => {
    if (!tr.dataset.mac) return;   // 空行没什么可存的
    post('/api/label', { mac: tr.dataset.mac, color: color.value, material: mat.value });
  };
  color.addEventListener('change', save);
  mat.addEventListener('change', save);
  tr.querySelectorAll('button[data-cmd]').forEach(b => {
    b.addEventListener('click', () => {
      const cmd = b.dataset.cmd;
      if (cmd === 'detail') return showDetail(i);
      if (cmd === 'del') return delDevice(i);
      // mac 优先（行号可能已经漂了），idx 只是给老接口兜底
      post('/api/control', { mac: tr.dataset.mac, idx: i, cmd: cmd }).then(r => {
        if (r.ok) toast('已发送：' + b.textContent + (r.sent ? '（' + r.sent + ' 台）' : ''));
      });
    });
  });
  return tr;
}
// 表格行数跟着设备数走：来一台加一行，一台不剩就显示一行提示
let rowCount = -1;
function syncRows(n) {
  if (n === rowCount) return;   // 行数没变就什么都不做，避免每 1.5 秒重建
  rowCount = n;
  rowsEl.innerHTML = '';
  if (n === 0) {
    const tr = document.createElement('tr');
    tr.className = 'emptyrow';
    tr.innerHTML = '<td colspan="6">还没有设备 · 点右上角「添加设备」，或让进料器连上来</td>';
    tr.addEventListener('click', openAdd);
    rowsEl.appendChild(tr);
    return;
  }
  for (let i = 0; i < n; i++) rowsEl.appendChild(makeRow(i));
}

function toast(msg) {
  const t = el('toast');
  t.textContent = msg;
  t.classList.add('on');
  clearTimeout(t._h);
  t._h = setTimeout(() => t.classList.remove('on'), 1800);
}

async function post(url, data) {
  const body = new URLSearchParams(data).toString();
  try {
    const r = await fetch(url, { method: 'POST', headers: { 'Content-Type': 'application/x-www-form-urlencoded' }, body });
    const j = await r.json();
    if (!j.ok) toast(j.err || '操作失败');
    return j;
  } catch (e) {
    toast('控制端没有响应');
    return { ok: false };
  }
}

// 同样的 POST，但错误交给调用方自己显示（弹窗里要逐字提示，不再弹 toast）
async function postQuiet(url, data) {
  const body = new URLSearchParams(data).toString();
  try {
    const r = await fetch(url, { method: 'POST', headers: { 'Content-Type': 'application/x-www-form-urlencoded' }, body });
    return await r.json();
  } catch (e) {
    return { ok: false, err: '控制端没有响应' };
  }
}

function setPill(id, textId, cls, text) {
  el(id).className = 'pill ' + cls;
  el(textId).textContent = text;
}

function fmtAge(s) {
  if (s < 60) return s + ' 秒前';
  if (s < 3600) return Math.floor(s / 60) + ' 分钟前';
  return Math.floor(s / 3600) + ' 小时前';
}

let last = null;

function render(j) {
  last = j;
  if (firstLoad) {
    el('port').value = j.mqtt.port || 1883;
    el('user').value = j.mqtt.user || '';
    el('prefix').value = j.mqtt.prefix || 'semiams/';
    firstLoad = false;
  }
  const m = j.mqtt;
  setPill('mqttPill', 'mqttText', !m.running ? 'bad' : (m.clients ? 'ok' : 'warn'), m.status);
  const ipEl = el('ip');
  ipEl.textContent = j.ip || '未联网';  // 纯文本展示，不是输入框
  ipEl.classList.toggle('empty', !j.ip);
  setPill('wifiPill', 'wifiText', j.wifi.ip ? 'ok' : 'bad',
          j.wifi.ip ? ('本机 ' + j.wifi.ip + ' · ' + j.wifi.rssi + ' dBm') : 'WiFi 未连接');
  el('devHint').textContent = '共 ' + j.devices_used + ' 台 · 在线 ' + j.devices_online + ' 台';
  syncRows(j.devices.length);

  j.devices.forEach((d, i) => {
    const tr = rowsEl.children[i];
    tr.dataset.mac = d.mac || '';
    tr.querySelector('[data-c=mac]').textContent = d.mac || '—';
    tr.querySelector('[data-c=mac]').classList.toggle('empty', !d.mac);
    tr.querySelector('[data-c=ip]').textContent = d.ip || '—';
    tr.querySelector('[data-c=ip]').classList.toggle('empty', !d.ip);

    const st = tr.querySelector('[data-c=state]');
    if (!d.mac) {
      st.className = 'st empty';
      st.textContent = '未接入';
      tr.querySelector('[data-c=motor]').textContent = '';
    } else if (!d.online) {
      st.className = 'st-pill';
      st.textContent = '离线';
      tr.querySelector('[data-c=motor]').textContent =
        d.age >= 0 ? '最后通信 ' + fmtAge(d.age) : '等待设备上报';
    } else {
      st.className = KEY_CLASS(d.key);
      st.textContent = d.state || d.key || '--';
      const m = d.motor && d.motor !== '停止' ? d.motor + ' ' + d.duty + '%' : '';
      tr.querySelector('[data-c=motor]').textContent = m;
    }

    const color = tr.querySelector('[data-c=color]');
    const mat = tr.querySelector('[data-c=material]');
    color.disabled = mat.disabled = !d.mac;
    if (focusRow !== i) {
      if (color.value.toLowerCase() !== (d.color || '#3b6fd4').toLowerCase()) color.value = d.color || '#3b6fd4';
      if (mat.value !== (d.material || '')) mat.value = d.material || '';
    }
    tr.querySelectorAll('button[data-cmd]').forEach(b => {
      const cmd = b.dataset.cmd;
      // 详情 / 删除只依赖「这一行有 mac」，离线也能点；电机类动作必须在线
      const can = !!d.mac && (cmd === 'detail' || cmd === 'del' || d.online);
      b.disabled = !can;
      b.title = can ? (cmd === 'del' ? '从设备表删掉这一行' : '')
                    : (d.mac ? '设备离线' : '这一行还没有设备');
    });
  });
}

async function tick() {
  try {
    const r = await fetch('/api/state', { cache: 'no-store' });
    render(await r.json());
  } catch (e) {
    setPill('mqttPill', 'mqttText', 'bad', '控制端没有响应');
  }
}

el('saveBtn').addEventListener('click', async () => {
  el('saveBtn').disabled = true;
  const j = await post('/api/config', {
    port: el('port').value,
    user: el('user').value, pass: el('pass').value, prefix: el('prefix').value
  });
  el('saveBtn').disabled = false;
  if (j.ok) {
    el('pass').value = '';
    el('prefix').value = j.prefix || el('prefix').value;
    if (j.running === false) toast('设置已保存，但服务端没起来：' + (j.status || '未知原因'));
    else toast('已保存，服务端按新设置重启监听');
  }
});

function showDetail(i) {
  const d = last.devices[i];
  el('dtTitle').textContent = '设备详情 · ' + (d.mac || '第 ' + (i + 1) + ' 行');
  const rows = [
    ['状态', (d.state || '--') + (d.key ? '（' + d.key + '）' : '')],
    ['在线', d.online ? '在线' : '离线'],
    ['设备 mac', d.mac || '--'],
    ['设备 ip', d.ip || '--'],
    ['电机', (d.motor || '--') + ' / 占空比 ' + d.duty + '%'],
    ['进料口开关', d.in ? '有耗材' : '无'],
    ['就绪开关', d.ready ? '有耗材' : '无'],
    ['EMPTY 霍尔', d.lack ? '有效' : '无效'],
    ['FULL 霍尔', d.full ? '有效' : '无效'],
    ['WiFi 信号', d.rssi ? d.rssi + ' dBm' : '--'],
    ['设备空闲内存', d.heap ? d.heap + ' 字节' : '--'],
    ['设备运行时长', d.uptime ? Math.floor(d.uptime / 60) + ' 分 ' + (d.uptime % 60) + ' 秒' : '--'],
    ['最后通信', d.age >= 0 ? fmtAge(d.age) : '--'],
    ['颜色 / 材料', (d.color || '--') + ' / ' + (d.material || '--')]
  ];
  el('dtBody').innerHTML = rows.map(r =>
    '<div>' + r[0] + '</div><div>' + String(r[1]).replace(/[<>&]/g, c => ({ '<': '&lt;', '>': '&gt;', '&': '&amp;' }[c])) + '</div>').join('');
  el('mask').classList.add('on');
}
el('dtClose').addEventListener('click', () => el('mask').classList.remove('on'));
el('mask').addEventListener('click', e => { if (e.target.id === 'mask') el('mask').classList.remove('on'); });

// ---------------- 删除一行 ----------------
async function delDevice(i) {
  const d = last && last.devices ? last.devices[i] : null;
  const name = d && d.mac ? d.mac : ('第 ' + (i + 1) + ' 行');
  if (!confirm('从设备表删掉 ' + name + '？\n它的颜色、材料类型也一起删掉。\n如果这台进料器还在连服务器，下次上报会被重新加到最后一行的位置。')) return;
  el('mask').classList.remove('on');   // 详情弹窗可能正指着这一行
  const j = await postQuiet('/api/deldev', { mac: (d && d.mac) || '', idx: i });
  if (!j.ok) { toast(j.err || '删除失败'); return; }
  rowCount = -1;   // 强制重建表格，后面的行号往前挪一位
  toast('已删除 ' + name);
  tick();
}

// ---------------- 添加设备 ----------------
const PALETTE = ['#3b6fd4','#c0392b','#2f9e44','#e0b400','#7d4bb3','#3aa99a','#e07b39','#2c3e50'];

function openAdd() {
  el('addMac').value = '';
  el('addMat').value = 'PLA';
  const used = last ? last.devices_used : 0;   // 按已用行数挑一个还没用过的颜色
  el('addColor').value = PALETTE[used % PALETTE.length];
  el('addErr').textContent = '';
  el('addOk').disabled = false;
  el('addMask').classList.add('on');
  setTimeout(() => el('addMac').focus(), 30);
}

function closeAdd() { el('addMask').classList.remove('on'); }

async function submitAdd() {
  const mac = el('addMac').value.trim();
  if (!mac) { el('addErr').textContent = '先填 mac'; el('addMac').focus(); return; }
  el('addOk').disabled = true;
  const j = await postQuiet('/api/adddev', {
    mac: mac, color: el('addColor').value, material: el('addMat').value
  });
  el('addOk').disabled = false;
  if (!j.ok) { el('addErr').textContent = j.err || '添加失败'; return; }
  closeAdd();
  toast('已添加' + (j.idx >= 0 ? '到第 ' + (j.idx + 1) + ' 行' : ''));
  tick();
}

el('addBtn').addEventListener('click', openAdd);
el('addCancel').addEventListener('click', closeAdd);
el('addOk').addEventListener('click', submitAdd);
el('addMask').addEventListener('click', e => { if (e.target.id === 'addMask') closeAdd(); });
['addMac', 'addMat'].forEach(id => el(id).addEventListener('keydown', e => {
  if (e.key === 'Enter') submitAdd();
}));

tick();
setInterval(tick, 1500);
</script>
</body>
</html>
)HTMLX";

// 配网页面：连不上路由器时开热点，手机连 SEMI-AMS-CTRL 打开 http://192.168.4.1
static const char PAGE_PORTAL[] PROGMEM = R"HTMLP(
<!DOCTYPE html>
<html lang="zh-CN">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>SEMI-AMS 控制端 · 配网</title>
<style>
*{box-sizing:border-box}
body{margin:0;background:#f4f5f7;color:#1f2329;
     font:14px/1.5 -apple-system,BlinkMacSystemFont,"PingFang SC","Microsoft YaHei",sans-serif}
.wrap{max-width:420px;margin:8vh auto;padding:0 16px}
.card{background:#fff;border:1px solid #e6e8ec;border-radius:12px;padding:20px 20px 22px;
      box-shadow:0 1px 2px rgba(16,24,40,.05)}
h1{font-size:17px;margin:0 0 4px;font-weight:600}
p.hint{font-size:12.5px;color:#7b8492;margin:0 0 16px}
label{display:block;font-size:12px;color:#5b6472;margin:12px 0 5px}
input,select{width:100%;height:36px;padding:0 10px;border:1px solid #dcdfe5;border-radius:7px;font-size:13.5px;outline:none;background:#fff}
input:focus,select:focus{border-color:#3b6fd4;box-shadow:0 0 0 3px rgba(59,111,212,.12)}
button{width:100%;height:38px;margin-top:16px;background:#2b7fd4;color:#fff;border:none;
       border-radius:7px;font-size:14px;cursor:pointer}
button:disabled{opacity:.5}
</style>
</head>
<body>
<div class="wrap">
  <div class="card">
    <h1>SEMI-AMS 控制端</h1>
    <p class="hint">先连上家里/车间的 2.4GHz WiFi（ESP32-C3 不支持 5GHz），保存后设备会自动重启。</p>
    <form method="POST" action="/wifi">
      <label>WiFi 名称</label>
      <input name="ssid" list="nets" placeholder="选择或输入 SSID" required autocomplete="off">
      <datalist id="nets">%SSIDS%</datalist>
      <label>密码</label>
      <input name="pass" type="password" placeholder="没有密码就留空">
      <button type="submit">保存并重启</button>
    </form>
  </div>
</div>
</body>
</html>
)HTMLP";
