"""Dump a .vrma's node hierarchy, humanoid bone mapping and animation channels.

The retargeting step in main.cpp matches bones purely by
VRMC_vrm_animation.humanoid.humanBones[bone].node, so this prints exactly that
mapping plus each node's rest local transform - the numbers the engine's
RetargetAnimation() will consume.
"""
import json
import struct
import sys

JSON_CHUNK = 0x4E4F534A


def load_json(path):
    with open(path, "rb") as f:
        data = f.read()
    _, _, length = struct.unpack_from("<4sII", data, 0)
    off = 12
    while off < length:
        clen, ctype = struct.unpack_from("<II", data, off)
        off += 8
        if ctype == JSON_CHUNK:
            return json.loads(data[off:off + clen].decode("utf-8"))
        off += clen
    raise SystemExit("no JSON chunk")


def main(path):
    g = load_json(path)
    nodes = g.get("nodes", [])

    parent = {}
    for i, n in enumerate(nodes):
        for c in n.get("children", []):
            parent[c] = i

    def name_of(i):
        return nodes[i].get("name", f"<node {i}>")

    print("=" * 70)
    print("NODE HIERARCHY")
    print("=" * 70)

    def walk(i, depth):
        n = nodes[i]
        bits = []
        if "translation" in n:
            bits.append("T=" + ",".join(f"{v:.4f}" for v in n["translation"]))
        if "rotation" in n:
            bits.append("R=" + ",".join(f"{v:.4f}" for v in n["rotation"]))
        if "scale" in n:
            bits.append("S=" + ",".join(f"{v:.4f}" for v in n["scale"]))
        print(f"{'  ' * depth}[{i:>2}] {name_of(i):<28} {'  '.join(bits)}")
        for c in n.get("children", []):
            walk(c, depth + 1)

    roots = [i for i in range(len(nodes)) if i not in parent]
    for r in roots:
        walk(r, 0)

    print()
    print("=" * 70)
    print("VRMC_vrm_animation.humanoid.humanBones")
    print("=" * 70)
    hb = g.get("extensions", {}).get("VRMC_vrm_animation", {}) \
          .get("humanoid", {}).get("humanBones", {})
    print(f"{len(hb)} bones")
    for bone in sorted(hb.keys()):
        idx = hb[bone]["node"]
        print(f"  {bone:<24} -> node[{idx:>2}] {name_of(idx)}")

    print()
    print("=" * 70)
    print("ANIMATION CHANNELS")
    print("=" * 70)
    for a in g.get("animations", []):
        print(f"name = {a.get('name')!r}   channels={len(a['channels'])}   "
              f"samplers={len(a['samplers'])}")
        by_path = {}
        for ch in a["channels"]:
            t = ch["target"]
            by_path.setdefault(t["path"], []).append(t["node"])
        for path, ns in sorted(by_path.items()):
            print(f"  path {path:<12} on {len(ns)} nodes")
        # keyframe count / duration for the first sampler
        acc = g["accessors"]
        smp = a["samplers"][0]
        inp = acc[smp["input"]]
        print(f"  sampler[0] interp={smp.get('interpolation','LINEAR')} "
              f"keys={inp['count']} t=[{inp.get('min')} .. {inp.get('max')}]")

    print()
    print("=" * 70)
    print("SCENE ROOTS")
    print("=" * 70)
    for s in g.get("scenes", []):
        print(f"  scene {s.get('name')!r} roots={s.get('nodes')}")


if __name__ == "__main__":
    main(sys.argv[1])
