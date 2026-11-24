#!/usr/bin/env python3
"""Draw the launcher's icon: a BMX wheel on an orange rounded square, drawn here, with no art taken from the game.

usage: python tools/make_icon.py [out.ico] [preview.png]     (default: launcher/icon.ico)

Writes a Windows icon holding 16, 24, 32, 48, 64 and 256 pixel pictures (each a PNG inside the .ico), drawn with
4 x 4 samples per pixel for smooth edges. Needs only the Python standard library.
"""
import math, os, struct, sys, zlib

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SIZES = (16, 24, 32, 48, 64, 256)
SUB = 4   # samples per pixel along each axis

TOP, BOTTOM = (255, 138, 36), (214, 61, 15)   # the square: orange, darker towards the bottom
TIRE, RIM, SPOKE, HUB = (28, 28, 32), (236, 236, 240), (250, 250, 252), (250, 250, 252)


def inside_rounded(x, y, r):
    """(x, y) in 0..1 inside a square with corners of radius r."""
    cx = min(max(x, r), 1 - r); cy = min(max(y, r), 1 - r)
    return (x - cx) ** 2 + (y - cy) ** 2 <= r * r


def near_segment(px, py, ax, ay, bx, by, half):
    dx, dy = bx - ax, by - ay
    t = max(0.0, min(1.0, ((px - ax) * dx + (py - ay) * dy) / (dx * dx + dy * dy)))
    return (px - ax - t * dx) ** 2 + (py - ay - t * dy) ** 2 <= half * half


def colour_at(x, y, size):
    """The colour at (x, y) in 0..1, or None outside the icon. Small icons get bolder lines."""
    if not inside_rounded(x, y, 0.2):
        return None
    bold = size <= 24
    d = math.hypot(x - 0.5, y - 0.5)
    tire_in, tire_out = (0.25, 0.42) if bold else (0.29, 0.41)
    if tire_in <= d <= tire_out:
        return TIRE
    if not bold and 0.255 <= d < tire_in:
        return RIM
    hub = 0.1 if bold else 0.075
    if d <= hub:
        return TIRE if d <= hub * 0.42 else HUB
    if d < tire_in:
        spokes = 6 if bold else 12
        half = 0.035 if bold else 0.012
        for k in range(spokes):
            a = math.pi * 2 * k / spokes + math.pi / 12
            ex, ey = 0.5 + math.cos(a) * tire_in, 0.5 + math.sin(a) * tire_in
            if near_segment(x, y, 0.5, 0.5, ex, ey, half):
                return SPOKE
    t = y
    return tuple(int(TOP[i] + (BOTTOM[i] - TOP[i]) * t) for i in range(3))


def render(size):
    """RGBA rows of the icon at size x size pixels."""
    rows = []
    for py in range(size):
        row = bytearray()
        for px in range(size):
            r = g = b = n = 0
            for sy in range(SUB):
                for sx in range(SUB):
                    c = colour_at((px + (sx + 0.5) / SUB) / size, (py + (sy + 0.5) / SUB) / size, size)
                    if c:
                        r += c[0]; g += c[1]; b += c[2]; n += 1
            if n:
                row += bytes((r // n, g // n, b // n, 255 * n // (SUB * SUB)))
            else:
                row += b'\0\0\0\0'
        rows.append(bytes(row))
    return rows


def png(size, rows):
    def chunk(t, c):
        return struct.pack('>I', len(c)) + t + c + struct.pack('>I', zlib.crc32(t + c) & 0xffffffff)
    raw = b''.join(b'\0' + r for r in rows)
    return (b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', size, size, 8, 6, 0, 0, 0))
            + chunk(b'IDAT', zlib.compress(raw, 9)) + chunk(b'IEND', b''))


def main(argv):
    out = argv[0] if argv else os.path.join(ROOT, 'launcher', 'icon.ico')
    pngs = [(s, png(s, render(s))) for s in SIZES]
    head = struct.pack('<HHH', 0, 1, len(pngs))
    offset = 6 + 16 * len(pngs)
    entries = b''; data = b''
    for s, p in pngs:
        entries += struct.pack('<BBBBHHII', s % 256, s % 256, 0, 0, 1, 32, len(p), offset + len(data))
        data += p
    with open(out, 'wb') as f:
        f.write(head + entries + data)
    if len(argv) > 1:   # the largest picture as a PNG, to look at
        with open(argv[1], 'wb') as f:
            f.write(pngs[-1][1])
    print('wrote %s (%s)' % (out, ', '.join(str(s) for s in SIZES)))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
