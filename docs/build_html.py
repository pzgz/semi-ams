#!/usr/bin/env python3
"""把 docs 里的 Markdown 文档生成为单文件 HTML（Mermaid 图预先渲染成内嵌 SVG，离线可看）。

依赖：  pip3 install markdown        npm install -g @mermaid-js/mermaid-cli
用法：  python3 docs/build_html.py              生成 docs 里所有 .md 对应的 .html
        python3 docs/build_html.py 文件.md ...   只生成指定的文档
"""
import html
import json
import re
import subprocess
import sys
import tempfile
from pathlib import Path

import markdown
from markdown.extensions.toc import slugify_unicode

HERE = Path(__file__).resolve().parent

MERMAID_CONFIG = {
    "theme": "neutral",
    "fontFamily": '"PingFang SC","Noto Sans CJK SC","Microsoft YaHei",sans-serif',
    "themeVariables": {"fontSize": "15px"},
    "flowchart": {"curve": "basis", "htmlLabels": True},
}


def render_mermaid(blocks):
    svgs = []
    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)
        cfg = tmp / "config.json"
        cfg.write_text(json.dumps(MERMAID_CONFIG), encoding="utf-8")
        for i, code in enumerate(blocks):
            src, out = tmp / f"d{i}.mmd", tmp / f"d{i}.svg"
            src.write_text(code, encoding="utf-8")
            subprocess.run(
                ["mmdc", "-q", "-i", str(src), "-o", str(out), "-I", f"diagram-{i}", "-b", "white", "-c", str(cfg)],
                check=True,
            )
            svg = out.read_text(encoding="utf-8")
            svg = re.sub(r"^<\?xml[^>]*>\s*", "", svg)
            svgs.append(svg)
    return svgs


def main():
    srcs = [Path(a).resolve() for a in sys.argv[1:]] or sorted(HERE.glob("*.md"))
    for src in srcs:
        build(src, src.with_suffix(".html"))


def build(SRC, OUT):
    text = SRC.read_text(encoding="utf-8")

    blocks = []

    def take(m):
        blocks.append(m.group(1))
        return f"\n\nMERMAIDBLOCK{len(blocks) - 1}\n\n"

    text = re.sub(r"```mermaid\n(.*?)```", take, text, flags=re.S)

    md = markdown.Markdown(
        extensions=["tables", "fenced_code", "toc", "sane_lists"],
        extension_configs={"toc": {"slugify": slugify_unicode, "toc_depth": "2-3"}},
    )
    body = md.convert(text)
    # 文档之间的链接：.md 换成生成的 .html
    body = re.sub(r'href="([^":#]+)\.md(#[^"]*)?"', lambda m: f'href="{m.group(1)}.html{m.group(2) or ""}"', body)
    toc = md.toc.replace('<div class="toc">', '<div class="toc-list">')

    svgs = render_mermaid(blocks)
    for i, svg in enumerate(svgs):
        body = body.replace(f"<p>MERMAIDBLOCK{i}</p>", f'<figure class="diagram">{svg}</figure>')

    body = body.replace("<table>", '<div class="table-wrap"><table>').replace("</table>", "</table></div>")

    title_match = re.search(r"<h1[^>]*>(.*?)</h1>", body)
    title = re.sub(r"<[^>]+>", "", title_match.group(1)) if title_match else "SEMI-AMS"

    page = TEMPLATE.replace("{{TITLE}}", html.escape(title)).replace("{{TOC}}", toc).replace("{{BODY}}", body)
    OUT.write_text(page, encoding="utf-8")
    print(f"wrote {OUT} ({len(page)//1024} KB, {len(svgs)} diagrams)")


TEMPLATE = r"""<!doctype html>
<html lang="zh-CN">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>{{TITLE}}</title>
<style>
:root{
  --bg:#f5f6f8; --panel:#ffffff; --text:#1f2329; --muted:#646a73; --border:#e3e5e9;
  --accent:#1a5fd6; --accent-weak:#e9f0fd; --code-bg:#f1f3f5; --code-text:#24292f;
  --row:#fafbfc; --shadow:0 1px 2px rgba(15,23,42,.06),0 2px 8px rgba(15,23,42,.04);
}
@media (prefers-color-scheme: dark){
  :root:not([data-theme="light"]){
    --bg:#111317; --panel:#1a1d23; --text:#e6e8eb; --muted:#9aa1ab; --border:#2c313a;
    --accent:#6ea2ff; --accent-weak:#1d2a42; --code-bg:#22262e; --code-text:#e6e8eb;
    --row:#1e2128; --shadow:none;
  }
}
:root[data-theme="dark"]{
  --bg:#111317; --panel:#1a1d23; --text:#e6e8eb; --muted:#9aa1ab; --border:#2c313a;
  --accent:#6ea2ff; --accent-weak:#1d2a42; --code-bg:#22262e; --code-text:#e6e8eb;
  --row:#1e2128; --shadow:none;
}
*{box-sizing:border-box}
html{scroll-behavior:smooth;scroll-padding-top:16px}
body{margin:0;background:var(--bg);color:var(--text);
  font:15px/1.75 -apple-system,BlinkMacSystemFont,"PingFang SC","Hiragino Sans GB","Noto Sans CJK SC","Microsoft YaHei",sans-serif;
  -webkit-font-smoothing:antialiased}
.layout{display:grid;grid-template-columns:270px minmax(0,1fr);gap:28px;max-width:1240px;margin:0 auto;padding:28px 24px 80px}
.toc{position:sticky;top:20px;align-self:start;max-height:calc(100vh - 40px);overflow:auto;
  background:var(--panel);border:1px solid var(--border);border-radius:12px;padding:14px 10px 14px 6px;box-shadow:var(--shadow);font-size:13.5px}
.toc summary{list-style:none;cursor:pointer;font-weight:600;padding:2px 12px 8px;color:var(--text)}
.toc summary::-webkit-details-marker{display:none}
.toc ul{list-style:none;margin:0;padding-left:12px}
.toc-list>ul{padding-left:4px}
.toc-list>ul>li{margin-top:6px}
.toc-list>ul>li>a{color:var(--text);font-weight:600}
.toc li{margin:1px 0}
.toc a{display:block;padding:3px 8px;border-radius:6px;color:var(--muted);text-decoration:none;line-height:1.45}
.toc a:hover{background:var(--accent-weak);color:var(--text)}
.toc a.active{background:var(--accent-weak);color:var(--accent);font-weight:600}
main{background:var(--panel);border:1px solid var(--border);border-radius:14px;padding:36px 44px 48px;box-shadow:var(--shadow);min-width:0}
h1{font-size:28px;line-height:1.3;margin:0 0 18px;letter-spacing:.01em}
h2{font-size:21px;margin:44px 0 14px;padding-top:22px;border-top:1px solid var(--border)}
h3{font-size:17px;margin:30px 0 10px}
h2+h3{margin-top:16px}
h4{font-size:15px;margin:22px 0 8px}
h3+h4{margin-top:12px}
p,li{color:var(--text)}
a{color:var(--accent)}
hr{display:none}
strong{font-weight:650}
ul,ol{padding-left:1.4em}
li{margin:3px 0}
code{font-family:"SF Mono",SFMono-Regular,Menlo,Consolas,monospace;font-size:.88em;background:var(--code-bg);color:var(--code-text);
  padding:.12em .4em;border-radius:5px;word-break:break-word}
pre{position:relative;background:var(--code-bg);border:1px solid var(--border);border-radius:10px;padding:14px 16px;overflow:auto;line-height:1.55}
pre code{background:none;padding:0;font-size:13px;word-break:normal;white-space:pre}
.copy{position:absolute;top:8px;right:8px;border:1px solid var(--border);background:var(--panel);color:var(--muted);
  font-size:12px;border-radius:6px;padding:2px 8px;cursor:pointer;opacity:0;transition:opacity .15s}
pre:hover .copy,.copy:focus{opacity:1}
.table-wrap{overflow-x:auto;margin:14px 0 18px;border:1px solid var(--border);border-radius:10px}
table{border-collapse:collapse;width:100%;font-size:14px;line-height:1.6}
th,td{padding:8px 12px;text-align:left;vertical-align:top;border-bottom:1px solid var(--border)}
th{background:var(--row);font-weight:600;white-space:nowrap}
tr:last-child td{border-bottom:none}
tbody tr:nth-child(even) td{background:var(--row)}
td code{white-space:nowrap}
figure.diagram{margin:18px 0 22px;padding:16px;background:#fff;color:#1f2329;color-scheme:light;border:1px solid var(--border);border-radius:12px;overflow-x:auto;text-align:center}
figure.diagram svg{max-width:100%;height:auto}
figure.diagram svg foreignObject div,figure.diagram svg foreignObject span,figure.diagram svg foreignObject p{color:#1f2329}
.theme-toggle{position:fixed;right:16px;bottom:16px;border:1px solid var(--border);background:var(--panel);color:var(--muted);
  border-radius:999px;padding:6px 12px;font-size:13px;cursor:pointer;box-shadow:var(--shadow)}
@media (max-width: 1000px){
  .layout{grid-template-columns:1fr;padding:16px 16px 72px;gap:14px}
  .toc{position:static;max-height:none}
  main{padding:22px 18px 32px;border-radius:12px}
  h1{font-size:23px}
  h2{font-size:19px}
  figure.diagram{padding:10px}
  figure.diagram svg{min-width:560px}
}
@media print{
  body{background:#fff}
  .toc,.theme-toggle,.copy{display:none}
  .layout{display:block;padding:0}
  main{border:none;box-shadow:none;padding:0}
  h2{break-after:avoid}
  pre,figure,.table-wrap{break-inside:avoid}
}
</style>
</head>
<body>
<div class="layout">
  <details class="toc" open>
    <summary>目录</summary>
    {{TOC}}
  </details>
  <main>
{{BODY}}
  </main>
</div>
<button class="theme-toggle" type="button" aria-label="切换深色/浅色">深色 / 浅色</button>
<script>
(function(){
  var toc=document.querySelector('.toc');
  if(window.matchMedia('(max-width: 1000px)').matches) toc.removeAttribute('open');

  document.querySelectorAll('pre').forEach(function(pre){
    var b=document.createElement('button'); b.className='copy'; b.type='button'; b.textContent='复制';
    b.addEventListener('click',function(){
      var t=pre.querySelector('code').innerText;
      var done=function(){b.textContent='已复制';setTimeout(function(){b.textContent='复制'},1200)};
      if(navigator.clipboard){navigator.clipboard.writeText(t).then(done,function(){})}
    });
    pre.appendChild(b);
  });

  var root=document.documentElement, btn=document.querySelector('.theme-toggle');
  try{var saved=localStorage.getItem('semi-doc-theme'); if(saved) root.setAttribute('data-theme',saved);}catch(e){}
  btn.addEventListener('click',function(){
    var dark=root.getAttribute('data-theme')==='dark' ||
      (!root.getAttribute('data-theme') && window.matchMedia('(prefers-color-scheme: dark)').matches);
    var next=dark?'light':'dark'; root.setAttribute('data-theme',next);
    try{localStorage.setItem('semi-doc-theme',next)}catch(e){}
  });

  var links={}; document.querySelectorAll('.toc a').forEach(function(a){links[decodeURIComponent(a.getAttribute('href').slice(1))]=a});
  var heads=[].slice.call(document.querySelectorAll('main h2[id], main h3[id]'));
  if('IntersectionObserver' in window){
    var current=null;
    var io=new IntersectionObserver(function(entries){
      entries.forEach(function(e){ if(e.isIntersecting){ current=e.target.id; } });
      Object.keys(links).forEach(function(k){links[k].classList.toggle('active',k===current)});
    },{rootMargin:'0px 0px -70% 0px'});
    heads.forEach(function(h){io.observe(h)});
  }
})();
</script>
</body>
</html>
"""

if __name__ == "__main__":
    sys.exit(main())
