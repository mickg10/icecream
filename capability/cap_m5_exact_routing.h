#pragma once

// Socket-free exact C->F event accounting for the protocol-50 M5 codec.
//
// The ordinary routing planner operates on independently compressed Region
// estimates.  That is useful for causal policies, but it cannot serve as the
// exact rolling-horizon oracle: M5 Root, Block, public-Line and Fill costs are
// path dependent.  This state applies the real component serializer against
// the real shared-C authority and one ReceiverMirror per F.  Every transition
// can be committed or rolled back, so bounded search can explore futures
// without copying the complete catalogue for every branch.

#include "cap_codec.h"
#include "cap_m5_state.h"
#include "cap_protocol.h"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <utility>
#include <vector>

namespace capm5 {

struct ExactRoutingTu {
  uint32_t logical = 0;
  uint64_t raw_bytes = 0;
  uint64_t release_ns = 0;
  uint64_t compile_ns = 0;
  std::vector<uint8_t> root_raw;
  std::vector<uint32_t> required_blocks;
  std::vector<uint32_t> required_regions;
};

struct ExactRoutingModel {
  const capc::Interner *dictionary = nullptr;
  const std::vector<uint32_t> *block_children = nullptr;
  const std::vector<size_t> *block_offsets = nullptr;
  std::vector<ExactRoutingTu> tus;
};

struct ExactRoutingConfig {
  std::vector<uint32_t> compiler_slots;
  uint32_t egress_lanes = 1;
  uint64_t link_bits_per_second = 1000000000ULL;
  capp::CodecPolicy codec = capp::CodecPolicy::Zstd1;
};

struct ExactRoutingCost {
  uint64_t c_root = 0;
  uint64_t c_fill = 0;
  uint64_t c_control = 0;

  uint64_t total() const {
    if (c_fill > std::numeric_limits<uint64_t>::max() - c_root ||
        c_control > std::numeric_limits<uint64_t>::max() - c_root - c_fill)
      throw std::overflow_error("exact M5 event total overflow");
    return c_root + c_fill + c_control;
  }
};

struct ExactRoutingAction {
  uint32_t logical = 0;
  uint32_t worker = 0;
  ExactRoutingCost cost;
  uint64_t transfer_start_ns = 0;
  uint64_t transfer_finish_ns = 0;
  uint64_t compile_start_ns = 0;
  uint64_t compile_finish_ns = 0;
};

struct ExactRoutingResult {
  std::vector<ExactRoutingAction> rows;
  uint64_t event_c_to_f = 0;
  uint64_t makespan_ns = 0;
};

struct ExactR5Options {
  size_t horizon = 4;
  size_t beam_width = 64;
  uint64_t time_weight_bytes = 0;
  uint64_t time_weight_ns = 1;
};

struct ExactR5Result {
  ExactRoutingResult feasible;
  uint64_t expanded_paths = 0;
  uint64_t pruned_paths = 0;
  uint64_t symmetric_actions_collapsed = 0;
  size_t horizon = 0;
  size_t beam_width = 0;
};

namespace exact_detail {

inline uint64_t checked_add(uint64_t left, uint64_t right, const char *what) {
  if (right > std::numeric_limits<uint64_t>::max() - left)
    throw std::overflow_error(what);
  return left + right;
}

inline uint64_t mul_div_ceil(uint64_t value, uint64_t numerator,
                             uint64_t denominator, const char *what) {
  if (!denominator)
    throw std::invalid_argument(what);
  if (!value || !numerator)
    return 0;
  if (value > std::numeric_limits<uint64_t>::max() / numerator)
    throw std::overflow_error(what);
  const uint64_t product = value * numerator;
  return product / denominator + (product % denominator != 0);
}

inline size_t earliest(const std::vector<uint64_t> &values) {
  if (values.empty())
    throw std::logic_error("exact M5 routing has no resource lane");
  return size_t(std::min_element(values.begin(), values.end()) -
                values.begin());
}

inline uint64_t frame_bytes(const std::vector<uint8_t> &payload) {
  return checked_add(4, payload.size(), "exact M5 frame size overflow");
}

class CodecContexts {
public:
  CodecContexts()
      : z1_(ZSTD_createCCtx()), z3_(ZSTD_createCCtx()) {
    if (!z1_ || !z3_)
      throw std::bad_alloc();
  }
  CodecContexts(const CodecContexts &) = delete;
  CodecContexts &operator=(const CodecContexts &) = delete;
  ~CodecContexts() {
    ZSTD_freeCCtx(z1_);
    ZSTD_freeCCtx(z3_);
  }

  capp::EncodedComponent encode(const std::vector<uint8_t> &raw,
                                capp::CodecPolicy policy) {
    return capp::encode_component(raw, policy, z1_, z3_, true);
  }

private:
  ZSTD_CCtx *z1_ = nullptr;
  ZSTD_CCtx *z3_ = nullptr;
};

} // namespace exact_detail

class ExactRoutingState {
public:
  struct Undo {
    capc::MixedEncoder::AuthorityTransaction authority;
    std::vector<uint32_t> installed_blocks;
    uint32_t worker = 0;
    uint32_t prior_path_count = 0;
    size_t egress_lane = 0;
    size_t compiler_slot = 0;
    uint64_t prior_egress_ready_ns = 0;
    uint64_t prior_compiler_ready_ns = 0;
    uint64_t prior_c_to_f = 0;
    uint64_t prior_makespan_ns = 0;
    uint64_t prior_worker_commits = 0;
    bool active = false;
  };

  ExactRoutingState(const ExactRoutingModel &model,
                    const ExactRoutingConfig &config)
      : model_(model), config_(config) {
    validate_model();
    encoder_.init(model.dictionary->distinct(),
                  uint32_t(model.dictionary->region_count()));
    mirrors_.resize(config.compiler_slots.size());
    compiler_ready_ns_.resize(config.compiler_slots.size());
    worker_commits_.assign(config.compiler_slots.size(), 0);
    const uint32_t blocks = uint32_t(model.block_offsets->size() - 1);
    for (size_t worker = 0; worker < mirrors_.size(); ++worker) {
      mirrors_[worker].init(uint32_t(model.dictionary->region_count()), blocks);
      compiler_ready_ns_[worker].assign(config.compiler_slots[worker], 0);
    }
    egress_ready_ns_.assign(config.egress_lanes, 0);
  }

  ExactRoutingState(const ExactRoutingState &) = delete;
  ExactRoutingState &operator=(const ExactRoutingState &) = delete;

  size_t workers() const { return mirrors_.size(); }
  uint64_t c_to_f_bytes() const { return c_to_f_bytes_; }
  uint64_t makespan_ns() const { return makespan_ns_; }

  // Every exact-mode F starts empty.  Two still-unused Fs with equal compiler
  // capacity are interchangeable: neither has object/path/public state and
  // neither has compiler work queued.  Used Fs remain distinct.  Collapsing
  // only this provable symmetry removes the large cold-width fanout without
  // treating merely similar warm mirrors as equal.
  std::vector<uint32_t> distinct_candidate_workers() const {
    std::vector<uint32_t> result;
    for (uint32_t worker = 0; worker < workers(); ++worker) {
      bool duplicate = false;
      if (worker_commits_[worker] == 0) {
        for (uint32_t retained : result)
          if (worker_commits_[retained] == 0 &&
              compiler_ready_ns_[retained] == compiler_ready_ns_[worker]) {
            duplicate = true;
            break;
          }
      }
      if (!duplicate)
        result.push_back(worker);
    }
    return result;
  }

  ExactRoutingAction apply(size_t tu_index, uint32_t worker, Undo &undo) {
    if (tu_index >= model_.tus.size() || worker >= mirrors_.size())
      throw std::out_of_range("exact M5 routing action index");
    if (undo.active)
      throw std::logic_error("exact M5 routing Undo reused while active");
    const ExactRoutingTu &tu = model_.tus[tu_index];
    ReceiverMirror &mirror = mirrors_[worker];
    undo = Undo{};
    undo.worker = worker;
    undo.prior_path_count = mirror.path_count;
    undo.prior_c_to_f = c_to_f_bytes_;
    undo.prior_makespan_ns = makespan_ns_;
    undo.prior_worker_commits = worker_commits_[worker];

    std::vector<uint8_t> block_raw;
    size_t definition_count = 0;
    for (uint32_t block : tu.required_blocks) {
      if (block >= mirror.blocks.size())
        throw std::out_of_range("exact M5 required Block outside mirror");
      if (!mirror.blocks[block])
        ++definition_count;
    }
    if (definition_count) {
      capc::put_varint(block_raw, definition_count);
      for (uint32_t block : tu.required_blocks) {
        if (mirror.blocks[block])
          continue;
        mirror.blocks[block] = 1;
        undo.installed_blocks.push_back(block);
        capc::put_varint(block_raw, block);
        block_raw.push_back(0);
        const size_t begin = (*model_.block_offsets)[block];
        const size_t end = (*model_.block_offsets)[block + 1];
        capc::put_varint(block_raw, end - begin);
        for (size_t index = begin; index < end; ++index)
          capc::put_varint(block_raw, (*model_.block_children)[index]);
      }
    }

    const auto root = codec_.encode(tu.root_raw, config_.codec);
    const auto blocks = codec_.encode(block_raw, config_.codec);
    ExactRoutingCost cost;
    cost.c_root = exact_detail::frame_bytes(
        capp::pack_root_m4(tu.logical, root.wire, blocks.wire));

    std::vector<uint32_t> missing;
    missing.reserve(tu.required_regions.size());
    for (uint32_t region : tu.required_regions) {
      if (region >= mirror.regions.size())
        throw std::out_of_range("exact M5 required Region outside mirror");
      if (!mirror.regions[region])
        missing.push_back(region);
    }

    encoder_.begin_authority_transaction(undo.authority);
    const uint32_t path_base = mirror.path_count;
    if (!missing.empty()) {
      MirrorScope scope(encoder_, mirror);
      encoder_.materialize(*model_.dictionary, missing, tu.logical,
                           &undo.authority);
    } else {
      for (auto &part : encoder_.mixedRaw)
        part.clear();
      encoder_.fill_paths.clear();
      encoder_.fill_public_base = encoder_.nextMixedPublic;
    }
    const uint32_t public_base = encoder_.fill_public_base;
    if (path_base > encoder_.paths.size())
      throw std::logic_error("exact M5 path mirror moved beyond authority");
    std::vector<uint8_t> path_raw;
    for (size_t index = path_base; index < encoder_.paths.size(); ++index) {
      capc::put_varint(path_raw, encoder_.paths[index].size());
      path_raw.insert(path_raw.end(), encoder_.paths[index].begin(),
                      encoder_.paths[index].end());
    }
    mirror.path_count = uint32_t(encoder_.paths.size());

    const auto path = codec_.encode(path_raw, config_.codec);
    std::array<std::vector<uint8_t>, 6> mixed_wire;
    for (size_t part = 0; part < 4; ++part)
      mixed_wire[part] =
          codec_.encode(encoder_.mixedRaw[part], config_.codec).wire;
    cost.c_fill = exact_detail::frame_bytes(capp::pack_fill_m4(
        tu.logical, path_base, public_base, path.wire, mixed_wire, 4));
    cost.c_control =
        exact_detail::frame_bytes(capp::pack_tu_ack(tu.logical, true));

    ExactRoutingAction action;
    action.logical = tu.logical;
    action.worker = worker;
    action.cost = cost;
    undo.egress_lane = exact_detail::earliest(egress_ready_ns_);
    undo.prior_egress_ready_ns = egress_ready_ns_[undo.egress_lane];
    action.transfer_start_ns =
        std::max(tu.release_ns, undo.prior_egress_ready_ns);
    action.transfer_finish_ns = exact_detail::checked_add(
        action.transfer_start_ns,
        exact_detail::mul_div_ceil(cost.total(), 8000000000ULL,
                                   config_.link_bits_per_second,
                                   "exact M5 transfer duration overflow"),
        "exact M5 transfer finish overflow");
    undo.compiler_slot = exact_detail::earliest(compiler_ready_ns_[worker]);
    undo.prior_compiler_ready_ns =
        compiler_ready_ns_[worker][undo.compiler_slot];
    action.compile_start_ns =
        std::max(action.transfer_finish_ns, undo.prior_compiler_ready_ns);
    action.compile_finish_ns = exact_detail::checked_add(
        action.compile_start_ns, tu.compile_ns,
        "exact M5 compile finish overflow");

    egress_ready_ns_[undo.egress_lane] = action.transfer_finish_ns;
    compiler_ready_ns_[worker][undo.compiler_slot] = action.compile_finish_ns;
    c_to_f_bytes_ = exact_detail::checked_add(
        c_to_f_bytes_, cost.total(), "exact M5 C-to-F total overflow");
    makespan_ns_ = std::max(makespan_ns_, action.compile_finish_ns);
    worker_commits_[worker] = exact_detail::checked_add(
        worker_commits_[worker], 1, "exact M5 worker commit count overflow");
    undo.active = true;
    return action;
  }

  void commit(Undo &undo) {
    if (!undo.active)
      throw std::logic_error("exact M5 commit without active transition");
    encoder_.commit_authority_transaction(undo.authority);
    undo.active = false;
    undo.installed_blocks.clear();
  }

  void rollback(Undo &undo) {
    if (!undo.active)
      throw std::logic_error("exact M5 rollback without active transition");
    ReceiverMirror &mirror = mirrors_[undo.worker];
    {
      MirrorScope scope(encoder_, mirror);
      encoder_.rollback_authority_transaction(undo.authority);
    }
    mirror.path_count = undo.prior_path_count;
    for (uint32_t block : undo.installed_blocks) {
      if (block >= mirror.blocks.size() || !mirror.blocks[block])
        throw std::logic_error("exact M5 Block undo disagrees with mirror");
      mirror.blocks[block] = 0;
    }
    egress_ready_ns_[undo.egress_lane] = undo.prior_egress_ready_ns;
    compiler_ready_ns_[undo.worker][undo.compiler_slot] =
        undo.prior_compiler_ready_ns;
    c_to_f_bytes_ = undo.prior_c_to_f;
    makespan_ns_ = undo.prior_makespan_ns;
    worker_commits_[undo.worker] = undo.prior_worker_commits;
    undo.active = false;
    undo.installed_blocks.clear();
  }

private:
  const ExactRoutingModel &model_;
  const ExactRoutingConfig &config_;
  capc::MixedEncoder encoder_;
  std::vector<ReceiverMirror> mirrors_;
  std::vector<uint64_t> egress_ready_ns_;
  std::vector<std::vector<uint64_t>> compiler_ready_ns_;
  std::vector<uint64_t> worker_commits_;
  uint64_t c_to_f_bytes_ = 0;
  uint64_t makespan_ns_ = 0;
  exact_detail::CodecContexts codec_;

  void validate_model() const {
    if (!model_.dictionary || !model_.block_children ||
        !model_.block_offsets || model_.block_offsets->empty())
      throw std::invalid_argument("exact M5 routing model is incomplete");
    if (config_.compiler_slots.empty() || !config_.egress_lanes ||
        !config_.link_bits_per_second)
      throw std::invalid_argument("exact M5 routing capacity is empty");
    for (uint32_t slots : config_.compiler_slots)
      if (!slots)
        throw std::invalid_argument("exact M5 F has no compiler slots");
    for (const ExactRoutingTu &tu : model_.tus) {
      if (!std::is_sorted(tu.required_blocks.begin(),
                          tu.required_blocks.end()) ||
          std::adjacent_find(tu.required_blocks.begin(),
                             tu.required_blocks.end()) !=
              tu.required_blocks.end())
        throw std::invalid_argument(
            "exact M5 required Blocks are not sorted unique");
      if (!std::is_sorted(tu.required_regions.begin(),
                          tu.required_regions.end()) ||
          std::adjacent_find(tu.required_regions.begin(),
                             tu.required_regions.end()) !=
              tu.required_regions.end())
        throw std::invalid_argument(
            "exact M5 required Regions are not sorted unique");
      for (uint32_t block : tu.required_blocks)
        if (size_t(block) + 1 >= model_.block_offsets->size())
          throw std::invalid_argument("exact M5 required Block is undefined");
      for (uint32_t region : tu.required_regions)
        if (region >= model_.dictionary->region_count())
          throw std::invalid_argument("exact M5 required Region is undefined");
    }
  }
};

inline ExactRoutingResult exact_routing_replay(
    const ExactRoutingModel &model, const ExactRoutingConfig &config,
    const std::vector<uint32_t> &assignments) {
  if (assignments.size() != model.tus.size())
    throw std::invalid_argument("exact M5 assignment length differs from trace");
  ExactRoutingState state(model, config);
  ExactRoutingResult result;
  result.rows.reserve(assignments.size());
  for (size_t index = 0; index < assignments.size(); ++index) {
    ExactRoutingState::Undo undo;
    result.rows.push_back(state.apply(index, assignments[index], undo));
    state.commit(undo);
  }
  result.event_c_to_f = state.c_to_f_bytes();
  result.makespan_ns = state.makespan_ns();
  return result;
}

namespace exact_detail {

struct R5Path {
  std::vector<uint32_t> workers;
  uint64_t c_to_f = 0;
  uint64_t makespan_ns = 0;
};

inline uint64_t r5_score(const R5Path &path,
                         const ExactR5Options &options) {
  return checked_add(
      path.c_to_f,
      mul_div_ceil(path.makespan_ns, options.time_weight_bytes,
                   options.time_weight_ns, "exact R5 score overflow"),
      "exact R5 score overflow");
}

inline bool r5_path_less(const R5Path &left, const R5Path &right,
                         const ExactR5Options &options) {
  const uint64_t left_score = r5_score(left, options);
  const uint64_t right_score = r5_score(right, options);
  if (left_score != right_score)
    return left_score < right_score;
  if (left.c_to_f != right.c_to_f)
    return left.c_to_f < right.c_to_f;
  if (left.makespan_ns != right.makespan_ns)
    return left.makespan_ns < right.makespan_ns;
  return left.workers < right.workers;
}

} // namespace exact_detail

// Deterministic receding-horizon beam search over the real M5 transition.
// Every retained choice is physically feasible.  It is called bounded, not
// optimal: beam pruning can discard a globally better future.
inline ExactR5Result bounded_exact_r5_replay(
    const ExactRoutingModel &model, const ExactRoutingConfig &config,
    const ExactR5Options &options) {
  if (!options.horizon || !options.beam_width || !options.time_weight_ns)
    throw std::invalid_argument("exact R5 has a zero search parameter");
  ExactRoutingState state(model, config);
  ExactR5Result result;
  result.horizon = options.horizon;
  result.beam_width = options.beam_width;
  result.feasible.rows.reserve(model.tus.size());

  for (size_t begin = 0; begin < model.tus.size(); ++begin) {
    std::vector<exact_detail::R5Path> beam(1);
    beam.front().c_to_f = state.c_to_f_bytes();
    beam.front().makespan_ns = state.makespan_ns();
    const size_t depth_limit =
        std::min(options.horizon, model.tus.size() - begin);
    for (size_t depth = 0; depth < depth_limit; ++depth) {
      std::vector<exact_detail::R5Path> next;
      if (beam.size() > std::numeric_limits<size_t>::max() / state.workers())
        throw std::overflow_error("exact R5 next-beam capacity overflow");
      next.reserve(beam.size() * state.workers());
      for (const exact_detail::R5Path &prefix : beam) {
        std::vector<ExactRoutingState::Undo> prefix_undos(prefix.workers.size());
        for (size_t offset = 0; offset < prefix.workers.size(); ++offset)
          (void)state.apply(begin + offset, prefix.workers[offset],
                            prefix_undos[offset]);
        const std::vector<uint32_t> candidates =
            state.distinct_candidate_workers();
        result.symmetric_actions_collapsed = exact_detail::checked_add(
            result.symmetric_actions_collapsed,
            state.workers() - candidates.size(),
            "exact R5 symmetry count overflow");
        for (uint32_t worker : candidates) {
          ExactRoutingState::Undo candidate_undo;
          (void)state.apply(begin + depth, worker, candidate_undo);
          exact_detail::R5Path child = prefix;
          child.workers.push_back(worker);
          child.c_to_f = state.c_to_f_bytes();
          child.makespan_ns = state.makespan_ns();
          next.push_back(std::move(child));
          state.rollback(candidate_undo);
          result.expanded_paths = exact_detail::checked_add(
              result.expanded_paths, 1,
              "exact R5 expanded-path count overflow");
        }
        for (auto undo = prefix_undos.rbegin(); undo != prefix_undos.rend();
             ++undo)
          state.rollback(*undo);
      }
      std::stable_sort(next.begin(), next.end(),
                       [&](const exact_detail::R5Path &left,
                           const exact_detail::R5Path &right) {
                         return exact_detail::r5_path_less(left, right,
                                                           options);
                       });
      if (next.size() > options.beam_width) {
        result.pruned_paths = exact_detail::checked_add(
            result.pruned_paths, next.size() - options.beam_width,
            "exact R5 pruned-path count overflow");
        next.resize(options.beam_width);
      }
      beam = std::move(next);
    }
    if (beam.empty() || beam.front().workers.empty())
      throw std::logic_error("exact R5 produced no feasible action");
    ExactRoutingState::Undo committed;
    result.feasible.rows.push_back(
        state.apply(begin, beam.front().workers.front(), committed));
    state.commit(committed);
  }
  result.feasible.event_c_to_f = state.c_to_f_bytes();
  result.feasible.makespan_ns = state.makespan_ns();
  return result;
}

} // namespace capm5
