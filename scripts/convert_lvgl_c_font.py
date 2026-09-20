#!/usr/bin/env python3
"""Convert an LVGL generated C font into the legacy CBIN format.

The xiaozhi-fonts component already ships text fonts in CBIN format, while
the icon fonts are kept as LVGL C sources.  This converter preserves the
glyph data and cmap from those generated C files so they can share the same
runtime loader as text fonts.
"""

import argparse
import re
import struct
from pathlib import Path


def align4(value: int) -> int:
    return (value + 3) & ~3


def parse_ints(source: str) -> list[int]:
    return [int(token, 0) for token in re.findall(r"(?<![A-Za-z0-9_])-?(?:0[xX][0-9A-Fa-f]+|\d+)", source)]


def array_body(source: str, name: str) -> str:
    match = re.search(
        rf"(?:static\s+[^;]+\s+)?{re.escape(name)}\[\]\s*=\s*\{{(.*?)\}}\s*;",
        source,
        re.DOTALL,
    )
    if match is None:
        raise ValueError(f"array not found: {name}")
    return match.group(1)


def parse_glyphs(source: str) -> list[tuple[int, int, int, int, int, int]]:
    body = array_body(source, "glyph_dsc")
    pattern = re.compile(
        r"\{\.bitmap_index\s*=\s*(?P<bitmap>[^,]+),"
        r"\s*\.adv_w\s*=\s*(?P<adv>[^,]+),"
        r"\s*\.box_w\s*=\s*(?P<w>[^,]+),"
        r"\s*\.box_h\s*=\s*(?P<h>[^,]+),"
        r"\s*\.ofs_x\s*=\s*(?P<x>[^,]+),"
        r"\s*\.ofs_y\s*=\s*(?P<y>[^}]+)\}",
    )
    glyphs = []
    for match in pattern.finditer(body):
        glyphs.append(tuple(int(match.group(key).strip(), 0) for key in ("bitmap", "adv", "w", "h", "x", "y")))
    if not glyphs:
        raise ValueError("glyph descriptors not found")
    return glyphs


def parse_cmaps(source: str) -> list[dict[str, int | list[int]]]:
    body = array_body(source, "cmaps")
    cmap_pattern = re.compile(
        r"\{\s*\.range_start\s*=\s*(?P<start>[^,]+),"
        r"\s*\.range_length\s*=\s*(?P<length>[^,]+),"
        r"\s*\.glyph_id_start\s*=\s*(?P<glyph>[^,]+),"
        r"\s*\.unicode_list\s*=\s*(?P<unicode>[^,]+),"
        r"\s*\.glyph_id_ofs_list\s*=\s*(?P<ids>[^,]+),"
        r"\s*\.list_length\s*=\s*(?P<count>[^,]+),"
        r"\s*\.type\s*=\s*(?P<type>[^}]+)\}",
        re.DOTALL,
    )
    cmaps = []
    for match in cmap_pattern.finditer(body):
        unicode_name = match.group("unicode").strip()
        ids_name = match.group("ids").strip()
        unicode_values = []
        if unicode_name != "NULL":
            unicode_values = parse_ints(array_body(source, unicode_name))
        ids_values = []
        if ids_name != "NULL":
            ids_values = parse_ints(array_body(source, ids_name))
        cmap_type = {
            "LV_FONT_FMT_TXT_CMAP_FORMAT0_FULL": 0,
            "LV_FONT_FMT_TXT_CMAP_SPARSE_FULL": 1,
            "LV_FONT_FMT_TXT_CMAP_FORMAT0_TINY": 2,
            "LV_FONT_FMT_TXT_CMAP_SPARSE_TINY": 3,
        }.get(match.group("type").strip())
        if cmap_type is None:
            raise ValueError(f"unsupported cmap type: {match.group('type').strip()}")
        cmaps.append(
            {
                "range_start": int(match.group("start"), 0),
                "range_length": int(match.group("length"), 0),
                "glyph_id_start": int(match.group("glyph"), 0),
                "list_length": int(match.group("count"), 0),
                "type": cmap_type,
                "unicode": unicode_values,
                "ids": ids_values,
            }
        )
    if not cmaps:
        raise ValueError("character maps not found")
    return cmaps


def scalar(source: str, field: str) -> int:
    match = re.search(rf"\.{field}\s*=\s*(-?(?:0[xX][0-9A-Fa-f]+|\d+))", source)
    if match is None:
        raise ValueError(f"font field not found: {field}")
    return int(match.group(1), 0)


def build_cbin(source: str) -> bytes:
    bitmap = bytes(parse_ints(array_body(source, "glyph_bitmap")))
    glyphs = parse_glyphs(source)
    cmaps = parse_cmaps(source)
    line_height = scalar(source, "line_height")
    base_line = scalar(source, "base_line")
    bpp = scalar(source, "bpp")
    bitmap_format = scalar(source, "bitmap_format")
    kern_classes = scalar(source, "kern_classes")

    # The CBIN writer uses the large LVGL glyph descriptor layout regardless
    # of the source build's LV_FONT_FMT_TXT_LARGE setting.
    glyph_data = b"".join(struct.pack("<IIHHhh", *glyph) for glyph in glyphs)
    bitmap_data = bitmap + b"\0" * (align4(len(bitmap)) - len(bitmap))

    # Four-byte pointers make each wire cmap descriptor 20 bytes.
    cmap_size = 20
    cmap_data = bytearray(cmap_size * len(cmaps))
    cmap_payload = bytearray()
    for index, cmap in enumerate(cmaps):
        unicode_values = cmap["unicode"]
        ids_values = cmap["ids"]
        unicode_payload = b"".join(struct.pack("<H", value) for value in unicode_values)
        unicode_payload += b"\0" * (align4(len(unicode_payload)) - len(unicode_payload))
        ids_payload = b""
        if ids_values:
            if cmap["type"] == 0:
                ids_payload = bytes(ids_values)
            else:
                ids_payload = b"".join(struct.pack("<H", value) for value in ids_values)
            ids_payload += b"\0" * (align4(len(ids_payload)) - len(ids_payload))

        payload_offset = cmap_size * len(cmaps) + len(cmap_payload)
        unicode_offset = payload_offset if unicode_values else 0
        cmap_payload.extend(unicode_payload)
        ids_offset = cmap_size * len(cmaps) + len(cmap_payload) if ids_values else 0
        cmap_payload.extend(ids_payload)

        offset = index * cmap_size
        struct.pack_into("<IHHI IHBx", cmap_data, offset,
                         int(cmap["range_start"]), int(cmap["range_length"]),
                         int(cmap["glyph_id_start"]), unicode_offset, ids_offset,
                         int(cmap["list_length"]), int(cmap["type"]))

    sz_font = 36
    sz_dsc = 24
    bitmap_offset = sz_dsc
    glyph_offset = bitmap_offset + len(bitmap_data)
    cmap_offset = glyph_offset + len(glyph_data)

    font_header = bytearray(sz_font)
    struct.pack_into("<IIIii", font_header, 0, 0, 0, 0, line_height, base_line)
    font_header[20] = 0  # subpx / kerning flags
    font_header[21] = 0  # underline position
    font_header[22] = 0  # underline thickness
    struct.pack_into("<III", font_header, 24, sz_font, 0, 0)

    dsc = bytearray(sz_dsc)
    struct.pack_into("<IIIIHH", dsc, 0,
                     bitmap_offset, glyph_offset, cmap_offset, 0,
                     0, len(cmaps) | (bpp << 9) | (kern_classes << 13) | (bitmap_format << 14))

    return bytes(font_header + dsc + bitmap_data + glyph_data + cmap_data + cmap_payload)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("source", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(build_cbin(args.source.read_text(encoding="utf-8")))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
