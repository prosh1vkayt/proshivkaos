#!/usr/bin/env python3
"""tools/fontgen.py — сглаженные шрифты для proshivkaOS.

На телефоне нет ни плавающей точки в ядре, ни места для растеризатора
TrueType — и не нужно: буквы растрируются здесь, на компьютере, со
сглаживанием, во всех размерах, которые использует интерфейс. На телефоне
остаётся наложить готовую маску прозрачности нужным цветом.

Размеры привязаны к старой сетке 8x8: масштаб s — это клетка 8*s пикселей.
Базовая линия стоит на 7*s от верха клетки, как у прежнего шрифта: вёрстка
приложений, рассчитанная на него, не съезжает.

    ui    — Roboto, кегль 8*s: подписи, заголовки, часы;
    mono  — Roboto Mono, кегль 7*s: терминал, в клетку влезают и хвосты
            букв вроде «у» и «р».

Использование: python3 tools/fontgen.py > gui/font_data.c
Шрифты: third_party/fonts (SIL Open Font License 1.1, файлы лицензии там же).
"""
import sys, os
from PIL import Image, ImageDraw, ImageFont

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FONTS = os.path.join(ROOT, "third_party", "fonts")

SCALES = range(1, 9)
CHARS = [c for c in range(0x20, 0x7F)] + [0xB0, 0x2022, 0x2190, 0x2192] + \
        [0x401] + list(range(0x410, 0x450)) + [0x451]


def load(path, px, weight):
    f = ImageFont.truetype(path, px)
    try:
        axes = f.get_variation_axes()
        vals = []
        for a in axes:
            name = a.get("name", b"")
            name = name.decode() if isinstance(name, bytes) else str(name)
            if "eight" in name or name.lower().startswith("wght"):
                vals.append(weight)
            else:
                vals.append(a.get("default", 100))
        f.set_variation_by_axes(vals)
    except Exception:
        pass
    return f


def render_face(path, px, weight):
    font = load(path, px, weight)
    ascent, descent = font.getmetrics()
    glyphs = []
    bits = bytearray()
    for cp in CHARS:
        ch = chr(cp)
        adv = font.getlength(ch)
        # Рисуем с запасом вокруг, базовая линия — на ascent.
        pad = px
        W, H = int(adv) + 2 * pad + 4, ascent + descent + 2 * pad
        im = Image.new("L", (W, H), 0)
        d = ImageDraw.Draw(im)
        d.text((pad, pad), ch, font=font, fill=255)
        bbox = im.getbbox()
        if bbox is None:
            glyphs.append((cp, 0, 0, 0, 0, int(round(adv * 64)), len(bits)))
            continue
        x0, y0, x1, y1 = bbox
        crop = im.crop(bbox)
        w, h = crop.size
        bx = x0 - pad                      # от точки пера
        by = (pad + ascent) - y0           # верх над базовой линией
        glyphs.append((cp, w, h, bx, by, int(round(adv * 64)), len(bits)))
        bits.extend(crop.tobytes())
    return ascent, descent, glyphs, bits


def emit(name, path, em_of_scale, weight, out):
    faces = []
    for s in SCALES:
        px = em_of_scale(s)
        asc, desc, glyphs, bits = render_face(path, px, weight)
        tag = "%s_%d" % (name, s)
        out.write("static const uint8_t %s_bits[] = {\n" % tag)
        for i in range(0, len(bits), 24):
            out.write("  " + ",".join(str(b) for b in bits[i:i + 24]) + ",\n")
        out.write("};\n")
        out.write("static const font_glyph_t %s_glyphs[] = {\n" % tag)
        for (cp, w, h, bx, by, adv, off) in glyphs:
            out.write("  {0x%04X,%d,%d,%d,%d,%d,%d},\n" % (cp, w, h, bx, by, adv, off))
        out.write("};\n\n")
        faces.append((s, px, asc, desc, len(glyphs), tag))
    out.write("const font_face_t g_font_%s[FONT_SCALES] = {\n" % name)
    out.write("  {0},\n")
    for (s, px, asc, desc, n, tag) in faces:
        out.write("  { %d, %d, %d, %d, %s_glyphs, %s_bits },\n" % (px, asc, desc, n, tag, tag))
    out.write("};\n\n")


def main():
    out = sys.stdout
    out.write("/* gui/font_data.c — СГЕНЕРИРОВАНО tools/fontgen.py, руками не править.\n")
    out.write(" * Roboto и Roboto Mono, SIL Open Font License 1.1 (third_party/fonts). */\n")
    out.write('#include "font.h"\n\n')
    emit("ui", os.path.join(FONTS, "Roboto.ttf"), lambda s: 8 * s, 400, out)
    emit("mono", os.path.join(FONTS, "RobotoMono.ttf"), lambda s: 7 * s, 400, out)


if __name__ == "__main__":
    main()
