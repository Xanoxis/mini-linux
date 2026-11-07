#!/usr/bin/env python3
"""imgdata.py - read and change LG's "imgdata" partition (bootloader pictures).

LG's bootloader (LK) draws the boot logo and its other screens from the imgdata
partition. Format (as on the Nexus 5 / LG G2, see github.com/bitdomo/imgdata):

    0x00  "IMGDATA!"   magic
    0x08  u32          version (1)
    0x0C  u32          number of images
    0x10  8 bytes      zero
    0x18  per image, 40 bytes: name[16], width, height, x, y, offset, size (u32 LE)
    data  run-length coded pixels: 4-byte runs (count, R, G, B), count 1..255;
          the first image starts at 0x400, each one padded to 512 bytes

Standard library only (works in a plain WSL / pmbootstrap python).

    imgdata.py list IMG
    imgdata.py extract IMG DIR                      every picture as PNG
    imgdata.py replace IMG OUT NAME PICTURE.png [--full] [--flip-x] [--flip-y] [--max-size BYTES]
        --full   the new picture covers the whole 320x320 screen at 0,0 (default:
                 same box as the old picture, the new one scaled to fit, centred)
        --round  (with --full) cut the black border off the picture and make the rest
                 as big as fits inside the round glass
"""
import struct
import sys
import zlib

MAGIC = b"IMGDATA!"
SCREEN = 320


# ---------------------------------------------------------------- PNG (read + write)
def png_read(path):
    """-> (w, h, rows of (r,g,b) tuples). 8-bit gray/RGB/RGBA/gray+alpha/palette, no interlace."""
    data = open(path, "rb").read()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise SystemExit(f"{path}: not a PNG")
    pos, idat, plte, trns = 8, b"", None, None
    while pos < len(data):
        ln, typ = struct.unpack(">I4s", data[pos:pos + 8])
        body = data[pos + 8:pos + 8 + ln]
        pos += 12 + ln
        if typ == b"IHDR":
            w, h, depth, ctype, _, _, interlace = struct.unpack(">IIBBBBB", body)
        elif typ == b"PLTE":
            plte = body
        elif typ == b"tRNS":
            trns = body
        elif typ == b"IDAT":
            idat += body
        elif typ == b"IEND":
            break
    if depth != 8 or interlace:
        raise SystemExit(f"{path}: only 8-bit, non-interlaced PNGs (save it again as plain RGB)")
    bpp = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}[ctype]
    raw = zlib.decompress(idat)
    stride = w * bpp
    rows, prev, i = [], bytearray(stride), 0
    for _ in range(h):
        f = raw[i]; line = bytearray(raw[i + 1:i + 1 + stride]); i += 1 + stride
        for x in range(stride):
            a = line[x - bpp] if x >= bpp else 0
            b = prev[x]
            c = prev[x - bpp] if x >= bpp else 0
            if f == 1: line[x] = (line[x] + a) & 255
            elif f == 2: line[x] = (line[x] + b) & 255
            elif f == 3: line[x] = (line[x] + ((a + b) >> 1)) & 255
            elif f == 4:
                p = a + b - c; pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                line[x] = (line[x] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
        prev = line
        px = []
        for x in range(w):
            o = x * bpp
            if ctype == 0: r = g = bl = line[o]; al = 255
            elif ctype == 4: r = g = bl = line[o]; al = line[o + 1]
            elif ctype == 2: r, g, bl = line[o:o + 3]; al = 255
            elif ctype == 6: r, g, bl, al = line[o:o + 4]
            else:
                k = line[o]; r, g, bl = plte[3 * k:3 * k + 3]
                al = trns[k] if trns and k < len(trns) else 255
            # transparent parts become black (the screen background)
            px.append((r * al // 255, g * al // 255, bl * al // 255))
        rows.append(px)
    return w, h, rows


def png_write(path, w, h, rows):
    raw = b"".join(b"\0" + bytes(c for p in row for c in p) for row in rows)
    def chunk(t, b):
        return struct.pack(">I", len(b)) + t + b + struct.pack(">I", zlib.crc32(t + b) & 0xffffffff)
    open(path, "wb").write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
                           + chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))


def resize(w, h, rows, nw, nh):
    """area-average scaling (good for shrinking a big logo)"""
    out = []
    for y in range(nh):
        y0, y1 = y * h / nh, (y + 1) * h / nh
        line = []
        for x in range(nw):
            x0, x1 = x * w / nw, (x + 1) * w / nw
            acc = [0.0, 0.0, 0.0]; tot = 0.0
            yy = int(y0)
            while yy < y1 and yy < h:
                wy = min(y1, yy + 1) - max(y0, yy)
                xx = int(x0)
                while xx < x1 and xx < w:
                    wx = min(x1, xx + 1) - max(x0, xx)
                    p = rows[yy][xx]; k = wx * wy
                    acc[0] += p[0] * k; acc[1] += p[1] * k; acc[2] += p[2] * k; tot += k
                    xx += 1
                yy += 1
            line.append(tuple(int(v / tot + 0.5) for v in acc))
        out.append(line)
    return out


# ---------------------------------------------------------------- imgdata
def parse(buf):
    if buf[:8] != MAGIC:
        raise SystemExit("not an imgdata image (no IMGDATA! magic) - refusing")
    version, n = struct.unpack_from("<II", buf, 8)
    if not 0 < n < 64:
        raise SystemExit(f"odd image count {n} - refusing")
    ents = []
    for i in range(n):
        name, w, h, x, y, off, size = struct.unpack_from("<16s6I", buf, 0x18 + 40 * i)
        ents.append(dict(name=name.split(b"\0")[0].decode("ascii", "replace"),
                         w=w, h=h, x=x, y=y, off=off, size=size))
    return version, ents


def decode(buf, e):
    px, pos, end = [], e["off"], e["off"] + e["size"]
    need = e["w"] * e["h"]
    while pos + 4 <= end and len(px) < need:
        c, r, g, b = buf[pos:pos + 4]; pos += 4
        px.extend([(r, g, b)] * c)
    px = (px + [(0, 0, 0)] * need)[:need]
    return [px[i * e["w"]:(i + 1) * e["w"]] for i in range(e["h"])]


def encode(rows):
    out = bytearray()
    flat = [p for row in rows for p in row]
    i = 0
    while i < len(flat):
        p, c = flat[i], 1
        while i + c < len(flat) and flat[i + c] == p and c < 255:
            c += 1
        out += bytes((c,) + p)
        i += c
    return bytes(out)


def build(version, ents, payloads):
    hdr = bytearray(0x400)
    hdr[:8] = MAGIC
    struct.pack_into("<II", hdr, 8, version, len(ents))
    off, body = 0x400, bytearray()
    for i, (e, data) in enumerate(zip(ents, payloads)):
        name = e["name"].encode()[:15]
        struct.pack_into("<16s6I", hdr, 0x18 + 40 * i, name, e["w"], e["h"], e["x"], e["y"], off, len(data))
        pad = (-len(data)) % 512
        body += data + b"\0" * pad
        off += len(data) + pad
    if 0x18 + 40 * len(ents) > 0x400:
        raise SystemExit("too many images for the header")
    return bytes(hdr) + bytes(body)


def main(a):
    if len(a) < 2 or a[0] not in ("list", "extract", "replace"):
        print(__doc__); return 2
    buf = open(a[1], "rb").read()
    version, ents = parse(buf)
    if a[0] == "list":
        for e in ents:
            print(f"{e['name']:<16} {e['w']:>4}x{e['h']:<4} at {e['x']:>3},{e['y']:<3} {e['size']:>8} bytes")
        print(f"(version {version}, {len(ents)} pictures, file {len(buf)} bytes)")
        return 0
    if a[0] == "extract":
        import os
        os.makedirs(a[2], exist_ok=True)
        for e in ents:
            png_write(f"{a[2]}/{e['name']}.png", e["w"], e["h"], decode(buf, e))
        print(f"{len(ents)} pictures -> {a[2]}/")
        return 0
    # replace
    out, name, pic = a[2], a[3], a[4]
    opts = set(a[5:])
    maxsize = len(buf)
    for i, o in enumerate(a[5:]):
        if o == "--max-size":
            maxsize = int(a[5 + i + 1])
    tgt = [e for e in ents if e["name"] == name]
    if not tgt:
        raise SystemExit(f"no picture named {name!r} (have: {', '.join(e['name'] for e in ents)})")
    e = tgt[0]
    w, h, rows = png_read(pic)
    if "--round" in opts:
        # crop to what is not (nearly) black
        ys = [y for y in range(h) if any(max(p) > 24 for p in rows[y])]
        xs = [x for x in range(w) if any(max(rows[y][x]) > 24 for y in ys)] if ys else []
        if xs:
            rows = [r[xs[0]:xs[-1] + 1] for r in rows[ys[0]:ys[-1] + 1]]
            w, h = xs[-1] - xs[0] + 1, ys[-1] - ys[0] + 1
    if "--full" in opts:
        bw, bh, bx, by = SCREEN, SCREEN, 0, 0
    else:
        bw, bh, bx, by = e["w"], e["h"], e["x"], e["y"]
    s = min(bw / w, bh / h)
    if "--round" in opts and "--full" in opts:
        # the whole picture inside a circle of radius 150 px around the centre
        s = min(s, 150 / ((w / 2) ** 2 + (h / 2) ** 2) ** 0.5)
    nw, nh = max(1, round(w * s)), max(1, round(h * s))
    small = resize(w, h, rows, nw, nh)
    canvas = [[(0, 0, 0)] * bw for _ in range(bh)]
    ox, oy = (bw - nw) // 2, (bh - nh) // 2
    for y in range(nh):
        canvas[oy + y][ox:ox + nw] = small[y]
    if "--flip-y" in opts:
        canvas.reverse()
    if "--flip-x" in opts:
        canvas = [list(reversed(r)) for r in canvas]
    payloads = []
    for x in ents:
        if x is e:
            x.update(w=bw, h=bh, x=bx, y=by)
            payloads.append(encode(canvas))
        else:
            payloads.append(buf[x["off"]:x["off"] + x["size"]])
    img = build(version, ents, payloads)
    if len(img) > maxsize:
        raise SystemExit(f"new imgdata is {len(img)} bytes, the partition holds {maxsize} - refusing")
    # pad to the old length so a dd writes over the whole old content
    img += b"\0" * (len(buf) - len(img)) if len(img) < len(buf) else b""
    open(out, "wb").write(img)
    png_write(out + ".preview.png", bw, bh, canvas)
    print(f"{name}: {bw}x{bh} at {bx},{by}, {len(payloads[ents.index(e)])} bytes of picture data")
    print(f"-> {out} ({len(img)} bytes), preview {out}.preview.png")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
