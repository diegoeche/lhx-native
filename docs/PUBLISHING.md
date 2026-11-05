# Preparing a public source snapshot

The public package contains project source, vendored source/licence notices,
documentation and the three approved gameplay screenshots. It excludes Git
metadata/history, local agent coordination, game files, generated translations,
native executables, saves, optional art packs and diagnostic captures.

The screenshots are documentation images with [separate attribution](screenshots/README.md).
They do not change the requirement to supply your own supported copy of LHX.

## Audit and package

From the development checkout:

```sh
python3 tools/package_source.py --check
python3 tools/package_source.py --output dist/lhx-native-source.tar.gz
```

The packager reads current working-tree files, including new non-ignored source
files. It uses an explicit allowlist and rejects unexpected binaries, generated
original-image payloads, private keys and symlinks. It does not stage, commit,
change remotes, upload or publish anything. The archive has deterministic
metadata and a SHA-256 manifest. It deliberately carries no private development
history; use this clean snapshot when preparing a new public repository.

## Review before publication

Review the front-page README, actual screenshots, MIT licence and retained
upstream notices together. Confirm that the archive file list contains only the
intended source/documentation, then run tests from an extracted copy without
locally installed game files:

```sh
mkdir -p /tmp/lhx-source-review
tar -xzf dist/lhx-native-source.tar.gz -C /tmp/lhx-source-review
cd /tmp/lhx-source-review/lhx-native
python3 -m unittest discover -s tests -v
```

Integration builds and game screenshots require the owner's local media and
optional resources; their outputs must stay out of the public snapshot.
GitHub source CI runs synthetic tests without game files and skips checks that
need the original release. It does not build or upload a translated game binary.

Actual repository creation, visibility changes, pushes and release publication
are separate from preparation and should follow the maintainer's review.
