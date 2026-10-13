"""Measure the rotation each .vrma channel actually applies.

Reads the GLB accessors directly and, for every channel, reports the largest
angle between a keyframe rotation and identity. That separates two very
different explanations for a pose that looks wrong:

  * the animation genuinely asks for a large rotation on that bone, or
  * the animation asks for a small one and the retarget is exaggerating it.

Only the humanoid bones matter - the rest of the channels are dropped by
RetargetAnimation() because they have no bone identity to match on - so the
output is annotated with which bones survive and which are dropped.

usage: dump_vrma_rotations.py <file.vrma>
"""
import json
import math
import struct
import sys

JSON_CHUNK = 0x4E4F534A
BIN_CHUNK = 0x004E4942

COMPONENT = {5120: ("b", 1), 5121: ("B", 1), 5122: ("h", 2),
             5123: ("H", 2), 5125: ("I", 4), 5126: ("f", 4)}
NUM_COMPONENTS = {"SCALAR": 1, "VEC2": 2, "VEC3": 3, "VEC4": 4,
                  "MAT2": 4, "MAT3": 9, "MAT4": 16}


def load(path):
    with open(path, "rb") as f:
        data = f.read()
    _, _, length = struct.unpack_from("<4sII", data, 0)
    gltf, binary = None, b""
    off = 12
    while off < length:
        clen, ctype = struct.unpack_from("<II", data, off)
        off += 8
        chunk = data[off:off + clen]
        if ctype == JSON_CHUNK:
            gltf = json.loads(chunk.decode("utf-8"))
        elif ctype == BIN_CHUNK:
            binary = chunk
        off += clen
    return gltf, binary


def accessor(gltf, binary, index):
    acc = gltf["accessors"][index]
    fmt, size = COMPONENT[acc["componentType"]]
    n = NUM_COMPONENTS[acc["type"]]
    view = gltf["bufferViews"][acc["bufferView"]]
    base = view.get("byteOffset", 0) + acc.get("byteOffset", 0)
    stride = view.get("byteStride") or (size * n)
    out = []
    for i in range(acc["count"]):
        vals = struct.unpack_from("<" + fmt * n, binary, base + i * stride)
        out.append(vals if n > 1 else vals[0])
    return out


def quat_angle(q):
    """Angle in degrees between quaternion q and identity."""
    x, y, z, w = q
    w = max(-1.0, min(1.0, abs(w)))
    return math.degrees(2.0 * math.acos(w))


def main(path):
    gltf, binary = load(path)
    nodes = gltf["nodes"]
    human = gltf["extensions"]["VRMC_vrm_animation"]["humanoid"]["humanBones"]
    bone_of = {v["node"]: k for k, v in human.items()}
    anim = gltf["animations"][0]

    print(f"{path}")
    print(f"  channels={len(anim['channels'])}  samplers={len(anim['samplers'])}")
    print()
    print(f"{'node':>4} {'name':<14} {'path':<12} {'bone':<18} {'max|angle|':>10}  kept")
    print("-" * 74)

    rows = []
    for ch in anim["channels"]:
        node = ch["target"]["node"]
        tpath = ch["target"]["path"]
        sam = anim["samplers"][ch["sampler"]]
        out = accessor(gltf, binary, sam["output"])
        bone = bone_of.get(node)
        kept = "yes" if bone else "DROPPED"

        if tpath == "rotation":
            worst = max(quat_angle(v) for v in out)
        elif tpath == "translation":
            worst = max(math.sqrt(sum(c * c for c in v)) for v in out)
        else:
            worst = float("nan")

        rows.append((node, nodes[node].get("name", "?"), tpath, bone or "-", worst, kept))

    for node, name, tpath, bone, worst, kept in rows:
        unit = "deg" if tpath == "rotation" else "units"
        print(f"{node:>4} {name:<14} {tpath:<12} {bone:<18} {worst:>8.1f} {unit}  {kept}")

    print()
    print("Kept channels are the ones RetargetAnimation() can match to a bone.")
    print("A DROPPED rotation still moves its children in the source rig, so its")
    print("contribution is simply missing - never inverted - in the result.")


if __name__ == "__main__":
    main(sys.argv[1])
