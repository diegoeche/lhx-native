#!/usr/bin/env python3
"""Audit source, or publish committed development and public source revisions.

The development remote receives normal Git history. The public remote receives
only the package_source allowlist, in a new commit on its existing main branch.
No private Git objects, agent instructions, game files or local reports enter
the temporary public repository. Without --push this command only audits files.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

import package_source


def git(root, *args, capture=True, check=True):
    return subprocess.run(['git', '-C', str(root), *args], check=check,
                          capture_output=capture, text=True,
                          env=os.environ | {'GIT_TERMINAL_PROMPT': '0'})


def clean_revision(root):
    if git(root, 'status', '--porcelain').stdout:
        raise ValueError('Commit pending source changes before publishing.')
    if git(root, 'symbolic-ref', '--short', 'HEAD').stdout.strip() != 'main':
        raise ValueError('Publish from the development main branch.')
    return git(root, 'rev-parse', 'HEAD').stdout.strip()


def public_snapshot(files):
    manifest = {name: hashlib.sha256(data).hexdigest()
                for name, data in sorted(files.items())}
    return files | {'SOURCE-MANIFEST.json':
                    (json.dumps(manifest, indent=2, sort_keys=True) + '\n').encode()}


def publish(root, development='origin', public='github',
            message='Publish updated native LHX source'):
    root = Path(root).resolve()
    revision = clean_revision(root)
    files = package_source.inspect(root)
    destination = git(root, 'remote', 'get-url', public).stdout.strip()
    development_url = git(root, 'remote', 'get-url', development).stdout.strip()
    if destination == development_url:
        raise ValueError('Development and public remotes must be separate.')
    identity = {key: git(root, 'config', '--get', key).stdout.strip()
                for key in ('user.name', 'user.email')}
    # Fail before pushing if a local configuration cannot make a public commit.
    if not all(identity.values()):
        raise ValueError('Configure Git user.name and user.email before publishing.')

    git(root, 'push', development, f'{revision}:refs/heads/main', capture=False)
    print(f'Development main published: {revision}', flush=True)
    with tempfile.TemporaryDirectory(prefix='lhx-public-source-') as folder:
        checkout = Path(folder)
        git(checkout, 'init', '-q')
        git(checkout, 'remote', 'add', 'origin', destination)
        git(checkout, 'fetch', '--quiet', '--depth=1', 'origin', 'refs/heads/main')
        git(checkout, 'checkout', '-q', '-b', 'main', 'FETCH_HEAD')
        parent = git(checkout, 'rev-parse', 'HEAD').stdout.strip()
        for key, value in identity.items():
            git(checkout, 'config', key, value)
        for entry in checkout.iterdir():
            if entry.name == '.git':
                continue
            if entry.is_dir() and not entry.is_symlink():
                shutil.rmtree(entry)
            else:
                entry.unlink()
        for name, data in public_snapshot(files).items():
            path = checkout / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(data)
            if name == 'run.sh':
                path.chmod(0o755)
        git(checkout, 'add', '--all')
        unchanged = git(checkout, 'diff', '--cached', '--quiet', check=False)
        if unchanged.returncode not in (0, 1):
            raise RuntimeError('Could not compare the public source snapshot.')
        if clean_revision(root) != revision or package_source.inspect(root) != files:
            raise ValueError('Source changed during publication; public update stopped.')
        if unchanged.returncode == 0:
            print(f'Public source already current: {parent}')
            return revision, parent
        git(checkout, '-c', 'commit.gpgsign=false', 'commit', '-q', '-m', message)
        published = git(checkout, 'rev-parse', 'HEAD').stdout.strip()
        git(checkout, 'push', 'origin', 'HEAD:refs/heads/main', capture=False)
        print(f'Public source published: {published}')
        return revision, published


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument('--check', action='store_true', help='Audit only (the default)')
    mode.add_argument('--push', action='store_true', help='Push development and public source main')
    parser.add_argument('--root', type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument('--message', default='Publish updated native LHX source')
    args = parser.parse_args()
    try:
        if args.push:
            publish(args.root, message=args.message)
        else:
            files = package_source.inspect(args.root)
            print(f'Checked {len(files)} source/documentation files; nothing was published.')
    except (ValueError, RuntimeError, subprocess.CalledProcessError) as exc:
        parser.exit(1, f'Publication stopped: {exc}\n')


if __name__ == '__main__':
    main()
