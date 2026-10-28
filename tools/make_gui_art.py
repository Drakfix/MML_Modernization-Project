"""Builds the launcher's background (art/gui_bg.png) from the cover art
(art/gui_art_round.png): the art blurred and tinted to the game's HUD navy, a
faint hex grid like the title screen's, scanlines and a soft blue glow behind
the title. Needs Pillow. Run it again after changing the art or the look.

    python tools/make_gui_art.py
"""
import math
import os

from PIL import Image, ImageChops, ImageDraw, ImageFilter

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ART = os.path.join(ROOT, "art")
W, H = 1010, 930
NAVY = (8, 16, 30)


def blurred_art():
    art = Image.open(os.path.join(ART, "gui_art_round.png")).convert("RGB")
    scale = max(W / art.width, H / art.height) * 1.15
    art = art.resize((int(art.width * scale), int(art.height * scale)), Image.LANCZOS)
    x, y = (art.width - W) // 2, (art.height - H) // 3
    art = art.crop((x, y, x + W, y + H)).filter(ImageFilter.GaussianBlur(38))
    # keep the shapes, lose the colours: luminance into the navy-to-blue ramp
    lum = art.convert("L").point(lambda v: int(v * 0.30))
    lo, hi = Image.new("RGB", (W, H), NAVY), Image.new("RGB", (W, H), (34, 70, 130))
    return Image.composite(hi, lo, lum)


def hex_grid(size=26):
    """Faint cyan hex outlines, strongest bottom right, gone towards the top left."""
    grid = Image.new("L", (W, H), 0)
    d = ImageDraw.Draw(grid)
    hw = size * math.sqrt(3)
    row = 0
    y = -size
    while y < H + size:
        x = -hw + (hw / 2 if row % 2 else 0)
        while x < W + hw:
            pts = [(x + size * math.cos(math.radians(a)), y + size * math.sin(math.radians(a)))
                   for a in range(30, 390, 60)]
            d.line(pts, fill=255, width=1)
            x += hw
        y += size * 1.5
        row += 1
    fade = Image.linear_gradient("L").rotate(45, expand=True).resize((W, H))
    fade = fade.point(lambda v: int(max(0, v - 70) * 0.11))
    return ImageChops.multiply(grid, fade)


def glow():
    g = Image.new("L", (W, H), 0)
    ImageDraw.Draw(g).ellipse((520, -170, 1120, 230), fill=70)
    return g.filter(ImageFilter.GaussianBlur(90))


def main():
    bg = blurred_art()
    bg = Image.composite(Image.new("RGB", (W, H), (92, 200, 255)), bg, hex_grid())
    bg = Image.composite(Image.new("RGB", (W, H), (40, 100, 220)), bg, glow())
    # scanlines: every third row a little darker
    lines = Image.new("L", (W, H), 255)
    d = ImageDraw.Draw(lines)
    for y in range(0, H, 3):
        d.line((0, y, W, y), fill=236)
    bg = ImageChops.multiply(bg, Image.merge("RGB", (lines, lines, lines)))
    # vignette
    v = Image.new("L", (W, H), 0)
    ImageDraw.Draw(v).ellipse((-260, -220, W + 260, H + 260), fill=255)
    v = v.filter(ImageFilter.GaussianBlur(120))
    bg = Image.composite(bg, Image.new("RGB", (W, H), (4, 8, 15)), v)
    out = os.path.join(ART, "gui_bg.png")
    bg.save(out, optimize=True)
    print("wrote", out)


if __name__ == "__main__":
    main()
