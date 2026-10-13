"""Analyse the RGBA captures produced by the VRMPet prototype.

The question being answered: when the avatar is composited over the Windows
desktop using a stencil-derived alpha mask, do the alphaMode=BLEND facial
materials (eyelashes, brows, eye highlights) survive - or are they clipped,
because blended geometry never writes the stencil buffer?

What this reports:
  * alpha histogram - a stencil mask should be almost perfectly binary
    (0 or 255). Any spread in the middle means real soft coverage.
  * silhouette bounding box and coverage fraction
  * how the two captures differ, if both are present
  * composites over a checkerboard, so the result can be eyeballed
  * a magnified crop of the head, where the BLEND materials live

usage: analyze_mask.py <run-dir>
"""
import os
import sys

import numpy as np
from PIL import Image

CHECKER = 16


def checkerboard(size, a=(58, 58, 62), b=(92, 92, 98)):
    w, h = size
    yy, xx = np.mgrid[0:h, 0:w]
    mask = ((xx // CHECKER) + (yy // CHECKER)) % 2 == 0
    out = np.zeros((h, w, 3), np.uint8)
    out[mask] = a
    out[~mask] = b
    return out


def load_rgba(path):
    img = Image.open(path)
    if img.mode != "RGBA":
        img = img.convert("RGBA")
    return np.array(img)


def hr(title):
    print()
    print("=" * 64)
    print(title)
    print("=" * 64)


def describe(path, arr):
    hr(os.path.basename(path))
    alpha = arr[..., 3]
    total = alpha.size

    n_zero = int(np.count_nonzero(alpha == 0))
    n_full = int(np.count_nonzero(alpha == 255))
    n_mid = total - n_zero - n_full

    print(f"size           : {arr.shape[1]} x {arr.shape[0]}")
    print(f"alpha == 0     : {n_zero:>9,}  ({100.0 * n_zero / total:6.2f}%)")
    print(f"alpha == 255   : {n_full:>9,}  ({100.0 * n_full / total:6.2f}%)")
    print(f"0 < alpha < 255: {n_mid:>9,}  ({100.0 * n_mid / total:6.2f}%)")

    if n_mid == 0:
        print("\n  -> strictly binary. A stencil mask has hard edges by")
        print("     construction, so this is expected, and it means the")
        print("     silhouette will not be antialiased when composited.")
    else:
        print(f"\n  -> {n_mid:,} partially transparent pixels: real soft coverage.")

    ys, xs = np.nonzero(alpha > 0)
    if len(xs) == 0:
        print("\n  !! mask is completely empty")
        return None

    x0, x1 = int(xs.min()), int(xs.max())
    y0, y1 = int(ys.min()), int(ys.max())
    print(f"\nsilhouette bbox: x {x0}..{x1}  y {y0}..{y1}"
          f"  ({x1 - x0 + 1} x {y1 - y0 + 1})")
    print(f"coverage       : {100.0 * len(xs) / total:.2f}% of frame")
    return (x0, y0, x1, y1)


def composite(arr, out_path, label):
    rgb = arr[..., :3].astype(np.float32) / 255.0
    a = (arr[..., 3:4].astype(np.float32) / 255.0)
    bg = checkerboard((arr.shape[1], arr.shape[0])).astype(np.float32) / 255.0
    out = rgb * a + bg * (1.0 - a)
    Image.fromarray((out * 255.0).astype(np.uint8)).save(out_path)
    print(f"  wrote {out_path}   ({label})")


def crop_head(arr, bbox, out_path, zoom=4):
    if bbox is None:
        return
    x0, y0, x1, y1 = bbox
    h = y1 - y0 + 1
    # head = top 22% of the silhouette, plus a margin
    head_h = max(1, int(h * 0.22))
    pad = max(8, int(h * 0.03))
    cy0 = max(0, y0 - pad)
    cy1 = min(arr.shape[0], y0 + head_h + pad)
    cx0 = max(0, x0 - pad)
    cx1 = min(arr.shape[1], x1 + pad)

    sub = arr[cy0:cy1, cx0:cx1]
    rgb = sub[..., :3].astype(np.float32) / 255.0
    a = sub[..., 3:4].astype(np.float32) / 255.0
    # Composite over magenta: anything the mask does not cover screams.
    flat = (rgb * a + (1.0 - a) * np.array([1.0, 0.1, 0.6])) * 255.0
    flat = np.clip(flat, 0, 255).astype(np.uint8)

    im = Image.fromarray(flat).resize(
        (flat.shape[1] * zoom, flat.shape[0] * zoom), Image.NEAREST)
    im.save(out_path)
    print(f"  wrote {out_path}   (head crop x{zoom}, uncomposited area shown magenta)")


def main():
    run_dir = sys.argv[1] if len(sys.argv) > 1 else "."
    files = [f for f in ("mask_default.png", "mask_user.png")
             if os.path.exists(os.path.join(run_dir, f))]
    if not files:
        raise SystemExit(f"no mask_*.png found in {run_dir}")

    loaded = {}
    bboxes = {}
    for f in files:
        p = os.path.join(run_dir, f)
        arr = load_rgba(p)
        loaded[f] = arr
        bboxes[f] = describe(p, arr)

    hr("composites")
    for f, arr in loaded.items():
        composite(arr, os.path.join(run_dir, f.replace(".png", "_over_checker.png")), f)
        crop_head(arr, bboxes[f],
                  os.path.join(run_dir, f.replace(".png", "_head.png")))

    if len(loaded) == 2:
        a = loaded["mask_default.png"][..., 3]
        b = loaded["mask_user.png"][..., 3]
        hr("mask_default vs mask_user")
        diff = int(np.count_nonzero(a != b))
        print(f"pixels differing: {diff:,}  ({100.0 * diff / a.size:.4f}% of frame)")
        if diff == 0:
            print("  -> identical. Tagging with a user stencil ref changes nothing;")
            print("     the default background heuristic already captures every")
            print("     piece of geometry that writes the stencil buffer.")
        else:
            only_a = int(np.count_nonzero((a > 0) & (b == 0)))
            only_b = int(np.count_nonzero((b > 0) & (a == 0)))
            print(f"  in default only: {only_a:,}")
            print(f"  in user only   : {only_b:,}")


if __name__ == "__main__":
    main()
