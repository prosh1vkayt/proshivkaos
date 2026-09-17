#!/usr/bin/env python3
"""tools/icongen.py — векторные иконки для proshivkaOS.

Иконки — Material Symbols (Rounded) из third_party/icons, те же, что в
Android. Файлы SVG разбираются здесь, на компьютере: путь переводится в
многоугольники (кривые Безье и дуги — ломаными), многоугольники заливаются
по правилу ненулевого обхода с суперсэмплингом 4x4. Получается маска
прозрачности со сглаженными краями.

На телефоне маска масштабируется билинейно до нужного размера и
накладывается цветом (gui/icon.c) — поэтому генерируются только несколько
опорных размеров, а не каждый возможный.

Использование: python3 tools/icongen.py > gui/icon_data.c
"""
import math, os, re, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ICONS = os.path.join(ROOT, "third_party", "icons")
SIZES = [48, 96, 192]
SS = 4

NAMES = ["terminal", "folder", "settings", "info", "arrow_back_ios_new",
         "radio_button_unchecked", "check_box_outline_blank", "chevron_right",
         "arrow_back", "description", "keyboard_hide", "battery_full"]

TOKEN = re.compile(r"[MmLlHhVvCcSsQqTtAaZz]|[-+]?(?:\d+\.?\d*|\.\d+)(?:[eE][-+]?\d+)?")


def tokens(d):
    return TOKEN.findall(d)


def arc_points(x0, y0, rx, ry, phi, large, sweep, x1, y1, steps):
    # SVG 1.1, приложение F.6: из концов в центр.
    if rx == 0 or ry == 0:
        return [(x1, y1)]
    rx, ry = abs(rx), abs(ry)
    cp, sp = math.cos(math.radians(phi)), math.sin(math.radians(phi))
    dx, dy = (x0 - x1) / 2, (y0 - y1) / 2
    x1p, y1p = cp * dx + sp * dy, -sp * dx + cp * dy
    lam = (x1p ** 2) / (rx ** 2) + (y1p ** 2) / (ry ** 2)
    if lam > 1:
        rx, ry = rx * math.sqrt(lam), ry * math.sqrt(lam)
    num = rx * rx * ry * ry - rx * rx * y1p * y1p - ry * ry * x1p * x1p
    den = rx * rx * y1p * y1p + ry * ry * x1p * x1p
    coef = math.sqrt(max(0, num / den)) if den else 0
    if large == sweep:
        coef = -coef
    cxp, cyp = coef * rx * y1p / ry, -coef * ry * x1p / rx
    cx = cp * cxp - sp * cyp + (x0 + x1) / 2
    cy = sp * cxp + cp * cyp + (y0 + y1) / 2

    def ang(ux, uy, vx, vy):
        a = math.atan2(ux * vy - uy * vx, ux * vx + uy * vy)
        return a
    t1 = ang(1, 0, (x1p - cxp) / rx, (y1p - cyp) / ry)
    dt = ang((x1p - cxp) / rx, (y1p - cyp) / ry, (-x1p - cxp) / rx, (-y1p - cyp) / ry)
    if not sweep and dt > 0:
        dt -= 2 * math.pi
    elif sweep and dt < 0:
        dt += 2 * math.pi
    pts = []
    for i in range(1, steps + 1):
        t = t1 + dt * i / steps
        x, y = rx * math.cos(t), ry * math.sin(t)
        pts.append((cp * x - sp * y + cx, sp * x + cp * y + cy))
    return pts


def parse_path(d, steps=24):
    tk = tokens(d)
    i = 0
    polys, cur = [], []
    x = y = sx = sy = 0.0
    lcx = lcy = None      # последняя контрольная точка (для S и T)
    lq = None
    cmd = None

    def num():
        nonlocal i
        v = float(tk[i]); i += 1
        return v

    while i < len(tk):
        if re.match(r"[A-Za-z]", tk[i]):
            cmd = tk[i]; i += 1
            if cmd in "Zz":
                if cur:
                    polys.append(cur); cur = []
                x, y = sx, sy
                lcx = lq = None
                continue
        rel = cmd.islower()
        c = cmd.upper()
        ox, oy = (x, y) if rel else (0.0, 0.0)
        if c == "M":
            if cur:
                polys.append(cur)
            x, y = ox + num(), oy + num()
            sx, sy = x, y
            cur = [(x, y)]
            cmd = "l" if rel else "L"
            lcx = lq = None
        elif c == "L":
            x, y = ox + num(), oy + num(); cur.append((x, y)); lcx = lq = None
        elif c == "H":
            x = ox + num(); cur.append((x, y)); lcx = lq = None
        elif c == "V":
            y = oy + num(); cur.append((x, y)); lcx = lq = None
        elif c in "CS":
            if c == "C":
                x1, y1 = ox + num(), oy + num()
            else:
                x1, y1 = (2 * x - lcx[0], 2 * y - lcx[1]) if lcx else (x, y)
            x2, y2 = ox + num(), oy + num()
            ex, ey = ox + num(), oy + num()
            for k in range(1, steps + 1):
                t = k / steps; u = 1 - t
                cur.append((u**3 * x + 3*u*u*t * x1 + 3*u*t*t * x2 + t**3 * ex,
                            u**3 * y + 3*u*u*t * y1 + 3*u*t*t * y2 + t**3 * ey))
            lcx = (x2, y2); lq = None
            x, y = ex, ey
        elif c in "QT":
            if c == "Q":
                qx, qy = ox + num(), oy + num()
            else:
                qx, qy = (2 * x - lq[0], 2 * y - lq[1]) if lq else (x, y)
            ex, ey = ox + num(), oy + num()
            for k in range(1, steps + 1):
                t = k / steps; u = 1 - t
                cur.append((u*u * x + 2*u*t * qx + t*t * ex, u*u * y + 2*u*t * qy + t*t * ey))
            lq = (qx, qy); lcx = None
            x, y = ex, ey
        elif c == "A":
            rx, ry, phi = num(), num(), num()
            large, sweep = int(num()), int(num())
            ex, ey = ox + num(), oy + num()
            cur.extend(arc_points(x, y, rx, ry, phi, large, sweep, ex, ey, steps * 2))
            x, y = ex, ey; lcx = lq = None
        else:
            raise ValueError("команда пути не поддержана: " + cmd)
    if cur:
        polys.append(cur)
    return polys


def rasterize(polys, vb, size):
    vx, vy, vw, vh = vb
    S = size * SS
    sc = S / vw
    edges = []
    for p in polys:
        n = len(p)
        for k in range(n):
            ax, ay = p[k]; bx, by = p[(k + 1) % n]
            ax, ay = (ax - vx) * sc, (ay - vy) * sc
            bx, by = (bx - vx) * sc, (by - vy) * sc
            if ay == by:
                continue
            wind = 1 if by > ay else -1
            if ay > by:
                ax, ay, bx, by = bx, by, ax, ay
            edges.append((ay, by, ax, (bx - ax) / (by - ay), wind))
    cov = [0] * (size * size)
    for sy in range(S):
        yc = sy + 0.5
        xs = []
        for (y0, y1, x0, slope, wind) in edges:
            if y0 <= yc < y1:
                xs.append((x0 + (yc - y0) * slope, wind))
        if not xs:
            continue
        xs.sort()
        row = sy // SS
        w = 0
        for k in range(len(xs) - 1):
            w += xs[k][1]
            if w == 0:
                continue
            a, b = xs[k][0], xs[k + 1][0]
            ia, ib = max(0, int(math.ceil(a - 0.5))), min(S, int(math.ceil(b - 0.5)))
            for sx in range(ia, ib):
                cov[row * size + sx // SS] += 1
    return bytes(min(255, c * 255 // (SS * SS)) for c in cov)


def main():
    out = sys.stdout
    out.write("/* gui/icon_data.c — СГЕНЕРИРОВАНО tools/icongen.py, руками не править.\n")
    out.write(" * Material Symbols (Rounded), Apache License 2.0 (third_party/icons). */\n")
    out.write('#include "icon.h"\n\n')
    for name in NAMES:
        svg = open(os.path.join(ICONS, name + ".svg")).read()
        vb = [float(v) for v in re.search(r'viewBox="([^"]+)"', svg).group(1).split()]
        polys = []
        for d in re.findall(r'\sd="([^"]+)"', svg):
            polys.extend(parse_path(d))
        for s in SIZES:
            data = rasterize(polys, vb, s)
            out.write("static const uint8_t icon_%s_%d[] = {\n" % (name, s))
            for k in range(0, len(data), 32):
                out.write("  " + ",".join(str(b) for b in data[k:k + 32]) + ",\n")
            out.write("};\n")
    out.write("\nconst icon_def_t g_icons[ICON_COUNT] = {\n")
    for name in NAMES:
        refs = ", ".join("icon_%s_%d" % (name, s) for s in SIZES)
        out.write("  { { %s } },\n" % refs)
    out.write("};\n")
    out.write("const int g_icon_sizes[ICON_SIZES] = { %s };\n" % ", ".join(map(str, SIZES)))


if __name__ == "__main__":
    main()
