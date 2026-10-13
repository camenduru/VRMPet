# VRM avatar over the Windows desktop, using a WickedEngine stencil mask

**Prototype result: the approach works. `alphaMode=BLEND` facial materials survive the
stencil mask — every visible avatar pixel is captured, with zero dropped.**

---

## 1. The question

Render a VRM avatar with WickedEngine, derive an alpha mask from the stencil buffer, and
composite the result over the Windows desktop. The specific risk being tested:

> Blended geometry normally does not write depth or stencil. If that is true here, the
> `alphaMode=BLEND` facial materials — eyelashes, brows, eye highlights, iris — would be
> missing from the mask, and the desktop would show through the face as transparent holes.

## 2. The answer

**They survive, and nothing is dropped.**

Two independent lines of evidence:

### 2.1 Measured

`tools/compare_mask.py` recovers the avatar's true screen-space footprint by differencing
two frames rendered through the identical code path — one with the avatar, one with every
object hidden. That footprint owes nothing to the stencil buffer. Comparing it against the
mask gives:

| capture | avatar footprint | mask coverage | **recall** | precision |
|---|---|---|---|---|
| whole body (front, MSAA 4) | 74,221 px | 88,924 px | **100.00%** | 83.47% |
| face mesh isolated (MSAA 4) | 206,418 px | 207,674 px | **100.00%** | 99.40% |

**Recall 100.00% — zero missed pixels**, at every colour threshold tested including `eps=0`.
Recall is the number that matters: nothing the avatar contributes is clipped out of the mask.

The `face` capture is the decisive one. `mesh[0]` has exactly 8 subsets and the model has
exactly 8 face materials, so `mesh[0]` *is* the face mesh — and it contains all five BLEND
materials. Isolated and composited over a checkerboard
(`build/run/out/face/mask_default_over_checker.png`), the eyebrow, eyelash, eyeliner, iris
and eye highlight are all present with **zero magenta** (magenta = unmasked). Precision is
99.40%, so the mask is tight as well as complete.

The lower 83.47% precision on the full body is an artefact of the ground-truth threshold,
not of the mask: the model's dark navy jacket and black shoes render below the `eps=6`
colour-difference threshold against a black background, so they are counted as
"mask without avatar". At `eps=0` precision rises to 96.07% — and recall stays at 100.00%.

### 2.2 Read from the engine source

`wiRenderer.cpp:1998-2007`:

```cpp
case RENDERPASS_MAIN:
    if (blendMode == BLENDMODE_ADDITIVE)
        desc.dss = &depthStencils[DSSTYPE_DEPTHREAD];        // stencil_enable = false
    else
        desc.dss = &depthStencils[transparency ? DSSTYPE_TRANSPARENT : DSSTYPE_DEPTHREADEQUAL];
    break;
```

`DSSTYPE_TRANSPARENT` is built at `wiRenderer.cpp:2396-2415` as a copy of `DSSTYPE_DEFAULT`
with **only `depth_func` changed** (GREATER → GREATER_EQUAL). It therefore inherits:

```
stencil_enable        = true
stencil_func          = ALWAYS
stencil_pass_op       = REPLACE      <-- writes the reference value
stencil_write_mask    = 0xFF
```

So in WickedEngine, alpha-blended geometry **does** write the stencil buffer in the main
pass. Only `BLENDMODE_ADDITIVE` objects skip it (`DSSTYPE_DEPTHREAD` has
`stencil_enable = false`) — a real caveat for the overlay, but not one this avatar hits.

## 3. What was built

`C:/Users/PC/Documents/VRMPet` — a CMake project that consumes the engine checkout
**read-only**. No file inside `WickedEngine/` was modified.

| file | role |
|---|---|
| `CMakeLists.txt` | builds WickedEngine as a subproject with every editor/sample/test option forced OFF, then the `VRMPet` exe |
| `src/main.cpp` | the capture tool: staged state machine, camera framing, mask captures |
| `build.sh` | MSVC + Ninja build, toolchain mirrored from `RoguelikeGame/build_output.sh` |
| `third_party/wicked_importer/` | verbatim copy of `Editor/ModelImporter_GLTF.cpp` + a `stdafx.h` stub |
| `tools/inspect_vrm.py` | stdlib-only GLB parser (extensions, materials, alphaMode distribution) |
| `tools/analyze_mask.py` | alpha histogram, silhouette bbox, checkerboard composites |
| `tools/compare_mask.py` | mask vs. ground-truth footprint: recall, precision, failure maps |

Run:

```bash
./build.sh
cd build/run
./VRMPet.exe out/body "…/AvatarSample_B.vrm" 180 -1 -1 4   # whole body, front, MSAA 4
./VRMPet.exe out/face "…/AvatarSample_B.vrm" 180  0  0 4   # face mesh only, close-up
```

Args: `out-dir`, `model.vrm`, `azimuth-deg`, `focus-mesh` (-1 = whole body),
`isolate-mesh` (-1 = all), `msaa`.

Each run writes `color.png`, `mask_default.png` (stencil != 0), `mask_user.png` (user
stencil ref 1), `mask_nostencil.png` (stencil ALWAYS — the full frame), `background.png`
(all objects hidden) and `engine.log`.

## 4. Engine findings worth keeping

### 4.1 A bare `RenderPath3D` renders nothing — the projection is NaN

This cost the most time and is not obvious.

`RenderPath3D::camera` points at the global `wi::scene::GetCamera()`
(`wiRenderPath3D.h:190`), and the render path only ever writes `camera->width/height` into
it (`ResizeBuffers`, `Render`). Nothing in the render path calls `CreatePerspective()` —
establishing the projection is left to the host; the Editor does it from its camera window.

In this build the global camera reads as **all zeros**, so `zNearP == zFarP == 0` and
`XMMatrixPerspectiveFovLH(fov, aspect, 0, 0)` computes `0/0` → an all-NaN projection →
degenerate frustum → **every object culled, empty frame, empty mask.**

Probes in `Initialize()` pin it down:

```
probe local ctor           near=0.1000 far=5000.0000 fov=1.0472 sizeof=1472
probe boot (pre-engine)    near=0.0000 far=0.0000   fov=0.0000
probe pre-path global cam  near=0.0000 far=0.0000   fov=0.0000
probe post-path path cam   near=0.0000 far=0.0000   fov=0.0000  same_object=1
```

A stack-constructed `CameraComponent{}` gets the documented defaults correctly, so the
header is fine. The function-local static behind the `inline GetCamera()` is
zero-initialised and its default member initialisers never run. It is zero as the very
first thing `wWinMain` does, so nothing in the engine zeroes it — it is a
static-initialisation problem in this static-library build.

**Workaround (`ApplyProjection()` in `src/main.cpp`): call `cam.CreatePerspective()` every
frame.** Note `CameraComponent::At` is a normalised *forward direction*, not a look-at
point — it feeds `XMMatrixLookToLH(eye, direction, up)`.

### 4.2 Capture timing

`Application::Run()` is `PreUpdate → FixedUpdate → Update → Render`, so a capture taken
from `Update()` reflects the **previous** frame. `CreateScreenshotWithAlphaBackground()`
does not re-render: it re-draws `GetLastPostprocessRT()` through a stencil test against the
existing depth-stencil buffer. The state machine therefore tags objects one frame and
captures the next.

`Run()` also returns early when `!is_window_active && !alwaysactive`, so a background
launch never renders — the tool sets `is_window_active = true`.

### 4.3 MSAA gives antialiased mask edges

The stencil test is evaluated per sample, and `CreateScreenshotWithAlphaBackground()`
resolves a multisampled target. With `path->setMSAASampleCount(4)` the mask stops being
binary: **3,768 partially transparent pixels across 5 alpha levels** (0/64/128/191/255).
Without MSAA the mask is strictly 0 or 255 and the composite has hard, jagged edges.
This matters for a desktop overlay, and costs one line.

### 4.4 The VRM importer is Editor-only

`wi::scene::LoadModel()` reads only the WickedEngine Archive format — pointed at a `.vrm`
it reports *"The archive version (9769872487) is higher than the program's (93)"*, because
it is reading the GLB magic `glTF` as a version number. There is no glTF/VRM importer in
the library.

Rather than patch the engine (excluded by the constraint), `Editor/ModelImporter_GLTF.cpp`
is vendored **byte-for-byte** (sha256 `12c6bb7c…88293`) and called directly. Its
`#include "stdafx.h"` resolves to a local stub that omits `Editor.h`, avoiding the whole
editor dependency. Compiles with `/bigobj`.

### 4.5 The asset

`AvatarSample_B.vrm` — VRoid Studio 1.22.0, VRM 1.0, 54 humanoid bones. Imported as
3 objects / 3 meshes / 18 materials / 1 humanoid / 80 springs.

- `mesh[0]` **face**, 8 subsets, aabb y 1.286…1.501
- `mesh[1]` **body**, 4 subsets, aabb y 0.000…1.508
- `mesh[2]` **hair**, 6 subsets, aabb y 0.741…1.580

All 18 materials are MToon: **5 BLEND** (`EyeIris`, `EyeHighlight`, `FaceBrow`,
`FaceEyelash`, `FaceEyeline`), **13 MASK**, 0 OPAQUE. All six hair materials are MASK.
`idle_loop.vrma` is VRM Animation 1.0 (22 bones, 28 channels, 0 expressions) — not used
by this prototype, which captures the bind pose.

Also worth knowing: the model has **0 spring bone groups** (28 colliders configured, no
joints), so the hair is rigid regardless.

## 5. Desktop transparency — where the boundary is

WickedEngine **cannot** present a transparent window. `GraphicsDevice_DX12::CreateSwapChain`
uses `CreateSwapChainForHwnd` with `DXGI_SWAP_EFFECT_FLIP_DISCARD` and hardcodes
`DXGI_ALPHA_MODE_IGNORE`; `SwapChainDesc` has no transparency field; nothing touches
`WS_EX_LAYERED` or DirectComposition. Making the engine's own swap chain transparent would
require patching it.

That is why this prototype takes the other route: **render offscreen, extract alpha from
the stencil buffer, composite in a separate window.** The mask is produced without any
engine modification, so the constraint holds.

## 6. Limitations and next steps

Verified working: offscreen render, stencil-derived alpha mask, correct treatment of BLEND
materials, antialiased edges via MSAA.

Not yet built — the actual overlay window:

1. A borderless topmost window with `WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW`
   for click-through, fed by `UpdateLayeredWindow` with the premultiplied RGBA bitmap.
   DirectComposition with a premultiplied-alpha visual is the cleaner alternative and also
   needs no engine patch, since the engine's swap chain is not involved.
2. Per-frame cost. Each capture currently allocates and reads back a 1024×1024 RGBA
   texture on the CPU. A real overlay should keep everything on the GPU and only read back
   when needed.
3. The mask is a hard silhouette. It has no notion of the avatar's own translucency — a
   semi-transparent skirt would still be fully opaque in the mask. MSAA softens the edges
   only.
4. `BLENDMODE_ADDITIVE` materials (glow, some effects) do **not** write stencil and would
   be dropped. None are present in this avatar, but a general solution should account for
   them.
5. The tool currently captures the bind pose. Driving `idle_loop.vrma` is a separate step.

## 7. Reproduce

```bash
cd C:/Users/PC/Documents/VRMPet
./build.sh                       # ~230 TUs, incremental thereafter
cd build/run
./VRMPet.exe out/face "C:/Users/PC/Documents/fish.audio/web/vrm/AvatarSample_B.vrm" 180 0 0 4

cd C:/Users/PC/Documents/VRMPet
python tools/compare_mask.py build/run/out/face
python tools/analyze_mask.py build/run/out/face
```

`python` must have `numpy` and `pillow`.
