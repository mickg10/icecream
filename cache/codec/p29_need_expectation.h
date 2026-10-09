#pragma once

#include <cstdint>
#include <vector>

namespace icecc::codec {

// Route pipelining (P50_ROUTE_PIPELINING.md section 1.1, 2026-10-08 errata):
// at depth > 1 the sender predict-commits TX n locally (P29Serializer::commit)
// before TX n+1's begin_tu() runs, which clear_pending()s the NEED expectation
// TX n's confirm would otherwise validate through pending_. A predict-committed
// TX snapshots it here (P29Serializer::need_expectation()) and confirms later
// through the snapshot (P29Serializer::confirm_need_snapshot()), which needs no
// pending TU. Its own header, not part of p29_wire.h, so route bookkeeping
// (CActiveTx in p50_slice0.h) can name it without pulling the zstd-dependent
// codec into every translation unit that includes the endpoint headers.
struct P29NeedExpectation {
  bool expected = false;
  std::vector<std::uint32_t> missing_regions;
};

} // namespace icecc::codec
