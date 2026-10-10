"""Translate cached high-fidelity Figma context into fixed-size LVGL primitives.

Geometry is correlated with Figma metadata; text, colors, component variants and
assets come from design context. Run after fetch_assets.py. No network at build time.
"""

import hashlib
import io
import json
import pathlib
import re
import subprocess
import sys
import xml.etree.ElementTree as ET
from PIL import Image, ImageDraw, ImageFont

ROOT = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(ROOT / "vendor"))
import resvg_py

OUT = ROOT.parents[1] / "main/boards/esp-sparkbot"
data = json.loads((ROOT / "figma_context.json").read_text(encoding="utf-8"))
meta = ET.fromstring(data["metadata"])
by_id = {n.get("id"): n for n in meta.iter()}
pages = []
images = {}


def color(style, prefix, default=0):
    match = re.search(prefix + r"-\[(?:color:)?var\([^,]+,(#[0-9a-fA-F]+)\)\]", style)
    return int(match[1][1:], 16) if match else default


def text_of(el):
    return "".join(el.itertext()).replace("\u200b", "").strip()


for page in data["pages"]:
    code = page["code"]
    assets = dict(re.findall(r'const (\w+) = "([^"]+)";', code))
    jsx = (
        code.split("export default", 1)[1]
        .split("return (", 1)[1]
        .rsplit(");", 1)[0]
        .strip()
    )
    jsx = re.sub(r"\{`([^`]+)`\}", lambda m: m[1], jsx)
    jsx = re.sub(r"src=\{(\w+)\}", r'src="\1"', jsx)
    tree = ET.fromstring(jsx)
    nodes = []

    def add(
        kind,
        x,
        y,
        w,
        h,
        text="",
        size=14,
        fg=0xF2F7FA,
        bg=0,
        radius=0,
        action="",
        align=0,
        asset="",
        border=0,
    ):
        nodes.append(
            dict(
                kind=kind,
                x=round(x),
                y=round(y),
                w=round(w),
                h=round(h),
                text=text,
                size=size,
                fg=fg,
                bg=bg,
                radius=radius,
                action=action,
                align=align,
                asset=asset,
                border=border,
            )
        )

    def walk(el, mn, px=0, py=0, inherited=""):
        x = px + (float(mn.get("x", 0)) if mn is not by_id[page["id"]] else 0)
        y = py + (float(mn.get("y", 0)) if mn is not by_id[page["id"]] else 0)
        w, h = float(mn.get("width", 0)), float(mn.get("height", 0))
        style = el.get("className", "")
        fg, bg = color(style, "text", color(inherited, "text", 0xF2F7FA)), color(
            style, "bg"
        )
        sz = re.search(r"text-\[(\d+)px\]", style) or re.search(
            r"text-\[(\d+)px\]", inherited
        )
        size = int(sz[1]) if sz else 14
        radius = 6 if "rounded-" in style else 0
        action = ""
        if el.get("data-name") == "Panel / Row":
            children = list(el)
            label, value = text_of(children[0]), text_of(children[1])
            add(
                "Box",
                x,
                y,
                w,
                h,
                bg=bg or 0x1C2932,
                radius=6,
                action=label,
                border=2 if "border-2" in style else 0,
            )
            label_size = int(
                re.search(r"text-\[(\d+)px\]", children[0].get("className", ""))[1]
            )
            value_size = int(
                re.search(r"text-\[(\d+)px\]", children[1].get("className", ""))[1]
            )
            add("Text", x + 12, y, w - 24, h, label, label_size)
            add(
                "Text",
                x + 12,
                y,
                w - 24,
                h,
                value,
                value_size,
                color(children[1].get("className", ""), "text", 0x63E0CB),
                align=2,
            )
            return
        if el.tag.startswith("Panel"):
            label, value = el.get("label", ""), el.get("value", "")
            action = label
            if el.tag == "PanelRow":
                add("Box", x, y, w, h, bg=bg or 0x1C2932, radius=6, action=action)
                add("Text", x + 12, y, w - 24, h, label, 16, align=0)
                add("Text", x + 12, y, w - 24, h, value, 14, 0x63E0CB, align=2)
            else:
                primary = el.tag == "PanelPrimary"
                add(
                    "Box",
                    x,
                    y,
                    w,
                    h,
                    bg=bg or (0x63E0CB if primary else 0x263842),
                    radius=6,
                    action=action,
                )
                add(
                    "Text",
                    x,
                    y,
                    w,
                    h,
                    label,
                    14,
                    0x082B27 if primary else 0xF2F7FA,
                    align=1,
                )
            return
        if el.tag in ("p", "a") and (el.tag == "p" or not el.get("data-name")):
            text = text_of(el)
            if text:
                add(
                    "Text",
                    x,
                    y,
                    w,
                    h,
                    text,
                    size,
                    fg,
                    action=text if el.tag == "a" else "",
                )
            return
        name = el.get("data-name", "")
        if name.startswith("焦点 / "):
            action = name.split(" / ", 1)[1]
        elif page["name"].startswith("menu / ") and name in (
            "状态参数",
            "设置参数",
            "定时/日程",
            "当前故障",
            "历史故障",
            "用户设置",
        ):
            action = name
        if (bg or action or "border-2" in style) and mn is not by_id[page["id"]]:
            if el.tag == "a" or el.get("data-name") == "Panel / Row":
                action = text_of(el).split("\n")[0].strip()
            add(
                "Box",
                x,
                y,
                w,
                h,
                bg=bg or 0x10191F,
                radius=radius,
                action=action,
                border=2 if "border-2" in style else 0,
            )
        for child in el:
            if child.tag == "img":
                filename = assets[child.get("src")].rsplit("/", 1)[1]
                path = ROOT / "assets" / filename
                assert path.stat().st_size, path
                # Rasterize original SVG as supplied, retaining its intrinsic dimensions.
                png = resvg_py.svg_to_bytes(svg_path=str(path))
                im = Image.open(io.BytesIO(png)).convert("RGBA")
                assert im.size == (round(w), round(h)), (filename, im.size, w, h)
                key = (
                    "panel_img_"
                    + hashlib.sha256(im.tobytes() + str(im.size).encode()).hexdigest()[
                        :12
                    ]
                )
                images[key] = im
                add("Image", x, y, w, h, asset=key)
                continue
            cid = child.get("data-node-id")
            cm = by_id.get(cid) if cid else None
            if cm is None and child.tag.startswith("Panel"):
                used = getattr(walk, "used", set())
                cm = next(
                    n for n in mn if n.tag == "instance" and n.get("id") not in used
                )
                used.add(cm.get("id"))
                walk.used = used
            if cm is not None:
                if cm.tag == "instance":
                    walk.used.add(cm.get("id"))
                walk(child, cm, x, y, style + " " + inherited)

    walk.used = set()
    walk(tree, by_id[page["id"]])
    expected_images = sum(1 for element in tree.iter() if element.tag == "img")
    actual_images = [node for node in nodes if node["kind"] == "Image"]
    assert len(actual_images) == expected_images, (page["name"], "missing image slot")
    for node in actual_images:
        assert 0 <= node["x"] <= 320 - node["w"], (page["name"], node)
        assert 0 <= node["y"] <= 240 - node["h"], (page["name"], node)
    pages.append(dict(name=page["name"].split(" / ")[0], id=page["id"], nodes=nodes))

# Generate subset fonts at the design sizes. The bitmap fonts are checked in;
# the desktop font is only needed when regenerating, not for firmware builds.
font_path = pathlib.Path("C:/Windows/Fonts/NotoSansSC-VF.ttf")
extra = "未连接暂无数据本地设定已保存等待设备接入在线离线未知开启关闭低风中风高风制冷制热送风除湿自动确认取消返回保存0123456789-:/% °CabcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ"
for source in OUT.glob("*.cc"):
    extra += "".join(re.findall(r'"([^"\n]*)"', source.read_text(encoding="utf-8")))
chars = sorted(
    set(extra + "".join(n["text"] for p in pages for n in p["nodes"])) - {"\n"}
)
sizes = sorted(set(n["size"] for p in pages for n in p["nodes"] if n["kind"] == "Text"))
fonts = {}
symbol_fonts = {}
font_c = [
    "// Generated by scripts/controller_ui/generate.py",
    '#include "lvgl.h"',
    "LV_FONT_DECLARE(BUILTIN_TEXT_FONT);",
]
for size in sizes:
    runtime_glyphs = {
        18: "环境回水出水设备故障通讯状态暂无历史记录模式未知热制冷送风除湿速未连接--故障码：",
        20: "设备未连接",
        40: "-",
        44: "-",
    }.get(size, "")
    size_chars = (
        chars
        if size < 18
        else sorted(
            set(
                "0123456789-: °C"
                + runtime_glyphs
                + "".join(
                    n["text"] for p in pages for n in p["nodes"] if n["size"] == size
                )
            )
        )
    )
    f = ImageFont.truetype(str(font_path), size)
    f.set_variation_by_axes([400 if size == 12 else 500])
    if size == 44:
        f = ImageFont.truetype(str(ROOT / "fonts/Inter[opsz,wght].ttf"), size)
        f.set_variation_by_axes([14, 500])
    fonts[size] = f
    symbol_fonts[size] = ImageFont.truetype(
        str(ROOT / "fonts/NotoSansSymbols2-Regular.ttf"), size
    )
    ascent, descent = f.getmetrics()
    bitmap = bytearray()
    desc = ["{0,0,0,0,0,0}"]
    for ch in size_chars:
        glyph_font = symbol_fonts[size] if ch in "▾❄" else f
        mask, offset = glyph_font.getmask2(ch, mode="L", anchor="ls")
        width, height = mask.size
        desc.append(
            "{%d,%d,%d,%d,%d,%d}"
            % (
                len(bitmap),
                round(glyph_font.getlength(ch) * 16),
                width,
                height,
                offset[0],
                -offset[1] - height,
            )
        )
        pixels = bytes(mask)
        # LVGL 4bpp anti-aliased coverage: half the flash of 8bpp, no layout loss.
        bitmap.extend(
            ((pixels[i] >> 4) << 4)
            | ((pixels[i + 1] >> 4) if i + 1 < len(pixels) else 0)
            for i in range(0, len(pixels), 2)
        )
    prefix = f"panel_font_{size}"
    font_c += [
        f"static const uint8_t {prefix}_bitmap[] = {{"
        + ",".join(map(str, bitmap))
        + "};",
        f"static const lv_font_fmt_txt_glyph_dsc_t {prefix}_glyphs[] = {{"
        + ",".join(desc)
        + "};",
        f"static const uint16_t {prefix}_unicode[] = {{"
        + ",".join(str(ord(c) - ord(size_chars[0])) for c in size_chars)
        + "};",
        f"static const lv_font_fmt_txt_cmap_t {prefix}_cmap[] = {{{{.range_start={ord(size_chars[0])},.range_length={ord(size_chars[-1])-ord(size_chars[0])+1},.glyph_id_start=1,.unicode_list={prefix}_unicode,.list_length={len(size_chars)},.type=LV_FONT_FMT_TXT_CMAP_SPARSE_TINY}}}};",
        f"static lv_font_fmt_txt_dsc_t {prefix}_dsc = {{.glyph_bitmap={prefix}_bitmap,.glyph_dsc={prefix}_glyphs,.cmaps={prefix}_cmap,.cmap_num=1,.bpp=4}};",
        f"const lv_font_t {prefix} = {{.get_glyph_dsc=lv_font_get_glyph_dsc_fmt_txt,.get_glyph_bitmap=lv_font_get_bitmap_fmt_txt,.line_height={ascent+descent},.base_line={descent},.dsc=&{prefix}_dsc,.fallback=&BUILTIN_TEXT_FONT}};",
    ]
(OUT / "controller_fonts.c").write_text("\n".join(font_c), encoding="utf-8")

asset_c = ["// Generated from original Figma SVG assets.", '#include "lvgl.h"']
for key, im in images.items():
    raw = im.tobytes("raw", "BGRA")
    asset_c += [
        f"static const uint8_t {key}_data[] = {{" + ",".join(map(str, raw)) + "};",
        f"const lv_image_dsc_t {key} = {{.header={{.magic=LV_IMAGE_HEADER_MAGIC,.cf=LV_COLOR_FORMAT_ARGB8888,.w={im.width},.h={im.height},.stride={im.width*4}}},.data_size={len(raw)},.data={key}_data}};",
    ]
(OUT / "controller_icons.c").write_text("\n".join(asset_c), encoding="utf-8")
quote = lambda s: json.dumps(s, ensure_ascii=False)
h = [
    "// Generated from Figma context; regenerate with scripts/controller_ui/generate.py.",
    "#pragma once",
]
h += [f"LV_FONT_DECLARE(panel_font_{s});" for s in sizes]
h += [f"LV_IMAGE_DECLARE({k});" for k in images]
for p in pages:
    h.append(f'static const PanelNode nodes_{p["name"]}[] = {{')
    for n in p["nodes"]:
        h.append(
            "    {PanelKind::%s,%d,%d,%d,%d,%s,%d,0x%x,0x%x,%d,%s,%d,%s,%d},"
            % (
                n["kind"],
                n["x"],
                n["y"],
                n["w"],
                n["h"],
                quote(n["text"]),
                n["size"],
                n["fg"],
                n["bg"],
                n["radius"],
                quote(n["action"]),
                n["align"],
                "&" + n["asset"] if n["asset"] else "nullptr",
                n["border"],
            )
        )
    h.append("};")
h.append("static const PanelPage panel_pages[] = {")
for p in pages:
    h.append(
        f'    {{{quote(p["name"])}, nodes_{p["name"]}, sizeof(nodes_{p["name"]}) / sizeof(PanelNode)}},'
    )
h.append("};")
h.append("static const lv_font_t* PanelFont(int size) { switch(size) {")
h += [f"case {s}: return &panel_font_{s};" for s in sizes]
h.append("default: return &panel_font_14; }}")
(OUT / "controller_pages.inc").write_text("\n".join(h), encoding="utf-8")
subprocess.run(
    [
        "clang-format",
        "--style=file:" + str(ROOT / ".clang-format"),
        "-i",
        str(OUT / "controller_pages.inc"),
        str(OUT / "controller_fonts.c"),
        str(OUT / "controller_icons.c"),
    ],
    check=True,
)
(ROOT / "layout.json").write_text(
    json.dumps(pages, ensure_ascii=False, indent=2), encoding="utf-8"
)

# A reference-sized contact sheet for geometry and resource review.
sheet = Image.new("RGB", (320 * 6, 264 * ((len(pages) + 5) // 6)), "#33404a")
for i, p in enumerate(pages):
    im = Image.new("RGBA", (320, 240), "#10191f")
    draw = ImageDraw.Draw(im)
    for n in p["nodes"]:
        x, y, w, h = n["x"], n["y"], n["w"], n["h"]
        if n["kind"] == "Box":
            draw.rounded_rectangle(
                (x, y, x + w - 1, y + h - 1), radius=n["radius"], fill=f'#{n["bg"]:06x}'
            )
            if n["border"]:
                draw.rounded_rectangle(
                    (x, y, x + w - 1, y + h - 1),
                    radius=n["radius"],
                    outline="#63e0cb",
                    width=n["border"],
                )
        elif n["kind"] == "Image":
            im.alpha_composite(images[n["asset"]], (x, y))
        else:
            f = fonts[n["size"]]
            tw = sum(
                (symbol_fonts[n["size"]] if ch in "▾❄" else f).getlength(ch)
                for ch in n["text"]
            )
            tx = x + (w - tw) * (n["align"] / 2)
            a, d = f.getmetrics()
            for ch in n["text"]:
                gf = symbol_fonts[n["size"]] if ch in "▾❄" else f
                draw.text(
                    (tx, y + (h - a - d) / 2 + a),
                    ch,
                    font=gf,
                    fill=f'#{n["fg"]:06x}',
                    anchor="ls",
                )
                tx += gf.getlength(ch)
    sheet.paste(im, (i % 6 * 320, i // 6 * 264 + 24))
    ImageDraw.Draw(sheet).text(
        (i % 6 * 320 + 8, i // 6 * 264 + 4), p["name"], fill="white"
    )
sheet.save(ROOT / "preview.png")
print(
    f"Generated {len(pages)} pages, {len(images)} unique images, {len(chars)} glyphs at {sizes}"
)
