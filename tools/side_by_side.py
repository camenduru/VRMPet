"""Put the animated and non-animated captures side by side for eyeballing.

The vertical extent is the tell: a correct humanoid retarget keeps the feet on
the ground and the head at the same height, and only changes the pose. A broken
one (wrong scale, wrong bone mapping) shows up as a squashed or gigantic body.
"""
import sys

import numpy as np
from PIL import Image


def load(path):
    img = Image.open(path)
    if img.mode != "RGBA":
        img = img.convert("RGBA")
    return np.array(img)


def over_checker(arr, size=16, a=(58, 58, 62), b=(92, 92, 98)):
    h, w = arr.shape[:2]
    yy, xx = np.mgrid[0:h, 0:w]
    m = ((xx // size) + (yy // size)) % 2 == 0
    bg = np.zeros((h, w, 3), np.uint8)
    bg[m] = a
    bg[~m] = b
    rgb = arr[..., :3].astype(np.float32)
    alpha = arr[..., 3:4].astype(np.float32) / 255.0
    return (rgb * alpha + bg.astype(np.float32) * (1.0 - alpha)).astype(np.uint8)


def label_strip(w, text_h=22):
    return np.full((text_h, w, 3), 24, np.uint8)


def main(out_path, pairs):
    """pairs: list of (png, caption)"""
    tiles = []
    for path, _ in pairs:
        tiles.append(over_checker(load(path)))

    gap = 8
    h = max(t.shape[0] for t in tiles)
    w = sum(t.shape[1] for t in tiles) + gap * (len(tiles) - 1)
    canvas = np.full((h, w, 3), 24, np.uint8)

    x = 0
    for t in tiles:
        canvas[:t.shape[0], x:x + t.shape[1]] = t
        x += t.shape[1] + gap

    Image.fromarray(canvas).save(out_path)
    print(f"wrote {out_path}  ({w}x{h})")
    for _, cap in pairs:
        print(f"  tile: {cap}")


if __name__ == "__main__":
    out = sys.argv[1]
    args = sys.argv[2:]
    main(out, [(args[i], args[i + 1]) for i in range(0, len(args), 2)])
