#!/usr/bin/env python3
"""Genera docs/chart.svg a partir de readings.csv, sin dependencias externas.

Uso:  python3 tools/plot_csv.py [readings.csv] [chart.svg]
"""
import csv
import sys

W = 800
H = 320
MARGIN = 46
TEMP_MIN, TEMP_MAX = 10, 45
HUM_MIN, HUM_MAX = 20, 100
TEMP_THRESHOLD = 40
HUM_THRESHOLD = 85
COLOR_TEMP = "#f0883e"
COLOR_HUM = "#3fb950"
COLOR_BAD = "#f85149"
COLOR_GRID = "#30363d"
COLOR_TEXT = "#c9d6d4"
COLOR_BG = "#0d1117"


def load(path):
    temp, hum = [], []
    with open(path, newline="") as f:
        for row in csv.DictReader(f):
            try:
                t = int(row["epoch_ms"])
                v = int(row["valor"])
            except (KeyError, ValueError):
                continue
            if row["sensor"] == "temperatura":
                temp.append((t, v))
            elif row["sensor"] == "humedad":
                hum.append((t, v))
    return temp, hum


def panel(series, t0, span, y0, y1, vmin, vmax, color, label, threshold):
    """Devuelve el SVG de un panel (rejilla, serie y umbral)."""
    out = []
    pw = W - 2 * MARGIN

    def px(t):
        return MARGIN + (0 if span == 0 else (t - t0) * pw / span)

    def py(v):
        frac = (v - vmin) / (vmax - vmin)
        frac = max(0.0, min(1.0, frac))
        return y1 - frac * (y1 - y0)

    out.append(f'<rect x="{MARGIN}" y="{y0}" width="{pw}" height="{y1 - y0}" '
               f'fill="none" stroke="{COLOR_GRID}"/>')

    for frac in (0.0, 0.5, 1.0):
        v = vmin + frac * (vmax - vmin)
        y = py(v)
        out.append(f'<line x1="{MARGIN}" y1="{y:.1f}" x2="{W - MARGIN}" y2="{y:.1f}" '
                   f'stroke="{COLOR_GRID}" stroke-dasharray="2,4"/>')
        out.append(f'<text x="{MARGIN - 6}" y="{y + 4:.1f}" fill="{COLOR_TEXT}" '
                   f'font-size="11" text-anchor="end" '
                   f'font-family="monospace">{int(v)}</text>')

    if threshold is not None and vmin < threshold < vmax:
        y = py(threshold)
        out.append(f'<line x1="{MARGIN}" y1="{y:.1f}" x2="{W - MARGIN}" y2="{y:.1f}" '
                   f'stroke="{COLOR_BAD}" stroke-dasharray="6,4"/>')
        out.append(f'<text x="{W - MARGIN - 4}" y="{y - 5:.1f}" fill="{COLOR_BAD}" '
                   f'font-size="11" text-anchor="end" '
                   f'font-family="monospace">umbral {threshold}</text>')

    if series:
        pts = " ".join(f"{px(t):.1f},{py(v):.1f}" for t, v in series)
        out.append(f'<polyline points="{pts}" fill="none" stroke="{color}" '
                   f'stroke-width="1.8" stroke-linejoin="round"/>')

    out.append(f'<text x="{MARGIN}" y="{y0 - 8}" fill="{color}" font-size="12" '
               f'font-family="monospace">{label}</text>')
    return "".join(out)


def main():
    src = sys.argv[1] if len(sys.argv) > 1 else "readings.csv"
    dst = sys.argv[2] if len(sys.argv) > 2 else "docs/chart.svg"

    temp, hum = load(src)
    all_pts = temp + hum

    if len(all_pts) < 2:
        print(f"pocos datos en {src} ({len(all_pts)} filas), no se genera grafica")
        return 1

    t0 = min(t for t, _ in all_pts)
    t1 = max(t for t, _ in all_pts)
    span = max(1, t1 - t0)

    svg = [
        f'<svg xmlns="http://www.w3.org/2000/svg" width="{W}" height="{H}" '
        f'viewBox="0 0 {W} {H}" font-family="monospace">',
        f'<rect width="{W}" height="{H}" fill="{COLOR_BG}"/>',
        f'<text x="{MARGIN}" y="24" fill="{COLOR_TEXT}" font-size="14" '
        f'font-weight="bold">Estacion FreeRTOS: temperatura y humedad</text>',
        f'<text x="{W - MARGIN}" y="24" fill="{COLOR_TEXT}" font-size="11" '
        f'text-anchor="end">readings.csv ({len(temp)} temp / {len(hum)} hum)</text>',
        panel(temp, t0, span, 44, 160, TEMP_MIN, TEMP_MAX, COLOR_TEMP,
              "temperatura (C)", TEMP_THRESHOLD),
        panel(hum, t0, span, 196, 300, HUM_MIN, HUM_MAX, COLOR_HUM,
              "humedad (%)", HUM_THRESHOLD),
        f'<text x="{MARGIN}" y="{H - 6}" fill="{COLOR_TEXT}" font-size="11">'
        f'0 s</text>',
        f'<text x="{W - MARGIN}" y="{H - 6}" fill="{COLOR_TEXT}" font-size="11" '
        f'text-anchor="end">+{span / 1000:.0f} s</text>',
        "</svg>",
    ]

    with open(dst, "w") as f:
        f.write("\n".join(svg) + "\n")

    print(f"{dst}: {len(temp)} temp, {len(hum)} hum, span {span / 1000:.0f} s")
    return 0


if __name__ == "__main__":
    sys.exit(main())
