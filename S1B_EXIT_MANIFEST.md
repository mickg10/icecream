# S1b exit roster: distribution / replay / install / distro evidence

Branch `provisional/s1b-release-identity`; commit lineage
`97ef314f` (version bump) -> `e3a51c52` (test-gate successor) ->
`050845ba` (this file's rows 1-4, build-tree distro probes) -> this commit
(row 5, per-distro INSTALLED identity, BigOracle successor on `050845ba`).
`distro_probe.sh` (repo root, alongside this file) reproduces the three
build-tree distro-probe rows (4a-4c); `distro_installed_identity.sh`
(repo root) reproduces the fresh-by-construction installed-identity rows
(5a-5c); both need docker and the `icecc-1.5.90.tar.gz` dist tarball
extracted somewhere accessible.

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

## 5. Per-distro INSTALLED identity, fresh-by-construction (BigOracle successor on `050845ba`)

BigOracle HOLD on `050845ba`: rows 4a-4c above prove **build-tree** identity
(`./client/icecc --version` run straight out of the build dir) and reuse
`mkdir -p` build/DESTDIR paths across runs -- not fresh-by-construction, and
not proof of what a real `make install` places on a system. This section is
the bounded successor: `make install DESTDIR=...` run *inside* each distro
container (same invocation as configure/build, per the Ubuntu 24.04 `--rm`
lesson from 4b -- installed packages, and now also the identity probes
themselves, do not survive into a later, separate `docker run --rm`), against
a build dir and DESTDIR that are unconditionally `rm -rf`'d and recreated
empty *inside the container as root* immediately beforehand (root-owned
output from a `-u 0:0` container is not reliably removable by a non-root host
shell, so the wipe has to happen where the files were made). Rows 4a-4c and
`distro_probe.sh` are left exactly as they were; nothing here replaces them.

Script: `distro_installed_identity.sh` (repo root, alongside `distro_probe.sh`),
sha256 = `dda444c8218c2a305e7ec5bcaf7847c1d8e7c097ab05fa20af530acf0b0fa3cf`.
Usage: `distro_installed_identity.sh SRC_DIR WORK_DIR {ubuntu22,ubuntu24,fedora40} [--stale-control]`.
`SRC_DIR` is the extracted `icecc-1.5.90` dist tree (the same one rows 2-4
already used); read-only bind-mounted into every container -- only
build/DESTDIR need to be fresh, per the HOLD's own wording ("the HOLD is
about build-output freshness").

**Three genuine bugs found and fixed while building this successor** (kept
here, not smoothed over, matching how the rest of this manifest reports
real hiccups rather than only final green output):

1. **Read-only `/src` vs. autotools auto-remake.** This tree does not use
   `AM_MAINTAINER_MODE` (`configure.ac:24` is a plain `AM_INIT_AUTOMAKE`), so
   `--disable-maintainer-mode` does not exist and the generated Makefiles'
   `$(srcdir)/Makefile.in: $(srcdir)/Makefile.am $(am__configure_deps)`
   auto-remake rule is unconditionally live. When it judged the dist
   tarball's extracted timestamps ambiguous, its recipe tried `cd /src &&
   automake --foreign`, which needs to create `/src/autom4te.cache` -- a
   write, correctly refused by the deliberate `:ro` mount:
   `autom4te: error: cannot create autom4te.cache in /src: Read-only file
   system`. A single flat `touch` (equal mtimes for every file) did not
   reliably prevent this. Fixed with a two-tier `touch`, both timestamps
   safely in the past relative to wall-clock (a future-dated generated tier
   was tried and fails differently -- autoconf's own sanity check rejects a
   newly-created file being older than a "distributed" file): hand-authored
   sources (`Makefile.am`, `configure.ac`, `m4/*.m4`) get `2020-01-01T00:00`,
   autotools-generated outputs (`Makefile.in`, `configure`, `aclocal.m4`,
   `config.h.in`, and the vendored auxiliary scripts) get `2020-01-01T00:10`.
   This is a one-time, idempotent, metadata-only step against `SRC_DIR`,
   applied by the script itself before the first container run (not
   something the operator has to remember to do by hand).
2. **`doc/` needs `asciidoc`/`a2x`, which no image here installs, and the
   dist tarball does not ship pre-built man pages either** -- `make install`
   died in `install-man1` with `cannot stat '/src/doc/icecc.1'`. Unrelated to
   identity verification; fixed with `configure --without-man` (the same flag
   already used elsewhere in this repo's own build harnesses, e.g. the P50
   final-candidate gate script).
3. **Identity probes run in a *separate* fresh `docker run --rm` were
   getting the exact same class of bug as the 4b `--rm` hiccup, one step
   later in the pipeline**: `apt-get`/`dnf`-installed runtime libraries
   (`liblzo2`, `libzstd`, `libboost`, ...) only exist inside the container
   that ran the install command; a later, separate probe container starts
   from the bare image again, so the freshly-installed `icecc` binary
   couldn't even start on Ubuntu 24.04: `error while loading shared
   libraries: liblzo2.so.2: cannot open shared object file`. Separately,
   `strings` (used to identity-probe `iceccd`, which has no `--version` flag)
   was not installed on either bare image at all. Fixed by moving every
   identity probe (`icecc --version`, the `iceccd` strings-probe,
   `icecc-scheduler --version`) into the *same* container invocation as
   configure/build/install, writing results to files under `/build` that the
   outer script reads back afterward, and adding `binutils` to the Ubuntu
   24.04/Fedora 40 dependency-install lists (Ubuntu 22.04's pinned farm-node
   image already ships it). `--stale-control`'s own probe legitimately stays
   a separate fresh container -- its planted artifact is a dependency-free
   `#!/bin/sh` script, so it never hits either problem.

   (A fourth issue, cosmetic rather than a build/install defect: the gate's
   own final `grep -q '...\tPASS'` checks used a literal backslash-t, which
   this system's `grep` does not treat as a tab even though the `fact()`
   helper writes a real tab byte -- confirmed with `cat -A`. The real
   evidence was already correct; only the gate's own read of it was wrong.
   Fixed by switching both checks to `grep -qE '...[[:space:]]+...'`.)

### 5a. Normal mode -- all three distros, full facts

Every stage exit code, the resolved image digest, every installed-artifact
sha256, every build/install log sha256, and the resolved package inventory
are machine-emitted by the script itself into `facts-normal.txt` per distro
(the `fact()` helper: one `KEY<TAB>VALUE` line per line, not prose). Full
verbatim content of all three:

**Ubuntu 22.04** (`icecream/farm-node:ubuntu22-gcc11-boost174`, digest
`sha256:bdb55d4287a473e3ebfbaa7715a50ee670659777278b8d84c350724e6fa8de58`):

```
CONFIGURE_exit	0
SERVICES_exit	0
CACHE_exit	0
DAEMON_exit	0
SCHEDULER_exit	0
CLIENT_exit	0
INSTALL_exit	0
installed_icecc_version_output	ICECC 1.5.90
installed_icecc_sha256	c4277ee0a3ee255407f42866a87c32d930631a69dec3e83f95fb1b760b0c233e
installed_icecc_identity_check	PASS
installed_usr_local_sbin_iceccd_sha256	8981ae012b90c9a1accb17643545ac2f3ef2d76221e8b1b0c91891f80a3b0a60
installed_usr_local_sbin_icecc-scheduler_sha256	87f090013045174a89f16c8950899e3e6ba1a91bc8aad8ea30006344ba15938c
installed_usr_local_lib_libicecc_a_sha256	619b06a356178a2b74342482bf7a0747bd048c58a0480ba19db0d7c74428f36c
installed_usr_local_lib_pkgconfig_icecc_pc_sha256	0b1f4815fd84c90c73c418588a8cf56ba496f34732c8ff242eb4c3c13ae030d8
installed_iceccd_version_probe	ICECREAM daemon 1.5.90 starting up (nice level
installed_scheduler_version_probe	ICECREAM scheduler 1.5.90
installed_icecc_pc_version_line	Version: 1.5.90
package_inventory: autoconf=2.71-2 automake=1:1.16.5-1.3 g++=4:11.2.0-1ubuntu1
  gcc=4:11.2.0-1ubuntu1 libarchive-dev=3.6.0-1ubuntu1.8 libboost-dev=1.74.0.3ubuntu7
  libcap-ng-dev=0.7.9-2.2build3 liblzo2-dev=2.10-2build3 libtool=2.4.6-15build2
  libxxhash-dev=0.8.1-1 libzstd-dev=1.4.8+dfsg-3build1 make=4.3-4.1build1
  pkg-config=0.29.2-1ubuntu3
```

**Ubuntu 24.04** (`ubuntu:24.04`, digest
`sha256:33ceb71981b602c1a7443a53469e4dba065f7503eab3078a2d7a57a2ab987517`):

```
CONFIGURE_exit	0
SERVICES_exit	0
CACHE_exit	0
DAEMON_exit	0
SCHEDULER_exit	0
CLIENT_exit	0
INSTALL_exit	0
installed_icecc_version_output	ICECC 1.5.90
installed_icecc_sha256	9a14553a161ddba754882eb5cb322222fa4784e1b7fcbb0ca0b6f3af62ffccd9
installed_icecc_identity_check	PASS
installed_usr_local_sbin_iceccd_sha256	f4a0a9ec12bd4faffd6ac73d15cf12cfc701e757cfdf1f829c08e7bf8170f40e
installed_usr_local_sbin_icecc-scheduler_sha256	f4e2a075f2412b2a7a950c1423ea1e88acfd78b376982f2dad2c39fa0d86459e
installed_usr_local_lib_libicecc_a_sha256	811e251a519b9d2651515265795c2e84710de44d3911779620f23b5375314e4b
installed_usr_local_lib_pkgconfig_icecc_pc_sha256	da9e01dd426e34f86ed85f6a2ff98ded1f2b9bdaf0afd709f68288449017f2cc
installed_iceccd_version_probe	ICECREAM daemon 1.5.90 starting up (nice level
installed_scheduler_version_probe	ICECREAM scheduler 1.5.90
installed_icecc_pc_version_line	Version: 1.5.90
package_inventory: autoconf=2.71-3 automake=1:1.16.5-1.3ubuntu1 binutils=2.42-4ubuntu2.10
  g++=4:13.2.0-7ubuntu1 gcc=4:13.2.0-7ubuntu1 libarchive-dev=3.7.2-2ubuntu0.8
  libboost-dev=1.83.0.1ubuntu2 libcap-ng-dev=0.8.4-2build2 liblzo2-dev=2.10-2build4
  libtool=2.4.7-7build1 libxxhash-dev=0.8.2-2build1 libzstd-dev=1.5.5+dfsg2-2build1.1
  make=4.3-4.1build2 pkg-config=1.8.1-2build1
```

**Fedora 40** (`fedora:40`, digest
`sha256:3c86d25fef9d2001712bc3d9b091fc40cf04be4767e48f1aa3b785bf58d300ed`):

```
CONFIGURE_exit	0
SERVICES_exit	0
CACHE_exit	0
DAEMON_exit	0
SCHEDULER_exit	0
CLIENT_exit	0
INSTALL_exit	0
installed_icecc_version_output	ICECC 1.5.90
installed_icecc_sha256	c10bd94825ba9925a651a13c6a79014318001c5e8ecc661117bc7e23189a10f0
installed_icecc_identity_check	PASS
installed_usr_local_sbin_iceccd_sha256	58e0e933684c11a391270ad1cfe0f11dd294f12658ceb7609248fa29afcb5158
installed_usr_local_sbin_icecc-scheduler_sha256	28c1d31a177ec2bc826a53e1caae8eb20ebc900569d4f7ddbd7731902ae88d72
installed_usr_local_lib_libicecc_a_sha256	f100a411443a74689e7dd8b2cd1fce7b7dab594ce2ae4d6b734875f2f253ea6b
installed_usr_local_lib_pkgconfig_icecc_pc_sha256	0b1f4815fd84c90c73c418588a8cf56ba496f34732c8ff242eb4c3c13ae030d8
installed_iceccd_version_probe	ICECREAM daemon 1.5.90 starting up (nice level
installed_scheduler_version_probe	ICECREAM scheduler 1.5.90
installed_icecc_pc_version_line	Version: 1.5.90
package_inventory: gcc-14.2.1-3.fc40.x86_64 gcc-c++-14.2.1-3.fc40.x86_64
  make-4.4.1-6.fc40.x86_64 autoconf-2.71-10.fc40.noarch automake-1.16.5-16.fc40.noarch
  libtool-2.4.7-10.fc40.x86_64 pkgconf-pkg-config-2.1.1-2.fc40.x86_64
  libzstd-devel-1.5.7-1.fc40.x86_64 lzo-devel-2.10-12.fc40.x86_64
  libarchive-devel-3.7.2-7.fc40.x86_64 boost-devel-1.83.0-5.fc40.x86_64
  libcap-ng-devel-0.8.4-4.fc40.x86_64 xxhash-devel-0.8.3-1.fc40.x86_64
  binutils-2.41-38.fc40.x86_64
```

Note the three `installed_icecc_sha256` values are all different (distinct
compiler/library combinations per distro, exactly as expected for a real
recompile), while `installed_icecc_pc_version_line` is `Version: 1.5.90` on
all three and Ubuntu 22.04's/Fedora 40's `icecc.pc` sha256 happen to match
byte-for-byte -- both correct and unsurprising, since that file's content is
just the `/usr/local` prefix and version string, not compiler-specific.

Per-distro `configure.log`/`services.log`/`cache.log`/`daemon.log`/
`scheduler.log`/`client.log`/`install.log`/`package-inventory.txt` sha256
values (not reproduced inline here to keep this section readable) are
preserved in each `WORK_DIR/facts-normal.txt` alongside the container's full
`container-run-normal.log` transcript.

### 5b. KNOWN-CAUGHT stale-build control (`--stale-control`)

Proves the omitted-clean-step failure mode cannot silently pass: reuses an
**already-populated** DESTDIR from a prior normal run *without* wiping it,
overwrites only `usr/local/bin/icecc` with a trivial dependency-free
`#!/bin/sh` script that prints a wrong version, runs *only* the identity
probe (no reconfigure/rebuild/reinstall), and requires the exact-match check
to FAIL. Captured on two distros (different base-image families):

```
$ distro_installed_identity.sh <src> <ubuntu22-workdir> ubuntu22 --stale-control
installed_icecc_version_output	ICECC 0.0.0-STALE-PLANTED
installed_icecc_identity_check	FAIL (expected exactly 'ICECC 1.5.90', got 'ICECC 0.0.0-STALE-PLANTED')
STALE-CONTROL RESULT: RED as required (planted stale artifact was correctly rejected)
SCRIPT-EXIT=1

$ distro_installed_identity.sh <src> <fedora40-workdir> fedora40 --stale-control
installed_icecc_version_output	ICECC 0.0.0-STALE-PLANTED
installed_icecc_identity_check	FAIL (expected exactly 'ICECC 1.5.90', got 'ICECC 0.0.0-STALE-PLANTED')
STALE-CONTROL RESULT: RED as required (planted stale artifact was correctly rejected)
SCRIPT-EXIT=1
```

This is committed as a fail-able row on purpose: `--stale-control` is
expected to, and does, exit non-zero. It is not part of the pass/fail gate
for the release itself.

### 5c. p50 binary-set root linkage (note only -- S4 branch untouched)

**Clarified per BigOracle**: only ONE of the three distro rows is the
designated build provenance for the future S4 P50 binary-set root -- the
pinned Ubuntu 22.04 row (`icecream/farm-node:ubuntu22-gcc11-boost174`),
unless the owner chooses a different one. Ubuntu 24.04 and Fedora 40 are
INSTALLED-IDENTITY COMPATIBILITY rows only: they prove the same source
builds and installs correctly, with the same exact version identity, on
those toolchains too -- they are not each independently expected to also
become S4 roots, and there is no requirement that one S4 root's binaries
equal all three distros' installed binaries (they are genuinely different
compiles, different toolchains, different hashes, by construction -- see
5a). The `installed_icecc_sha256` / `installed_iceccd_sha256` /
`installed_scheduler_sha256` / `installed_libicecc_a_sha256` values for
the **pinned Ubuntu 22.04 row specifically** in 5a are what a future S4
binary-set root claiming to be "1.5.90" must match, once such a root is
built for this release -- these are the first real, hash-bound, installed
(not build-tree) artifacts to exist for 1.5.90. No S4 file was read or
modified to write this note; it is a forward-pointing record only, for
whoever next wires a 1.5.90 binary-set root to reconcile against.

## Summary

| Row | Result |
|---|---|
| 1. dist archive hashes | recorded, both formats |
| 2. extracted-archive replay | fresh build (proven non-reused), both tests PASS |
| 3. installed identity (single build, not fresh-by-construction) | icecc/iceccd/icecc-scheduler/pkgconfig all report 1.5.90; installed icecc exact-string assertion HOLDS |
| 4a. Ubuntu 22.04 (pinned), build-tree only | full build + both tests PASS |
| 4b. Ubuntu 24.04, build-tree only | full build + both tests PASS (after a self-corrected recipe mistake, recorded above) |
| 4c. Fedora 40, build-tree only | full build + both tests PASS |
| 5a. Ubuntu 22.04, INSTALLED, fresh-by-construction | PASS, exact hashes recorded |
| 5a. Ubuntu 24.04, INSTALLED, fresh-by-construction | PASS, exact hashes recorded |
| 5a. Fedora 40, INSTALLED, fresh-by-construction | PASS, exact hashes recorded |
| 5b. stale-build control (Ubuntu 22.04, Fedora 40) | RED as required, committed fail-able row |

No row is BLOCKED-ENV -- both non-pinned registries were reachable from q3.
