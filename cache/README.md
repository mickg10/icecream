# Protocol 50 cache

Start with [P50_PROTOCOL.md](P50_PROTOCOL.md) for the current protocol and
runtime contract: assignment, source admission, CacheWire revision 1, socket
ownership, committed input, sidecar lifecycle and restart behavior.

| Format | Specification |
|---|---|
| P29V1 (profile 1) | [P29V1_FORMAT.md](codec/P29V1_FORMAT.md) |
| ZSTD_TU and ZSTD_ROUTE (profiles 2 and 3) | [ZSTD_FORMATS.md](codec/ZSTD_FORMATS.md) |
| Retained historical codec fixtures | [FORMAT.md](codec/FORMAT.md) |

Package version 1.5.0, ordinary protocol 50, CacheWire revision 1 and P29v1
are separate version spaces. Historical fixtures and test-only components do
not add deployable profiles.

## Code map

- [protocol50.h](protocol50.h) / [protocol50.cpp](protocol50.cpp):
  CacheWire records, validation, framing and digests.
- [p50_endpoint.h](p50_endpoint.h) / [p50_endpoint.cpp](p50_endpoint.cpp):
  sessions, route reconciliation, resource limits and input publication.
- [p50_profile.h](p50_profile.h) / [p50_profile.cpp](p50_profile.cpp):
  profile dispatch; [codec](codec/README.md) contains the P29 templates.
- [p50_cache_service.cpp](p50_cache_service.cpp) and
  [p50_daemon_sidecar_adapter.h](p50_daemon_sidecar_adapter.h):
  the private service and daemon integration.
- [formal](formal/README.md): bounded models and trace checks.
- [key-layout census](KEY_LAYOUT_V1_CENSUS.md): generated, checked measurement
  summary; not a protocol specification.

The build groups live code into wire/profile, endpoint, local transport,
input, sidecar lifecycle, daemon adapter and outcome-writer libraries.
Reference reducers and the retired synchronous supervisor live under
`unittests/support/` in a check-only library. They are not alternate product
paths. Shared launch/clock types live in `p50_sidecar_identity.h`; local
operation encoding is implemented in `p50_control_operation.cpp`.

For build and QA commands, use the [project README](../README.md) and
[test entrypoints](../farmharness/integration/tests/README.md).

The [transfer-concurrency implementation and test contract](../doc/p50-transfer-concurrency.md)
separates independent revision-1 relationships from later persistent-link
and multi-TU pipelining work. Its acceptance matrix is not a passing-test receipt.
