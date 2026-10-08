"""Unit tests for tools/stagetrace/analyze.py.

Synthetic-fixture only: no real hostnames, no real stage_trace output, no
private-codebase source paths or bytes. Hosts are named "chost"/
"fhost1"/"fhost2" and job identities are small made-up integers.

Covers the two review items that needed dedicated coverage:
  - --check-clocks (check_clock_sanity): a clean cross-host pair, a
    deliberately backwards cross-host pair, and the same backwards pair
    fixed up by a correct --offsets correction.
  - the protocol-level NTP clock-offset derivation (derive_ntp_offsets):
    exact-offset recovery from symmetric-latency samples, outlier
    rejection via the lowest-delay-decile filter, and the "no usable
    samples" (host omitted) cases.
"""
from __future__ import annotations

import sys
from pathlib import Path
from typing import List

sys.path.insert(0, str(Path(__file__).resolve().parent))

from analyze import (  # noqa: E402
    FResultSample,
    OwnerLagOpSample,
    OwnerLagSecondSample,
    OwnerLagStallSample,
    Record,
    SourceResultSample,
    check_clock_sanity,
    client_stall_stage_name,
    derive_ntp_offsets,
    load_owner_lag_trace,
    print_ntp_offsets,
    print_owner_lag_summary,
)


def _rec(component: str, stage: str, host: str, realtime_ns: int,
         epoch: int = 1, wire_id: int = 1) -> Record:
    return Record(
        realtime_ns=realtime_ns, mono_ns=realtime_ns, component=component,
        stage=stage, epoch=epoch, wire_id=wire_id, logical_job=1, attempt=0,
        host=host, pid=1, tid=1,
    )


# --- check_clock_sanity ----------------------------------------------------

def test_check_clock_sanity_clean_cross_host_pair_has_no_warning():
    # scheduler.assign_prepare_sent -> fdaemon.assign_prepare_recv is a
    # real adjacent cross-host pair in CANONICAL_STAGE_ORDER.
    jobs = {
        (1, 1): [
            _rec("scheduler", "assign_prepare_sent", "chost", 1_000_000_000),
            _rec("fdaemon", "assign_prepare_recv", "fhost1", 1_000_050_000),
        ],
    }
    warnings = check_clock_sanity(jobs, offsets={})
    assert warnings == []


def test_check_clock_sanity_flags_backwards_cross_host_pair():
    jobs = {
        (1, 1): [
            _rec("scheduler", "assign_prepare_sent", "chost", 1_000_050_000),
            # fdaemon's clock claims it received this before it was sent.
            _rec("fdaemon", "assign_prepare_recv", "fhost1", 1_000_000_000),
        ],
    }
    warnings = check_clock_sanity(jobs, offsets={})
    assert len(warnings) == 1
    assert "assign_prepare_sent" in warnings[0]
    assert "assign_prepare_recv" in warnings[0]
    assert "backwards" in warnings[0]


def test_check_clock_sanity_offset_correction_clears_the_warning():
    # Same backwards-looking raw data as above, but fhost1's clock is known
    # (from offsets.json) to be 100ms slow; adding that correction to
    # fhost1's raw timestamp (per corrected_ts()) puts the pair back in
    # causal order.
    jobs = {
        (1, 1): [
            _rec("scheduler", "assign_prepare_sent", "chost", 1_000_050_000),
            _rec("fdaemon", "assign_prepare_recv", "fhost1", 1_000_000_000),
        ],
    }
    warnings = check_clock_sanity(jobs, offsets={"fhost1": 100_000_000})
    assert warnings == []


def test_check_clock_sanity_same_host_adjacent_pair_never_checked():
    # csidecar.source_read_done -> csidecar.prepare_source_done is adjacent
    # in CANONICAL_STAGE_ORDER but same-host; even a backwards same-host
    # pair must not be flagged (check_clock_sanity only compares
    # cross-host neighbors).
    jobs = {
        (1, 1): [
            _rec("csidecar", "source_read_done", "chost", 2_000_000),
            _rec("csidecar", "prepare_source_done", "chost", 1_000_000),
        ],
    }
    warnings = check_clock_sanity(jobs, offsets={})
    assert warnings == []


# --- derive_ntp_offsets -----------------------------------------------------

def _sample(guid: str, tu_seq: int, t1: int, up_ns: int, delta_ns: int,
            processing_ns: int, down_ns: int):
    """Builds one synthetic (c_record, f_record) pair for a BEGIN/COMMIT
    round trip, given the true one-way latencies, F's clock skew
    (delta_ns = how far F's clock reads ahead of the reference/C clock),
    and F's BEGIN-to-COMMIT processing time -- see the derivation in
    analyze.py's module comment above corrected_ts().
    """
    t2 = t1 + up_ns + delta_ns
    t3 = t2 + processing_ns
    t4 = (t3 - delta_ns) + down_ns
    c_rec = SourceResultSample(c_store_guid=guid, tu_seq=tu_seq, t1=t1, t4=t4)
    f_rec = FResultSample(c_store_guid=guid, tu_seq=tu_seq, t2=t2, t3=t3)
    return c_rec, f_rec


def test_derive_ntp_offsets_recovers_exact_offset_from_symmetric_samples():
    delta_ns = 50_000_000  # fhost1's clock reads 50ms ahead of chost
    c_records: List[SourceResultSample] = []
    f_records: List[FResultSample] = []
    base_t1 = 1_000_000_000_000
    # 18 "good" samples: symmetric 5ms one-way latency each direction, so
    # the sample offset recovers delta_ns exactly (see module derivation).
    for i in range(18):
        c_rec, f_rec = _sample(
            guid="route-a", tu_seq=i, t1=base_t1 + i * 1_000_000,
            up_ns=5_000_000, delta_ns=delta_ns, processing_ns=1_000_000,
            down_ns=5_000_000)
        c_records.append(c_rec)
        f_records.append(f_rec)
    # 2 noisy outliers: much larger, asymmetric latency, which pulls the
    # naive sample offset far from delta_ns -- these must be filtered out
    # by the lowest-delay-decile selection rather than polluting the
    # median.
    for i in range(18, 20):
        c_rec, f_rec = _sample(
            guid="route-a", tu_seq=i, t1=base_t1 + i * 1_000_000,
            up_ns=490_000_000, delta_ns=delta_ns, processing_ns=1_000_000,
            down_ns=10_000_000)
        c_records.append(c_rec)
        f_records.append(f_rec)

    results = derive_ntp_offsets(c_records, {"fhost1": f_records})

    assert set(results) == {"fhost1"}
    r = results["fhost1"]
    assert r.n_total == 20
    assert r.n_kept == 2  # max(1, 20 // 10)
    assert r.offset_ns == delta_ns  # exact: both kept samples are symmetric
    # error bound is half the kept samples' round-trip delay (10ms here).
    assert r.error_ns == 5_000_000


def test_derive_ntp_offsets_omits_host_with_no_joinable_samples():
    c_records = [SourceResultSample(c_store_guid="route-a", tu_seq=0,
                                     t1=1_000_000_000, t4=1_000_020_000)]
    # Different c_store_guid -- never joins against the C record above.
    f_records = [FResultSample(c_store_guid="route-b", tu_seq=0,
                                t2=1_000_005_000, t3=1_000_006_000)]
    results = derive_ntp_offsets(c_records, {"fhost1": f_records})
    assert results == {}


def test_derive_ntp_offsets_omits_host_when_a_timestamp_is_missing():
    # Joined on (c_store_guid, tu_seq), but F never captured t3 (0 means
    # "not captured", matching ICECC_P50_F_RESULT_TRACE being off or the
    # field predating this instrumentation).
    c_records = [SourceResultSample(c_store_guid="route-a", tu_seq=0,
                                     t1=1_000_000_000, t4=1_000_020_000)]
    f_records = [FResultSample(c_store_guid="route-a", tu_seq=0,
                                t2=1_000_005_000, t3=0)]
    results = derive_ntp_offsets(c_records, {"fhost1": f_records})
    assert results == {}


def test_derive_ntp_offsets_drops_physically_impossible_negative_delay():
    # t4 - t1 - (t3 - t2) < 0 can only happen if the clocks/timestamps are
    # too broken to trust; such samples must never be averaged in.
    c_records = [SourceResultSample(c_store_guid="route-a", tu_seq=0,
                                     t1=1_000_000_000, t4=1_000_000_100)]
    f_records = [FResultSample(c_store_guid="route-a", tu_seq=0,
                                t2=1_000_000_200, t3=1_000_050_000)]
    results = derive_ntp_offsets(c_records, {"fhost1": f_records})
    assert results == {}


def test_print_ntp_offsets_flags_disagreement_over_5ms(capsys):
    from analyze import NtpOffsetResult
    derived = {"fhost1": NtpOffsetResult(offset_ns=50_000_000.0,
                                          error_ns=5_000_000.0,
                                          n_kept=2, n_total=20)}
    print_ntp_offsets(derived, existing_offsets={"fhost1": 40_000_000})
    out = capsys.readouterr().out
    assert "WARNING: disagreement > 5ms" in out


def test_print_ntp_offsets_no_warning_within_5ms(capsys):
    from analyze import NtpOffsetResult
    derived = {"fhost1": NtpOffsetResult(offset_ns=50_000_000.0,
                                          error_ns=1_000_000.0,
                                          n_kept=2, n_total=20)}
    print_ntp_offsets(derived, existing_offsets={"fhost1": 49_000_000})
    out = capsys.readouterr().out
    assert "WARNING" not in out


# --- owner-lag probe (ICECC_P50_OWNER_LAG_TRACE) ----------------------------
#
# Covers the Round 3 owner-lag diagnostic: the per-second
# icecream-p50-owner-lag-v1 schema (an asio steady_timer's scheduling lag
# plus owner-thread CPU time, emitted once a second, including the
# trace_write_max_ns/trace_write_total_ns self-validation fields covering
# the probe's own open+write+close cost on the owner thread) and the
# per-op icecream-p50-owner-lag-op-v1 schema (post_to_start_ns /
# owner_exec_ns / in_flight_tx per attach/finish_attach/apply_lifecycle
# call), plus the per-stall icecream-p50-owner-lag-stall-v1 schema (one
# record written just before SidecarRuntime::transfer_source's
# fail_stop()/std::_Exit(125), when a route-owner coroutine never
# published a result before wait_limit), all written interleaved into the
# same file named by ICECC_P50_OWNER_LAG_TRACE. See
# append_owner_lag_second_trace / append_owner_lag_op_trace /
# append_owner_lag_stall_trace in cache/p50_cache_service.cpp for the
# producing side.

def test_load_owner_lag_trace_parses_all_interleaved_schemas(tmp_path):
    path = tmp_path / "owner_lag.jsonl"
    path.write_text(
        '{"schema":"icecream-p50-owner-lag-v1","pid":111,'
        '"wall_clock_ns":1000000000,"n":50,"max_lag_ns":500000,'
        '"p99_lag_ns":400000,"owner_cpu_ns":470000000,'
        '"trace_write_max_ns":3000,"trace_write_total_ns":9000}\n'
        '{"schema":"icecream-p50-owner-lag-op-v1","pid":111,'
        '"wall_clock_ns":1000000500,"op":"attach",'
        '"job_identity_hash":123456789,"post_to_start_ns":20000,'
        '"owner_exec_ns":5000,"result_status":3,'
        '"in_flight_tx":false,"in_flight_tx_age_ns":0}\n'
        '{"schema":"icecream-p50-owner-lag-stall-v1","pid":111,'
        '"wall_clock_ns":1000000900,"route_identity_hash":987654321,'
        '"transfer_age_ns":9500000000,"configured_deadline_ns":8000000000,'
        '"configured_grace_ns":1000000000,"last_stage":3,'
        '"tu_seq":0,"rel_seq":0}\n'
        # A line from an unrelated schema sharing nothing but valid JSON --
        # must be silently skipped, not treated as a parse failure.
        '{"schema":"icecream-p50-source-result-v5","c_store_guid":"g",'
        '"tu_seq":0}\n'
        # Malformed JSON -- must be skipped with a warning, not raise.
        'not even json\n'
    )
    seconds, ops, stalls = load_owner_lag_trace(str(path))
    assert len(seconds) == 1
    assert seconds[0] == OwnerLagSecondSample(
        pid=111, wall_clock_ns=1_000_000_000, n=50, max_lag_ns=500_000,
        p99_lag_ns=400_000, owner_cpu_ns=470_000_000,
        trace_write_max_ns=3_000, trace_write_total_ns=9_000)
    assert len(ops) == 1
    assert ops[0] == OwnerLagOpSample(
        pid=111, wall_clock_ns=1_000_000_500, op="attach",
        job_identity_hash=123456789, post_to_start_ns=20_000,
        owner_exec_ns=5_000, result_status=3, in_flight_tx=False,
        in_flight_tx_age_ns=0)
    assert len(stalls) == 1
    assert stalls[0] == OwnerLagStallSample(
        pid=111, wall_clock_ns=1_000_000_900, route_identity_hash=987654321,
        transfer_age_ns=9_500_000_000, configured_deadline_ns=8_000_000_000,
        configured_grace_ns=1_000_000_000, last_stage=3, tu_seq=0, rel_seq=0)


def test_load_owner_lag_trace_defaults_trace_write_fields_when_absent(tmp_path):
    # Older trace files (pre-self-validation-fields) never wrote
    # trace_write_max_ns/trace_write_total_ns; the loader must still parse
    # the line instead of raising, defaulting both to 0.
    path = tmp_path / "owner_lag.jsonl"
    path.write_text(
        '{"schema":"icecream-p50-owner-lag-v1","pid":111,'
        '"wall_clock_ns":1000000000,"n":50,"max_lag_ns":500000,'
        '"p99_lag_ns":400000,"owner_cpu_ns":470000000}\n'
    )
    seconds, ops, stalls = load_owner_lag_trace(str(path))
    assert len(seconds) == 1
    assert seconds[0].trace_write_max_ns == 0
    assert seconds[0].trace_write_total_ns == 0
    assert ops == []
    assert stalls == []


def test_print_owner_lag_summary_flags_second_over_one_second(capsys):
    seconds = [
        OwnerLagSecondSample(pid=1, wall_clock_ns=1_000_000_000, n=50,
                              max_lag_ns=200_000, p99_lag_ns=150_000,
                              owner_cpu_ns=300_000_000,
                              trace_write_max_ns=2_000,
                              trace_write_total_ns=6_000),
        # This second's owner lagged its scheduled 20ms fire by over 1s --
        # exactly the condition the coordinator asked be flagged.
        OwnerLagSecondSample(pid=1, wall_clock_ns=2_000_000_000, n=10,
                              max_lag_ns=1_500_000_000, p99_lag_ns=900_000_000,
                              owner_cpu_ns=50_000_000,
                              trace_write_max_ns=2_500,
                              trace_write_total_ns=4_000),
    ]
    print_owner_lag_summary(seconds, ops=[], stalls=[])
    out = capsys.readouterr().out
    assert "seconds with max_lag_ns > 1s: 1" in out
    assert "wall_clock_ns=2000000000" in out
    assert "max_lag_ns=1500000000" in out


def test_print_owner_lag_summary_reports_trace_write_self_cost(capsys):
    # The self-validation fields (coordinator-required) must be surfaced in
    # the summary so a reader can confirm the probe's own cost is nowhere
    # near max_lag_ns -- i.e. the probe is not the stall it measures.
    seconds = [
        OwnerLagSecondSample(pid=1, wall_clock_ns=1_000_000_000, n=50,
                              max_lag_ns=200_000, p99_lag_ns=150_000,
                              owner_cpu_ns=300_000_000,
                              trace_write_max_ns=3_000,
                              trace_write_total_ns=9_000),
    ]
    print_owner_lag_summary(seconds, ops=[], stalls=[])
    out = capsys.readouterr().out
    assert "trace_write_max_ns (probe's own cost)" in out
    assert "trace_write_total_ns (probe's own cost" in out


def test_print_owner_lag_summary_no_stalls_reports_none(capsys):
    seconds = [
        OwnerLagSecondSample(pid=1, wall_clock_ns=1_000_000_000, n=50,
                              max_lag_ns=200_000, p99_lag_ns=150_000,
                              owner_cpu_ns=300_000_000,
                              trace_write_max_ns=1_000,
                              trace_write_total_ns=1_000),
    ]
    print_owner_lag_summary(seconds, ops=[], stalls=[])
    out = capsys.readouterr().out
    assert "seconds with max_lag_ns > 1s: none" in out


def test_print_owner_lag_summary_handles_no_records(capsys):
    print_owner_lag_summary([], [], [])
    out = capsys.readouterr().out
    assert "no icecream-p50-owner-lag-v1 records found" in out
    assert "no icecream-p50-owner-lag-op-v1 records found" in out
    assert "no icecream-p50-owner-lag-stall-v1 records found" in out


def test_print_owner_lag_summary_per_op_breakdown_reports_in_flight_tx(capsys):
    ops = [
        OwnerLagOpSample(pid=1, wall_clock_ns=1_000_000_000, op="attach",
                          job_identity_hash=1, post_to_start_ns=10_000,
                          owner_exec_ns=2_000, result_status=3,
                          in_flight_tx=False, in_flight_tx_age_ns=0),
        OwnerLagOpSample(pid=1, wall_clock_ns=1_000_000_100,
                          op="apply_lifecycle", job_identity_hash=1,
                          post_to_start_ns=15_000, owner_exec_ns=3_000,
                          result_status=0, in_flight_tx=True,
                          in_flight_tx_age_ns=4_000_000_000),
    ]
    print_owner_lag_summary(seconds=[], ops=ops, stalls=[])
    out = capsys.readouterr().out
    assert "attach: n=1" in out
    assert "apply_lifecycle: n=1" in out
    assert "in_flight_tx: 1/1 had a concurrent attach/finish window open" in out
    assert "in_flight_tx: none observed" in out


# --- owner-lag stall probe (icecream-p50-owner-lag-stall-v1) ---------------
#
# Covers coordinator message B: the fail_stop stall-diagnosis record
# written just before SidecarRuntime::transfer_source's own
# fail_stop()/std::_Exit(125). See OwnerLagStallSample's docstring in
# analyze.py and append_owner_lag_stall_trace in cache/p50_cache_service.cpp
# for the producing side.

def test_client_stall_stage_name_covers_every_enum_ordinal():
    # Mirrors cache/p50_endpoint.h's ClientStallStage declaration order
    # exactly: Connecting=0, Armed=1, SessionReady=2, BeginWritten=3,
    # StateRead=4, CommitRead=5.
    assert client_stall_stage_name(0) == "connecting"
    assert client_stall_stage_name(1) == "armed"
    assert client_stall_stage_name(2) == "session_ready"
    assert client_stall_stage_name(3) == "begin_written"
    assert client_stall_stage_name(4) == "state_read"
    assert client_stall_stage_name(5) == "commit_read"


def test_client_stall_stage_name_handles_out_of_range_value():
    # Must never raise -- a future enum value from a newer binary than the
    # one analyze.py was last updated for should still print something
    # useful instead of crashing the whole report.
    assert client_stall_stage_name(6) == "unknown(6)"
    assert client_stall_stage_name(-1) == "unknown(-1)"


def test_print_owner_lag_summary_reports_stall_record(capsys):
    stalls = [
        OwnerLagStallSample(
            pid=222, wall_clock_ns=5_000_000_000, route_identity_hash=42,
            transfer_age_ns=9_500_000_000, configured_deadline_ns=8_000_000_000,
            configured_grace_ns=1_000_000_000, last_stage=4, tu_seq=0,
            rel_seq=0),
    ]
    print_owner_lag_summary(seconds=[], ops=[], stalls=stalls)
    out = capsys.readouterr().out
    assert "fail_stop stall-diagnosis records" in out
    assert "pid=222" in out
    assert "route_identity_hash=42" in out
    assert "transfer_age_ns=9500000000" in out
    assert "configured_deadline_ns=8000000000" in out
    assert "configured_grace_ns=1000000000" in out
    # last_stage must render as the human-readable stage name, not a bare
    # integer -- ordinal 4 is StateRead.
    assert "last_stage=state_read" in out
    assert "tu_seq=0 rel_seq=0" in out


def test_print_owner_lag_summary_no_stall_records_reports_none(capsys):
    print_owner_lag_summary(seconds=[], ops=[], stalls=[])
    out = capsys.readouterr().out
    assert "no icecream-p50-owner-lag-stall-v1 records found" in out
    assert "no observed fail_stop stall" in out
