# Repository publication and source packages

## Repository synchronization

Gitea is the development repository (`origin`). The public
[GitHub repository](https://github.com/diegoeche/lhx-native) receives audited
source snapshots through the `github` remote. Development history and agent
coordination stay on Gitea; public commits extend the existing public history.
The two repositories therefore have different commit IDs.

After reviewing and committing project changes on development `main`, publish
both repositories with:

```sh
python3 tools/publish_source.py --push
```

The command requires a clean checkout and separate `origin` and `github`
remotes. It pushes the development commit to Gitea, then prepares an isolated
public checkout containing only the source-package allowlist and a regenerated
`SOURCE-MANIFEST.json`. Removed files are removed from the public snapshot.
Normal fast-forward pushes preserve public history, and an unchanged snapshot
creates no new public commit. A failure stops publication and reports which
step completed; it never force-pushes or rolls back a successful Gitea update.

Use `python3 tools/publish_source.py --check` to audit working-tree source
without committing, connecting to either server or publishing. Ignored game
data, art packs, build outputs, saves and local reports remain excluded.

There is no automatic server mirror or scheduled synchronization. A plain
`git push origin main` updates Gitea alone; use the publish command to update
both services. Public branches and tags beyond `main` are not synchronized.
Check staged files before committing: ignoring a file does not remove an
already tracked file from Git history.

## Source packages

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
metadata and a SHA-256 manifest. The archive contains no Git history and can be
distributed separately from the mirrored repository.

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

Creating an archive does not push repository changes or publish a release.
Review the package before sharing it or attaching it to a release.
