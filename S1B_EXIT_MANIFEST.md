# S1b exit roster: distribution / replay / install / distro evidence

Committed tree: `git rev-parse HEAD` = `e3a51c5234c46dc5711933c2dd2ba8550a826f60`
(branch `provisional/s1b-release-identity`, successor to `97ef314f`).
`distro_probe.sh` (repo root, alongside this file) reproduces the three
distro-probe rows below; it needs docker and the `icecc-1.5.90.tar.gz`
dist tarball extracted somewhere accessible.

## 1. Hash-bound distribution archive

`make dist` from the `e3a51c52` tree, in-tree build directory:

```
icecc-1.5.90.tar.gz  sha256=686efd89026941c06b6012894eb972718fd7e565269c8a497bd2fb7b0458e840  bytes=1650163
icecc-1.5.90.tar.xz  sha256=f97cde41a2f1b6ff476d7e15bd702ace1a4a8d01d8243704d2f3cf079ff63278  bytes=1143952
```

Confirmed the tarball's `EXTRA_DIST` actually carries both release-identity
tests (`tar -tzf icecc-1.5.90.tar.gz | grep releaseidentity` ->
`unittests/releaseidentity-source.sh`, `unittests/releaseidentity-artifact.sh`)
-- required for rows 2 and 4 below to be self-contained from the archive
alone, not the git worktree.

## 2. Extracted-archive replay (source-tree objects NOT reused)

Extracted `icecc-1.5.90.tar.gz` into a directory with no relation to the
git worktree or its `build/`, then out-of-tree configured (the deps-root
recipe: `CPPFLAGS=-isystem $dep/include LDFLAGS=... PKG_CONFIG_PATH=...
../icecc-1.5.90/configure --with-boost=$dep --without-libcap-ng
--without-man`) and built `services` -> `cache` -> `client` (`icecc`).

```
CONFIGURE-EXIT=0  SERVICES-EXIT=0  CACHE-EXIT=0  ICECC-EXIT=0
./client/icecc --version -> ICECC 1.5.90
```

Proof this is a genuinely fresh build, not the worktree's `build/`
reused: different sha256 and different mtime for `client/icecc` /
`client/main.o` between the two trees:

```
replay   client/icecc  sha256=c3ce67de0156decc0c5c6a2ed75bbaf8488d61d2b708eb82e9e226ef474a6d46
worktree client/icecc  sha256=9fb570217fc7d934f2314c3e58d07ecb743f554e870f688f449c9889cd10797a
```

Both release-identity tests, run from the **replay's own extracted
copies**, against the replay build:

```
$ ICECC_TEST_TOP_SRCDIR=<replay>/icecc-1.5.90 <replay>/icecc-1.5.90/unittests/releaseidentity-source.sh
ok - configure.ac has exactly one active major definition
ok - configure.ac has exactly one active minor definition
ok - configure.ac has exactly one active micro definition
ok - extracted major.minor.micro = 1.5.90
ok - configure.ac composes to exactly 1.5.90
PASS: development release identity is 1.5.90 (source only)
SOURCE_EXIT=0

$ ICECC_RELEASE_IDENTITY_BIN=<replay>/build-replay/client/icecc <replay>/icecc-1.5.90/unittests/releaseidentity-artifact.sh
ok - <replay>/build-replay/client/icecc exists and is executable
ok - <replay>/build-replay/client/icecc --version is exactly 'ICECC 1.5.90'
PASS: built release identity artifact is exactly ICECC 1.5.90
ARTIFACT_EXIT=0
```

## 3. Installed identity (`make install DESTDIR=...`)

**Deviation from the assigned path**: this harness's own runtime policy
requires temp/scratch output to go under the session scratchpad rather than
system `/tmp` ("Only use /tmp if the user explicitly requests it" -- a
teammate suggesting the path in passing doesn't meet that bar, and
`/tmp` on this host has previously filled from unrelated scratch buildup).
Used an equivalent scratchpad path (`.../scratchpad/s1b-install`) instead
of `/tmp/s1b-install`; functionally identical, DESTDIR-relative layout is
unaffected. Full replay tree built (`make -j8`, all components, exit 0),
then:

```
$ make install DESTDIR=<destdir>
INSTALL-EXIT=0
```

Installed tree: `usr/local/{bin/icecc,bin/icecc-create-env,
bin/icecc-test-env,sbin/iceccd,sbin/icecc-scheduler,
lib/libicecc.{a,la},lib/pkgconfig/icecc.pc,include/icecc/*.h,
libexec/icecc/compilerwrapper}`.

```
installed icecc --version           -> ICECC 1.5.90
installed icecc sha256               -> c3ce67de0156decc0c5c6a2ed75bbaf8488d61d2b708eb82e9e226ef474a6d46
                                         (identical to the replay build's client/icecc -- install copies, does not relink)
installed icecc-scheduler --version  -> (rejects --version as unrecognized, but still banners) ICECREAM scheduler 1.5.90
installed icecc-scheduler sha256     -> 1cda2b62e1a3c5161ae6046a0315b384895907216a75a68fe484ecc479355cb4
installed iceccd (strings-probed)    -> ICECREAM daemon 1.5.90 starting up (nice level ...
installed iceccd sha256              -> 2250dbd97571b19c3ff26beb42a80d42f083e05476cbf4942bb778255014cf11
installed lib/pkgconfig/icecc.pc     -> Version: 1.5.90
```

**Assertion: installed icecc reports exactly `ICECC 1.5.90` -- HOLDS**
(exact string, verified above).

## 4. Distro probe manifests

All three rows below are reproducible via `distro_probe.sh SRC_DIR WORK_DIR
{ubuntu22,ubuntu24,fedora40}` and were each independently re-verified
through that exact checked-in script (not just the ad-hoc commands used
first) before this manifest was written.

### 4a. Ubuntu 22.04 (pinned farm-node container)

Image `icecream/farm-node:ubuntu22-gcc11-boost174` -- every icecc build
dependency (libarchive-dev, libboost-dev, libcap-ng-dev, liblzo2-dev,
libxxhash-dev, libzstd-dev, gcc/g++ 11.4.0, autoconf 2.71, automake 1.16.5)
is already a system package; no extra install step needed; plain
`./configure` (no custom prefix) suffices.

```
CONFIGURE-EXIT=0  SERVICES-EXIT=0  CACHE-EXIT=0  CLIENT-EXIT=0
./client/icecc --version -> ICECC 1.5.90
SOURCE_TEST_EXIT=0    (PASS: development release identity is 1.5.90 (source only))
ARTIFACT_TEST_EXIT=0  (PASS: built release identity artifact is exactly ICECC 1.5.90)
```

### 4b. Ubuntu 24.04

`docker pull ubuntu:24.04` on q3 succeeded (registry reachable) -> real
probe, not BLOCKED-ENV. Dependency manifest installed via apt:
`g++ gcc make autoconf automake libtool pkg-config libzstd-dev liblzo2-dev
libarchive-dev libboost-dev libcap-ng-dev libxxhash-dev` (resolves to
gcc/g++ 13.3.0, boost 1.83.0 on this image).

**One real hiccup, honestly recorded**: the first attempt ran `apt-get
install` and `configure` in two separate `docker run --rm` invocations;
`--rm` containers do not persist installed packages between invocations,
so the second (configure) container had nothing installed:

```
CONFIGURE-EXIT=1
...
checking for gcc... no
checking for cc... no
configure: error: no acceptable C compiler found in $PATH
```

This is an artifact of my own two-invocation mistake, not a real Ubuntu
24.04 / icecc-1.5.90 incompatibility. Corrected by running install+configure
in a single `docker run` invocation (codified in `distro_probe.sh`), then
went past probe level to a full build + both tests, all green:

```
CONFIGURE-EXIT=0  SERVICES-EXIT=0  CACHE-EXIT=0  CLIENT-EXIT=0
./client/icecc --version -> ICECC 1.5.90
SOURCE_TEST_EXIT=0    (PASS: development release identity is 1.5.90 (source only))
ARTIFACT_TEST_EXIT=0  (PASS: built release identity artifact is exactly ICECC 1.5.90)
```

### 4c. Fedora 40

`docker pull fedora:40` on q3 succeeded (registry reachable) -> real probe,
not BLOCKED-ENV. Dependency manifest installed via dnf: `gcc gcc-c++ make
autoconf automake libtool pkgconf-pkg-config libzstd-devel lzo-devel
libarchive-devel boost-devel libcap-ng-devel xxhash-devel`. Configure
succeeded on the first attempt (install+configure done in one `docker run`,
learning applied from the Ubuntu 24.04 row above); went past probe level to
a full build + both tests, all green:

```
CONFIGURE-EXIT=0  SERVICES-EXIT=0  CACHE-EXIT=0  CLIENT-EXIT=0
./client/icecc --version -> ICECC 1.5.90
SOURCE_TEST_EXIT=0    (PASS: development release identity is 1.5.90 (source only))
ARTIFACT_TEST_EXIT=0  (PASS: built release identity artifact is exactly ICECC 1.5.90)
```

## Summary

| Row | Result |
|---|---|
| 1. dist archive hashes | recorded, both formats |
| 2. extracted-archive replay | fresh build (proven non-reused), both tests PASS |
| 3. installed identity | icecc/iceccd/icecc-scheduler/pkgconfig all report 1.5.90; installed icecc exact-string assertion HOLDS |
| 4a. Ubuntu 22.04 (pinned) | full build + both tests PASS |
| 4b. Ubuntu 24.04 | full build + both tests PASS (after a self-corrected recipe mistake, recorded above) |
| 4c. Fedora 40 | full build + both tests PASS |

No row is BLOCKED-ENV -- both non-pinned registries were reachable from q3.
