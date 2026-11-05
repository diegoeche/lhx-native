#!/usr/bin/env python3
"""Convert the reference project's satellite images into optional native textures.

Reads the source directory only. Elevation grids are deliberately not imported:
this enhancement displays imagery on the existing flat ground.
"""
import argparse
import hashlib
import json
import math
from pathlib import Path
import shutil
import subprocess


def convert(source, output, near_size=2048, far_size=1024):
    magick = shutil.which("magick")
    if not magick:
        raise ValueError("ImageMagick (magick) is needed only for this optional import")
    if not 1 <= near_size <= 2048 or not 1 <= far_size <= 2048:
        raise ValueError("Texture size must be 1..2048")
    source, output = Path(source).resolve(), Path(output).resolve()
    if output == source or source in output.parents or output in source.parents:
        raise ValueError("Use a separate destination outside the reference checkout")
    prepared = []
    manifest = {"source": str(source), "format": "LHX texture 1", "relief": False,
                "orientation": "x east, z north; image rows north to south", "theatres": {}}
    for name in ("gulf", "asia", "europe"):
        meta_path = source / f"{name}.json"
        meta = json.loads(meta_path.read_text())
        entry = {"attribution": meta["attribution"], "source_metadata_sha256":
                 hashlib.sha256(meta_path.read_bytes()).hexdigest(), "textures": {}}
        for label, size in (("near", near_size), ("far", far_size)):
            bounds = meta[label]["bounds"]
            if (len(bounds) != 4 or not all(math.isfinite(v) for v in bounds) or
                    bounds[0] >= bounds[1] or bounds[2] >= bounds[3]):
                raise ValueError(f"Invalid bounds: {name}/{label}")
            image_path = source / f"{name}_{label}.jpg"
            dimensions = subprocess.run([magick, "identify", "-ping", "-format", "%w %h",
                                          str(image_path)], check=True, capture_output=True,
                                         text=True).stdout.split()
            if list(map(int, dimensions)) != [meta[label]["pixels"]] * 2:
                raise ValueError(f"Unexpected image dimensions: {image_path}")
            width = height = min(size, meta[label]["pixels"])
            rgb = subprocess.run([magick, str(image_path), "-filter", "Lanczos",
                                  "-resize", f"{width}x{height}!", "-depth", "8", "rgb:-"],
                                 check=True, capture_output=True).stdout
            if len(rgb) != width * height * 3:
                raise ValueError(f"Unexpected RGB length: {image_path}")
            header = (f"LHX texture 1\n{width} {height}\n" +
                      " ".join(format(v, ".17g") for v in bounds) + "\n").encode("ascii")
            payload = header + rgb
            filename = f"{name}_{label}.lht"
            prepared.append((filename, payload))
            entry["textures"][label] = {"file": filename, "width": width,
                "height": height, "bounds": bounds,
                "source_sha256": hashlib.sha256(image_path.read_bytes()).hexdigest(),
                "sha256": hashlib.sha256(payload).hexdigest()}
        manifest["theatres"][name] = entry
    output.mkdir(parents=True, exist_ok=True)
    for filename, payload in prepared:
        (output / filename).write_bytes(payload)
    (output / "SOURCE.json").write_text(json.dumps(manifest, indent=2) + "\n")
    return manifest


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, required=True,
                        help="Reference assets/terrain directory")
    parser.add_argument("--output", type=Path, default=Path("bin/enhanced/terrain"))
    args = parser.parse_args()
    manifest = convert(args.source, args.output)
    print(f"Imported {len(manifest['theatres'])} theatres into {args.output}")


if __name__ == "__main__":
    main()
