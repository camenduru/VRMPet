"""Crop the overlay's window region out of a full-desktop screenshot.

The overlay is a layered, click-through, topmost window with no taskbar button,
so the only way to inspect it is to screenshot the desktop and cut the region
the engine reported in the log ("overlay window WxH at X,Y").

usage: crop_overlay.py <desktop.png> <out.png> <x> <y> <w> <h> [pad] [head-only]
"""
import sys

from PIL import Image


def main():
    path, out, x, y, w, h = sys.argv[1:7]
    x, y, w, h = int(x), int(y), int(w), int(h)
    pad = int(sys.argv[7]) if len(sys.argv) > 7 else 20
    head_only = len(sys.argv) > 8 and sys.argv[8] == "head"

    im = Image.open(path)
    bottom = y + (int(h * 0.34) if head_only else h)
    crop = im.crop((x - pad, y - pad, x + w + pad, bottom + pad))

    zoom = 3 if head_only else 1
    if zoom > 1:
        crop = crop.resize((crop.width * zoom, crop.height * zoom), Image.LANCZOS)

    crop.save(out)
    print(f"{out}  {crop.size}  (from {path}, window {w}x{h} at {x},{y})")


if __name__ == "__main__":
    main()
