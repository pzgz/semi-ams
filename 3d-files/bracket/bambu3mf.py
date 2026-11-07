"""写 Bambu Studio 能直接当工程打开的 3MF（带打印机 / 耗材 / 工艺预设和零件单独设置）。

Bambu Studio 判断"是不是自家工程"看两处：3D/3dmodel.model 里的 Application 以 "BambuStudio-" 开头，
并且有完整的 Metadata/project_settings.config。缺一个就提示"该3mf文件不是来自Bambu Lab，
将只加载几何数据和颜色数据"，零件上的单独设置也会被清掉。

工程预设在 bambu_project_settings.json（make_project_settings.py 生成）。预设名都是 Bambu Studio 自带的
系统预设，打开时其余数值会换成本机同名预设的值；强度相关的设置放在每个零件上（对象设置），
换工艺预设也不会丢。
"""
import io
import json
import math
import zipfile
from datetime import date
from pathlib import Path
from xml.sax.saxutils import escape

import numpy as np

APP_VERSION = "02.07.01.57"      # 和 bambu_project_settings.json 的结构对应；比本机版本旧没关系，新了会弹"版本较新"
OBJ_SUFFIX = "-61cb-4c03-9d28-80fed5dfa1dc"
SUB_SUFFIX = "-81cb-4c03-9d28-80fed5dfa1dc"
COMP_SUFFIX = "-b206-40ff-9872-83e8017abed1"
ITEM_SUFFIX = "-b1ec-4553-aec9-835e5b724bb4"
BUILD_UUID = "2c7c17d8-22b5-4d84-8835-1976022ea369"
NS = ('xmlns="http://schemas.microsoft.com/3dmanufacturing/core/2015/02" '
      'xmlns:BambuStudio="http://schemas.bambulab.com/package/2021" '
      'xmlns:p="http://schemas.microsoft.com/3dmanufacturing/production/2015/06" requiredextensions="p"')


def _mesh_xml(vertices, faces):
    out = io.StringIO()
    out.write("   <mesh>\n    <vertices>\n")
    for x, y, z in vertices:
        out.write(f'     <vertex x="{x:.6g}" y="{y:.6g}" z="{z:.6g}"/>\n')
    out.write("    </vertices>\n    <triangles>\n")
    for a, b, c in faces:
        out.write(f'     <triangle v1="{a}" v2="{b}" v3="{c}"/>\n')
    out.write("    </triangles>\n   </mesh>\n")
    return out.getvalue()


def _plate_cols(n):
    """和 Bambu Studio 的 compute_colum_count 一样：打印板按 ceil(sqrt(n)) 列排。"""
    v = math.sqrt(n)
    r = round(v)
    return int(r + 1 if v > r else r)


# Bambu Studio 读工程时板名要过 Plater::has_illegal_filename_characters()（PartPlate::set_plate_name），
# 有这些字符就弹"无效名称"并丢掉板名；零件名在对象列表里改名时也是同一个检查
ILLEGAL_NAME_CHARS = '<>:/\\|?*"'
ILLEGAL_NAME_ESCAPES = ("&lt;", "&gt;", "&amp;", "&quot;", "&apos;")


def check_name(name):
    bad = [c for c in ILLEGAL_NAME_CHARS if c in name] + [e for e in ILLEGAL_NAME_ESCAPES if e in name]
    if bad:
        raise ValueError(f"名字 {name!r} 里有 Bambu Studio 不允许的字符 {' '.join(bad)}")


def write_project(path, parts, settings_json, title="", description="", plate_names=None, thumbnails=None):
    """parts: [dict(name, mesh=trimesh.Trimesh（打印姿态，Z 朝上）, offset=(dx, dy) 相对板中心,
                    settings={键: 值}, plate=板序号（从 0 开始）)]
    plate_names / thumbnails：每块板的名字和缩略图 PNG（可省）。"""
    project = json.loads(Path(settings_json).read_text(encoding="utf-8"))
    pts = [tuple(map(float, p.split("x"))) for p in project["printable_area"]]
    bed_w = max(p[0] for p in pts) - min(p[0] for p in pts)
    bed_d = max(p[1] for p in pts) - min(p[1] for p in pts)
    n_plates = max([p.get("plate", 0) for p in parts] + [len(plate_names or []) - 1]) + 1
    cols = _plate_cols(n_plates)
    stride_x, stride_y = bed_w * 1.2, bed_d * 1.2           # Bambu Studio: LOGICAL_PART_PLATE_GAP = 1/5
    plate_names = plate_names or [""] * n_plates
    for n in list(plate_names) + [p["name"] for p in parts]:
        check_name(n)
    thumbnails = thumbnails or [None] * n_plates
    today = date.today().isoformat()
    files = {}
    rels, resources, build, objects_cfg, assemble, cut = [], [], [], [], [], []
    plate_inst = [[] for _ in range(n_plates)]

    for i, part in enumerate(parts):
        mesh = part["mesh"]
        lo, hi = mesh.bounds
        center = (lo + hi) / 2
        verts = np.asarray(mesh.vertices) - center          # 对象坐标以包围盒中心为原点
        oid, sid = 2 * i + 2, 2 * i + 1                     # 顶层对象 id / 子模型里的对象 id
        fname = f"3D/Objects/object_{sid}.model"
        files[fname] = (
            f'<?xml version="1.0" encoding="UTF-8"?>\n<model unit="millimeter" xml:lang="en-US" {NS}>\n'
            f' <metadata name="BambuStudio:3mfVersion">1</metadata>\n <resources>\n'
            f'  <object id="{sid}" p:UUID="{sid << 16:08x}{SUB_SUFFIX}" type="model">\n'
            + _mesh_xml(verts, mesh.faces) +
            '  </object>\n </resources>\n <build/>\n</model>\n')
        rels.append(f' <Relationship Target="/{fname}" Id="rel-{i + 1}" Type="http://schemas.microsoft.com/3dmanufacturing/2013/01/3dmodel"/>')
        resources.append(
            f'  <object id="{oid}" p:UUID="{oid:08x}{OBJ_SUFFIX}" type="model">\n   <components>\n'
            f'    <component p:path="/{fname}" objectid="{sid}" p:UUID="{sid << 16:08x}{COMP_SUFFIX}" transform="1 0 0 0 1 0 0 0 1 0 0 0"/>\n'
            '   </components>\n  </object>')
        p = part.get("plate", 0)
        ox, oy = (p % cols) * stride_x, -(p // cols) * stride_y
        tx = ox + bed_w / 2 + part.get("offset", (0, 0))[0]
        ty = oy + bed_d / 2 + part.get("offset", (0, 0))[1]
        tz = (hi[2] - lo[2]) / 2
        tr = f"1 0 0 0 1 0 0 0 1 {tx:.6g} {ty:.6g} {tz:.6g}"
        build.append(f'  <item objectid="{oid}" p:UUID="{oid:08x}{ITEM_SUFFIX}" transform="{tr}" printable="1"/>')

        meta = [f'    <metadata key="name" value="{escape(part["name"])}"/>', '    <metadata key="extruder" value="1"/>']
        for k, v in part.get("settings", {}).items():
            meta.append(f'    <metadata key="{k}" value="{escape(str(v))}"/>')
        objects_cfg.append(
            f'  <object id="{oid}">\n' + "\n".join(meta) + f'\n    <metadata face_count="{len(mesh.faces)}"/>\n'
            f'    <part id="{sid}" subtype="normal_part">\n'
            f'      <metadata key="name" value="{escape(part["name"])}"/>\n'
            '      <metadata key="matrix" value="1 0 0 0 0 1 0 0 0 0 1 0 0 0 0 1"/>\n'
            '      <metadata key="source_object_id" value="0"/>\n      <metadata key="source_volume_id" value="0"/>\n'
            '      <metadata key="source_offset_x" value="0"/>\n      <metadata key="source_offset_y" value="0"/>\n'
            '      <metadata key="source_offset_z" value="0"/>\n'
            f'      <mesh_stat face_count="{len(mesh.faces)}" edges_fixed="0" degenerate_facets="0" facets_removed="0" facets_reversed="0" backwards_edges="0"/>\n'
            '    </part>\n  </object>')
        plate_inst[p].append(
            f'    <model_instance>\n      <metadata key="object_id" value="{oid}"/>\n'
            f'      <metadata key="instance_id" value="0"/>\n      <metadata key="identify_id" value="{100 + i}"/>\n    </model_instance>')
        assemble.append(f'   <assemble_item object_id="{oid}" instance_id="0" transform="{tr}" offset="0 0 0" />')
        cut.append(f' <object id="{i + 1}">\n  <cut_id id="0" check_sum="1" connectors_cnt="0"/>\n </object>')

    meta_root = [("Application", f"BambuStudio-{APP_VERSION}"), ("BambuStudio:3mfVersion", "1"),
                 ("CreationDate", today), ("ModificationDate", today), ("Title", title), ("Description", description)]
    files["3D/3dmodel.model"] = (
        f'<?xml version="1.0" encoding="UTF-8"?>\n<model unit="millimeter" xml:lang="en-US" {NS}>\n'
        + "".join(f' <metadata name="{k}">{escape(v)}</metadata>\n' for k, v in meta_root)
        + ' <resources>\n' + "\n".join(resources) + '\n </resources>\n'
        + f' <build p:UUID="{BUILD_UUID}">\n' + "\n".join(build) + '\n </build>\n</model>\n')
    files["3D/_rels/3dmodel.model.rels"] = (
        '<?xml version="1.0" encoding="UTF-8"?>\n<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships">\n'
        + "\n".join(rels) + '\n</Relationships>\n')
    thumb_rel = ('\n <Relationship Target="/Metadata/plate_1.png" Id="rel-2" '
                 'Type="http://schemas.openxmlformats.org/package/2006/relationships/metadata/thumbnail"/>') if thumbnails[0] else ""
    files["_rels/.rels"] = (
        '<?xml version="1.0" encoding="UTF-8"?>\n<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships">\n'
        ' <Relationship Target="/3D/3dmodel.model" Id="rel-1" Type="http://schemas.microsoft.com/3dmanufacturing/2013/01/3dmodel"/>'
        + thumb_rel + '\n</Relationships>\n')
    files["[Content_Types].xml"] = (
        '<?xml version="1.0" encoding="UTF-8"?>\n<Types xmlns="http://schemas.openxmlformats.org/package/2006/content-types">\n'
        ' <Default Extension="rels" ContentType="application/vnd.openxmlformats-package.relationships+xml"/>\n'
        ' <Default Extension="model" ContentType="application/vnd.ms-package.3dmanufacturing-3dmodel+xml"/>\n'
        ' <Default Extension="png" ContentType="image/png"/>\n'
        ' <Default Extension="gcode" ContentType="text/x.gcode"/>\n</Types>\n')
    plates_xml = []
    for p in range(n_plates):
        meta = [f'    <metadata key="plater_id" value="{p + 1}"/>', f'    <metadata key="plater_name" value="{escape(plate_names[p])}"/>',
                '    <metadata key="locked" value="false"/>', '    <metadata key="filament_map_mode" value="Auto For Flush"/>',
                '    <metadata key="filament_maps" value="1"/>', '    <metadata key="filament_volume_maps" value="0"/>']
        if thumbnails[p]:
            meta.append(f'    <metadata key="thumbnail_file" value="Metadata/plate_{p + 1}.png"/>')
        plates_xml.append('  <plate>\n' + "\n".join(meta + plate_inst[p]) + '\n  </plate>')
    files["Metadata/model_settings.config"] = (
        '<?xml version="1.0" encoding="UTF-8"?>\n<config>\n' + "\n".join(objects_cfg) + "\n"
        + "\n".join(plates_xml) + '\n  <assemble>\n' + "\n".join(assemble) + '\n  </assemble>\n</config>\n')
    files["Metadata/project_settings.config"] = json.dumps(project, ensure_ascii=False, indent=4)
    files["Metadata/slice_info.config"] = (
        '<?xml version="1.0" encoding="UTF-8"?>\n<config>\n  <header>\n'
        '    <header_item key="X-BBL-Client-Type" value="slicer"/>\n'
        f'    <header_item key="X-BBL-Client-Version" value="{APP_VERSION}"/>\n  </header>\n</config>\n')
    files["Metadata/filament_sequence.json"] = json.dumps(
        {f"plate_{p + 1}": {"nozzle_sequence": [], "optimal_assignment": [], "sequence": []} for p in range(n_plates)})
    files["Metadata/cut_information.xml"] = '<?xml version="1.0" encoding="utf-8"?>\n<objects>\n' + "\n".join(cut) + '\n</objects>\n'

    order = ["[Content_Types].xml", "_rels/.rels", "3D/3dmodel.model", "3D/_rels/3dmodel.model.rels"]
    order += sorted(k for k in files if k.startswith("3D/Objects/")) + sorted(k for k in files if k.startswith("Metadata/"))
    with zipfile.ZipFile(path, "w", zipfile.ZIP_DEFLATED) as z:
        for name in order:
            z.writestr(name, files[name])
        for p, png in enumerate(thumbnails):
            if png:
                z.writestr(f"Metadata/plate_{p + 1}.png", png)
