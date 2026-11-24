#!/usr/bin/env python3
"""Compare two GIF images pixel by pixel (Python standard library only).

    compare.py EXPECTED.gif ACTUAL.gif [DIFF.png]

Prints the number of differing pixels. DIFF.png (optional) shows the
expected image in grey, with pixels only the actual image has in red and
pixels only the expected image has in blue. Exit status: 0 if identical,
1 if they differ, 2 on error.

Icon writes GIF87a with one image and a global colour table; this reader
handles that, plus local colour tables and interlacing for safety.
"""
import struct
import sys
import zlib


def read_gif(path):
    data = open(path, "rb").read()
    if data[:6] not in (b"GIF87a", b"GIF89a"):
        raise ValueError(f"{path}: not a GIF")
    w, h, flags = struct.unpack("<HHB", data[6:11])
    pos = 13
    gct = None
    if flags & 0x80:
        n = 2 << (flags & 7)
        gct = [tuple(data[pos + 3 * i:pos + 3 * i + 3]) for i in range(n)]
        pos += 3 * n
    while True:
        b = data[pos]
        if b == 0x21:                      # extension: skip it
            pos += 2
            while data[pos]:
                pos += data[pos] + 1
            pos += 1
        elif b == 0x2C:                    # image descriptor
            ix, iy, iw, ih, f = struct.unpack("<HHHHB", data[pos + 1:pos + 10])
            pos += 10
            ct = gct
            if f & 0x80:
                n = 2 << (f & 7)
                ct = [tuple(data[pos + 3 * i:pos + 3 * i + 3]) for i in range(n)]
                pos += 3 * n
            minsize = data[pos]
            pos += 1
            sub = bytearray()
            while data[pos]:
                sub += data[pos + 1:pos + 1 + data[pos]]
                pos += data[pos] + 1
            idx = lzw(bytes(sub), minsize, iw * ih)
            if f & 0x40:
                idx = deinterlace(idx, iw, ih)
            img = [[(0, 0, 0)] * w for _ in range(h)]
            for y in range(ih):
                for x in range(iw):
                    img[iy + y][ix + x] = ct[idx[y * iw + x]]
            return w, h, img
        else:
            raise ValueError(f"{path}: no image")


def lzw(sub, minsize, npix):
    clear, end = 1 << minsize, (1 << minsize) + 1
    size = minsize + 1
    table = [bytes([i]) for i in range(clear)] + [b"", b""]
    out = bytearray()
    prev = None
    bitpos = 0
    nbits = len(sub) * 8
    while bitpos + size <= nbits and len(out) < npix:
        code = 0
        for i in range(size):
            if sub[(bitpos + i) >> 3] >> ((bitpos + i) & 7) & 1:
                code |= 1 << i
        bitpos += size
        if code == clear:
            size = minsize + 1
            table = table[:clear + 2]
            prev = None
            continue
        if code == end:
            break
        if code < len(table):
            entry = table[code]
            if prev is not None:
                table.append(prev + entry[:1])
        else:
            entry = prev + prev[:1]
            table.append(entry)
        out += entry
        prev = entry
        if len(table) == (1 << size) and size < 12:
            size += 1
    return out


def deinterlace(idx, w, h):
    rows = []
    for start, step in ((0, 8), (4, 8), (2, 4), (1, 2)):
        rows += list(range(start, h, step))
    out = bytearray(len(idx))
    for i, y in enumerate(rows):
        out[y * w:(y + 1) * w] = idx[i * w:(i + 1) * w]
    return out


def write_png(path, w, h, rows):
    raw = b"".join(b"\0" + bytes(c for px in row for c in px) for row in rows)
    def chunk(t, d):
        c = struct.pack(">I", len(d)) + t + d
        return c + struct.pack(">I", zlib.crc32(t + d) & 0xFFFFFFFF)
    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n")
        f.write(chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)))
        f.write(chunk(b"IDAT", zlib.compress(raw, 9)))
        f.write(chunk(b"IEND", b""))


def main():
    if len(sys.argv) not in (3, 4):
        print(__doc__.strip().splitlines()[2].strip(), file=sys.stderr)
        return 2
    try:
        ew, eh, e = read_gif(sys.argv[1])
        aw, ah, a = read_gif(sys.argv[2])
    except (OSError, ValueError, IndexError) as err:
        print(f"compare.py: {err}", file=sys.stderr)
        return 2
    if (ew, eh) != (aw, ah):
        print(f"size differs: expected {ew}x{eh}, got {aw}x{ah}")
        return 1
    diff = 0
    rows = []
    for y in range(eh):
        row = []
        for x in range(ew):
            pe, pa = e[y][x], a[y][x]
            g = 255 - (255 - sum(pe) // 3) // 3      # faded expected image
            if pe == pa:
                row.append((g, g, g))
            else:
                diff += 1
                bg = e[0][0]
                row.append((220, 30, 30) if pe == bg else
                           (30, 60, 220) if pa == bg else (230, 140, 0))
        rows.append(row)
    if len(sys.argv) == 4:
        write_png(sys.argv[3], ew, eh, rows)
    total = ew * eh
    print(f"{diff} of {total} pixels differ ({100.0 * diff / total:.2f}%)")
    return 0 if diff == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
