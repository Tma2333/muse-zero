"""Package the Jolly blink/bob frames as a Flipper SD animation.

Follows the firmware's own converter (scripts/flipper/assets/icon.py):
PNG -> 1-bit -> invert -> XBM bytes; stored uncompressed as b"\\x00" + raw
(the format animation_storage.c loads from /ext/dolphin/<name>/frame_N.bm).
Round-trip-decodes every .bm and asserts it matches the source frame.
"""
from PIL import Image, ImageOps
import io
import os

SRC_DIR = "/home/hatch/workspace/flipper-gadget/jolly-art/animation"
OUT_DIR = "/home/hatch/workspace/flipper-gadget/jolly-art/package/Jolly"
os.makedirs(OUT_DIR, exist_ok=True)

FRAME_COUNT = 6


def png_to_bm(png_path):
    with Image.open(png_path) as im:
        bw = ImageOps.invert(im.convert("1"))
        with io.BytesIO() as buf:
            bw.save(buf, format="XBM")
            xbm = buf.getvalue()
    text = xbm.decode().strip()
    lines = text.splitlines()
    width = int(lines[0].split()[2])
    height = int(lines[1].split()[2])
    hexpart = "".join(lines[2:]).split("=")[1].rstrip("; }").strip().strip("{}")
    data_bin = bytes(int(h, 16) for h in hexpart.replace(",", " ").split())
    assert (width, height) == (128, 64), (width, height)
    assert len(data_bin) == width * height // 8, len(data_bin)
    return b"\x00" + data_bin


def bm_ink(bm):
    """Decode an uncompressed .bm back to a boolean ink mask."""
    raw = bm[1:]
    ink = [[False] * 128 for _ in range(64)]
    for y in range(64):
        for xb in range(16):
            byte = raw[y * 16 + xb]
            for bit in range(8):
                if byte & (1 << bit):
                    ink[y][xb * 8 + bit] = True
    return ink


for i in range(FRAME_COUNT):
    src = os.path.join(SRC_DIR, f"frame_{i}.png")
    bm = png_to_bm(src)
    dst = os.path.join(OUT_DIR, f"frame_{i}.bm")
    with open(dst, "wb") as f:
        f.write(bm)
    # round-trip check against the source ink
    src_ink = Image.open(src).convert("L").point(lambda p: p < 128)
    dec = bm_ink(bm)
    mismatch = sum(
        1 for y in range(64) for x in range(128)
        if bool(src_ink.getpixel((x, y))) != dec[y][x])
    assert mismatch == 0, f"frame {i}: {mismatch} mismatched pixels"
    print(f"frame_{i}.bm: {len(bm)} bytes, round-trip OK")

meta = """Filetype: Flipper Animation
Version: 1

Width: 128
Height: 64
Passive frames: 6
Active frames: 0
Frames order: 0 1 2 3 4 5
Active cycles: 0
Frame rate: 3
Duration: 3600
Active cooldown: 0

Bubble slots: 0
"""
with open(os.path.join(OUT_DIR, "meta.txt"), "w", newline="\n") as f:
    f.write(meta)
print("meta.txt written ->", OUT_DIR)
