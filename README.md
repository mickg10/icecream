# Icecream / Sorbet 1.5.0

Icecream distributes C and C++ compilation across a build farm. A scheduler
selects workers; a local daemon serves the submitting client; worker daemons
run the compiler environment supplied by the client.

This branch is `sorbet_v1.5` in
[mickg10/icecream](https://github.com/mickg10/icecream). It extends
[upstream Icecream](https://github.com/icecc/icecream) with Protocol-50 cache
transport and its integration tests. Package version, ordinary wire protocol,
and CacheWire revision are separate: **1.5.0**, **50**, and **1** respectively.

## Build and test

On a Linux Docker host, install Docker, Git, Make and uv 0.9.21. From a full
checkout, select storage for temporary files and retained build/test results:

```sh
mkdir -p /path/to/scratch
ICEFARM_TMPDIR=/path/to/scratch make qa
```

The directory must exist, be writable and be absolute. Docker keeps its own
image/layer storage in its configured data directory; the command reports and
checks that location rather than moving it.

Make runs `uv sync --locked` using the checked-in Python version and `uv.lock`.
No host Python or global pip install is needed. Python downloads, the package
cache and per-checkout environments default to the selected scratch directory.
Use `make python-sync` to prepare just Python, or
`sh dev/python.sh --exec COMMAND ...` to run another tool in that environment.
Native test helpers, local research scripts and formal checks use the same
environment; Docker, compilers and Java remain separate dependencies.

The root [farm.json](farm.json) defaults to Ubuntu 24.04, two build jobs and an
8 GiB build-container limit. Override it with `FARM=/path/to/farm.json`.
`make qa` builds the checkout, runs native and Python tests, builds genuine
P43 and current product images, and runs five isolated mixed-version/profile
Docker cases. It does not deploy the external qualification farm.
`make dev-bootstrap` builds and installs into retained scratch without QA.

See [the developer guide](dev/README.md) for repository/proxy selection,
Docker save/load, offline operation, prerequisites and result files.
Ubuntu 24.04 has been tested on nas642 and q4; selecting another profile is
not a claim that it has passed the same gate.

For native builds, install the dependencies checked by `configure.ac`:
C++23, Autotools, Boost 1.74+, xxHash, zstd, LZO and libarchive. Optional
features have additional configure checks. Use an out-of-tree build on the
chosen storage:

```sh
export ICEFARM_TMPDIR=/path/to/existing/scratch
export TMPDIR="$ICEFARM_TMPDIR"
source_dir=$PWD
./autogen.sh
build_dir=$(mktemp -d "$ICEFARM_TMPDIR/icecream-build.XXXXXX")
cd "$build_dir"
"$source_dir/configure" --prefix=/path/to/install --without-man
make -j2
sh "$source_dir/dev/python.sh" --exec make -j2 check
make install
```

The Docker QA path supplies its own toolchain and test dependencies. Native
`make check`, Python harness tests, TLC checks and external farm qualification
are distinct gates; none alone proves all of the others.

## Running a farm

Run `icecc-scheduler` on the scheduler host and `iceccd` on submitting and
worker hosts. A submitting-only daemon can use `--no-remote`. Consult the
installed manual pages and each executable's `--help` for service options;
startup service names and wrapper paths depend on the package/prefix.

Use the compiler wrappers installed under your prefix, or invoke the client
explicitly, for example:

```sh
icecc g++ -c example.cpp -o example.o
```

Choose build parallelism to fit the available farm and the submitting host's
preprocessing capacity. Unlimited parallelism can make the build slower.
Ordinary operation permits local compilation when remote work is unavailable.
For tests or builds that must not silently fall back, `ICECC_REMOTE_REQUIRED=1`
requires the remote path for eligible distributed compilations; it does not
make linking or unsupported compiler invocations remotely compilable.

Use a shared netname for nodes in one farm, or an explicit scheduler host.
The default ports are TCP 10245 for workers, TCP 8765 for the scheduler,
UDP 8765 for discovery, and TCP 8766 for the optional scheduler control
interface. Limit access to the intended build network. Compiler-environment
installation on workers requires the daemon's supported privileged setup;
starting an arbitrary unprivileged worker is not equivalent.

## Compiler environments and wrappers

Icecream can prepare a native compiler environment automatically. For an
explicit environment, use `icecc --build-native` or `icecc-create-env`, then
set `ICECC_VERSION` to the resulting archive. The archive must contain a
toolchain that can execute on the selected worker and produce the desired
target code. The platform selector describes where that toolchain executes,
not the target architecture it emits.

For cross compilers, set `ICECC_CC` and `ICECC_CXX` to the intended compiler
and supply the corresponding environment. See `icecc --help` and the
installed manuals for multi-platform/multi-toolchain `ICECC_VERSION` syntax.

To combine with ccache, set `CCACHE_PREFIX=icecc` and use ccache's compiler
wrappers. Do not put both sets of compiler-name wrappers ahead of the real
compiler: that can create a wrapper recursion.

For diagnostics use `ICECC_DEBUG=debug`; daemon and scheduler verbosity is
controlled by repeated `-v`. Check the actual compiler invocation, compiler
environment, scheduler registration and selected worker before increasing
parallelism.

## Protocols, testing and documentation

- [P50 protocol and CacheWire](cache/P50_PROTOCOL.md): `P29V1=1`,
  `ZSTD_TU=2`, `ZSTD_ROUTE=3`; `OFF` selects ordinary legacy transport,
  not uncompressed bytes.
- Codec specifications: [P29v1](cache/codec/P29V1_FORMAT.md) and
  [ZSTD_TU / ZSTD_ROUTE](cache/codec/ZSTD_FORMATS.md).
- [Assignment identity and mixed versions](doc/p50-assignment-identity.md):
  strict P50 admission is explicit; P43 compatibility does not imply P50
  cache support.
- [Local harness tests](farmharness/integration/tests/README.md):
  `make test-harness-fast` and `make test-harness-thorough`, both with
  explicit `ICEFARM_TMPDIR`.
- [TLA+/TLC models](cache/formal/README.md): bounded model checks and their
  assumptions; these are not an unbounded proof of the C++ implementation.
- [Package builders](package_builder/README.md) and
  [release procedure](ReleaseProcess.md).
- [Recorded validation and remaining work](PROJECT_STATE.md): exact tested
  source/image identities, rather than a blanket claim about every checkout.
- [Research tools and fixtures](research/README.md): historical replay and
  experiments, separate from current product qualification.

Report branch-specific problems at
[mickg10/icecream/issues](https://github.com/mickg10/icecream/issues).
