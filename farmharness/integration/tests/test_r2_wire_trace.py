from __future__ import annotations

from copy import deepcopy

import pytest

from farmharness.integration.r2_wire_trace import (
    MAX_R2_INTERVALS_PER_RESULT,
    R2_ACCOUNTING_SCHEMA,
    R2_INTERVAL_EVENT_SCHEMA,
    R2_LINK_EVENT_SCHEMA,
    R2WireTraceError,
    validate_r2_wire_trace,
)


C_GUID = "1" * 32
F_GUID = "2" * 32
RELATIONSHIP_ID = "3" * 32
RAW_DIGEST = "4" * 32
LINK = {
    "c_store_guid": C_GUID,
    "f_store_guid": F_GUID,
    "logical_link_id": RELATIONSHIP_ID,
    "relationship_epoch": 7,
    "physical_link_generation": 2,
}
JOB_KEY = {
    "c_store_guid": C_GUID,
    "f_store_guid": F_GUID,
    "logical_link_id": RELATIONSHIP_ID,
    "tu_seq": 9,
    "raw_digest": RAW_DIGEST,
}
JOB_KEY_TUPLE = (C_GUID, F_GUID, RELATIONSHIP_ID, 9, RAW_DIGEST)


def _interval(
    sequence: int = 1,
    *,
    end: str = "DrainedAckCheckpoint",
    c_to_f: int = 10,
    f_to_c: int = 6,
    c_shared: int = 2,
    f_shared: int = 1,
    ack_prefix: int | None = None,
    recovery_prefix: int = 0,
    c_job: int | None = None,
    f_job: int | None = None,
    bundle_attempts: int = 1,
    replay_attempts: int = 0,
) -> dict:
    if ack_prefix is None:
        ack_prefix = 1 if end == "DrainedAckCheckpoint" else 0
    if c_job is None:
        c_job = c_to_f - c_shared
    if f_job is None:
        f_job = f_to_c - f_shared
    return {
        "link": dict(LINK),
        "interval_sequence": sequence,
        "end": end,
        "total_c_to_f_bytes": c_to_f,
        "total_f_to_c_bytes": f_to_c,
        "shared_c_to_f_bytes": c_shared,
        "shared_f_to_c_bytes": f_shared,
        "drained_ack_prefix": ack_prefix,
        "recovery_confirmed_prefix": recovery_prefix,
        "jobs": [
            {
                "key": dict(JOB_KEY),
                "c_to_f_bundle_bytes": c_job,
                "f_to_c_receipt_bytes": f_job,
                "bundle_attempts": bundle_attempts,
                "replay_attempts": replay_attempts,
                "valid": True,
            }
        ],
        "valid": True,
    }


def _accounting(
    *,
    counters: tuple[int, int, int, int] = (8, 5, 1, 0),
    intervals: list[dict] | None = None,
) -> dict:
    if intervals is None:
        intervals = [_interval(c_to_f=10, f_to_c=6)]
    return {
        "schema": R2_ACCOUNTING_SCHEMA,
        "valid": True,
        "job_key": dict(JOB_KEY),
        "job": {
            "c_to_f_bundle_bytes": counters[0],
            "f_to_c_receipt_bytes": counters[1],
            "bundle_attempts": counters[2],
            "replay_attempts": counters[3],
        },
        "intervals": intervals,
    }


def _source(
    assignment: int = 1,
    *,
    accounting: dict | None = None,
) -> dict:
    return {
        "wire_job_id": assignment,
        "assignment_epoch": 1,
        "assignment_nonce": assignment,
        "c_store_guid": C_GUID,
        "raw_digest": RAW_DIGEST,
        "tu_seq": 9,
        "r2_wire_accounting": _accounting() if accounting is None else accounting,
    }


def _ack_event(prefix: int = 1) -> dict:
    return {
        "schema": R2_LINK_EVENT_SCHEMA,
        "event": "ack_validated",
        "link": dict(LINK),
        "acknowledged_prefix": prefix,
        "valid": True,
    }


def _release_event(
    prefix: int = 1,
    *,
    acknowledged: int | None = None,
    committed: int | None = None,
) -> dict:
    if acknowledged is None:
        acknowledged = prefix
    if committed is None:
        committed = prefix
    return {
        "schema": R2_LINK_EVENT_SCHEMA,
        "event": "link_released",
        "link": dict(LINK),
        "committed_prefix": committed,
        "acknowledged_prefix": acknowledged,
        "valid": True,
    }


def _interval_event(interval: dict | None = None) -> dict:
    return {
        "schema": R2_INTERVAL_EVENT_SCHEMA,
        "interval": _interval() if interval is None else interval,
    }


def test_r2_open_link_prefix_is_valid_without_claiming_terminal_complete() -> None:
    source = _source(
        accounting=_accounting(
            intervals=[_interval(end="DrainedAckCheckpoint", ack_prefix=1)]
        )
    )

    summary = validate_r2_wire_trace([source])

    assert summary["links"][0]["physical_complete"] is False
    assert summary["links"][0]["relationship_settled"] is False
    assert summary["links"][0]["c_to_f_bytes"] == 10
    assert summary["links"][0]["shared_c_to_f_bytes"] == 2
    assert summary["links"][0]["job_counters"][JOB_KEY_TUPLE] == (
        8,
        5,
        1,
        0,
    )
    with pytest.raises(R2WireTraceError, match="C retirement/F physical-release"):
        validate_r2_wire_trace([source], require_terminal=True)


def test_r2_standalone_interval_sink_conserves_job_snapshot() -> None:
    source = _source(
        accounting=_accounting(
            intervals=[],
        )
    )

    summary = validate_r2_wire_trace(
        [source],
        c_interval_events=[_interval_event()],
        require_job_conservation=True,
    )

    assert summary["links"][0]["interval_count"] == 1
    assert summary["links"][0]["job_counters"][JOB_KEY_TUPLE] == (8, 5, 1, 0)


def test_r2_standalone_interval_sink_rejects_missing_or_wrong_job_delta() -> None:
    source = _source(accounting=_accounting(intervals=[]))
    malformed = _interval(c_shared=3, c_job=8)

    with pytest.raises(R2WireTraceError, match="C-to-F interval bytes do not conserve"):
        validate_r2_wire_trace([source], c_interval_events=[_interval_event(malformed)])

    changed = _interval(c_to_f=9, c_job=7)
    with pytest.raises(R2WireTraceError, match="cumulative per-job snapshot"):
        validate_r2_wire_trace(
            [source], c_interval_events=[_interval_event(changed)], require_job_conservation=True
        )


def test_r2_inline_and_standalone_duplicate_interval_is_reference_only() -> None:
    interval = _interval()
    source = _source(accounting=_accounting(intervals=[interval]))

    summary = validate_r2_wire_trace([source], c_interval_events=[_interval_event(interval)])

    assert summary["duplicate_intervals_reference_only"] == 1
    assert summary["links"][0]["c_to_f_bytes"] == 10


def test_r2_terminal_requires_c_retirement_and_f_ack_and_release() -> None:
    source = _source(
        accounting=_accounting(
            intervals=[
                _interval(sequence=1, end="DrainedAckCheckpoint", ack_prefix=1),
                _interval(
                    sequence=2,
                    end="PhysicalLinkRetired",
                    c_to_f=0,
                    f_to_c=0,
                    c_shared=0,
                    f_shared=0,
                    ack_prefix=0,
                    c_job=0,
                    f_job=0,
                    bundle_attempts=0,
                    replay_attempts=0,
                ),
            ]
        )
    )
    summary = validate_r2_wire_trace(
        [source], [_ack_event(), _release_event()], require_settled=True
    )
    assert summary["links"][0]["physical_complete"] is True
    assert summary["links"][0]["relationship_settled"] is True
    assert summary["links"][0]["ack_write_prefix"] == 1


@pytest.mark.parametrize(
    ("events", "end", "message"),
    (
        ([], "PhysicalLinkRetired", "matching F ACK-settlement"),
        ([_ack_event()], "PhysicalLinkRetired", "matching F ACK-settlement"),
        ([_release_event()], "PhysicalLinkRetired", "matching F ACK-settlement"),
        ([_ack_event(), _release_event(2)], "PhysicalLinkRetired", "matching F ACK-settlement"),
        ([_ack_event(), _release_event(1)], "DrainedAckCheckpoint", "matching F ACK-settlement"),
    ),
)
def test_r2_terminal_rejects_missing_or_mismatched_f_witness(
    events: list[dict], end: str, message: str,
) -> None:
    source = _source(accounting=_accounting(intervals=[_interval(end=end)]))
    with pytest.raises(R2WireTraceError, match=message):
        validate_r2_wire_trace([source], events, require_settled=True)


def test_r2_intervals_are_conserved_and_sequence_complete() -> None:
    first = _interval(
        1,
        end="WindowPressure",
        c_to_f=10,
        f_to_c=6,
        c_shared=2,
        f_shared=1,
        ack_prefix=0,
        c_job=8,
        f_job=5,
        bundle_attempts=1,
    )
    second = _interval(
        2,
        end="DrainedAckCheckpoint",
        c_to_f=5,
        f_to_c=4,
        c_shared=1,
        f_shared=1,
        ack_prefix=1,
        c_job=4,
        f_job=3,
        bundle_attempts=0,
        replay_attempts=1,
    )
    retired = _interval(
        3,
        end="PhysicalLinkRetired",
        c_to_f=0,
        f_to_c=0,
        c_shared=0,
        f_shared=0,
        ack_prefix=0,
        c_job=0,
        f_job=0,
        bundle_attempts=0,
        replay_attempts=0,
    )
    source1 = _source(accounting=_accounting(counters=(8, 5, 1, 0), intervals=[first]))
    source2 = _source(
        2,
        accounting=_accounting(counters=(12, 8, 1, 1), intervals=[second, retired]),
    )
    summary = validate_r2_wire_trace(
        [source1, source2], [_ack_event(), _release_event()], require_terminal=True
    )
    link = summary["links"][0]
    assert link["interval_count"] == 3
    assert link["c_to_f_bytes"] == 15
    assert link["f_to_c_bytes"] == 10
    assert link["physical_complete"] is True
    assert link["relationship_settled"] is True
    assert link["shared_c_to_f_bytes"] == 3
    assert summary["jobs"][JOB_KEY_TUPLE]["counters"] == (
        12,
        8,
        1,
        1,
    )


def test_r2_exact_duplicate_snapshots_and_intervals_are_reference_only() -> None:
    interval = _interval(end="PhysicalLinkRetired")
    source1 = _source(accounting=_accounting(intervals=[interval]))
    source2 = _source(2, accounting=_accounting(intervals=[deepcopy(interval)]))

    summary = validate_r2_wire_trace(
        [source1, source2], [_ack_event(), _release_event()], require_terminal=True
    )

    assert summary["duplicate_job_snapshots_reference_only"] == 1
    assert summary["duplicate_intervals_reference_only"] == 1
    assert len(summary["links"]) == 1
    assert summary["links"][0]["interval_count"] == 1


def test_r2_conflicting_same_sequence_and_nonconservation_fail_closed() -> None:
    original = _interval(end="PhysicalLinkRetired")
    conflict = deepcopy(original)
    conflict["total_c_to_f_bytes"] += 1
    conflict["shared_c_to_f_bytes"] += 1
    with pytest.raises(R2WireTraceError, match="conflicting duplicate"):
        validate_r2_wire_trace(
            [
                _source(accounting=_accounting(intervals=[original])),
                _source(2, accounting=_accounting(intervals=[conflict])),
            ]
        )

    broken = deepcopy(original)
    broken["shared_c_to_f_bytes"] += 1
    with pytest.raises(R2WireTraceError, match="do not conserve"):
        validate_r2_wire_trace([_source(accounting=_accounting(intervals=[broken]))])


def test_r2_global_sequence_gap_invalid_rows_and_pressure_ack_are_rejected() -> None:
    first = _interval(1, end="WindowPressure", ack_prefix=0)
    third = _interval(3, end="PhysicalLinkRetired", ack_prefix=0)
    with pytest.raises(R2WireTraceError, match="sender interval sequence has a gap"):
        validate_r2_wire_trace(
            [_source(accounting=_accounting(intervals=[first])),
             _source(2, accounting=_accounting(intervals=[third]))]
        )

    pressure_with_ack = _interval(1, end="WindowPressure", ack_prefix=1)
    with pytest.raises(R2WireTraceError, match="cannot claim a drained ACK"):
        validate_r2_wire_trace([_source(accounting=_accounting(intervals=[pressure_with_ack]))])

    invalid = _interval(end="PhysicalLinkRetired")
    invalid["valid"] = False
    with pytest.raises(R2WireTraceError, match="explicitly invalid"):
        validate_r2_wire_trace([_source(accounting=_accounting(intervals=[invalid]))])


def test_r2_sequence_continues_across_physical_generations_without_starting_at_one() -> None:
    prior_link = dict(LINK, physical_link_generation=2)
    next_link = dict(LINK, physical_link_generation=3)
    first = _interval(8, end="WindowPressure", ack_prefix=0)
    first["link"] = prior_link
    second = _interval(9, end="PhysicalLinkRetired", ack_prefix=0)
    second["link"] = next_link
    accounting = _accounting(counters=(16, 10, 2, 0), intervals=[first, second])

    summary = validate_r2_wire_trace([_source(accounting=accounting)])

    assert [row["sequence_start"] for row in summary["links"]] == [8, 9]
    assert [row["sequence_end"] for row in summary["links"]] == [8, 9]


def test_r2_partial_sequence_is_parseable_but_not_full_numeric_conservation() -> None:
    suffix = _interval(8, end="PhysicalLinkRetired", ack_prefix=0)
    record = _source(accounting=_accounting(intervals=[suffix]))

    # Prefix/suffix diagnostics are structurally valid, but a full byte-total
    # claim cannot omit sequence 1 (which may contain HELLO/control bytes).
    validate_r2_wire_trace([record])
    with pytest.raises(R2WireTraceError, match="initial sender interval"):
        validate_r2_wire_trace([record], require_job_conservation=True)


def test_r2_only_ack_write_checkpoint_may_carry_drained_prefix() -> None:
    retired_with_ack = _interval(1, end="PhysicalLinkRetired", ack_prefix=1)
    with pytest.raises(R2WireTraceError, match="only a drained-ACK checkpoint"):
        validate_r2_wire_trace([_source(accounting=_accounting(intervals=[retired_with_ack]))])


def test_r2_reset_epoch_change_does_not_create_a_new_physical_link() -> None:
    before_reset = dict(LINK, physical_link_generation=4, relationship_epoch=7)
    after_reset = dict(LINK, physical_link_generation=4, relationship_epoch=8)
    checkpoint = _interval(20, end="DrainedAckCheckpoint", ack_prefix=1)
    checkpoint["link"] = before_reset
    retired = _interval(
        21,
        end="PhysicalLinkRetired",
        c_to_f=0,
        f_to_c=0,
        c_shared=0,
        f_shared=0,
        ack_prefix=0,
        c_job=0,
        f_job=0,
        bundle_attempts=0,
        replay_attempts=0,
    )
    retired["link"] = after_reset
    ack = _ack_event(1)
    ack["link"] = after_reset
    release = _release_event(1)
    release["link"] = after_reset

    summary = validate_r2_wire_trace(
        [_source(accounting=_accounting(intervals=[checkpoint, retired]))],
        [ack, release],
        require_settled=True,
    )

    assert len(summary["links"]) == 2  # epoch-specific evidence rows
    assert all(row["physical_complete"] for row in summary["links"])
    assert summary["closed_relationships"] == [(C_GUID, F_GUID, RELATIONSHIP_ID)]


def test_r2_failed_reconnect_bytes_stay_on_attempted_generation() -> None:
    accepted = dict(LINK, physical_link_generation=2)
    attempted = dict(LINK, physical_link_generation=3)
    prior = _interval(30, end="PhysicalLinkRetired", ack_prefix=0)
    prior["link"] = accepted
    failed_attempt = _interval(31, end="WindowPressure", ack_prefix=0)
    failed_attempt["link"] = attempted
    source = _source(accounting=_accounting(intervals=[prior, failed_attempt]))
    release = _release_event(0)
    release["link"] = accepted

    summary = validate_r2_wire_trace([source], [_ack_event(0), release])

    assert [row["link"][4] for row in summary["links"]] == [2, 3]
    assert [row["c_to_f_bytes"] for row in summary["links"]] == [10, 10]
    assert [row["physical_complete"] for row in summary["links"]] == [True, False]


def test_r2_checkpoint_prefix_ignores_zero_fields_on_pressure_and_retirement() -> None:
    checkpoint = _interval(8, end="DrainedAckCheckpoint", ack_prefix=1)
    pressure = _interval(9, end="WindowPressure", ack_prefix=0)
    retired = _interval(10, end="PhysicalLinkRetired", ack_prefix=0)
    accounting = _accounting(
        counters=(24, 15, 3, 0), intervals=[checkpoint, pressure, retired]
    )

    summary = validate_r2_wire_trace([_source(accounting=accounting)])

    assert summary["links"][0]["ack_write_prefix"] == 1
    assert summary["links"][0]["physical_complete"] is False


def test_r2_recovery_confirmation_is_not_a_completed_ack_write() -> None:
    checkpoint = _interval(8, end="DrainedAckCheckpoint", ack_prefix=1)
    recovery = _interval(
        9,
        end="RecoveryConfirmed",
        ack_prefix=0,
        recovery_prefix=2,
        c_to_f=0,
        f_to_c=0,
        c_shared=0,
        f_shared=0,
        c_job=0,
        f_job=0,
        bundle_attempts=0,
        replay_attempts=0,
    )
    source = _source(
        accounting=_accounting(counters=(8, 5, 1, 0), intervals=[checkpoint, recovery])
    )

    summary = validate_r2_wire_trace([source])

    assert summary["links"][0]["ack_write_prefix"] == 1


def test_r2_recovery_confirmation_cannot_claim_ack_write_prefix() -> None:
    interval = _interval(
        end="RecoveryConfirmed", ack_prefix=1, recovery_prefix=2
    )
    with pytest.raises(R2WireTraceError, match="only a drained-ACK checkpoint"):
        validate_r2_wire_trace([_source(accounting=_accounting(intervals=[interval]))])


def test_r2_physical_retirement_can_preserve_unacknowledged_receipt_recovery() -> None:
    interval = _interval(end="PhysicalLinkRetired", ack_prefix=0)
    accounting = _accounting(intervals=[interval])
    source = _source(accounting=accounting)
    # F has one committed but not yet acknowledged receipt when the physical
    # socket is released. This is valid recovery state, not settled state.
    events = [_ack_event(0), _release_event(acknowledged=0, committed=1)]

    summary = validate_r2_wire_trace([source], events, require_terminal=True)

    assert summary["links"][0]["physical_complete"] is True
    assert summary["links"][0]["relationship_settled"] is False
    assert summary["settled_relationships"] == []


def test_r2_first_tu_zero_and_zero_digest_are_valid() -> None:
    source = _source()
    accounting = source["r2_wire_accounting"]
    accounting["job_key"]["tu_seq"] = 0
    accounting["job_key"]["raw_digest"] = "0" * 32
    accounting["intervals"][0]["jobs"][0]["key"]["tu_seq"] = 0
    accounting["intervals"][0]["jobs"][0]["key"]["raw_digest"] = "0" * 32
    source["tu_seq"] = 0
    source["raw_digest"] = "0" * 32

    summary = validate_r2_wire_trace([source])

    assert (C_GUID, F_GUID, RELATIONSHIP_ID, 0, "0" * 32) in summary["jobs"]


def test_r2_interval_vector_cap_matches_sender_bound() -> None:
    intervals = [
        _interval(sequence=index + 1, end="WindowPressure", ack_prefix=0)
        for index in range(MAX_R2_INTERVALS_PER_RESULT + 1)
    ]
    with pytest.raises(R2WireTraceError, match="bounded list"):
        validate_r2_wire_trace([_source(accounting=_accounting(intervals=intervals))])
