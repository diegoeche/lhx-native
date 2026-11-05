#!/usr/bin/env python3
"""Prepare the reference CC0 panoramas for native rendering; source stays read-only."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import struct
import subprocess


def convert(source, output, width=4096):
    source, output = Path(source).resolve(), Path(output).resolve()
    if source == output or source in output.parents or output in source.parents:
        raise ValueError("Use a destination outside the reference checkout")
    if width not in (512, 1024, 2048, 4096):
        raise ValueError("Sky width must be 512, 1024, 2048 or 4096")
    magick = shutil.which("magick")
    if not magick:
        raise ValueError("ImageMagick is needed only for optional sky preparation")
    manifest = {"source": str(source), "format": "LHXSKY1", "width": width,
                "height": width//2, "licence": "Poly Haven CC0", "images": {}}
    prepared = []
    for name in ("day", "night"):
        path = source / (name+".jpg")
        rgb = subprocess.check_output([magick, str(path), "-resize", f"{width}x{width//2}!", "-depth", "8", "rgb:-"])
        if len(rgb) != width*(width//2)*3:
            raise ValueError("Unexpected panorama RGB size")
        data = b"LHXSKY1\0"+struct.pack("<II", width, width//2)+rgb
        prepared.append((name+".lhs", data))
        manifest["images"][name] = {"source_sha256": hashlib.sha256(path.read_bytes()).hexdigest(),
                                    "sha256": hashlib.sha256(data).hexdigest()}
    output.mkdir(parents=True, exist_ok=True)
    for name, data in prepared:
        (output / name).write_bytes(data)
    shutil.copyfile(source / "SOURCE.txt", output / "SOURCE.txt")
    (output / "SOURCE.json").write_text(json.dumps(manifest, indent=2)+"\n")
    return manifest


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--output", type=Path, default=Path("bin/enhanced/sky"))
    parser.add_argument("--width", type=int, default=4096)
    args = parser.parse_args()
    convert(args.source, args.output, args.width)
    print(f"Imported day/night panoramas into {args.output}")
