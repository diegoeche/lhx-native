#!/usr/bin/env python3
"""Audit and export source, notices and approved documentation screenshots.

Exports working-tree bytes, including new non-ignored source files. Git metadata,
agent coordination, original assets and generated game translations are excluded.
An export is preparation only: this command never commits, pushes or publishes.
"""
import argparse
import gzip
import hashlib
import io
import json
from pathlib import Path, PurePosixPath
import re
import struct
import subprocess
import tarfile

ROOT_FILES = {'.gitignore', '.gitattributes', 'README.md', 'LICENSE',
              'THIRD-PARTY.txt', 'CMakeLists.txt', 'controls.ini', 'run.sh'}
REQUIRED = {'README.md', 'LICENSE', 'THIRD-PARTY.txt', 'CMakeLists.txt',
            'run.sh', 'controls.ini', 'third_party/pcrecomp/LICENSE',
            'third_party/lhx-gpu-reference.LICENSE', 'third_party/wolf3d-recomp.LICENSE'}
SCREENSHOTS = {'cockpit.png', 'chase.png', 'terrain.png'}
EXCLUDED = {'AGENTS.md', 'SOURCE-MANIFEST.json'}  # Manifest is regenerated.
ASSET_SUFFIXES = {'.iso', '.exe', '.lib', '.ram', '.ppm', '.lhp', '.lhc', '.lht',
                  '.lhr', '.lhs', '.glb', '.gltf', '.i16', '.jpg', '.jpeg', '.wav',
                  '.mp3', '.fmd', '.w', '.pxpk', '.zip', '.7z', '.tar', '.gz'}
KEY = re.compile(rb'-----BEGIN (?:[A-Z0-9]+ )*PRIVATE KEY-----')
GENERATED_IMAGE = re.compile(rb'(?m)^\s*(?:static\s+)?(?:const\s+)?(?:uint8_t|unsigned char)\s+g_image\s*\[[^\]]*\]\s*=')


def allowed(name):
    path = PurePosixPath(name)
    if path.is_absolute() or '..' in path.parts or '\\' in name:
        return False
    if name in ROOT_FILES:
        return True
    if path.parts[0] in ('runtime', 'tests'):
        return path.suffix in ('.c', '.h', '.py')
    if path.parts[0] == 'tools':
        return path.suffix in ('.py', '.seeds')
    if path.parts[0] == 'third_party':
        return path.suffix in ('.c', '.h', '.py', '.LICENSE') or path.name == 'LICENSE'
    if path.parts[0] == 'docs':
        return path.suffix == '.md' or (path.parent.as_posix() == 'docs/screenshots' and path.name in SCREENSHOTS)
    return name == '.github/workflows/ci.yml'


def inspect(root, candidates=None):
    root = Path(root).resolve()
    if candidates is None:
        output = subprocess.check_output(['git', 'ls-files', '-z', '--cached',
                                          '--others', '--exclude-standard'], cwd=root)
        candidates = output.decode().split('\0')
    files = {}
    for name in sorted(set(candidates) - {''}):
        if name in EXCLUDED:
            continue
        path = root / name
        if not path.exists() and not path.is_symlink():
            continue  # A tracked deletion is not exported.
        if path.is_symlink() or not path.is_file() or not path.resolve().is_relative_to(root):
            raise ValueError(f'Not a regular in-repository file: {name}')
        if not allowed(name) or path.suffix.lower() in ASSET_SUFFIXES:
            raise ValueError(f'Outside the public source allowlist: {name}')
        if path.name.startswith(('recomp_', 'audio_ibm_gen.', 'cockpits_gen.', 'missions_gen.')):
            raise ValueError(f'Generated game code: {name}')
        data = path.read_bytes()
        if path.suffix == '.png':
            if len(data) < 33 or data[:8] != b'\x89PNG\r\n\x1a\n' or data[12:16] != b'IHDR':
                raise ValueError(f'Invalid documentation PNG: {name}')
            width, height = struct.unpack_from('>II', data, 16)
            if not (0 < width <= 2560 and 0 < height <= 1600 and len(data) <= 4*1024*1024):
                raise ValueError(f'Documentation image exceeds bounds: {name}')
            if not data.endswith(b'\0\0\0\0IEND\xaeB`\x82'):
                raise ValueError(f'Unexpected data after documentation PNG: {name}')
        else:
            if len(data) > 512*1024:
                raise ValueError(f'Unexpectedly large source file: {name}')
            try:
                data.decode('utf-8')
            except UnicodeDecodeError as exc:
                raise ValueError(f'Non-text source file: {name}') from exc
            if b'\0' in data or data.startswith((b'MZ', b'\x7fELF', b'PK\x03\x04')):
                raise ValueError(f'Binary content in source file: {name}')
            if KEY.search(data):
                raise ValueError(f'Private-key material: {name}')
            if path.suffix in ('.c', '.h') and GENERATED_IMAGE.search(data):
                raise ValueError(f'Generated original-image payload: {name}')
        files[name] = data
    missing = REQUIRED - files.keys()
    if missing:
        raise ValueError('Missing required source/notices: ' + ', '.join(sorted(missing)))
    return files


def archive(files, destination):
    destination = Path(destination)
    destination.parent.mkdir(parents=True, exist_ok=True)
    manifest = {name: hashlib.sha256(data).hexdigest() for name, data in files.items()}
    contents = dict(files)
    contents['SOURCE-MANIFEST.json'] = (json.dumps(manifest, indent=2, sort_keys=True)+'\n').encode()
    with destination.open('wb') as raw:
        with gzip.GzipFile(filename='', mode='wb', fileobj=raw, mtime=0) as compressed:
            with tarfile.open(fileobj=compressed, mode='w', format=tarfile.PAX_FORMAT) as tar:
                for name, data in sorted(contents.items()):
                    item = tarfile.TarInfo('lhx-native/' + name)
                    item.mode = 0o755 if name == 'run.sh' else 0o644
                    item.size = len(data)
                    tar.addfile(item, io.BytesIO(data))
    return manifest


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--check', action='store_true', help='Audit only; do not write an archive')
    parser.add_argument('--output', type=Path, default=Path('dist/lhx-native-source.tar.gz'))
    parser.add_argument('--root', type=Path, default=Path(__file__).resolve().parents[1])
    args = parser.parse_args()
    try:
        files = inspect(args.root)
        if not args.check:
            archive(files, args.output)
    except (ValueError, subprocess.CalledProcessError) as exc:
        parser.exit(1, f'Source export refused: {exc}\n')
    print(f'Checked {len(files)} source/documentation files ({sum(map(len, files.values())):,} bytes); no game payloads selected.')
    if not args.check:
        print(f'Prepared {args.output}; nothing was published.')


if __name__ == '__main__':
    main()
