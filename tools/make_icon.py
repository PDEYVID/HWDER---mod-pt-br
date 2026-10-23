"""Build the exe icon (data/hwder.ico) from an image, default: the game's own icon in your dump.

    python tools/make_icon.py [image.png|.jpg]

Without an argument the icon is taken from the control data that nx_extract.py writes
(data/control_romfs_*/icon_AmericanEnglish.dat, a 256x256 JPEG). The .ico lives under data/
like every other file derived from the user's dump, so it is never committed; CMake embeds it
into hwder.exe when it exists. Needs Pillow (pip install pillow).
"""
import glob
import os
import sys

from PIL import Image

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "data", "hwder.ico")
SIZES = [(16, 16), (24, 24), (32, 32), (48, 48), (64, 64), (128, 128), (256, 256)]


def default_source():
    for name in ("icon_AmericanEnglish.dat", "icon_BritishEnglish.dat", "icon_*.dat"):
        hits = sorted(glob.glob(os.path.join(ROOT, "data", "control_romfs_*", name)))
        if hits:
            return hits[0]
    return None


def main():
    src = sys.argv[1] if len(sys.argv) > 1 else default_source()
    if not src or not os.path.exists(src):
        sys.exit("no image given and no game icon under data/control_romfs_* (run tools/nx_extract.py first)")
    im = Image.open(src).convert("RGBA")
    # Square crop around the centre, then the largest size; Pillow downsamples the rest.
    side = min(im.size)
    left, top = (im.width - side) // 2, (im.height - side) // 2
    im = im.crop((left, top, left + side, top + side)).resize((256, 256), Image.LANCZOS)
    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    im.save(OUT, format="ICO", sizes=SIZES)
    print(f"{OUT} <- {src}")


if __name__ == "__main__":
    main()
