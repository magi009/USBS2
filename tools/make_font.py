# Genera data/font.bin: atlas de glifos (alfa de 8 bits) de Inter en varios tamaños.
# Formato (little endian):
#   u32 'FNT1', u32 n_sizes
#   por tamaño: u16 px, u16 ascent, u16 line_h, u16 count, luego count x
#     {u32 cp; s16 xoff; s16 yoff; u16 w; u16 h; u16 adv; u16 pad; u32 off}
#   después, todos los alfas seguidos (off es relativo al inicio del archivo).
import struct, sys
from PIL import Image, ImageDraw, ImageFont
D = "/usr/share/fonts/opentype/inter/"
SIZES = [(18, "Inter-Medium.otf"), (22, "Inter-SemiBold.otf"), (30, "Inter-Bold.otf"), (56, "InterDisplay-Black.otf")]
CHARS = [chr(c) for c in range(32, 127)] + list("áéíóúÁÉÍÓÚñÑüÜ¿¡·…–→←")
blocks, blobs = [], []
for px, fname in SIZES:
    f = ImageFont.truetype(D + fname, px)
    asc, desc = f.getmetrics()
    glyphs = []
    for ch in CHARS:
        bb = f.getbbox(ch)  # relative to origin at top (ascender)
        adv = int(round(f.getlength(ch)))
        w, h = max(0, bb[2] - bb[0]), max(0, bb[3] - bb[1])
        alpha = b""
        if w and h:
            im = Image.new("L", (w, h), 0)
            ImageDraw.Draw(im).text((-bb[0], -bb[1]), ch, font=f, fill=255)
            alpha = im.tobytes()
        glyphs.append([ord(ch), bb[0], bb[1], w, h, adv, alpha])
    blocks.append((px, asc, asc + desc, glyphs))
hdr = struct.pack("<4sI", b"FNT1", len(blocks))
size_tables = sum(8 + 20 * len(b[3]) for b in blocks)
off = len(hdr) + size_tables
out_tables = b""; out_alpha = b""
for px, asc, lh, glyphs in blocks:
    out_tables += struct.pack("<HHHH", px, asc, lh, len(glyphs))
    for cp, xo, yo, w, h, adv, a in glyphs:
        out_tables += struct.pack("<IhhHHHHI", cp, xo, yo, w, h, adv, 0, off + len(out_alpha))
        out_alpha += a
open(sys.argv[1], "wb").write(hdr + out_tables + out_alpha)
print("bytes", len(hdr + out_tables + out_alpha))
