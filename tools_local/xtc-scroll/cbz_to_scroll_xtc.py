#!/usr/bin/env python3
"""Convert a CBZ manga volume into an XTC of uncut, upright 1-bit pages.

    python cbz_to_scroll_xtc.py IN.cbz OUT.xtc [--width 800] [--height 1200]
                                [--dither atkinson|floyd] [--title ...]
    python cbz_to_scroll_xtc.py --verify OUT.xtc [--width 800] [--height 1200]

Made for the continuous-scroll reader on the 800x480 landscape screen: every
page is fitted to the full width, never split or rotated, and padded with
white (centred vertically) to exactly width x height, because the firmware
sizes its page buffer from page 1. Wide spreads follow the same rule.

Layout matches lib/Xtc/Xtc/XtcTypes.h and what XtcParser.cpp accepts:
56-byte header, 192-byte metadata block (title[128] + author[64]), page table
of 16-byte entries, then one XTG blob (22-byte header + bitmap) per page.
The page-table dataSize covers the whole blob, header included.
"""

import argparse
import io
import multiprocessing
import struct
import sys
import time
import zipfile

from PIL import Image

XTC_MAGIC = 0x00435458
XTG_MAGIC = 0x00475458
HEADER = struct.Struct("<IBBHBBBBIQQQQII")  # 56 bytes
ENTRY = struct.Struct("<QIHH")  # 16 bytes
XTG_HEADER = struct.Struct("<IHHBBIQ")  # 22 bytes
META_SIZE = 192  # title[128] + author[64], as readTitle()/readAuthor() read it
IMAGE_EXTS = (".png", ".jpg", ".jpeg", ".webp", ".gif", ".bmp")


def atkinson(gray):
    """Atkinson-dither an L image to mode '1'."""
    # shortcut: pure-Python error diffusion (~1-2 s/page); pages run in parallel. Use numba if it ever matters.
    w, h = gray.size
    stride = w + 3  # 1 px left pad, 2 px right pad so neighbours never wrap
    buf = [0] * (stride * (h + 2))
    src = gray.tobytes()
    for y in range(h):
        buf[y * stride + 1 : y * stride + 1 + w] = src[y * w : (y + 1) * w]
    out = bytearray(w * h)
    for y in range(h):
        row = y * stride + 1
        o = y * w
        for x in range(w):
            i = row + x
            old = buf[i]
            if old >= 128:
                out[o + x] = 255
                err = (old - 255) / 8
            else:
                err = old / 8
            if err:
                buf[i + 1] += err
                buf[i + 2] += err
                buf[i + stride - 1] += err
                buf[i + stride] += err
                buf[i + stride + 1] += err
                buf[i + 2 * stride] += err
    return Image.frombytes("L", (w, h), bytes(out)).convert("1", dither=Image.Dither.NONE)


def render_page(args):
    data, width, height, dither = args
    img = Image.open(io.BytesIO(data)).convert("L")
    w, h = width, max(1, round(img.height * width / img.width))
    if h > height:
        w, h = max(1, round(img.width * height / img.height)), height
    img = img.resize((w, h), Image.Resampling.LANCZOS)
    page = Image.new("L", (width, height), 255)
    page.paste(img, ((width - w) // 2, (height - h) // 2))
    bw = atkinson(page) if dither == "atkinson" else page.convert("1")
    # Pillow mode '1' bytes are row-major, MSB first, 1 = white: exactly the XTG bitmap.
    return bw.tobytes()


def write_xtc(out_path, bitmaps, width, height, title):
    n = len(bitmaps)
    bmp_size = (width + 7) // 8 * height
    blob = XTG_HEADER.size + bmp_size
    meta_off = HEADER.size
    table_off = meta_off + META_SIZE
    data_off = table_off + n * ENTRY.size
    title_bytes = title.encode("utf-8")[:127]
    with open(out_path, "wb") as f:
        f.write(HEADER.pack(XTC_MAGIC, 1, 0, n, 0, 1, 0, 0, 0, meta_off, table_off, data_off, 0, 0, 0))
        f.write(title_bytes.ljust(128, b"\0") + b"\0" * 64)
        for i in range(n):
            f.write(ENTRY.pack(data_off + i * blob, blob, width, height))
        for bmp in bitmaps:
            assert len(bmp) == bmp_size
            f.write(XTG_HEADER.pack(XTG_MAGIC, width, height, 0, 0, bmp_size, 0))
            f.write(bmp)


def verify(path, width, height):
    """Re-read an XTC and check it the way XtcParser does. Returns page count."""
    with open(path, "rb") as f:
        data = f.read()
    (magic, vmaj, vmin, n, _rd, has_meta, _th, _ch, _cur, meta_off, table_off, data_off, _thumb, _co, _pad) = (
        HEADER.unpack_from(data, 0)
    )
    assert magic == XTC_MAGIC, f"bad magic {magic:#x}"
    assert (vmaj, vmin) == (1, 0), f"bad version {vmaj}.{vmin}"
    assert n > 0, "no pages"
    if has_meta:
        assert HEADER.size <= meta_off and meta_off + META_SIZE <= len(data), "metadata out of bounds"
    assert table_off >= HEADER.size and table_off + n * ENTRY.size <= len(data), "page table out of bounds"
    bmp_size = (width + 7) // 8 * height
    expected_end = data_off
    for i in range(n):
        off, size, w, h = ENTRY.unpack_from(data, table_off + i * ENTRY.size)
        assert (w, h) == (width, height), f"page {i}: table size {w}x{h}"
        assert off == expected_end, f"page {i}: offset {off} != {expected_end}"
        assert size == XTG_HEADER.size + bmp_size, f"page {i}: table dataSize {size}"
        pm, pw, ph, mode, comp, psize, _md5 = XTG_HEADER.unpack_from(data, off)
        assert pm == XTG_MAGIC, f"page {i}: bad XTG magic {pm:#x}"
        assert (pw, ph, mode, comp, psize) == (width, height, 0, 0, bmp_size), f"page {i}: bad XTG header"
        expected_end = off + size
    assert expected_end == len(data), f"file size {len(data)} != {expected_end}"
    return n


def read_page(path, index):
    """Decode one page back to a mode '1' image (for eyeballing)."""
    with open(path, "rb") as f:
        data = f.read()
    table_off = HEADER.unpack_from(data, 0)[10]
    off, _size, w, h = ENTRY.unpack_from(data, table_off + index * ENTRY.size)
    start = off + XTG_HEADER.size
    return Image.frombytes("1", (w, h), data[start : start + (w + 7) // 8 * h])


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("paths", nargs="+", help="IN.cbz OUT.xtc, or just OUT.xtc with --verify")
    ap.add_argument("--width", type=int, default=800)
    ap.add_argument("--height", type=int, default=1200)
    ap.add_argument("--dither", choices=("atkinson", "floyd"), default="atkinson")
    ap.add_argument("--title", help="defaults to the CBZ file name")
    ap.add_argument("--verify", action="store_true", help="only check an existing XTC")
    args = ap.parse_args()

    if args.verify:
        print(f"OK: {verify(args.paths[0], args.width, args.height)} pages")
        return
    if len(args.paths) != 2:
        ap.error("need IN.cbz OUT.xtc")
    in_path, out_path = args.paths

    start = time.time()
    with zipfile.ZipFile(in_path) as z:
        names = sorted(
            (n for n in z.namelist() if n.lower().endswith(IMAGE_EXTS) and not n.startswith("__MACOSX/")),
            key=str.lower,
        )
        if not names:
            sys.exit("no images in " + in_path)
        if len(names) > 0xFFFF:
            sys.exit("too many pages for XTC")
        jobs = [(z.read(n), args.width, args.height, args.dither) for n in names]
    with multiprocessing.Pool() as pool:
        bitmaps = []
        for i, bmp in enumerate(pool.imap(render_page, jobs)):
            bitmaps.append(bmp)
            print(f"\r{i + 1}/{len(jobs)}", end="", flush=True)
    print()
    title = args.title or in_path.replace("\\", "/").rsplit("/", 1)[-1].rsplit(".", 1)[0]
    write_xtc(out_path, bitmaps, args.width, args.height, title)
    n = verify(out_path, args.width, args.height)
    print(f"wrote {out_path}: {n} pages, {args.width}x{args.height}, {time.time() - start:.1f}s")


if __name__ == "__main__":
    main()
