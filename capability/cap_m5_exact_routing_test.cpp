#include "cap_m5_exact_routing.h"

#include <cstdio>
#include <cstdlib>
#include <string>

namespace {

void check(bool condition, const char *message) {
  if (!condition) {
    std::fprintf(stderr, "FAIL: %s\n", message);
    std::exit(1);
  }
}

std::vector<uint32_t> unique_in_order(const std::vector<uint32_t> &values) {
  std::vector<uint32_t> result;
  for (uint32_t value : values)
    if (std::find(result.begin(), result.end(), value) == result.end())
      result.push_back(value);
  return result;
}

capm5::ExactRoutingTu make_tu(uint32_t logical, uint64_t raw,
                              const std::vector<uint32_t> &regions) {
  capm5::ExactRoutingTu result;
  result.logical = logical;
  result.raw_bytes = raw;
  result.compile_ns = raw;
  result.required_regions = unique_in_order(regions);
  std::sort(result.required_regions.begin(), result.required_regions.end());
  for (uint32_t region : regions)
    capc::put_varint(result.root_raw, uint64_t(region) << 1);
  return result;
}

} // namespace

int main() {
  const std::string source =
      "# 1 \"shared.hpp\"\nshared line\nsecond shared line\n";
  capc::Interner dictionary;
  std::vector<uint32_t> ids(source.size() + 1), first_regions,
      second_regions;
  size_t line_count = 0;
  uint64_t hits = 0;
  dictionary.process(source.data(), source.data() + source.size(), ids.data(),
                     line_count, hits, true, &first_regions);
  line_count = 0;
  dictionary.process(source.data(), source.data() + source.size(), ids.data(),
                     line_count, hits, true, &second_regions);
  check(first_regions == second_regions && !first_regions.empty(),
        "fixture did not produce stable repeated Regions");

  const std::vector<uint32_t> block_children;
  const std::vector<size_t> block_offsets{0};
  capm5::ExactRoutingModel model;
  model.dictionary = &dictionary;
  model.block_children = &block_children;
  model.block_offsets = &block_offsets;
  model.tus.push_back(make_tu(0, source.size(), first_regions));
  model.tus.push_back(make_tu(1, source.size(), second_regions));

  capm5::ExactRoutingConfig config;
  config.compiler_slots = {1, 1};
  config.egress_lanes = 2;
  config.link_bits_per_second = 8000000000ULL;
  config.codec = capp::CodecPolicy::Best;

  capm5::ExactRoutingState state(model, config);
  capm5::ExactRoutingState::Undo first_undo;
  const auto first = state.apply(0, 0, first_undo);
  state.rollback(first_undo);
  check(state.c_to_f_bytes() == 0 && state.makespan_ns() == 0,
        "rollback did not restore exact routing totals");
  capm5::ExactRoutingState::Undo repeated_undo;
  const auto repeated = state.apply(0, 0, repeated_undo);
  check(first.cost.total() == repeated.cost.total() &&
            first.transfer_finish_ns == repeated.transfer_finish_ns &&
            first.compile_finish_ns == repeated.compile_finish_ns,
        "rolled-back exact transition did not replay identically");
  state.commit(repeated_undo);

  capm5::ExactRoutingState::Undo warm_undo;
  const auto warm = state.apply(1, 0, warm_undo);
  state.rollback(warm_undo);
  capm5::ExactRoutingState::Undo cold_undo;
  const auto cold = state.apply(1, 1, cold_undo);
  state.rollback(cold_undo);
  check(warm.cost.total() < cold.cost.total(),
        "per-F exact state did not distinguish warm and cold placement");

  capm5::ExactRoutingState::Undo warm_again_undo;
  const auto warm_again = state.apply(1, 0, warm_again_undo);
  check(warm.cost.total() == warm_again.cost.total() &&
            warm.transfer_finish_ns == warm_again.transfer_finish_ns &&
            warm.compile_finish_ns == warm_again.compile_finish_ns,
        "branch rollback changed the following exact warm transition");
  state.rollback(warm_again_undo);

  const auto replay = capm5::exact_routing_replay(model, config, {0, 0});
  check(replay.rows.size() == 2 &&
            replay.event_c_to_f ==
                replay.rows[0].cost.total() + replay.rows[1].cost.total(),
        "exact replay event ledger does not close");
  check(replay.rows[0].cost.total() == first.cost.total() &&
            replay.rows[1].cost.total() == warm.cost.total(),
        "committed exact replay differs from reversible transitions");

  capm5::ExactR5Options byte_options;
  byte_options.horizon = 2;
  byte_options.beam_width = 4;
  const auto byte_r5 =
      capm5::bounded_exact_r5_replay(model, config, byte_options);
  check(byte_r5.feasible.rows.size() == 2 &&
            byte_r5.feasible.rows[0].worker == 0 &&
            byte_r5.feasible.rows[1].worker == 0 &&
            byte_r5.feasible.event_c_to_f == replay.event_c_to_f &&
            byte_r5.expanded_paths > 0 &&
            byte_r5.symmetric_actions_collapsed > 0,
        "byte-only exact R5 did not retain the warm F");

  capm5::ExactR5Options time_options = byte_options;
  time_options.time_weight_bytes = 100;
  const auto time_r5 =
      capm5::bounded_exact_r5_replay(model, config, time_options);
  check(time_r5.feasible.rows[0].worker !=
            time_r5.feasible.rows[1].worker,
        "time-weighted exact R5 did not use available compiler parallelism");

  std::printf("cap_m5_exact_routing_test: PASS\n");
  return 0;
}
