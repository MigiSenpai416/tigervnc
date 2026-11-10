#!/usr/bin/env python3
"""Generates unix/xvnc-wayland/deco_font.h from the X.Org misc-fixed
6x13-ISO8859-1 font (public domain; Debian/Ubuntu package xfonts-base).
Needs python3-pil. Usage: gen-deco-font.py [output]"""
import sys
import gzip, io
from PIL import PcfFontFile
f = PcfFontFile.PcfFontFile(gzip.open("/usr/share/fonts/X11/misc/6x13-ISO8859-1.pcf.gz"))
W, H = 6, 13
asc = 11  # 6x13: ascent 11, descent 2
rows = []
for code in range(256):
    g = f.glyph[code] if code < len(f.glyph) else None
    bits = [0]*H
    if g:
        (dx, dy), (x0, y0, x1, y1), (bx0, by0, bx1, by1), im = g
        # bbox: x0,y0 (top-left rel. to origin, y negative up)
        for yy in range(im.size[1]):
            for xx in range(im.size[0]):
                if im.getpixel((xx, yy)):
                    X = x0 + xx; Y = asc + y0 + yy
                    if 0 <= X < W and 0 <= Y < H:
                        bits[Y] |= 0x80 >> X
    rows.append(bits)
out = io.StringIO()
out.write("/* 6x13 ISO8859-1 glyphs from the X.Org misc-fixed font\n"
          " * (font-misc-misc, 6x13-ISO8859-1): \"Public domain font. Share and\n"
          " * enjoy.\" Generated; one byte per row, MSB = leftmost pixel. */\n\n")
out.write("#define DECO_FONT_W %d\n#define DECO_FONT_H %d\n\n" % (W, H))
out.write("static const unsigned char deco_font[256][%d] = {\n" % H)
for c, b in enumerate(rows):
    out.write("  { %s }, /* %d */\n" % (", ".join("0x%02x" % x for x in b), c))
out.write("};\n")
open(sys.argv[1] if len(sys.argv) > 1 else "deco_font.h", "w").write(out.getvalue())
