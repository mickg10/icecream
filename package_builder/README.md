# Package builders (Docker)

This directory contains Docker Compose environments to build installable packages
from the current git checkout, using the target distro's packaging as a base.

Outputs are written to each builder directory's `out/` folder.

## Reproducibility contract

- Source0 is a **clean `git archive HEAD` export** (`make_source_tree.sh`),
  bootstrapped with `autoreconf -fi` at staging time so it carries generated
  `configure`/`Makefile.in` even for build roots whose own Autotools are too
  old to bootstrap (Fedora 28).  Nothing untracked -- stale generated files,
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

## Fedora 28 (unsupported)

Fedora 28's archived repositories provide GCC 8 and Boost 1.66. They cannot
satisfy this line's C++23 and Boost 1.74 baseline. Both the build and verify
commands therefore refuse deterministically with exit status 78; the builder
does not download an unpinned replacement toolchain.

## Fedora (latest) (rpm)

```bash
cd package_builder/fedora-latest
docker compose run --rm --build rpm
docker compose run --rm --build verify
ls -lh out/
```
