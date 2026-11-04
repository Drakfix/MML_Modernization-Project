"""Builds the app icons from the artwork in art/icon (needs Pillow):

    python tools/icons/make_icons.py

- art/mml.ico for Windows (the launcher, the installer, the standalone game): the hand-made
  16, 32 and 256 px images, with 48, 64 and 128 px scaled down from the 256 one.
- android/res/mipmap-*/ic_launcher_foreground.png for Android: the adaptive icon's foreground
  at each screen density (108 dp: 108 to 432 px). Launchers mask an adaptive icon to its central
  72 dp (a circle, a squircle, a rounded square), so the artwork is scaled until its farthest
  opaque pixel lies within the 66 dp safe circle; the background layer is a vector
  (android/res/drawable/ic_launcher_background.xml).
"""
import io
import math
import os

from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
ART = os.path.join(REPO, "art", "icon")
RES = os.path.join(REPO, "android", "res")

DENSITIES = {"mdpi": 108, "hdpi": 162, "xhdpi": 216, "xxhdpi": 324, "xxxhdpi": 432}
SAFE_RADIUS = 33.0 / 108.0   # of the layer's width: the 66 dp safe circle


def windows_ico():
    big = Image.open(os.path.join(ART, "windows_256.png")).convert("RGBA")
    own = {16: Image.open(os.path.join(ART, "windows_16.png")).convert("RGBA"),
           32: Image.open(os.path.join(ART, "windows_32.png")).convert("RGBA"),
           256: big}
    sizes = [16, 32, 48, 64, 128, 256]
    images = [own.get(n) or big.resize((n, n), Image.LANCZOS) for n in sizes]
    # (Pillow keeps each given image for its size)
    out = os.path.join(REPO, "art", "mml.ico")
    images[-1].save(out, format="ICO", sizes=[(n, n) for n in sizes], append_images=images[:-1])
    check = Image.open(out)
    for n in (16, 32):
        check.size = (n, n)
        check.load()
        assert check.convert("RGBA").tobytes() == own[n].tobytes(), "the %d px image changed" % n
    print("wrote", out, sizes)


def farthest(img):
    """the distance of the farthest opaque pixel from the centre, in pixels"""
    a = img.getchannel("A")
    w, h = img.size
    cx, cy = (w - 1) / 2.0, (h - 1) / 2.0
    px = a.load()
    best = 0.0
    for y in range(h):
        for x in range(w):
            if px[x, y] > 16:
                best = max(best, math.hypot(x - cx, y - cy))
    return best


def android_foreground():
    art = Image.open(os.path.join(ART, "android_432.png")).convert("RGBA")
    scale = SAFE_RADIUS * art.size[0] / farthest(art)   # of the artwork, to fit the safe circle
    for name, size in DENSITIES.items():
        n = max(1, round(art.size[0] * scale * size / art.size[0]))
        layer = Image.new("RGBA", (size, size), (0, 0, 0, 0))
        small = art.resize((n, n), Image.LANCZOS)
        layer.alpha_composite(small, ((size - n) // 2, (size - n) // 2))
        d = os.path.join(RES, "mipmap-" + name)
        os.makedirs(d, exist_ok=True)
        layer.save(os.path.join(d, "ic_launcher_foreground.png"), optimize=True)
    print("wrote the Android foreground at", ", ".join("%s %d" % kv for kv in DENSITIES.items()),
          "(artwork at %.0f%% of the layer)" % (scale * 100))


if __name__ == "__main__":
    windows_ico()
    android_foreground()
