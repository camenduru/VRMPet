"""Answer the prototype question quantitatively.

The question: when a VRM avatar is composited over the Windows desktop using a
stencil-derived alpha mask, do the alphaMode=BLEND facial materials (eyelashes,
brows, eye iris, eye highlights) survive - or are they clipped off, because
blended geometry never writes the stencil buffer?

Method
------
Three captures from the same camera pose, all produced by the same code path
(RenderPath3D::CreateScreenshotWithAlphaBackground with STENCILMODE_ALWAYS, so
every pixel passes and the result is the full post-processed frame):

    mask_nostencil.png   the scene WITH the avatar
    background.png       the scene with every object hidden
    mask_default.png     the stencil mask (alpha = 255 where stencil != 0)

Differencing the first two recovers the avatar's TRUE screen-space footprint,
with no reference to the stencil buffer at all. Comparing that footprint against
mask_default.png then measures exactly what the mask gets right and wrong.

    missed       avatar pixels the mask drops   -> holes punched in the desktop
    over_covered mask pixels with no avatar     -> background smeared onto desktop

A per-material breakdown is not possible from images alone, but the missed
pixels are localised, so they are reported by screen region. The head band is
where the BLEND materials live, so it is the region that matters.

usage: compare_mask.py <run-dir>
"""

import os
import sys

import numpy as np
from PIL import Image

# A pixel counts as "avatar" when the colour changes by more than this much
# (0..255 per channel, max over channels) once the avatar is shown. The render is
# deterministic and both frames come from the identical pipeline, so anything
# above the noise floor is real geometry. The background is a flat sky, so the
# floor is very low.
AVATAR_EPS = 6


def load_rgba(path):
    img = Image.open(path)
    if img.mode != "RGBA":
        img = img.convert("RGBA")
    return np.array(img)


def hr(title):
    print()
    print("=" * 68)
    print(title)
    print("=" * 68)


def band_name(y, h):
    frac = y / h
    if frac < 0.22:
        return "head/face band"
    if frac < 0.50:
        return "torso band"
    if frac < 0.80:
        return "legs band"
    return "feet band"


def main():
    run_dir = sys.argv[1] if len(sys.argv) > 1 else "."

    need = ("mask_nostencil.png", "background.png", "mask_default.png")
    missing = [f for f in need if not os.path.exists(os.path.join(run_dir, f))]
    if missing:
        raise SystemExit(f"missing {missing} in {run_dir}\n"
                         f"run the prototype with the background stage enabled")

    full = load_rgba(os.path.join(run_dir, "mask_nostencil.png"))
    bg = load_rgba(os.path.join(run_dir, "background.png"))
    mask = load_rgba(os.path.join(run_dir, "mask_default.png"))

    if full.shape != bg.shape or full.shape != mask.shape:
        raise SystemExit(f"size mismatch: full={full.shape} bg={bg.shape} "
                         f"mask={mask.shape}")

    h, w = full.shape[:2]

    # --- ground truth footprint, from colour alone -------------------------
    delta = np.abs(full[..., :3].astype(np.int16) - bg[..., :3].astype(np.int16))
    avatar = delta.max(axis=2) > AVATAR_EPS

    # --- what the stencil mask claims -------------------------------------
    covered = mask[..., 3] > 0

    n_avatar = int(avatar.sum())
    n_covered = int(covered.sum())
    missed = avatar & ~covered
    extra = covered & ~avatar
    both = avatar & covered
    n_missed = int(missed.sum())
    n_extra = int(extra.sum())

    hr("coverage")
    print(f"frame                       : {w} x {h}")
    print(f"avatar footprint (ground truth): {n_avatar:>8,}  ({100.0 * n_avatar / avatar.size:6.3f}%)")
    print(f"stencil mask coverage          : {n_covered:>8,}  ({100.0 * n_covered / avatar.size:6.3f}%)")
    print(f"agreement (in both)            : {int(both.sum()):>8,}")
    print()
    if n_avatar == 0:
        print("!! the avatar footprint is empty - the scene did not render")
        return
    recall = 100.0 * int(both.sum()) / n_avatar
    precision = 100.0 * int(both.sum()) / max(1, n_covered)
    print(f"recall    (avatar kept by mask) : {recall:6.2f}%")
    print(f"precision (mask that is avatar) : {precision:6.2f}%")

    hr("the two failure modes")
    print(f"missed       avatar dropped by mask : {n_missed:>8,}"
          f"  ({100.0 * n_missed / max(1, n_avatar):6.3f}% of the avatar)")
    print(f"over_covered mask with no avatar    : {n_extra:>8,}"
          f"  ({100.0 * n_extra / max(1, n_covered):6.3f}% of the mask)")
    print()
    print("  missed       -> transparent holes in the avatar: the desktop shows")
    print("                  through where eyelashes/brows/highlights should be")
    print("  over_covered -> desktop replaced by background where the avatar is not")

    # --- where are the missed pixels? --------------------------------------
    if n_missed:
        hr("where the mask misses (by vertical band)")
        print(f"{'band':<18}{'avatar px':>12}{'missed px':>12}{'missed %':>11}")
        edges = [0, int(h * 0.22), int(h * 0.50), int(h * 0.80), h]
        for i in range(4):
            y0, y1 = edges[i], edges[i + 1]
            a = int(avatar[y0:y1].sum())
            m = int(missed[y0:y1].sum())
            pct = 100.0 * m / a if a else 0.0
            print(f"{band_name(y0, h):<18}{a:>12,}{m:>12,}{pct:>10.2f}%")
        ys, xs = np.nonzero(missed)
        print()
        print(f"missed bbox: x {int(xs.min())}..{int(xs.max())}"
              f"  y {int(ys.min())}..{int(ys.max())}")

    # --- visual -----------------------------------------------------------
    hr("visual")
    # grey  = avatar and mask agree
    # red   = avatar but no mask   (the failure that matters)
    # blue  = mask but no avatar
    vis = np.zeros((h, w, 3), np.uint8)
    vis[both] = (150, 150, 150)
    vis[missed] = (255, 40, 40)
    vis[extra] = (40, 120, 255)
    vis[~avatar & ~covered] = (24, 24, 28)
    Image.fromarray(vis).save(os.path.join(run_dir, "diff_full.png"))
    print("  wrote diff_full.png    grey=agree  red=avatar missed  blue=mask only")

    if n_avatar:
        ys, xs = np.nonzero(avatar)
        y0, y1 = int(ys.min()), int(ys.max())
        x0, x1 = int(xs.min()), int(xs.max())
        pad = 12
        cy0, cy1 = max(0, y0 - pad), min(h, y0 + int((y1 - y0) * 0.24) + pad)
        cx0, cx1 = max(0, x0 - pad), min(w, x1 + pad)
        Image.fromarray(vis[cy0:cy1, cx0:cx1]).resize(
            ((cx1 - cx0) * 3, (cy1 - cy0) * 3), Image.NEAREST
        ).save(os.path.join(run_dir, "diff_head.png"))
        print("  wrote diff_head.png    head band, x3")

        # Side-by-side: what the composite looks like vs what it should look like.
        rgb = full[cy0:cy1, cx0:cx1, :3].astype(np.float32)
        a = (mask[cy0:cy1, cx0:cx1, 3:4].astype(np.float32)) / 255.0
        checker = np.zeros_like(rgb)
        yy, xx = np.mgrid[cy0:cy1, cx0:cx1]
        c = ((xx // 12) + (yy // 12)) % 2 == 0
        checker[c] = (58, 58, 62)
        checker[~c] = (92, 92, 98)
        composited = rgb * a + checker * (1.0 - a)
        stack = np.concatenate([composited, rgb], axis=1)
        Image.fromarray(np.clip(stack, 0, 255).astype(np.uint8)).resize(
            (stack.shape[1] * 2, stack.shape[0] * 2), Image.NEAREST
        ).save(os.path.join(run_dir, "head_composite_vs_truth.png"))
        print("  wrote head_composite_vs_truth.png  left=masked over desktop, right=truth")


if __name__ == "__main__":
    main()
