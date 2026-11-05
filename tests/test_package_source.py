"""Publication exports reject payloads and include new code without Git history."""
import importlib.util
import io
from pathlib import Path
import struct
import tarfile
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location('package_source', ROOT/'tools/package_source.py')
PACKAGE = importlib.util.module_from_spec(SPEC); SPEC.loader.exec_module(PACKAGE)


class SourcePackage(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='lhx-publication-')
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.paths = sorted(PACKAGE.REQUIRED)
        for name in self.paths:
            path = self.root/name;path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text('source or notice\n')

    def add(self, name, data):
        path = self.root/name;path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data);self.paths.append(name)

    def test_snapshot_contains_new_source_and_not_coordination_or_history(self):
        self.add('runtime/new_feature.c', b'int feature(void){return 1;}\n')
        self.add('AGENTS.md', b'local coordination details\n')
        self.add('SOURCE-MANIFEST.json', b'old manifest\n')
        files = PACKAGE.inspect(self.root, self.paths)
        one, two = self.root/'one.tar.gz', self.root/'two.tar.gz'
        PACKAGE.archive(files, one);PACKAGE.archive(files, two)
        self.assertEqual(one.read_bytes(), two.read_bytes())
        with tarfile.open(one) as tar:
            names = tar.getnames()
            self.assertIn('lhx-native/runtime/new_feature.c', names)
            self.assertIn('lhx-native/SOURCE-MANIFEST.json', names)
            self.assertNotIn('lhx-native/AGENTS.md', names)
            self.assertFalse(any('/.git/' in name for name in names))
            self.assertEqual(tar.getmember('lhx-native/run.sh').mode, 0o755)
            self.assertEqual(tar.getmember('lhx-native/README.md').uid, 0)

    def test_forced_tracked_game_file_is_refused(self):
        self.add('bin/LHX.EXE', b'MZgame payload')
        with self.assertRaisesRegex(ValueError, 'allowlist'):
            PACKAGE.inspect(self.root, self.paths)

    def test_generated_payload_cannot_be_disguised_as_runtime_source(self):
        self.add('runtime/data.c', b'const uint8_t g_image[] = {77,90};\n')
        with self.assertRaisesRegex(ValueError, 'original-image payload'):
            PACKAGE.inspect(self.root, self.paths)

    def test_only_approved_png_paths_are_allowed(self):
        self.add('docs/game-art.png', b'not a screenshot')
        with self.assertRaisesRegex(ValueError, 'allowlist'):
            PACKAGE.inspect(self.root, self.paths)

    def test_symlink_cannot_import_an_external_file(self):
        (self.root/'runtime').mkdir()
        (self.root/'runtime/external.h').symlink_to('/etc/hosts')
        with self.assertRaisesRegex(ValueError, 'regular in-repository'):
            PACKAGE.inspect(self.root, self.paths+['runtime/external.h'])

    def test_missing_notices_are_refused(self):
        with self.assertRaisesRegex(ValueError, 'Missing required'):
            PACKAGE.inspect(self.root, [p for p in self.paths if p != 'THIRD-PARTY.txt'])


if __name__ == '__main__':
    unittest.main()
