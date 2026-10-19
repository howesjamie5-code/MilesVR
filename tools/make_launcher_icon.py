"""Draw apps/launcher/spidy.ico: the launcher's mark, Miles's red spider on a black disc in a red ring.

The same picture as App::badge in apps/launcher/ui.cpp. Standard library only; run it again
after changing the design.
"""
import math
import pathlib
import struct
import zlib

ROOT = pathlib.Path(__file__).resolve().parents[1]
RED = (224, 27, 51)
BLACK = (14, 13, 16)
SIZES = (256, 64, 48, 32, 24, 16)
# App::badge's legs, in units of the disc's radius (y down): each a bent line, mirrored left and right.
LEGS = ((0.08, -0.22, 0.3, -0.44, 0.34, -0.76),
        (0.1, -0.14, 0.46, -0.3, 0.68, -0.52),
        (0.1, -0.04, 0.46, 0.1, 0.68, 0.4),
        (0.08, 0.04, 0.3, 0.32, 0.34, 0.74))


def segment_distance(px, py, ax, ay, bx, by):
    dx, dy = bx - ax, by - ay
    t = max(0.0, min(1.0, ((px - ax)*dx + (py - ay)*dy) / (dx*dx + dy*dy)))
    return math.hypot(px - ax - t*dx, py - ay - t*dy)


def spider(x, y, leg):
    """Whether the point (in units of the radius) is on the spider."""
    if math.hypot(x, y + 0.36) <= 0.1:
        return True
    if (x/0.13)**2 + ((y + 0.12)/0.19)**2 <= 1 or (x/0.12)**2 + ((y - 0.27)/0.27)**2 <= 1:
        return True
    ax = abs(x)
    for l in LEGS:
        if (segment_distance(ax, y, l[0], l[1], l[2], l[3]) <= leg or
                segment_distance(ax, y, l[2], l[3], l[4], l[5]) <= leg):
            return True
    return False


def coverage(size, samples=4):
    """RGBA rows for one icon size."""
    c = (size - 1) / 2
    radius = size * 0.47
    # Small icons get a thicker rim and legs, so the spider still reads.
    rim = 0.07 if size > 24 else 0.12
    leg = 0.0375 if size > 32 else 0.06
    rows = []
    for y in range(size):
        row = bytearray()
        for x in range(size):
            disc = red = 0
            for sy in range(samples):
                for sx in range(samples):
                    px = (x + (sx + .5) / samples - .5 - c) / radius
                    py = (y + (sy + .5) / samples - .5 - c) / radius
                    rho = math.hypot(px, py)
                    if rho > 1:
                        continue
                    disc += 1
                    if rho >= 1 - rim or spider(px, py, leg):
                        red += 1
            total = samples*samples
            alpha = disc/total
            share = red/max(disc, 1)
            rgb = [round(b*(1-share) + r*share) for r, b in zip(RED, BLACK)]
            row += bytes((*rgb, round(alpha*255)))
        rows.append(bytes(row))
    return rows


def png(size, rows):
    def chunk(name, data):
        return struct.pack('>I', len(data)) + name + data + struct.pack('>I', zlib.crc32(name + data))
    raw = b''.join(b'\0' + row for row in rows)
    return (b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', size, size, 8, 6, 0, 0, 0)) +
            chunk(b'IDAT', zlib.compress(raw, 9)) + chunk(b'IEND', b''))


def main():
    images = [png(size, coverage(size)) for size in SIZES]
    header = struct.pack('<HHH', 0, 1, len(images))
    offset = 6 + 16*len(images)
    entries = b''
    for size, image in zip(SIZES, images):
        entries += struct.pack('<BBBBHHII', size % 256, size % 256, 0, 0, 1, 32, len(image), offset)
        offset += len(image)
    target = ROOT/'apps/launcher/spidy.ico'
    target.write_bytes(header + entries + b''.join(images))
    print(f'Wrote {target} ({target.stat().st_size} bytes).')


if __name__ == '__main__':
    main()
