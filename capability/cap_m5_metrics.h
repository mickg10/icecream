#pragma once

#include <algorithm>
#include <cstdint>
#include <limits>
#include <vector>

namespace capm5 {

struct CurveSummary {
  double c50 = 0, h200 = -1, second_half = 0;
  uint32_t c50_tu = 0, h200_tu = UINT32_MAX;
};

// Evaluate the ruled learning gates at exact TU boundaries.  A trailing
// window includes complete TUs until it covers at least 5% of total raw bytes.
// H200 is the earliest boundary whose trailing window reaches 200x and whose
// rolling trailing window stays at 200x through at least the following 10% of
// total raw bytes.  A candidate in the final 10% cannot establish persistence.
template <class Row>
CurveSummary summarize_curve(const std::vector<Row> &rows) {
  CurveSummary result;
  if (rows.empty())
    return result;

  std::vector<uint64_t> raw(rows.size() + 1), wire(rows.size() + 1);
  for (size_t index = 0; index < rows.size(); ++index) {
    raw[index + 1] = raw[index] + rows[index].raw;
    wire[index + 1] = wire[index] + rows[index].wire;
  }
  if (!raw.back())
    return result;

  const uint64_t half = raw.back() / 2 + (raw.back() % 2 != 0);
  const size_t halfBoundary = size_t(
      std::lower_bound(raw.begin() + 1, raw.end(), half) - raw.begin());
  result.c50_tu = uint32_t(halfBoundary);
  result.c50 = wire[halfBoundary]
                   ? double(raw[halfBoundary]) / wire[halfBoundary]
                   : 0;
  const uint64_t secondRaw = raw.back() - raw[halfBoundary];
  const uint64_t secondWire = wire.back() - wire[halfBoundary];
  result.second_half = secondWire ? double(secondRaw) / secondWire : 0;

  const uint64_t trailingRaw = raw.back() / 20 + (raw.back() % 20 != 0);
  const uint64_t persistenceRaw = raw.back() / 10 + (raw.back() % 10 != 0);
  auto trailing_ratio = [&](size_t end) {
    const uint64_t target = raw[end] - trailingRaw;
    const auto firstAfter =
        std::upper_bound(raw.begin(), raw.begin() + end, target);
    const size_t begin = size_t(firstAfter - raw.begin() - 1);
    const uint64_t windowWire = wire[end] - wire[begin];
    return windowWire ? double(raw[end] - raw[begin]) / windowWire : 0.0;
  };

  for (size_t candidate = 1; candidate <= rows.size(); ++candidate) {
    if (raw[candidate] < trailingRaw ||
        raw[candidate] > raw.back() - persistenceRaw ||
        trailing_ratio(candidate) < 200.0)
      continue;
    const uint64_t persistenceEndRaw = raw[candidate] + persistenceRaw;
    const size_t persistenceEnd = size_t(
        std::lower_bound(raw.begin() + candidate, raw.end(), persistenceEndRaw) -
        raw.begin());
    bool holds = true;
    for (size_t boundary = candidate + 1; boundary <= persistenceEnd;
         ++boundary)
      if (trailing_ratio(boundary) < 200.0) {
        holds = false;
        break;
      }
    if (holds) {
      result.h200_tu = uint32_t(candidate);
      result.h200 = double(raw[candidate]) / raw.back();
      break;
    }
  }
  return result;
}

} // namespace capm5
