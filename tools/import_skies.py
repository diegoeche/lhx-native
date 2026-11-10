#!/usr/bin/env python3
"""Prepare JPG/PNG sky panoramas for native rendering; source files stay read-only."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import struct
import subprocess


def convert(source, output, width=4096, *, source_format="jpg", licence=None, nearest=False):
    source, output = Path(source).resolve(), Path(output).resolve()
    if source == output or source in output.parents:
        raise ValueError("Use a destination outside the source directory")
    if width not in (512, 1024, 2048, 4096):
        raise ValueError("Sky width must be 512, 1024, 2048 or 4096")
    if source_format not in ("jpg", "png"):
        raise ValueError("Sky source format must be jpg or png")
    if licence is None:
        if source_format != "jpg":
            raise ValueError("PNG skies require explicit --licence metadata; see source SOURCE.txt")
        licence = "Poly Haven CC0"
    if not licence.strip():
        raise ValueError("Sky licence metadata must not be empty")
    magick = shutil.which("magick")
    if not magick:
        raise ValueError("ImageMagick is needed only for optional sky preparation")
    attribution = (source / "SOURCE.txt").read_bytes()
    manifest = {"source": str(source), "format": "LHXSKY1", "width": width,
                "height": width//2, "source_format": source_format,
                "licence": licence, "resize_filter": "point" if nearest else "default", "images": {}}
    prepared = []
    for name in ("day", "night"):
        path = source / (name+"."+source_format)
        command = [magick, str(path)]
        if nearest:
            command += ["-filter", "point"]
        rgb = subprocess.check_output(command+["-resize", f"{width}x{width//2}!", "-depth", "8", "rgb:-"])
        if len(rgb) != width*(width//2)*3:
            raise ValueError("Unexpected panorama RGB size")
        data = b"LHXSKY1\0"+struct.pack("<II", width, width//2)+rgb
        prepared.append((name+".lhs", data))
        manifest["images"][name] = {"source_sha256": hashlib.sha256(path.read_bytes()).hexdigest(),
                                    "sha256": hashlib.sha256(data).hexdigest()}
    output.mkdir(parents=True, exist_ok=True)
    for name, data in prepared:
        (output / name).write_bytes(data)
    (output / "SOURCE.txt").write_bytes(attribution)
    (output / "SOURCE.json").write_text(json.dumps(manifest, indent=2)+"\n")
    return manifest


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--output", type=Path, default=Path("bin/enhanced/sky"))
    parser.add_argument("--width", type=int, default=4096)
    parser.add_argument("--source-format", choices=("jpg", "png"), default="jpg",
                        help="Input filenames: day/night.jpg (default) or day/night.png")
    parser.add_argument("--licence", help="Pack licence metadata; defaults to Poly Haven CC0 for JPG only. "
                        "Required for PNG; source SOURCE.txt supplies full attribution.")
    parser.add_argument("--nearest", action="store_true",
                        help="Use nearest-neighbour resizing to preserve pixel-art colours and edges")
    args = parser.parse_args()
    convert(args.source, args.output, args.width, source_format=args.source_format,
            licence=args.licence, nearest=args.nearest)
    print(f"Imported day/night panoramas into {args.output}")
