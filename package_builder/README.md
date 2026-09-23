# Package builders (Docker)

This directory contains Docker Compose environments to build installable packages
from the current git checkout, using the target distro's packaging as a base.

Outputs are written to each builder directory's `out/` folder.

Set `ICEFARM_TMPDIR` to an existing writable absolute scratch directory before
running Compose. Supported builder images include uv 0.9.21; their commands
run through `/src/dev/python.sh`, with scratch mounted at `/scratch`. This
uses the repository's `uv sync --locked` environment for package checks too.
The first sync needs network access or a populated uv cache. Fedora 28 remains
an unsupported negative-control row and does not attempt this setup.

## Reproducibility contract

- Source0 is a **clean `git archive HEAD` export** (`make_source_tree.sh`),
  bootstrapped with `autoreconf -fi` at staging time so it carries generated
  `configure`/`Makefile.in` even for build roots whose own Autotools are too
  old to bootstrap. Nothing untracked -- stale generated files,
  host-built executables, local edits -- can reach the package.  Uncommitted
  changes produce a loud warning and are NOT built.
- The staged tree records its revision in `.source-revision`.
- Each build **empties its per-run output** and writes `manifest.txt` naming
  exactly the packages it produced; the verifiers install exactly that list
  and fail on any unexpected package file in `out/`.
- The verifiers prove the *services*, not just an object file: process
  liveness, scheduler registration, and the compile reaching the scheduler
  (`NEW <id> client=`), with logs dumped on failure.
- Supported builders explicitly install the current product baseline instead
  of assuming the distro's historical `icecc` build dependencies are still
  sufficient: C++23, Boost 1.74 or newer, xxHash, Zstd, LZO, and libarchive.
  Before source staging, the committed `probe_build_requirements.sh` compiles,
  links, and runs a small feature probe. Its compiler and library versions are
  written to `manifest.meta` beside the package digests.
- When `RELEASE_TARBALL` is set, every supported builder verifies its sidecar
  digest and records the same `release_tarball_sha256` in `manifest.meta`.

The deletion-sensitive package contract can be checked without a container:

```bash
./package_builder/test-build-requirements.sh
```

## Proxies / TLS

The builders pass through `http_proxy`, `https_proxy`, and `no_proxy` (and their
uppercase variants) from your environment.

If you're behind a proxy that breaks TLS verification, set:

```bash
export ICECREAM_BUILDER_INSECURE=1
```

This disables TLS certificate verification for `apt`/`dnf` inside the builder
containers.

## Ubuntu 22.04 (deb)

```bash
cd package_builder/ubuntu22.04
docker compose run --rm --build deb
docker compose run --rm --build verify
ls -lh out/
```

## Ubuntu 24.04 (deb)

```bash
cd package_builder/ubuntu24.04
docker compose run --rm --build deb
docker compose run --rm --build verify
ls -lh out/
```

## Fedora 28 (unsupported negative-control row)

Fedora 28's archived repositories provide GCC 8 and Boost 1.66. They cannot
satisfy this line's C++23 and Boost 1.74 baseline. This directory remains only
as a deletion-sensitive negative-control test: both scripts refuse
deterministically with exit status 78. It is not a supported builder, is not
included in supported package aggregates, and must not be used as a release
command. The builder does not download an unpinned replacement toolchain.

## Fedora (latest) (rpm)

```bash
cd package_builder/fedora-latest
docker compose run --rm --build rpm
docker compose run --rm --build verify
ls -lh out/
```
