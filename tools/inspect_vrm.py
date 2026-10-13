"""Inspect a .vrm / .vrma (GLB container) without loading it into an engine.

Reports the things that decide whether a stencil-based alpha mask can work:
material alpha modes, MToon usage, and whether soft-alpha hair exists.
"""
import json
import struct
import sys
from collections import Counter

JSON_CHUNK = 0x4E4F534A
BIN_CHUNK = 0x004E4942


def read_glb(path):
    with open(path, "rb") as f:
        data = f.read()
    magic, version, length = struct.unpack_from("<4sII", data, 0)
    if magic != b"glTF":
        raise SystemExit(f"not a GLB container: magic={magic!r}")
    chunks = []
    off = 12
    while off < length:
        clen, ctype = struct.unpack_from("<II", data, off)
        off += 8
        chunks.append((ctype, data[off:off + clen]))
        off += clen
    return version, length, chunks


def json_of(chunks):
    for ctype, cdata in chunks:
        if ctype == JSON_CHUNK:
            return json.loads(cdata.decode("utf-8"))
    raise SystemExit("no JSON chunk found")


def hr(title):
    print()
    print("=" * 62)
    print(title)
    print("=" * 62)


def inspect(path):
    version, length, chunks = read_glb(path)
    g = json_of(chunks)

    hr(f"FILE  {path}")
    print(f"GLB container version : {version}")
    print(f"file size             : {length:,} bytes")
    for ctype, cdata in chunks:
        name = {JSON_CHUNK: "JSON", BIN_CHUNK: "BIN"}.get(ctype, hex(ctype))
        print(f"  chunk {name:<5} {len(cdata):>12,} bytes")

    hr("ASSET")
    asset = g.get("asset", {})
    print(f"generator : {asset.get('generator')}")
    print(f"version   : {asset.get('version')}")
    if "copyright" in asset:
        print(f"copyright : {asset['copyright']}")

    hr("EXTENSIONS")
    print("extensionsUsed:")
    for e in g.get("extensionsUsed", []):
        print(f"  - {e}")
    req = g.get("extensionsRequired", [])
    print("extensionsRequired:")
    for e in req:
        print(f"  - {e}")
    if not req:
        print("  (none)")

    top = g.get("extensions", {})
    print(f"\ntop-level extension blocks: {sorted(top.keys())}")

    for key in ("VRM", "VRMC_vrm"):
        if key in top:
            v = top[key]
            hr(f"{key} block")
            if key == "VRM":
                spec = v.get("specVersion", "?")
                print(f"specVersion : {spec}")
                meta = v.get("meta", {})
                print(f"title       : {meta.get('title')}")
                print(f"author      : {meta.get('author')}")
                print(f"license     : {meta.get('licenseName')}")
                hum = v.get("humanoid", {}).get("humanBones", [])
                print(f"human bones : {len(hum)}")
                sec = v.get("secondaryAnimation", {}).get("boneGroups", [])
                print(f"spring groups: {len(sec)}")
                total_joints = sum(len(bg.get("bones", [])) for bg in sec)
                print(f"spring joints: {total_joints}")
            else:
                spec = v.get("specVersion", "?")
                print(f"specVersion : {spec}")
                meta = v.get("meta", {})
                print(f"name        : {meta.get('name')}")
                print(f"authors     : {meta.get('authors')}")
                print(f"license     : {meta.get('licenseUrl')}")
                hum = v.get("humanoid", {}).get("humanBones", {})
                print(f"human bones : {len(hum)}")
                print("spring bones: see top-level VRMC_springBone block below")
            exp = v.get("exporter")
            if exp:
                print(f"exporter    : {exp}")

    if "VRMC_springBone" in top:
        hr("VRMC_springBone block  (top level, VRM 1.0)")
        sb = top["VRMC_springBone"]
        print(f"specVersion  : {sb.get('specVersion')}")
        groups = sb.get("springBones", [])
        print(f"spring groups: {len(groups)}")
        total_joints = sum(len(g.get("joints", [])) for g in groups)
        print(f"spring joints: {total_joints}")
        print(f"colliders    : {len(sb.get('colliders', []))}")
        for g in groups[:20]:
            print(f"  - {len(g.get('joints', [])):>3} joints  "
                  f"stiffness={g.get('stiffness')} drag={g.get('dragForce')}")

    if "VRMC_vrm_animation" in top:
        hr("VRMC_vrm_animation block")
        v = top["VRMC_vrm_animation"]
        print(f"specVersion : {v.get('specVersion')}")
        print(f"humanoid    : {len(v.get('humanoid', {}).get('humanBones', {}))} bones")
        print(f"expressions : {len(v.get('expressions', {}))}")

    mats = g.get("materials", [])
    hr(f"MATERIALS  ({len(mats)} total)")
    modes = Counter()
    mtoon_mats = []
    for i, m in enumerate(mats):
        am = m.get("alphaMode", "OPAQUE")
        modes[am] += 1
        if "VRMC_materials_mtoon" in m.get("extensions", {}):
            mtoon_mats.append((i, m))

    print("alpha mode distribution:")
    for mode, n in modes.most_common():
        flag = "   <-- soft edges possible" if mode in ("BLEND", "MASK") else ""
        print(f"  {mode:<8} {n:>3}{flag}")

    print(f"\nmaterials with VRMC_materials_mtoon: {len(mtoon_mats)} / {len(mats)}")
    print("\nfull material list:")
    for i, m in enumerate(mats):
        am = m.get("alphaMode", "OPAQUE")
        is_mtoon = "VRMC_materials_mtoon" in m.get("extensions", {})
        ds = m.get("doubleSided", False)
        print(f"  [{i:>3}] {am:<7} mtoon={str(is_mtoon):<5} ds={str(ds):<5} {m.get('name','<unnamed>')}")

    imgs = g.get("images", [])
    hr(f"IMAGES  ({len(imgs)} total)")
    mimes = Counter(i.get("mimeType", "?") for i in imgs)
    for mime, n in mimes.most_common():
        print(f"  {mime:<12} {n:>3}")
    print("\n(alpha-bearing PNGs are what MASK/BLEND hair typically samples)")

    meshes = g.get("meshes", [])
    prims = sum(len(m.get("primitives", [])) for m in meshes)
    print(f"\nmeshes: {len(meshes)}   primitives: {prims}")
    print(f"nodes: {len(g.get('nodes', []))}")
    print(f"skins: {len(g.get('skins', []))}")
    print(f"animations: {len(g.get('animations', []))}")
    for a in g.get("animations", [])[:10]:
        print(f"  - {a.get('name','<unnamed>')}  channels={len(a.get('channels',[]))}")


if __name__ == "__main__":
    if len(sys.argv) < 2:
        raise SystemExit("usage: inspect_vrm.py <file.vrm|file.vrma> [...]")
    for p in sys.argv[1:]:
        inspect(p)
