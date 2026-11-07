#!/usr/bin/env python3
"""生成 Bambu Studio 工程文件里的 Metadata/project_settings.config（打印机 / 耗材 / 工艺预设）。

Bambu Studio 只有在 3MF 里同时有 "Application = BambuStudio-x.y.z" 和完整的 project_settings.config 时，
才会当成自家工程打开；否则提示"该3mf文件不是来自Bambu Lab，将只加载几何数据和颜色数据"，
零件上的单独设置也会被清掉。

做法：拿一个 Bambu Studio 02.07.01.57 导出的工程当键的模板（键全、数组形状对），
换成 Bambu Studio 同版本 GitHub 标签里的官方预设值（继承链展开），只保留一个耗材。
预设名必须是 Bambu Studio 自带的系统预设：打开工程时，除了 different_settings_to_system 里列出的键，
其余值都会被替换成本机 Bambu Studio 里同名系统预设的值，所以版本差一点也没关系。

用法（需要联网访问 GitHub）：
  python3 make_project_settings.py <Bambu Studio 2.7.1 导出的任意 .3mf> -o bambu_project_settings.json
    [--printer "Bambu Lab P2S 0.4 nozzle"] [--process "0.20mm Standard @BBL P2S"]
    [--filament "Bambu PETG HF @BBL P2S 0.4 nozzle"] [--colour "#1F7FBF"] [--bed "Textured PEI Plate"]
"""
import argparse
import json
import urllib.parse
import urllib.request
import zipfile

TAG = "v02.07.01.57"
BASE = f"https://raw.githubusercontent.com/bambulab/BambuStudio/{TAG}/resources/profiles/"
STRUCT = {"filament_extruder_variant", "filament_self_index", "printer_extruder_variant", "print_extruder_variant",
          "printer_extruder_id", "print_extruder_id", "extruder_variant_list", "extruder_ams_count"}
META = {"name", "inherits", "from", "setting_id", "instantiation", "type", "description", "include",
        "renamed_from", "base_id", "filament_id", "compatible_printers", "compatible_printers_condition",
        "compatible_prints", "compatible_prints_condition", "version", "is_custom_defined", "model_id"}


def get_json(url):
    with urllib.request.urlopen(url, timeout=60) as r:
        return json.load(r)


def resolve(index, section, name):
    paths = {x["name"]: x["sub_path"] for x in index[section]}
    chain, cur = [], name
    while cur:
        d = get_json(BASE + "BBL/" + urllib.parse.quote(paths[cur]))
        chain.append(d)
        cur = d.get("inherits")
    out = {}
    for d in reversed(chain):
        out.update(d)
    return out


def fit(value, n, nvar):
    """把预设里的数组调整成工程里需要的长度（按变体重复）。
    长度和变体数无关的列表（打印区域、禁区、兼容机型等）原样使用。"""
    if not isinstance(value, list):
        return value
    if len(value) == n:
        return value
    if len(value) == 0 or n not in (1, nvar, 2 * nvar):
        return value
    if len(value) == 1:
        return value * n
    if n % len(value) == 0:
        return value * (n // len(value))
    return value[:n] if len(value) > n else None


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("template_3mf")
    ap.add_argument("-o", "--out", default="bambu_project_settings.json")
    ap.add_argument("--printer", default="Bambu Lab P2S 0.4 nozzle")
    ap.add_argument("--process", default="0.20mm Standard @BBL P2S")
    ap.add_argument("--filament", default="Bambu PETG HF @BBL P2S 0.4 nozzle")
    ap.add_argument("--colour", default="#1F7FBF")
    ap.add_argument("--bed", default="Textured PEI Plate")
    a = ap.parse_args()

    with zipfile.ZipFile(a.template_3mf) as z:
        tpl = json.loads(z.read("Metadata/project_settings.config"))
    nf_old = len(tpl["filament_colour"])
    nvar = len(tpl["filament_extruder_variant"]) // nf_old       # 每个耗材的变体数（P2S 2.7.1：2）

    index = get_json(BASE + "BBL.json")
    machine = resolve(index, "machine_list", a.printer)
    process = resolve(index, "process_list", a.process)
    filament = resolve(index, "filament_list", a.filament)

    cfg = {}
    # 1) 先把模板缩成一个耗材
    for k, v in tpl.items():
        if isinstance(v, list) and len(v) == nf_old * nvar and k not in ("printer_extruder_variant", "print_extruder_variant"):
            v = v[:nvar]
        elif isinstance(v, list) and len(v) == nf_old:
            v = v[:1]
        elif isinstance(v, list) and len(v) == nf_old * nf_old:
            v = ["0"]
        cfg[k] = v
    cfg["different_settings_to_system"] = ["", "", ""]
    cfg["inherits_group"] = ["", "", ""]
    cfg["filament_self_index"] = ["1"] * nvar

    # 2) 覆盖成官方预设的值
    skipped = []
    for src in (machine, process):
        for k, v in src.items():
            if k in META or k in STRUCT or k not in cfg:
                continue
            want = len(cfg[k]) if isinstance(cfg[k], list) else None
            nv = fit(v, want, nvar) if want is not None else (v[0] if isinstance(v, list) and len(v) == 1 else v)
            if nv is None or isinstance(nv, list) != isinstance(cfg[k], list):
                skipped.append(k)
                continue
            cfg[k] = nv
    for k, v in filament.items():
        if k in META or k in STRUCT or k not in cfg:
            continue
        want = len(cfg[k]) if isinstance(cfg[k], list) else None
        nv = fit(v, want, nvar) if want is not None else v
        if nv is None or isinstance(nv, list) != isinstance(cfg[k], list):
            skipped.append(k)
            continue
        cfg[k] = nv

    # 3) 预设名和工程级的键
    cfg.update({
        "printer_settings_id": a.printer,
        "print_settings_id": a.process,
        "filament_settings_id": [a.filament],
        "filament_ids": [filament.get("filament_id", "")],
        "filament_colour": [a.colour],
        "default_filament_colour": [""],
        "print_compatible_printers": process.get("compatible_printers", [a.printer]),
        "curr_bed_type": a.bed,
        "filament_map": ["1"],
        "filament_nozzle_map": ["0"],
        "filament_volume_map": ["0"],
        "flush_volumes_matrix": ["0"],
        "name": "project_settings",
        "from": "project",
        "version": TAG.lstrip("v"),
    })

    with open(a.out, "w", encoding="utf-8") as f:
        json.dump(cfg, f, ensure_ascii=False, indent=4, sort_keys=True)
    print(f"{a.out}: {len(cfg)} keys; printer={a.printer}; process={a.process}; filament={a.filament}")
    if skipped:
        print("形状对不上、保留模板值的键：", ", ".join(sorted(set(skipped))))


if __name__ == "__main__":
    main()
