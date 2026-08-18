#!/usr/bin/env python3
"""Observation-only per-TU Root/Need/Fill byte-split instrumentation for cap_m5.

Adds counters and TSV columns only.  No encoder input, no control flow, no
ordering change -- the emitted wire must stay byte-identical, which the caller
gates by diffing the pre-existing curve columns and the stdout summary against
the unpatched build.
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


# ---- 1. CurveRow carries the split -------------------------------------------------
sub("curve_row", """struct CurveRow {
  uint32_t logical = 0, physical = 0, worker = 0;
  uint64_t raw = 0, wire = 0, latency_ns = 0;
};""", """struct CurveRow {
  uint32_t logical = 0, physical = 0, worker = 0;
  uint64_t raw = 0, wire = 0, latency_ns = 0;
  // OBSERVATION-ONLY per-TU byte split.  Written at commit, read only by the
  // curve writer; nothing here feeds encoding, selection or ordering.
  uint64_t b_root = 0, b_need = 0, b_fill = 0, b_control = 0, b_carry = 0;
  uint64_t c_root = 0, c_block = 0, c_path = 0, c_region_control = 0,
           c_raw_run = 0, c_array_control = 0, c_array_values = 0;
  uint64_t missing_regions = 0;
};""")

# ---- 2. Pending remembers where its frame ledger started ----------------------------
sub("pending", """  std::vector<uint32_t> missing;
  MixedEncoder::AuthorityTransaction authority;
  bool authority_started = false;
  Clock::time_point started;
};""", """  std::vector<uint32_t> missing;
  MixedEncoder::AuthorityTransaction authority;
  bool authority_started = false;
  Clock::time_point started;
  // OBSERVATION-ONLY
  std::array<uint64_t, 7> frames_at_start{};
  uint64_t root_wire = 0, block_wire = 0;
};""")

sub("pending_snapshot", """                         {}});
      ++next;""", """                         {}});
      pending.back().frames_at_start = workers[worker].frames.bytes;  // OBSERVATION
      ++next;""")

# ---- 3. Root component sizes (accumulated: establish_need may resend) ---------------
sub("root_sizes", """      encodeSeconds += seconds_since(encodeBegin);
      return send_counted(
          worker.fd, cap::Frame::Root,""", """      encodeSeconds += seconds_since(encodeBegin);
      job.root_wire += root.wire.size();    // OBSERVATION
      job.block_wire += blocks.wire.size(); // OBSERVATION
      return send_counted(
          worker.fd, cap::Frame::Root,""")

# ---- 4. Commit: attribute this TU's frame bytes by kind ------------------------------
sub("commit", """      curve.push_back(
          {job.logical, job.physical, job.worker, file.len, wire, latencyNs});""",
    """      CurveRow committed{job.logical, job.physical, job.worker,
                         file.len,     wire,         latencyNs};
      { // OBSERVATION-ONLY: the per-kind deltas sum to (frames.total() -
        // wire_start) by construction, so b_* + b_carry == wire exactly.
        uint64_t attributed = 0;
        std::array<uint64_t, 7> delta{};
        for (size_t kind = 0; kind < delta.size(); ++kind) {
          delta[kind] = worker.frames.bytes[kind] - job.frames_at_start[kind];
          attributed += delta[kind];
        }
        committed.b_root = delta[size_t(cap::Frame::Root)];
        committed.b_need = delta[size_t(cap::Frame::Need)];
        committed.b_fill = delta[size_t(cap::Frame::Fill)];
        committed.b_control = delta[size_t(cap::Frame::Hello)] +
                              delta[size_t(cap::Frame::Done)] +
                              delta[size_t(cap::Frame::Ack)] +
                              delta[size_t(cap::Frame::Rejoin)];
        committed.b_carry = wire - attributed;
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

# ---- 5a. Each relationship's Done/Ack close is charged to its own last TU -------------
sub("worker_close", """      last->wire += workers[id].pending_curve_bytes;""",
    """      last->b_carry += workers[id].pending_curve_bytes; // OBSERVATION
      last->wire += workers[id].pending_curve_bytes;""")

# ---- 5. The residual the last row absorbs is carry, not payload ----------------------
sub("residual", """  if (!curve.empty() && curveWire < socketBytes)
    curve.back().wire += socketBytes - curveWire;
  else if (curveWire != socketBytes)""", """  if (!curve.empty() && curveWire < socketBytes) {
    curve.back().b_carry += socketBytes - curveWire; // OBSERVATION
    curve.back().wire += socketBytes - curveWire;
  } else if (curveWire != socketBytes)""")

# ---- 6. Emit the columns -------------------------------------------------------------
sub("header", """    out << "logical\\tphysical\\tworker\\traw\\twire\\tlatency_ns\\tcumulative_raw\\t"
           "cumulative_wire\\n";""", """    out << "logical\\tphysical\\tworker\\traw\\twire\\tlatency_ns\\tcumulative_raw\\t"
           "cumulative_wire\\tb_root\\tb_need\\tb_fill\\tb_control\\tb_carry\\t"
           "split_ok\\tc_root\\tc_block\\tc_path\\tc_region_control\\tc_raw_run\\t"
           "c_array_control\\tc_array_values\\tmissing_regions\\n";""")

sub("row", """      out << row.logical << '\\t' << row.physical << '\\t' << row.worker << '\\t'
          << row.raw << '\\t' << row.wire << '\\t' << row.latency_ns << '\\t' << cr
          << '\\t' << cw << '\\n';""", """      out << row.logical << '\\t' << row.physical << '\\t' << row.worker << '\\t'
          << row.raw << '\\t' << row.wire << '\\t' << row.latency_ns << '\\t' << cr
          << '\\t' << cw << '\\t' << row.b_root << '\\t' << row.b_need << '\\t'
          << row.b_fill << '\\t' << row.b_control << '\\t' << row.b_carry << '\\t'
          << ((row.b_root + row.b_need + row.b_fill + row.b_control +
               row.b_carry) == row.wire)
          << '\\t' << row.c_root << '\\t' << row.c_block << '\\t' << row.c_path
          << '\\t' << row.c_region_control << '\\t' << row.c_raw_run << '\\t'
          << row.c_array_control << '\\t' << row.c_array_values << '\\t'
          << row.missing_regions << '\\n';""")

open(dst, "w").write(text)
print("applied:", " ".join(applied))
