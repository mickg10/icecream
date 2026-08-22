# P48 to Protocol-50 product convergence

This branch is the final standalone Protocol-50 endpoint convergence onto the accepted P48
product. It deliberately does not attach that endpoint to scheduler, daemon, wrapper, or
compiler-pipe paths; those connections are M3. The P48 and R2 source histories have no Git
merge base, so every transplanted layer and resolution is recorded below.

## Fixed accepted inputs

- Destination R3 head: `fa03fa95d5f9f98a791897e3d66025a802fd85a1`.
- Selected R2 endpoint head: `c5575dc832ea7c1890344a3c930271ff807d4b7f`.
- The PR22, PR23, and PR24 endpoint trees were not merged or copied.
- R4 simulator and dashboard files are not part of this convergence.

## Commit mapping

| R2 source | Local convergence | Content |
| --- | --- | --- |
| `4517bf92` | `ee645f28` | M0 wire, identities, actions, digest, and initial formal core |
| `8e39ba1e` | `f49090d3` | M1 in-memory P29 transaction |
| `50c42f87` | `e1fb86c6` | trailing-FILL closure |
| `6c6c6f3f` | `8dd9d0e3` | transaction correspondence |
| `8247176d` | `35b5717e` | accepted formal suite |
| `bdbdc23d` | `08a884f9` | negotiation and empty-session correction |
| `bbf97a06` | `5305f782` | self-contained route-cursor mutation gate |
| `179fe7b6` | `740e6a05` | bounded ZSTD_TU dialogue core |
| required source inputs | `e037b87c`, `01254da7` | convergence record and three census/OnlineS1 inputs |
| `64f58600` | `2a1a3b06` | bounded reconciled loopback endpoint |
| `0e34d13d` | `091c5745` | bounded reusable Zstd decode windows |
| `8ae59e6f` | `c2b6f317` | staged server session activation |
| `da2ad422` | `b8d1e936` | publication and replay identity |
| `c8cc6f01` | `db9a4dfc` | endpoint-only Boost linker-path portion |
| `00190466` | `835dbb1d` | R2 endpoint validation corrections |
| `c5575dc8` | `89bede78` | exact asynchronous completion correspondence |
| convergence-only | `35a1608a` | endpoint-target Boost flags and P48 scope-gate registration |
| convergence-only | `f28822bc` | nested package-discovery forwarding for the P48 scope gate |
| convergence-only | `5be4a497` | preserve default pkg-config directories in the nested scope gate |

The M1 core consumes the header-only OnlineS1 implementation, and the M0 layout census reads
two retained measurement inputs. Only these missing dependencies came from the source tree:

- `capability/grouprlz/p29_online_s1.h`, blob
  `bde2fcc10e0892476f1f1954b1a02e7b6ea0690c`;
- `capability/grouprlz/runway-census.tsv`, blob
  `b333fbd8a83bf8580f8ffa9acc21b5d5d2602cfa`;
- `capability/distribution/firefox-corrected.compile-trace.tsv`, blob
  `46de69faa54bd7d565d40e72907d62549f9c9c8b`.

No simulator implementation or broader research harness was transplanted.

## Baseline and build resolution

R3 already supplies C++23, Boost 1.74, Boost.Asio coroutine/TCP probes, prefix isolation, and
its configuration-independent scope gate. Therefore the R2 baseline commits `4309f420` and
`498d7619` were not replayed. The configure portion of `c8cc6f01` was also superseded by R3's
stronger save/restore of `CPPFLAGS`, `LDFLAGS`, and `LIBS`; only its endpoint linker assignment
was retained.

Boost does not leak into global build flags. `p50_endpoint.cpp` lives in the separate
`libp50endpoint.a` target and alone receives `BOOST_CPPFLAGS`. The `p50endpoint` test receives
`BOOST_CPPFLAGS`, `BOOST_LDFLAGS`, and `BOOST_LIBS`; other product and Protocol-50 targets do
not. The existing P48 prefix-scope gate was minimally extended to build those consumers,
prove that they receive the prefix, reject its appearance on any other target, and forward
the now-mandatory M0 xxHash dependency into its nested configure. The nested gate forwards
an explicitly configured `PKG_CONFIG_LIBDIR`, but does not turn an absent directory override
into an empty override that would hide pkg-config's ordinary system directories.

All other conflicts were additive build/package-list unions:

- Top-level `Makefile.am` retains every P48 document, package tool, web GUI test, stress test,
  and compose test, while adding `cache`, the exact M0/M1 inputs, and formal checks.
- `README` retains the P48 requirements and adds the independent xxHash contract.
- `unittests/Makefile.am` retains all P48 tests and relocatable test environment, then adds the
  five P50 binaries, trace gate, and endpoint-only dependency flags.

## Tree-equivalence audit

The implementation, formal specifications, exact dependencies, and focused tests are
byte-identical to R2 `c5575dc8`:

```sh
git diff --exit-code c5575dc832ea7c1890344a3c930271ff807d4b7f HEAD -- \
  cache ':(exclude)cache/Makefile.am' \
  ':(exclude)cache/P48_P50_CONVERGENCE.md' \
  services/digest128.cpp services/digest128.h \
  capability/grouprlz/p29_online_s1.h capability/grouprlz/runway-census.tsv \
  capability/distribution/firefox-corrected.compile-trace.tsv \
  unittests/p50_wire_test.cpp unittests/p50_slice0_test.cpp \
  unittests/p50_zstd_test.cpp unittests/p50_zstd_window_test.cpp \
  unittests/p50_endpoint_test.cpp unittests/p50_trace_check.sh \
  unittests/digest128_bench.cpp
```

Every pre-existing P48 file under `scheduler/`, `daemon/`, `client/`, `compilerwrapper/`, and
`webgui/`, and every pre-existing compose/stress product file, remains byte-identical to
`fa03fa95`. The only pre-existing P48 harness changed after conflict resolution is
`unittests/boost-prefix-scope.sh`, for the documented endpoint/xxHash registration above.
Top-level and subdirectory Automake/configure inputs differ only where the new standalone
cache library, dependencies, tests, and distribution inventory require registration.

## M3 boundary

This head provides a standalone, tested loopback endpoint. M3 still owns assignment identity,
cache advertisement and selection, daemon/wrapper attachment, compiler input piping,
compiled-result attachment, endpoint process lifetime, fallback selection, persistence,
eviction, restart, and reroute integration. None of those product paths is implemented here.

## Required convergence gates

Run from clean GCC and Clang source archives, not a configured source tree:

```sh
./autogen.sh
mkdir build && cd build
../configure --with-boost=/path/to/boost-prefix --without-libcap-ng --without-man
make -j8
make -j8 check
make -C unittests check \
  TESTS='p50wire p50slice0 p50zstd p50zstdwindow p50endpoint p50_trace_check.sh'
```

Strict-warning, address/undefined-behavior instrumentation, `distcheck`, archive inventory,
formal checks, and the relevant P48 product harnesses are retained external evidence rather
than claims embedded in this source document.
