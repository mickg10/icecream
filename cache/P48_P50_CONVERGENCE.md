# P48 to Protocol-50 product convergence

Historical paths in this record name the original commits. In the current
tree, `p29_online_s1.h` lives under `cache/codec/`, and the two measurement
TSVs live under `research/measurements/`; their bytes are unchanged.

This branch is the standalone Protocol-50 endpoint/core convergence substep onto the accepted
P48 product. It does not complete Epoch 2 / M2.5 and does not attach the endpoint to scheduler,
daemon, wrapper, or compiler-pipe paths. The P48 and R2 source histories have no Git merge
base, so every transplanted layer and resolution is recorded below.

## Fixed accepted inputs

- Destination R3 head: `fa03fa95d5f9f98a791897e3d66025a802fd85a1`.
- Selected R2 endpoint head: `e1e8798064d11ceb862bd322fd5a9cf78d7ebbe5`.
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
| `e1e87980` | `d4a8cf00` | live asynchronous completion identity validation |
| convergence-only | `35a1608a` | endpoint-target Boost flags and P48 scope-gate registration |
| convergence-only | `f28822bc` | nested package-discovery forwarding for the P48 scope gate |
| convergence-only | `5be4a497` | preserve default pkg-config directories in the nested scope gate |
| convergence-only | `563683e8` | intermediate endpoint/scope interaction diagnosis, superseded below |
| convergence-only | `3d13542a` | omit an unused Boost library path on the header-only endpoint path |

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
`BOOST_CPPFLAGS` plus only the `BOOST_LDFLAGS` and `BOOST_LIBS` required by the selected Boost
link mode; other product and Protocol-50 targets do not. On the selected header-only
Boost.System path both linker variables are empty. The existing P48 prefix-scope gate was
minimally extended to build those consumers,
prove that they receive the prefix, reject its appearance on any other target, and forward
the now-mandatory M0 xxHash dependency into its nested configure. The nested gate forwards
an explicitly configured `PKG_CONFIG_LIBDIR`, but does not turn an absent directory override
into an empty override that would hide pkg-config's ordinary system directories. The gate
keeps all incomplete lzo, Zstd, and libarchive decoys present through the actual endpoint
link. On the header-only path, that link must consume the Boost include path without receiving
the unused Boost library path, so its intentional dependencies cannot be redirected.

All other conflicts were additive build/package-list unions:

- Top-level `Makefile.am` retains every P48 document, package tool, web GUI test, stress test,
  and compose test, while adding `cache`, the exact M0/M1 inputs, and formal checks.
- `README` retains the P48 requirements and adds the independent xxHash contract.
- `unittests/Makefile.am` retains all P48 tests and relocatable test environment, then adds the
  five P50 binaries, trace gate, and endpoint-only dependency flags.

## Tree-equivalence audit

The implementation, formal specifications, exact dependencies, and focused tests are
byte-identical to accepted R2 `e1e87980`. The final bounded R2 correction shares the
production completion-state checks with the clause-level endpoint tests; it applied without
conflict after the `c5575dc8` convergence baseline:

```sh
git diff --exit-code e1e8798064d11ceb862bd322fd5a9cf78d7ebbe5 HEAD -- \
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

## Remaining Epoch 2 / M2.5 sequence

This head stops after placing the corrected M2 endpoint/core on the P48 product base. Four
ordered, separately reviewable product commits still precede M3:

1. add P49 scheduler-to-worker assignment preparation/revocation and bump the negotiated main
   protocol from 48 to 49 in that same commit;
2. add P50 end-to-end assignment identity and bump the negotiated main protocol from 49 to 50
   in that same commit;
3. add inert cache-endpoint capability advertisement, gated on negotiated main protocol 50,
   without enabling cache-input selection; and
4. set the development package identity to 1.5.90 only after the preceding unified product
   commits and their core-inheritance gates pass.

The main-protocol bumps travel with the behavior they gate so each intermediate commit can
negotiate and test its own wire contract. The package-version change remains last.

## M3 boundary

M3 begins only after the remaining Epoch 2 / M2.5 sequence and its core-inheritance gate pass.
M3 then owns daemon/wrapper attachment, compiler input piping, compiled-result attachment,
endpoint process lifetime, cache capability selection, fallback selection, persistence,
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
