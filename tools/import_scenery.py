#!/usr/bin/env python3
"""Prepare the reference Kenney scenery without changing the source checkout.

Supports the bounded static glTF 2.0 subset used by these assets; unsupported
features fail explicitly. Native play needs neither a glTF loader nor ImageMagick.
Format reference: https://registry.khronos.org/glTF/specs/2.0/glTF-2.0.html
"""
import argparse
import hashlib
import json
import math
from pathlib import Path
import shutil
import struct
import subprocess

HALLS = [f"industrial/building-{c}" for c in "abghkopqst"]
HOUSES = [f"suburban/building-type-{c}" for c in "abcdefghijklmnopqrstu"]
SPECS = {
    "palm": ([f"nature/{n}" for n in ["tree_palmTall", "tree_palmDetailedTall", "tree_palm", "tree_palmBend"]], True, None),
    "tree2": (["nature/tree_default", "nature/tree_oak"], True, None),
    "tent": (["nature/tent_detailedClosed"], False, 0x6b7040),
    "tent2": (["nature/tent_detailedClosed"], False, 0x6b7040),
    "hooch": (["survival/structure-canvas"], False, 0xc8a060),
    **{n: (HALLS, False, None) for n in ["house2", "house5", "house6"]},
    "hangar": (["industrial/building-i", "industrial/building-j"], False, None),
    "factory": ([f"industrial/building-{c}" for c in "elmcf"], False, None),
    "oil": (["industrial/detail-tank-large"], False, None),
    **{n: (HOUSES, False, None) for n in ["house1", "house3", "house4"]},
}


def identity():
    return [[float(i == j) for j in range(4)] for i in range(4)]


def multiply(a, b):
    return [[sum(a[i][k] * b[k][j] for k in range(4)) for j in range(4)] for i in range(4)]


def transform(node):
    if "matrix" in node:
        values = node["matrix"]
        if len(values) != 16 or not all(math.isfinite(v) for v in values):
            raise ValueError("Invalid node matrix")
        return [[values[j * 4 + i] for j in range(4)] for i in range(4)]
    x, y, z, w = node.get("rotation", [0, 0, 0, 1])
    if abs(x*x+y*y+z*z+w*w-1) > .001:
        raise ValueError("Invalid node quaternion")
    scale, move = node.get("scale", [1, 1, 1]), node.get("translation", [0, 0, 0])
    r = [[1-2*(y*y+z*z), 2*(x*y-z*w), 2*(x*z+y*w)],
         [2*(x*y+z*w), 1-2*(x*x+z*z), 2*(y*z-x*w)],
         [2*(x*z-y*w), 2*(y*z+x*w), 1-2*(x*x+y*y)]]
    result = identity()
    for i in range(3):
        for j in range(3):
            result[i][j] = r[i][j] * scale[j]
        result[i][3] = move[i]
    if not all(math.isfinite(v) for row in result for v in row):
        raise ValueError("Non-finite node transform")
    return result


def read_glb(path):
    data = path.read_bytes()
    if len(data) < 28 or len(data) > 16 * 1024 * 1024:
        raise ValueError("Invalid GLB size")
    if struct.unpack_from("<III", data) != (0x46546c67, 2, len(data)):
        raise ValueError("Expected glTF 2.0 GLB")
    chunks, at = {}, 12
    while at < len(data):
        size, kind = struct.unpack_from("<II", data, at)
        at += 8
        if size > len(data)-at or kind in chunks:
            raise ValueError("Invalid GLB chunk")
        chunks[kind] = data[at:at+size]
        at += size
    document = json.loads(chunks[0x4e4f534a])
    if document.get("extensionsRequired") or document.get("skins") or document.get("animations"):
        raise ValueError("Only static, uncompressed source models are supported")
    return document, chunks[0x004e4942]


def accessor(document, binary, number):
    a = document["accessors"][number]
    if a.get("sparse") or a.get("normalized") or a["count"] > 65536:
        raise ValueError("Unsupported accessor")
    types = {"SCALAR": 1, "VEC2": 2, "VEC3": 3}
    formats = {5121: "B", 5123: "H", 5125: "I", 5126: "f"}
    fmt = "<" + formats[a["componentType"]] * types[a["type"]]
    size = struct.calcsize(fmt)
    v = document["bufferViews"][a["bufferView"]]
    stride = v.get("byteStride", size)
    if v.get("buffer", 0) or stride < size:
        raise ValueError("Unsupported buffer view")
    offset, start = a.get("byteOffset", 0), v.get("byteOffset", 0)
    end = offset + max(0, a["count"]-1)*stride + (size if a["count"] else 0)
    if offset < 0 or start < 0 or end > v["byteLength"] or start+v["byteLength"] > len(binary):
        raise ValueError("Accessor exceeds its buffer view")
    result = [struct.unpack_from(fmt, binary, start+offset+i*stride) for i in range(a["count"])]
    if not all(math.isfinite(v) for row in result for v in row):
        raise ValueError("Non-finite accessor data")
    return result


def srgb(value):
    return 12.92 * value if value <= .0031308 else 1.055 * value**(1/2.4)-.055


def material_image(document, material, directory, magick, override=None):
    if material.get("alphaMode", "OPAQUE") != "OPAQUE":
        raise ValueError("Only opaque scenery materials are supported")
    pbr = material.get("pbrMetallicRoughness", {})
    factor = pbr.get("baseColorFactor", [1, 1, 1, 1])
    if len(factor) != 4 or not all(math.isfinite(v) and 0 <= v <= 1 for v in factor) or factor[3] != 1:
        raise ValueError("Invalid material colour")
    factor = [srgb(v) for v in factor[:3]]  # Reference Props.model restores Kenney's sRGB colours.
    if override is not None:
        factor = [((override >> shift) & 255)/255 for shift in (16, 8, 0)]
    image_path = None
    if "baseColorTexture" in pbr:
        info = pbr["baseColorTexture"]
        ext = info.get("extensions", {}).get("KHR_texture_transform", {})
        if info.get("texCoord", 0) or ext.get("texCoord", 0) or any(k in ext for k in ["offset", "rotation", "scale"]):
            raise ValueError("Unsupported texture coordinate transform")
        texture = document["textures"][info["index"]]
        image_path = (directory / document["images"][texture["source"]]["uri"]).resolve()
        if directory.resolve() not in image_path.parents:
            raise ValueError("Texture path escapes its model directory")
        width, height = map(int, subprocess.check_output([magick, "identify", "-ping", "-format", "%w %h", str(image_path)], text=True).split())
        if not 1 <= width <= 1024 or not 1 <= height <= 1024:
            raise ValueError("Scenery texture exceeds 1024 pixels")
        rgb = subprocess.check_output([magick, str(image_path), "-depth", "8", "rgb:-"])
        if len(rgb) != width*height*3:
            raise ValueError("Invalid texture conversion")
    else:
        width = height = 1
        rgb = b"\xff\xff\xff"
    rgb = bytes(min(255, int(v*factor[i % 3]+.5)) for i, v in enumerate(rgb))
    return width, height, rgb, image_path


def convert_model(path, magick, override=None):
    document, binary = read_glb(path)
    groups, visited = [], set()
    def visit(number, parent):
        if number in visited or len(visited) > 256:
            raise ValueError("Invalid or shared scenery node hierarchy")
        visited.add(number)
        node = document["nodes"][number]
        matrix = multiply(parent, transform(node))
        if "mesh" in node:
            for primitive in document["meshes"][node["mesh"]]["primitives"]:
                if primitive.get("mode", 4) != 4 or primitive.get("targets"):
                    raise ValueError("Only static triangles are supported")
                attributes = primitive["attributes"]
                positions = accessor(document, binary, attributes["POSITION"])
                uv = accessor(document, binary, attributes["TEXCOORD_0"]) if "TEXCOORD_0" in attributes else [(0, 0)]*len(positions)
                if len(uv) != len(positions):
                    raise ValueError("UV/position count mismatch")
                indices = [v[0] for v in accessor(document, binary, primitive["indices"])] if "indices" in primitive else list(range(len(positions)))
                if not indices or len(indices) % 3 or len(indices) > 65535:
                    raise ValueError("Invalid triangle count")
                material = document.get("materials", [{}])[primitive.get("material", 0)]
                width, height, rgb, image = material_image(document, material, path.parent, magick, override)
                if image is None:
                    uv = [(0, 0)]*len(positions)  # Unbound UV attributes do not affect solid materials.
                vertices = []
                for i in indices:
                    if not isinstance(i, int) or not 0 <= i < len(positions):
                        raise ValueError("Invalid vertex index")
                    p = [sum(matrix[row][k]*positions[i][k] for k in range(3))+matrix[row][3] for row in range(3)]
                    if not all(0 <= v <= 1 for v in uv[i]):
                        raise ValueError("Scenery UV requires unsupported wrapping")
                    vertices.append((*p, *uv[i]))
                groups.append((vertices, width, height, rgb, bool(material.get("doubleSided")), image))
        for child in node.get("children", []):
            visit(child, matrix)
    for number in document["scenes"][document.get("scene", 0)]["nodes"]:
        visit(number, identity())
    if not groups or len(groups) > 16:
        raise ValueError("Invalid scenery mesh group count")
    points = [v[:3] for g in groups for v in g[0]]
    bounds = [min(p[i] for p in points) for i in range(3)] + [max(p[i] for p in points) for i in range(3)]
    if not all(math.isfinite(v) for v in bounds) or any(bounds[i] >= bounds[i+3] for i in range(3)):
        raise ValueError("Degenerate model bounds")
    out = bytearray(b"LHXPROP1" + struct.pack("<I6f", len(groups), *bounds))
    image_hashes = {}
    for vertices, width, height, rgb, double, image in groups:
        out += struct.pack("<4I", len(vertices), width, height, double)
        for v in vertices:
            out += struct.pack("<5f", *v)
        out += rgb
        if image:
            image_hashes[str(image)] = hashlib.sha256(image.read_bytes()).hexdigest()
    return bytes(out), image_hashes


def convert(source, output):
    source, output = Path(source).resolve(), Path(output).resolve()
    if source == output or source in output.parents or output in source.parents:
        raise ValueError("Use a destination outside the reference checkout")
    magick = shutil.which("magick")
    if not magick:
        raise ValueError("ImageMagick is needed only for optional scenery preparation")
    prepared, manifest = [], {"source": str(source), "format": "LHXPROP1", "licence": "Kenney CC0", "models": {}, "mapping": SPECS}
    names = sorted({name for variants, _, _ in SPECS.values() for name in variants})
    for name in names:
        path = source / (name + ".glb")
        overrides = {colour for variants, _, colour in SPECS.values() if name in variants and colour is not None}
        payload, image_hashes = convert_model(path, magick, next(iter(overrides), None))
        filename = name.replace("/", "_") + ".lhp"
        prepared.append((filename, payload))
        manifest["models"][name] = {"file": filename, "source_sha256": hashlib.sha256(path.read_bytes()).hexdigest(), "sha256": hashlib.sha256(payload).hexdigest(), "textures": image_hashes}
    output.mkdir(parents=True, exist_ok=True)
    for filename, payload in prepared:
        (output / filename).write_bytes(payload)
    catalog = bytearray(b"LHXCAT1\0" + struct.pack("<II", len(names), len(SPECS)))
    for name in names:
        catalog += (name.replace("/", "_")+".lhp").encode().ljust(64, b"\0")
    for original, (variants, height, _) in SPECS.items():
        catalog += original.encode().ljust(14, b"\0") + struct.pack("<BB", height, len(variants))
        catalog += struct.pack("<"+"H"*len(variants), *(names.index(v) for v in variants))
    (output / "catalog.lhc").write_bytes(catalog)
    (output / "SOURCE.json").write_text(json.dumps(manifest, indent=2)+"\n")
    for licence in source.glob("*/License.txt"):
        shutil.copyfile(licence, output/(licence.parent.name+"-LICENSE.txt"))
    return manifest


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--output", type=Path, default=Path("bin/enhanced/models"))
    args = parser.parse_args()
    manifest = convert(args.source, args.output)
    print(f"Imported {len(manifest['models'])} static scenery models into {args.output}")


if __name__ == "__main__":
    main()
