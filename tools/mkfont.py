#!/usr/bin/env python3
"""Turn a Terminus BDF into the C table the framebuffer console renders with.

The kernel console has no font stack: it needs 256 fixed-size glyph bitmaps,
indexed by the byte the terminal prints. This script extracts exactly those
from a Glyph Bitmap Distribution Format (BDF) file -- Terminus is shipped as
BDF -- and writes one C source file:

    python tools/mkfont.py ter-u16b.bdf kernel/drivers/font_terminus.c

The output is a plain `static const uint8_t[256 * height]` table plus the
`haku_font_t` descriptor that names it, because a generated table is the one
thing the freestanding build cannot produce at compile time.

Codepoints are laid out in code page 437 order, so the byte the terminal is
handed indexes the glyph directly. Position 0x20..0x7E is ASCII, which every
BDF has; the upper half is mapped through the CP437 -> Unicode table below so
the box-drawing and accented glyphs a DOS-style console expects show up with
Terminus shapes instead of the VGA ROM set. A byte CP437 leaves undefined, or
a codepoint this particular BDF omits, becomes a blank cell.

Terminus is licensed under the SIL Open Font License 1.1 (see
LICENSES/Terminus-OFL.txt); the generated table carries that notice.
"""

import sys

# Code page 437 -> Unicode. 0x00..0x1F are control codes: the console renderer
# handles them as control characters, so their cells stay blank.
CP437 = [
    0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
    0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
    0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
    0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
    0x0020, 0x0021, 0x0022, 0x0023, 0x0024, 0x0025, 0x0026, 0x0027,
    0x0028, 0x0029, 0x002A, 0x002B, 0x002C, 0x002D, 0x002E, 0x002F,
    0x0030, 0x0031, 0x0032, 0x0033, 0x0034, 0x0035, 0x0036, 0x0037,
    0x0038, 0x0039, 0x003A, 0x003B, 0x003C, 0x003D, 0x003E, 0x003F,
    0x0040, 0x0041, 0x0042, 0x0043, 0x0044, 0x0045, 0x0046, 0x0047,
    0x0048, 0x0049, 0x004A, 0x004B, 0x004C, 0x004D, 0x004E, 0x004F,
    0x0050, 0x0051, 0x0052, 0x0053, 0x0054, 0x0055, 0x0056, 0x0057,
    0x0058, 0x0059, 0x005A, 0x005B, 0x005C, 0x005D, 0x005E, 0x005F,
    0x0060, 0x0061, 0x0062, 0x0063, 0x0064, 0x0065, 0x0066, 0x0067,
    0x0068, 0x0069, 0x006A, 0x006B, 0x006C, 0x006D, 0x006E, 0x006F,
    0x0070, 0x0071, 0x0072, 0x0073, 0x0074, 0x0075, 0x0076, 0x0077,
    0x0078, 0x0079, 0x007A, 0x007B, 0x007C, 0x007D, 0x007E, 0x0000,
    0x00C7, 0x00FC, 0x00E9, 0x00E2, 0x00E4, 0x00E0, 0x00E5, 0x00E7,
    0x00EA, 0x00EB, 0x00E8, 0x00EF, 0x00EE, 0x00EC, 0x00C4, 0x00C5,
    0x00C9, 0x00E6, 0x00C6, 0x00F4, 0x00F6, 0x00F2, 0x00FB, 0x00F9,
    0x00FF, 0x00D6, 0x00DC, 0x00A2, 0x00A3, 0x00A5, 0x20A7, 0x0192,
    0x00E1, 0x00ED, 0x00F3, 0x00FA, 0x00F1, 0x00D1, 0x00AA, 0x00BA,
    0x00BF, 0x2310, 0x00AC, 0x00BD, 0x00BC, 0x00A1, 0x00AB, 0x00BB,
    0x2591, 0x2592, 0x2593, 0x2502, 0x2524, 0x2561, 0x2562, 0x2556,
    0x2555, 0x2563, 0x2551, 0x2557, 0x255D, 0x255C, 0x255B, 0x2510,
    0x2514, 0x2534, 0x252C, 0x251C, 0x2500, 0x253C, 0x255E, 0x255F,
    0x255A, 0x2554, 0x2569, 0x2566, 0x2560, 0x2550, 0x256C, 0x2567,
    0x2568, 0x2564, 0x2565, 0x2559, 0x2558, 0x2552, 0x2553, 0x256B,
    0x256A, 0x2518, 0x250C, 0x2588, 0x2584, 0x258C, 0x2590, 0x2580,
    0x03B1, 0x00DF, 0x0393, 0x03C0, 0x03A3, 0x03C3, 0x00B5, 0x03C4,
    0x03A6, 0x0398, 0x03A9, 0x03B4, 0x221E, 0x03C6, 0x03B5, 0x2229,
    0x2261, 0x00B1, 0x2265, 0x2264, 0x2320, 0x2321, 0x00F7, 0x2248,
    0x00B0, 0x2219, 0x00B7, 0x221A, 0x207F, 0x00B2, 0x25A0, 0x00A0,
]


def read_bdf(path):
    """Return (name, width, height, xoff, yoff, {codepoint: rows})."""
    meta = {}
    glyphs = {}
    family = "Terminus"
    glyph = None
    with open(path, "r", encoding="latin-1") as handle:
        for line in handle:
            parts = line.split()
            if not parts:
                continue
            keyword = parts[0]
            if keyword == "FONT":
                family = parts[1].split("-")[2] if len(parts) > 1 else family
            elif keyword == "FONTBOUNDINGBOX":
                meta["width"], meta["height"] = int(parts[1]), int(parts[2])
                meta["xoff"], meta["yoff"] = int(parts[3]), int(parts[4])
            elif keyword == "STARTCHAR":
                glyph = {"rows": [], "w": 0, "h": 0, "x": 0, "y": 0,
                         "code": None}
            elif keyword == "ENCODING" and glyph is not None:
                glyph["code"] = int(parts[1])
            elif keyword == "BBX" and glyph is not None:
                glyph["w"], glyph["h"] = int(parts[1]), int(parts[2])
                glyph["x"], glyph["y"] = int(parts[3]), int(parts[4])
            elif keyword == "BITMAP" and glyph is not None:
                glyph["on"] = True
            elif keyword == "ENDCHAR" and glyph is not None:
                if glyph["code"] is not None and glyph["code"] >= 0:
                    glyphs[glyph["code"]] = glyph
                glyph = None
            elif glyph is not None and glyph.get("on"):
                glyph["rows"].append(parts[0])
    return family, meta, glyphs


def render(glyph, cell, fbb):
    """Place one BDF glyph in the fixed cell; return `height` rows of bytes.

    BDF coordinates are relative to the baseline: a glyph's bbox sits `h +
    yoff` rows above it. The cell's baseline is `height + yoff` rows from its
    top (both taken from FONTBOUNDINGBOX), which is what keeps descenders and
    accents inside their 16 rows.
    """
    width, height, stride = cell
    rows = [bytearray(stride) for _ in range(height)]
    if glyph is None:
        return rows
    baseline = height + fbb[3]
    top = baseline - (glyph["h"] + glyph["y"])
    left = glyph["x"] - fbb[2]
    for gy, hexrow in enumerate(glyph["rows"]):
        y = top + gy
        if y < 0 or y >= height:
            continue
        value = int(hexrow, 16)
        bits = len(hexrow) * 4
        for gx in range(glyph["w"]):
            if not (value >> (bits - 1 - gx)) & 1:
                continue
            x = left + gx
            if x < 0 or x >= width:
                continue
            rows[y][x >> 3] |= 0x80 >> (x & 7)
    return rows


def main(argv):
    if len(argv) not in (2, 3):
        print(__doc__.strip().splitlines()[0])
        print("usage: mkfont.py <font.bdf> [output.c]")
        return 2
    family, meta, glyphs = read_bdf(argv[1])
    width, height = meta["width"], meta["height"]
    stride = (width + 7) // 8
    cell = (width, height, stride)
    fbb = (meta["width"], meta["height"], meta["xoff"], meta["yoff"])

    missing = [hex(code) for code in CP437 if code and code not in glyphs]
    # Codepoint 0 marks a byte the console treats as a control code (or an
    # unmapped one): it never reaches the renderer, so its cell stays blank
    # instead of borrowing whatever the BDF puts at U+0000.
    table = [render(glyphs.get(code) if code else None, cell, fbb)
             for code in CP437]
    blank = sum(1 for code in CP437 if code and code not in glyphs)

    if len(argv) == 2:
        # Preview: the checks that make a wrong baseline or a wrong byte order
        # visible without booting the kernel.
        for ch in "AaMg_|/1@":
            print(f"--- {ch!r} ---")
            for row in table[ord(ch)]:
                print("    " + "".join("#" if row[0] & (0x80 >> i) else "."
                                       for i in range(width)))
        print(f"{family} {width}x{height}: {256 - blank} glyphs, "
              f"{blank} CP437 codepoints absent"
              + (f" ({', '.join(missing[:8])}...)" if missing else ""))
        return 0

    # font.h declares `terminus_<w>x<h>`, so regeneration keeps the symbol the
    # renderer links against.
    name = f"terminus_{width}x{height}"
    lines = []
    add = lines.append
    add("/* Terminus %dx%d, code page 437 order." % (width, height))
    add(" *")
    add(" * Generated by tools/mkfont.py from %s (%s), which is"
        % ("ter-u%db.bdf" % height, family))
    add(" * Copyright (C) 2020 Dimitar Toshkov Zhekov and licensed under the")
    add(" * SIL Open Font License 1.1 -- see LICENSES/Terminus-OFL.txt.")
    add(" * Do not edit: change the generator or the BDF and regenerate.")
    add(" */")
    add('#include "font.h"')
    add("")
    add("static const uint8_t %s_glyphs[256 * %d] = {" % (name, height * stride))
    for index, rows in enumerate(table):
        flat = [byte for row in rows for byte in row]
        body = ", ".join("0x%02X" % byte for byte in flat)
        add("    /* 0x%02X */ %s%s" % (index, body,
                                     "," if index != 255 else ""))
    add("};")
    add("")
    add("const haku_font_t %s = {" % name)
    add('    "terminus-%dx%d",' % (width, height))
    add("    %d, %d, %d," % (width, height, stride))
    add("    %s_glyphs," % name)
    add("};")
    add("")
    with open(argv[2], "w", encoding="utf-8", newline="\n") as handle:
        handle.write("\n".join(lines))
    print("wrote %s: %d glyphs, %d bytes each" %
          (argv[2], 256, height * stride))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
