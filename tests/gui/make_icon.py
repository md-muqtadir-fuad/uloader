"""Generate src/gui.ico: a white down arrow on a blue rounded square.

Usage: python -I make_icon.py OUT.ico [preview.png]
Sizes 16..64 are stored as 32-bit DIBs (with AND mask), 256 as PNG.
"""
import io
import struct
import sys

from PIL import Image, ImageDraw

SIZES = [16, 20, 24, 32, 40, 48, 64, 256]
SS = 4  # supersampling factor for the 256 design


def design(px):
    """Render the 256-unit design at px x px (px = 256 * k)."""
    k = px / 256.0
    img = Image.new("RGBA", (px, px), (0, 0, 0, 0))
    # vertical gradient, clipped by a rounded-square mask
    grad = Image.new("RGBA", (px, px))
    top, bot = (0x3A, 0x96, 0xF0), (0x0A, 0x58, 0xC2)
    gd = ImageDraw.Draw(grad)
    for y in range(px):
        t = y / (px - 1)
        c = tuple(int(round(top[i] + (bot[i] - top[i]) * t)) for i in range(3))
        gd.line([(0, y), (px, y)], fill=c + (255,))
    mask = Image.new("L", (px, px), 0)
    ImageDraw.Draw(mask).rounded_rectangle([8 * k, 8 * k, 248 * k - 1, 248 * k - 1], radius=50 * k, fill=255)
    img.paste(grad, (0, 0), mask)

    d = ImageDraw.Draw(img)
    white = (255, 255, 255, 255)
    # shaft
    d.rectangle([104 * k, 46 * k, 152 * k - 1, 128 * k], fill=white)
    # head
    d.polygon([(56 * k, 112 * k), (200 * k, 112 * k), (128 * k, 186 * k)], fill=white)
    # tray bar
    d.rounded_rectangle([58 * k, 202 * k, 198 * k - 1, 230 * k - 1], radius=14 * k, fill=white)
    return img


def render(size):
    big = design(256 * SS)
    return big.resize((size, size), Image.LANCZOS)


def dib_entry(img):
    s = img.size[0]
    px = img.load()
    hdr = struct.pack("<IiiHHIIiiII", 40, s, s * 2, 1, 32, 0, 0, 0, 0, 0, 0)
    xor = bytearray()
    for y in range(s - 1, -1, -1):
        for x in range(s):
            r, g, b, a = px[x, y]
            xor += bytes((b, g, r, a))
    row = ((s + 31) // 32) * 4
    andm = bytearray()
    for y in range(s - 1, -1, -1):
        bits = bytearray(row)
        for x in range(s):
            if px[x, y][3] == 0:
                bits[x // 8] |= 0x80 >> (x % 8)
        andm += bits
    return hdr + bytes(xor) + bytes(andm)


def png_entry(img):
    b = io.BytesIO()
    img.save(b, "PNG", optimize=True)
    return b.getvalue()


def main():
    out = sys.argv[1]
    images = [(s, render(s)) for s in SIZES]
    blobs = [png_entry(im) if s >= 256 else dib_entry(im) for s, im in images]
    n = len(images)
    head = struct.pack("<HHH", 0, 1, n)
    offset = 6 + 16 * n
    dirents = b""
    for (s, _), blob in zip(images, blobs):
        dim = 0 if s >= 256 else s
        dirents += struct.pack("<BBBBHHII", dim, dim, 0, 0, 1, 32, len(blob), offset)
        offset += len(blob)
    with open(out, "wb") as f:
        f.write(head + dirents + b"".join(blobs))
    if len(sys.argv) > 2:
        sheet = Image.new("RGBA", (sum(s for s in SIZES) + 10 * len(SIZES), 256), (255, 255, 255, 255))
        x = 0
        for s, im in images:
            sheet.paste(im, (x, 0), im)
            x += s + 10
        sheet.save(sys.argv[2])


main()
