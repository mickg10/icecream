#!/usr/bin/env python3
"""Observation-only per-TU byte split for cap_m5, BY FRAME KIND AND BY DIRECTION (v2).

v2 attributes the carried control bytes to their actual direction, so cf_total is exact
rather than a lower bound.  The two invariants stay independent because they use DIFFERENT
carry terms: the kind split uses b_carry, the direction split uses cf_carry + fc_carry,
and cf_carry + fc_carry == b_carry.  (v1 added the carry to cf_control while b_carry was
still in the direction invariant -- a double count the dual gate caught on the first run.)

Extends the frame-kind split with a C->F / F->C dimension, because Ack is the one frame
both sides send: keying on frame type alone cannot separate the directions.  A second pair
of accumulators rides alongside the existing ledger (bytes_out[i] + bytes_in[i] ==
bytes[i] by construction), so nothing existing changes value.

Adds counters and TSV columns only.  No encoder input, no control flow, no ordering
change; the emitted wire must stay byte-identical.
"""
import sys

src, dst = sys.argv[1], sys.argv[2]
text = open(src).read()
applied = []


def sub(tag, old, new, count=1):
    global text
    if text.count(old) != count:
        sys.exit("anchor %s matched %d times, expected %d" % (tag, text.count(old), count))
    text = text.replace(old, new, count)
    applied.append(tag)


# ---- 1. ledger gains a direction dimension ------------------------------------------
sub("ledger", """struct FrameLedger {
  std::array<uint64_t, 7> bytes{}, count{};
  void note(cap::Frame frame, size_t payload) {
    size_t i = size_t(frame);
    bytes[i] += 4 + payload;
    ++count[i];
  }""", """struct FrameLedger {
  std::array<uint64_t, 7> bytes{}, count{};
  // OBSERVATION-ONLY: same bytes, split by direction. bytes_out+bytes_in == bytes.
  std::array<uint64_t, 7> bytes_out{}, bytes_in{};
  void note(cap::Frame frame, size_t payload) {
    size_t i = size_t(frame);
    bytes[i] += 4 + payload;
    ++count[i];
  }
  void note_dir(cap::Frame frame, size_t payload, bool outbound) { // OBSERVATION
    (outbound ? bytes_out : bytes_in)[size_t(frame)] += 4 + payload;
  }""")

sub("send", """  if (!cap::send_frame(fd, frame, payload))
    return false;
  ledger.note(frame, payload.size());
  return true;""", """  if (!cap::send_frame(fd, frame, payload))
    return false;
  ledger.note(frame, payload.size());
  ledger.note_dir(frame, payload.size(), true); // OBSERVATION
  return true;""")

sub("recv", """  if (!cap::recv_frame(fd, frame, payload))
    return false;
  ledger.note(frame, payload.size());
  return true;""", """  if (!cap::recv_frame(fd, frame, payload))
    return false;
  ledger.note(frame, payload.size());
  ledger.note_dir(frame, payload.size(), false); // OBSERVATION
  return true;""")

# ---- 2. the carried control bytes get the same dimension -----------------------------
SUM_OUT = "std::accumulate(worker.frames.bytes_out.begin(), worker.frames.bytes_out.end(), uint64_t(0))"
SUM_IN = "std::accumulate(worker.frames.bytes_in.begin(), worker.frames.bytes_in.end(), uint64_t(0))"

sub("worker_fields", """  uint64_t session_start = 0, pending_curve_bytes = 0, accepted = 0,""",
    """  uint64_t pending_curve_out = 0, pending_curve_in = 0; // OBSERVATION
  uint64_t session_start = 0, pending_curve_bytes = 0, accepted = 0,""")

sub("spawn", """  uint64_t before = worker.frames.total();""",
    """  uint64_t before = worker.frames.total();
  const uint64_t beforeOut = %s, beforeIn = %s; // OBSERVATION""" % (SUM_OUT, SUM_IN))

sub("spawn_add", """  worker.pending_curve_bytes += worker.frames.total() - before;
  return true;
}""", """  worker.pending_curve_bytes += worker.frames.total() - before;
  worker.pending_curve_out += %s - beforeOut;  // OBSERVATION
  worker.pending_curve_in += %s - beforeIn;
  return true;
}""" % (SUM_OUT, SUM_IN))

sub("finish", """  const uint64_t controlStart = worker.frames.total();""",
    """  const uint64_t controlStart = worker.frames.total();
  const uint64_t controlOut = %s, controlIn = %s; // OBSERVATION""" % (SUM_OUT, SUM_IN))

sub("finish_add", """  worker.pending_curve_bytes += worker.frames.total() - controlStart;
  return summary.exact""", """  worker.pending_curve_bytes += worker.frames.total() - controlStart;
  worker.pending_curve_out += %s - controlOut;  // OBSERVATION
  worker.pending_curve_in += %s - controlIn;
  return summary.exact""" % (SUM_OUT, SUM_IN))

sub("abort", """      worker.pending_curve_bytes += worker.frames.total() - job.wire_start;""",
    """      worker.pending_curve_bytes += worker.frames.total() - job.wire_start;
      for (size_t kind = 0; kind < 7; ++kind) {                     // OBSERVATION
        worker.pending_curve_out += worker.frames.bytes_out[kind] - job.out_at_start[kind];
        worker.pending_curve_in += worker.frames.bytes_in[kind] - job.in_at_start[kind];
      }""")



# ---- 3. CurveRow / Pending carry the new fields ---------------------------------------
sub("curve_row", """struct CurveRow {
  uint32_t logical = 0, physical = 0, worker = 0;
  uint64_t raw = 0, wire = 0, latency_ns = 0;
};""", """struct CurveRow {
  uint32_t logical = 0, physical = 0, worker = 0;
  uint64_t raw = 0, wire = 0, latency_ns = 0;
  // OBSERVATION-ONLY per-TU byte split, by frame kind and by direction.
  uint64_t b_root = 0, b_need = 0, b_fill = 0, b_control = 0, b_carry = 0;
  uint64_t cf_root = 0, cf_fill = 0, cf_control = 0, cf_carry = 0;
  uint64_t fc_need = 0, fc_control = 0, fc_carry = 0;
  uint64_t carry_undirected = 0;
  uint64_t c_root = 0, c_block = 0, c_path = 0, c_region_control = 0,
           c_raw_run = 0, c_array_control = 0, c_array_values = 0;
  uint64_t missing_regions = 0;
};""")

sub("pending", """  std::vector<uint32_t> missing;
  MixedEncoder::AuthorityTransaction authority;
  bool authority_started = false;
  Clock::time_point started;
};""", """  std::vector<uint32_t> missing;
  MixedEncoder::AuthorityTransaction authority;
  bool authority_started = false;
  Clock::time_point started;
  // OBSERVATION-ONLY
  std::array<uint64_t, 7> frames_at_start{}, out_at_start{}, in_at_start{};
  uint64_t root_wire = 0, block_wire = 0;
};""")

sub("pending_snapshot", """                         {}});
      ++next;""", """                         {}});
      pending.back().frames_at_start = workers[worker].frames.bytes;  // OBSERVATION
      pending.back().out_at_start = workers[worker].frames.bytes_out;
      pending.back().in_at_start = workers[worker].frames.bytes_in;
      ++next;""")

sub("root_sizes", """      encodeSeconds += seconds_since(encodeBegin);
      return send_counted(
          worker.fd, cap::Frame::Root,""", """      encodeSeconds += seconds_since(encodeBegin);
      job.root_wire += root.wire.size();    // OBSERVATION
      job.block_wire += blocks.wire.size(); // OBSERVATION
      return send_counted(
          worker.fd, cap::Frame::Root,""")

# ---- 4. commit: attribute by kind AND direction ---------------------------------------
sub("commit", """      curve.push_back(
          {job.logical, job.physical, job.worker, file.len, wire, latencyNs});""",
    """      CurveRow committed{job.logical, job.physical, job.worker,
                         file.len,     wire,         latencyNs};
      { // OBSERVATION-ONLY. Per-kind deltas sum to (frames.total() - wire_start), and the
        // carried control bytes are consumed here, so cf + fc + b_carry == wire exactly.
        uint64_t attributed = 0;
        std::array<uint64_t, 7> delta{}, dout{}, din{};
        for (size_t kind = 0; kind < delta.size(); ++kind) {
          delta[kind] = worker.frames.bytes[kind] - job.frames_at_start[kind];
          dout[kind] = worker.frames.bytes_out[kind] - job.out_at_start[kind];
          din[kind] = worker.frames.bytes_in[kind] - job.in_at_start[kind];
          attributed += delta[kind];
        }
        const size_t HELLO = size_t(cap::Frame::Hello), ROOT = size_t(cap::Frame::Root),
                     NEED = size_t(cap::Frame::Need), FILL = size_t(cap::Frame::Fill),
                     DONE = size_t(cap::Frame::Done), ACK = size_t(cap::Frame::Ack),
                     REJOIN = size_t(cap::Frame::Rejoin);
        committed.b_root = delta[ROOT];
        committed.b_need = delta[NEED];
        committed.b_fill = delta[FILL];
        committed.b_control =
            delta[HELLO] + delta[DONE] + delta[ACK] + delta[REJOIN];
        committed.cf_root = dout[ROOT];
        committed.cf_fill = dout[FILL];
        committed.cf_control =
            dout[HELLO] + dout[DONE] + dout[ACK] + dout[REJOIN] + dout[NEED];
        committed.fc_need = din[NEED];
        committed.fc_control =
            din[HELLO] + din[DONE] + din[ACK] + din[REJOIN] + din[ROOT] + din[FILL];
        committed.b_carry = wire - attributed;
        committed.cf_carry = carryOut;   // OBSERVATION: carry, by direction
        committed.fc_carry = carryIn;
        committed.c_root = job.root_wire;
        committed.c_block = job.block_wire;
        committed.c_path = prepared[i].path.wire.size();
        committed.c_region_control = prepared[i].mixed[0].wire.size();
        committed.c_raw_run = prepared[i].mixed[1].wire.size();
        committed.c_array_control = prepared[i].mixed[2].wire.size();
        committed.c_array_values = prepared[i].mixed[3].wire.size();
        committed.missing_regions = job.missing.size();
      }
      curve.push_back(committed);""")

# ---- 5. abort path and the per-worker close both keep the dimension --------------------
sub("commit_carry", """      uint64_t wire =
          worker.frames.total() - job.wire_start + worker.pending_curve_bytes;
      worker.pending_curve_bytes = 0;""", """      uint64_t wire =
          worker.frames.total() - job.wire_start + worker.pending_curve_bytes;
      worker.pending_curve_bytes = 0;
      const uint64_t carryOut = worker.pending_curve_out;   // OBSERVATION
      const uint64_t carryIn = worker.pending_curve_in;
      worker.pending_curve_out = worker.pending_curve_in = 0;""")

sub("worker_close", """      last->wire += workers[id].pending_curve_bytes;""",
    """      last->b_carry += workers[id].pending_curve_bytes;   // OBSERVATION
      last->cf_carry += workers[id].pending_curve_out;
      last->fc_carry += workers[id].pending_curve_in;
      last->wire += workers[id].pending_curve_bytes;""")

sub("residual", """  if (!curve.empty() && curveWire < socketBytes)
    curve.back().wire += socketBytes - curveWire;
  else if (curveWire != socketBytes)""", """  if (!curve.empty() && curveWire < socketBytes) {
    curve.back().b_carry += socketBytes - curveWire;          // OBSERVATION
    curve.back().carry_undirected += socketBytes - curveWire;
    curve.back().wire += socketBytes - curveWire;
  } else if (curveWire != socketBytes)""")

# ---- 6. emit -------------------------------------------------------------------------
sub("header", """    out << "logical\\tphysical\\tworker\\traw\\twire\\tlatency_ns\\tcumulative_raw\\t"
           "cumulative_wire\\n";""", """    out << "logical\\tphysical\\tworker\\traw\\twire\\tlatency_ns\\tcumulative_raw\\t"
           "cumulative_wire\\tb_root\\tb_need\\tb_fill\\tb_control\\tb_carry\\t"
           "split_ok\\tc_root\\tc_block\\tc_path\\tc_region_control\\tc_raw_run\\t"
           "c_array_control\\tc_array_values\\tmissing_regions\\t"
           "cf_root\\tcf_fill\\tcf_control\\tcf_carry\\tcf_total\\t"
           "fc_need\\tfc_control\\tfc_carry\\tfc_total\\tcarry_undirected\\tdir_ok\\n";""")

sub("row", """      out << row.logical << '\\t' << row.physical << '\\t' << row.worker << '\\t'
          << row.raw << '\\t' << row.wire << '\\t' << row.latency_ns << '\\t' << cr
          << '\\t' << cw << '\\n';""", """      const uint64_t cf = row.cf_root + row.cf_fill + row.cf_control + row.cf_carry;
      const uint64_t fc = row.fc_need + row.fc_control + row.fc_carry;
      out << row.logical << '\\t' << row.physical << '\\t' << row.worker << '\\t'
          << row.raw << '\\t' << row.wire << '\\t' << row.latency_ns << '\\t' << cr
          << '\\t' << cw << '\\t' << row.b_root << '\\t' << row.b_need << '\\t'
          << row.b_fill << '\\t' << row.b_control << '\\t' << row.b_carry << '\\t'
          << ((row.b_root + row.b_need + row.b_fill + row.b_control +
               row.b_carry) == row.wire)
          << '\\t' << row.c_root << '\\t' << row.c_block << '\\t' << row.c_path
          << '\\t' << row.c_region_control << '\\t' << row.c_raw_run << '\\t'
          << row.c_array_control << '\\t' << row.c_array_values << '\\t'
          << row.missing_regions
          << '\\t' << row.cf_root << '\\t' << row.cf_fill << '\\t' << row.cf_control
          << '\\t' << row.cf_carry << '\\t' << cf
          << '\\t' << row.fc_need << '\\t' << row.fc_control << '\\t' << row.fc_carry
          << '\\t' << fc << '\\t' << row.carry_undirected
          << '\\t' << ((cf + fc + row.carry_undirected) == row.wire) << '\\n';""")

open(dst, "w").write(text)
print("applied:", " ".join(applied))
