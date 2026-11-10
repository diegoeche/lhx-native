"""Public updates contain audited source and descend only from public history."""
import contextlib
import importlib.util
import io
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
with patch.object(sys, 'path', [str(ROOT / 'tools'), *sys.path]):
    SPEC = importlib.util.spec_from_file_location('publish_source', ROOT / 'tools/publish_source.py')
    PUBLISH = importlib.util.module_from_spec(SPEC)
    SPEC.loader.exec_module(PUBLISH)


@unittest.skipUnless(shutil.which('git'), 'Git required')
class SourcePublication(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='lhx-publish-test-')
        self.addCleanup(self.temp.cleanup)
        self.folder = Path(self.temp.name)
        self.root = self.folder / 'development'
        self.root.mkdir()
        self.development = self.folder / 'development.git'
        self.public = self.folder / 'public.git'
        self.env = patch.dict(os.environ, {'GIT_CONFIG_NOSYSTEM': '1',
                                          'GIT_CONFIG_GLOBAL': os.devnull})
        self.env.start()
        self.addCleanup(self.env.stop)
        self.git(self.folder, 'init', '-q', '--bare', str(self.development))
        self.git(self.folder, 'init', '-q', '--bare', str(self.public))
        for folder in (self.root, self.folder / 'public-seed'):
            folder.mkdir(exist_ok=True)
            self.git(folder, 'init', '-q', '-b', 'main')
            self.git(folder, 'config', 'user.name', 'Source test')
            self.git(folder, 'config', 'user.email', 'source@example.invalid')
        for name in PUBLISH.package_source.REQUIRED:
            path = self.root / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text('source or notice\n')
        (self.root / 'AGENTS.md').write_text('private coordination notes\n')
        (self.root / 'runtime').mkdir()
        (self.root / 'runtime/old_private.c').write_text('private historical marker\n')
        self.commit(self.root, 'Private development history')
        self.private_commit = self.git(self.root, 'rev-parse', 'HEAD')
        (self.root / 'runtime/old_private.c').unlink()
        (self.root / 'runtime/feature.c').write_text('int feature(void) { return 1; }\n')
        self.commit(self.root, 'Current source')
        for name, path in (('origin', self.development), ('github', self.public)):
            self.git(self.root, 'remote', 'add', name, str(path))
        seed = self.folder / 'public-seed'
        (seed / 'README.md').write_text('Old public release\n')
        (seed / 'obsolete.txt').write_text('Old removed public file\n')
        self.commit(seed, 'Original public release')
        self.public_parent = self.git(seed, 'rev-parse', 'HEAD')
        self.git(seed, 'push', '-q', str(self.public), 'main')

    def git(self, root, *args):
        return subprocess.check_output(['git', '-C', str(root), *args],
                                       text=True, stderr=subprocess.PIPE).strip()

    def commit(self, root, message):
        self.git(root, 'add', '--all')
        self.git(root, '-c', 'commit.gpgsign=false', 'commit', '-q', '-m', message)

    def publish(self):
        with contextlib.redirect_stdout(io.StringIO()):
            return PUBLISH.publish(self.root)

    def test_snapshot_updates_source_without_private_history_and_is_idempotent(self):
        revision, public = self.publish()
        self.assertEqual(self.git(self.development, 'rev-parse', 'main'), revision)
        self.assertEqual(self.git(self.public, 'rev-parse', 'main^'), self.public_parent)
        files = PUBLISH.package_source.inspect(self.root)
        names = set(self.git(self.public, 'ls-tree', '-r', '--name-only', 'main').splitlines())
        self.assertEqual(names, set(files) | {'SOURCE-MANIFEST.json'})
        self.assertNotIn('AGENTS.md', names)
        self.assertNotIn('obsolete.txt', names)
        self.assertEqual(self.git(self.public, 'show', 'main:runtime/feature.c'),
                         'int feature(void) { return 1; }')
        manifest = json.loads(self.git(self.public, 'show', 'main:SOURCE-MANIFEST.json'))
        self.assertEqual(manifest, {name: PUBLISH.hashlib.sha256(data).hexdigest()
                                    for name, data in files.items()})
        absent = subprocess.run(['git', '-C', str(self.public), 'cat-file', '-e',
                                 self.private_commit], capture_output=True)
        self.assertNotEqual(absent.returncode, 0)
        self.assertEqual(self.publish(), (revision, public))
        self.assertEqual(self.git(self.public, 'rev-parse', 'main'), public)
        self.assertEqual(self.git(self.root, 'status', '--porcelain'), '')

    def test_dirty_source_stops_before_any_remote_update(self):
        (self.root / 'README.md').write_text('Uncommitted change\n')
        with self.assertRaisesRegex(ValueError, 'Commit pending'):
            self.publish()
        self.assertEqual(self.git(self.public, 'rev-parse', 'main'), self.public_parent)
        self.assertEqual(self.git(self.development, 'for-each-ref', '--format=%(refname)'), '')

    def test_same_development_and_public_remote_is_rejected(self):
        self.git(self.root, 'remote', 'set-url', 'github', str(self.development))
        with self.assertRaisesRegex(ValueError, 'separate'):
            self.publish()
        self.assertEqual(self.git(self.development, 'for-each-ref', '--format=%(refname)'), '')


if __name__ == '__main__':
    unittest.main()
