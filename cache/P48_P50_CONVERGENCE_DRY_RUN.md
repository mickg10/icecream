# P48 to Protocol-50 convergence dry run

This branch rehearses the product-protocol transplant without attaching Protocol 50 to the
scheduler, daemon, compiler wrapper, or existing job path. The two source histories have no
Git merge base, so every imported layer is recorded explicitly.

## Fixed inputs

- Destination base: `e8e5e0b39373ec5952ea6595eeee77e92b680f55`
  (`implementer/issue16-r3-p48-cxx23-boost`).
- Accepted M0/M1 source lineage:
  - `4517bf92fba8d8a7ee70ee6d13b958f347c41900` -- wire, identities, actions, digest, and
    initial formal core.
  - `8e39ba1eae1009fa45bce70ffae9d8b745e55bea` -- in-memory P29 transaction.
  - `50c42f87c816a383fd03f3cfd26957b248831396` -- complete trailing-FILL closure.
  - `6c6c6f3fccd3863aeda35c5636fd1d9e06f36da7` -- transaction correspondence.
  - `8247176df8fc4835db2929cc52a5a79299382762` -- accepted formal suite.
  - `bdbdc23dfe2df86888fd26e257679250ae1980d9` -- negotiation and empty-session
    corrections.
  - `bbf97a06617adbe3162a826b81b9175894736063` -- self-contained route-cursor mutation
    gate.
- Bounded Zstd dialogue core:
  `179fe7b62b42fac4093e01e88140ee82b35dcd0d`.
- Provisional endpoint candidate:
  `64f58600f3b4b36ad539a214b5754767cdef58a9`. This remains the final product layer so a
  reviewed replacement can be replayed from the preceding convergence commit.

The M1 core consumes the header-only OnlineS1 implementation. The destination did not contain
the research tree, so only
`capability/grouprlz/p29_online_s1.h` was copied from the accepted M0/M1 tree. Its Git blob is
`bde2fcc10e0892476f1f1954b1a02e7b6ea0690c`; no other research harness was transplanted.

## Baseline handling and conflict resolutions

The destination already contains the complete C++23, Boost 1.74, Boost.Asio coroutine, and
asynchronous TCP compile probes in `9d7c8f770e7cb70240ebd32e26d80d5119337daa`.
Consequently, source commits `4309f420e9a0f1a78f15bb8e2ec46919e8dda725` and
`498d761997a30041c35f65ec3cdc2bdc567c8e54` were not replayed. M0 adds only its independent
xxHash dependency probe to that existing baseline.

All textual conflicts were build-list or package-list unions:

- Top-level `Makefile.am` retains every P48 document, package tool, web GUI test, stress test,
  and compose test. It adds the `cache` subdirectory, the one OnlineS1 header needed by M1,
  and the Protocol-50 formal target. Distribution-simulator inputs and the simulator-wide
  `integration_tests` target were omitted because those files do not exist in the P48 tree and
  are not part of this product transplant.
- `README` retains the P48 Boost 1.74 requirement and adds the M0 xxHash requirement and
  standard override instructions.
- `unittests/Makefile.am` keeps every P48 scheduler/daemon/web GUI-adjacent gate and its
  relocatable build-directory environment, adds the four Protocol-50 binaries and trace gate,
  and appends rather than replaces the existing distribution file list.
- `capability/grouprlz/p29_online_s1.h` is byte-identical to the accepted source blob.

The Protocol-50 implementation, formal files, and focused Protocol-50 tests are otherwise
tree-identical to the provisional source head. This can be checked with:

```sh
git diff --exit-code 64f58600f3b4b36ad539a214b5754767cdef58a9 HEAD -- \
  cache ':(exclude)cache/Makefile.am' \
  ':(exclude)cache/P48_P50_CONVERGENCE_DRY_RUN.md' \
  services/digest128.cpp services/digest128.h \
  capability/grouprlz/p29_online_s1.h \
  unittests/p50_wire_test.cpp unittests/p50_slice0_test.cpp \
  unittests/p50_zstd_test.cpp unittests/p50_endpoint_test.cpp \
  unittests/p50_trace_check.sh
```

## Deliberate stopping boundary

This rehearsal adds a standalone loopback endpoint and its tests only. It does not add P49/P50
assignment identity, cache advertisement, scheduler selection, daemon or compiler-wrapper
attachment, compiler input pipes, result attachment, sidecar lifetime, persistence, eviction,
or any M3 implementation. The P48 `scheduler/`, `daemon/`, `client/`, `compilerwrapper/`, and
existing integration-test trees must remain byte-identical to the fixed destination base.

## Required dry-run gates

Run from a clean checkout of the resulting exact head:

```sh
./autogen.sh
mkdir build && cd build
PKG_CONFIG_PATH=/path/to/xxhash/pkgconfig ../configure --with-boost=/path/to/boost-prefix
make -j8
make check
make -C unittests check TESTS='p50wire p50slice0 p50zstd p50endpoint p50_trace_check.sh'
```

The clean-build result and retained logs are external evidence; they are not hard-coded into
this source document.
