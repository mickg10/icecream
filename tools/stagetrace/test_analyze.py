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
    Record,
    SourceResultSample,
    check_clock_sanity,
    derive_ntp_offsets,
    print_ntp_offsets,
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
