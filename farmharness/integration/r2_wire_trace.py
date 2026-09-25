"""Fail-closed validation for Protocol-50 R2 byte-accounting traces.

The R2 sender reports two deliberately different kinds of evidence:

* a cumulative per-job snapshot, which is a witness and must never be summed;
* bounded per-link interval deltas, which are additive after link/sequence
  deduplication.

This module validates that distinction without inferring F-side ACK processing
or physical-link closure from C-side writes. A prefix trace may be valid while
the link is open; callers that need complete accounting pass
``require_terminal=True`` and supply F-side link events.
"""

from __future__ import annotations

from collections import defaultdict
from collections.abc import Iterable, Mapping
from typing import Any


R2_ACCOUNTING_SCHEMA = "icecream-p50-r2-wire-accounting-v1"
R2_LINK_EVENT_SCHEMA = "icecream-p50-r2-link-event-v1"
R2_INTERVAL_EVENT_SCHEMA = "icecream-p50-r2-interval-event-v1"
R2_INTERVAL_ENDS = frozenset(
    (
        "WindowPressure",
        "DrainedAckCheckpoint",
        "RecoveryConfirmed",
        "PhysicalLinkRetired",
    )
)
MAX_R2_INTERVALS_PER_RESULT = 30
MAX_R2_JOBS_PER_INTERVAL = 60
_UINT32_MAX = (1 << 32) - 1
_UINT64_MAX = (1 << 64) - 1


class R2WireTraceError(ValueError):
    """An R2 trace is malformed, contradictory, or incomplete for its claim."""


_JOB_KEY_FIELDS = frozenset(
    {"c_store_guid", "f_store_guid", "logical_link_id", "tu_seq", "raw_digest"}
)
_LINK_FIELDS = frozenset(
    {
        "c_store_guid",
        "f_store_guid",
        "logical_link_id",
        "relationship_epoch",
        "physical_link_generation",
    }
)
_JOB_COUNTER_FIELDS = frozenset(
    {
        "c_to_f_bundle_bytes",
        "f_to_c_receipt_bytes",
        "bundle_attempts",
        "replay_attempts",
    }
)
_ACCOUNTING_FIELDS = frozenset({"schema", "valid", "job_key", "job", "intervals"})
_INTERVAL_FIELDS = frozenset(
    {
        "link",
        "interval_sequence",
        "end",
        "total_c_to_f_bytes",
        "total_f_to_c_bytes",
        "shared_c_to_f_bytes",
        "shared_f_to_c_bytes",
        "drained_ack_prefix",
        "recovery_confirmed_prefix",
        "jobs",
        "valid",
    }
)
_INTERVAL_JOB_FIELDS = frozenset({"key", *_JOB_COUNTER_FIELDS, "valid"})


def _exact_fields(value: Any, fields: frozenset[str], where: str) -> Mapping[str, Any]:
    if not isinstance(value, Mapping) or frozenset(value) != fields:
        raise R2WireTraceError(f"{where}: fields do not match the R2 trace schema")
    return value


def _uint(
    value: Any,
    where: str,
    *,
    positive: bool = False,
    maximum: int = _UINT64_MAX,
) -> int:
    if (
        type(value) is not int
        or value < (1 if positive else 0)
        or value > maximum
    ):
        bound = "positive" if positive else "nonnegative"
        raise R2WireTraceError(f"{where}: expected a {bound} integer")
    return value


def _guid(value: Any, where: str) -> str:
    if (
        not isinstance(value, str)
        or len(value) != 32
        or any(char not in "0123456789abcdef" for char in value)
        or value == "0" * 32
    ):
        raise R2WireTraceError(f"{where}: invalid nonzero lowercase GUID")
    return value


def _digest(value: Any, where: str) -> str:
    if (
        not isinstance(value, str)
        or len(value) != 32
        or any(char not in "0123456789abcdef" for char in value)
    ):
        raise R2WireTraceError(f"{where}: invalid lowercase digest")
    return value


def _job_key(value: Any, where: str) -> tuple[str, str, str, int, str]:
    row = _exact_fields(value, _JOB_KEY_FIELDS, where)
    return (
        _guid(row["c_store_guid"], f"{where}.c_store_guid"),
        _guid(row["f_store_guid"], f"{where}.f_store_guid"),
        _guid(row["logical_link_id"], f"{where}.logical_link_id"),
        _uint(row["tu_seq"], f"{where}.tu_seq"),
        _digest(row["raw_digest"], f"{where}.raw_digest"),
    )


def canonical_r2_job_key(value: Any) -> tuple[str, str, str, int, str]:
    """Return the canonical key used to join a v5 R2 measurement reference."""
    return _job_key(value, "r2_accounting_key")


def _link_job_key(link: tuple[str, str, str, int, int], tu_seq: int, raw_digest: str):
    return link[0], link[1], link[2], tu_seq, raw_digest


def _link(value: Any, where: str) -> tuple[str, str, str, int, int]:
    row = _exact_fields(value, _LINK_FIELDS, where)
    return (
        _guid(row["c_store_guid"], f"{where}.c_store_guid"),
        _guid(row["f_store_guid"], f"{where}.f_store_guid"),
        _guid(row["logical_link_id"], f"{where}.logical_link_id"),
        _uint(row["relationship_epoch"], f"{where}.relationship_epoch", positive=True),
        _uint(
            row["physical_link_generation"],
            f"{where}.physical_link_generation",
            positive=True,
        ),
    )


def _physical_link_key(link: tuple[str, str, str, int, int]) -> tuple[str, str, str, int]:
    """Relationship identity plus socket generation (epoch may reset in-place)."""
    return link[0], link[1], link[2], link[4]


def _counters(value: Any, where: str) -> tuple[int, int, int, int]:
    row = _exact_fields(value, _JOB_COUNTER_FIELDS, where)
    return (
        _uint(row["c_to_f_bundle_bytes"], f"{where}.c_to_f_bundle_bytes"),
        _uint(row["f_to_c_receipt_bytes"], f"{where}.f_to_c_receipt_bytes"),
        _uint(
            row["bundle_attempts"],
            f"{where}.bundle_attempts",
            maximum=_UINT32_MAX,
        ),
        _uint(
            row["replay_attempts"],
            f"{where}.replay_attempts",
            maximum=_UINT32_MAX,
        ),
    )


def _validate_accounting(value: Any, where: str) -> dict[str, Any]:
    accounting = _exact_fields(value, _ACCOUNTING_FIELDS, where)
    if accounting["schema"] != R2_ACCOUNTING_SCHEMA or accounting["valid"] is not True:
        raise R2WireTraceError(f"{where}: unsupported schema or invalid accounting witness")
    key = _job_key(accounting["job_key"], f"{where}.job_key")
    counters = _counters(accounting["job"], f"{where}.job")
    intervals = accounting["intervals"]
    if not isinstance(intervals, list) or len(intervals) > MAX_R2_INTERVALS_PER_RESULT:
        raise R2WireTraceError(
            f"{where}.intervals: expected a bounded list of at most "
            f"{MAX_R2_INTERVALS_PER_RESULT} entries"
        )

    normalized = []
    for index, item in enumerate(intervals):
        item_where = f"{where}.intervals[{index}]"
        interval = _exact_fields(item, _INTERVAL_FIELDS, item_where)
        link = _link(interval["link"], f"{item_where}.link")
        if link[:3] != (key[0], key[1], key[2]):
            raise R2WireTraceError(f"{item_where}: link and accounting key disagree")
        if interval["valid"] is not True:
            raise R2WireTraceError(f"{item_where}: interval is explicitly invalid")
        sequence = _uint(
            interval["interval_sequence"], f"{item_where}.interval_sequence", positive=True
        )
        end = interval["end"]
        if end not in R2_INTERVAL_ENDS:
            raise R2WireTraceError(f"{item_where}.end: unknown interval end reason")
        totals = (
            _uint(interval["total_c_to_f_bytes"], f"{item_where}.total_c_to_f_bytes"),
            _uint(interval["total_f_to_c_bytes"], f"{item_where}.total_f_to_c_bytes"),
        )
        shared = (
            _uint(interval["shared_c_to_f_bytes"], f"{item_where}.shared_c_to_f_bytes"),
            _uint(interval["shared_f_to_c_bytes"], f"{item_where}.shared_f_to_c_bytes"),
        )
        ack_prefix = _uint(interval["drained_ack_prefix"], f"{item_where}.drained_ack_prefix")
        recovery_prefix = _uint(
            interval["recovery_confirmed_prefix"],
            f"{item_where}.recovery_confirmed_prefix",
        )
        if end == "WindowPressure" and ack_prefix != 0:
            raise R2WireTraceError(
                f"{item_where}: a window-pressure interval cannot claim a drained ACK prefix"
            )
        if end != "DrainedAckCheckpoint" and ack_prefix != 0:
            raise R2WireTraceError(
                f"{item_where}: only a drained-ACK checkpoint may report that prefix"
            )
        if end != "RecoveryConfirmed" and recovery_prefix != 0:
            raise R2WireTraceError(
                f"{item_where}: only a recovery-confirmed interval may report that prefix"
            )
        if end == "RecoveryConfirmed" and ack_prefix != 0:
            raise R2WireTraceError(
                f"{item_where}: recovery confirmation is not a completed ACK write"
            )
        jobs = interval["jobs"]
        if not isinstance(jobs, list):
            raise R2WireTraceError(f"{item_where}.jobs: expected a list")
        if len(jobs) > MAX_R2_JOBS_PER_INTERVAL:
            raise R2WireTraceError(
                f"{item_where}.jobs: exceeds bounded {MAX_R2_JOBS_PER_INTERVAL}-job snapshot"
            )
        job_deltas: dict[tuple[str, str, str, int, str], tuple[int, int, int, int]] = {}
        for job_index, job_value in enumerate(jobs):
            job_where = f"{item_where}.jobs[{job_index}]"
            job = _exact_fields(job_value, _INTERVAL_JOB_FIELDS, job_where)
            if job["valid"] is not True:
                raise R2WireTraceError(f"{job_where}: job interval is explicitly invalid")
            job_key = _job_key(job["key"], f"{job_where}.key")
            if job_key[:3] != link[:3]:
                raise R2WireTraceError(f"{job_where}: key belongs to another link")
            if job_key in job_deltas:
                raise R2WireTraceError(f"{job_where}: duplicate job key in interval")
            job_deltas[job_key] = _counters(
                {field: job[field] for field in _JOB_COUNTER_FIELDS}, job_where
            )
        if totals[0] != shared[0] + sum(delta[0] for delta in job_deltas.values()):
            raise R2WireTraceError(f"{item_where}: C-to-F interval bytes do not conserve")
        if totals[1] != shared[1] + sum(delta[1] for delta in job_deltas.values()):
            raise R2WireTraceError(f"{item_where}: F-to-C interval bytes do not conserve")
        normalized.append(
            {
                "ack_prefix": ack_prefix,
                "end": end,
                "recovery_prefix": recovery_prefix,
                "job_deltas": job_deltas,
                "link": link,
                "sequence": sequence,
                "shared": shared,
                "totals": totals,
            }
        )
    return {"counters": counters, "intervals": normalized, "job_key": key}


def _validate_link_event(value: Any, where: str) -> dict[str, Any]:
    if not isinstance(value, Mapping):
        raise R2WireTraceError(f"{where}: expected an object")
    event = value.get("event")
    expected = (
        frozenset({"schema", "event", "link", "acknowledged_prefix", "valid"})
        if event == "ack_validated"
        else frozenset(
            {
                "schema",
                "event",
                "link",
                "committed_prefix",
                "acknowledged_prefix",
                "valid",
            }
        )
        if event == "link_released"
        else frozenset()
    )
    row = _exact_fields(value, expected, where)
    if row["schema"] != R2_LINK_EVENT_SCHEMA or row["valid"] is not True:
        raise R2WireTraceError(f"{where}: unsupported or invalid F-side link event")
    link = _link(row["link"], f"{where}.link")
    if event == "ack_validated":
        prefix = _uint(row["acknowledged_prefix"], f"{where}.acknowledged_prefix")
        return {"event": event, "link": link, "prefix": prefix}
    committed = _uint(row["committed_prefix"], f"{where}.committed_prefix")
    acknowledged = _uint(row["acknowledged_prefix"], f"{where}.acknowledged_prefix")
    return {
        "acknowledged": acknowledged,
        "committed": committed,
        "event": event,
        "link": link,
    }


def _validate_interval_event(value: Any, where: str) -> dict[str, Any]:
    """Validate one standalone sender interval using the accounting DTO rules."""
    row = _exact_fields(value, frozenset({"schema", "interval"}), where)
    if row["schema"] != R2_INTERVAL_EVENT_SCHEMA:
        raise R2WireTraceError(f"{where}: unsupported C interval event schema")
    interval_value = row["interval"]
    if not isinstance(interval_value, Mapping):
        raise R2WireTraceError(f"{where}.interval: expected an object")
    link_value = interval_value.get("link")
    if not isinstance(link_value, Mapping):
        raise R2WireTraceError(f"{where}.interval.link: expected an object")
    link = _link(link_value, f"{where}.interval.link")
    # Standalone control intervals have no single owning job. The synthetic
    # zero TU/digest key exists only to reuse the exact interval validator;
    # its job deltas still carry their own full canonical keys.
    normalized = _validate_accounting(
        {
            "schema": R2_ACCOUNTING_SCHEMA,
            "valid": True,
            "job_key": {
                "c_store_guid": link[0],
                "f_store_guid": link[1],
                "logical_link_id": link[2],
                "tu_seq": 0,
                "raw_digest": "0" * 32,
            },
            "job": {
                "c_to_f_bundle_bytes": 0,
                "f_to_c_receipt_bytes": 0,
                "bundle_attempts": 0,
                "replay_attempts": 0,
            },
            "intervals": [interval_value],
        },
        f"{where}.accounting",
    )
    return normalized["intervals"][0]


def validate_r2_wire_trace(
    source_results: Iterable[Mapping[str, Any]],
    f_link_events: Iterable[Mapping[str, Any]] = (),
    c_interval_events: Iterable[Mapping[str, Any]] = (),
    *,
    require_terminal: bool = False,
    require_settled: bool = False,
    require_job_conservation: bool = False,
) -> dict[str, Any]:
    """Validate v5 R2 snapshots, intervals, and optional F-side evidence.

    Cumulative job values are retained as snapshots. Repeated identical
    snapshots and intervals are represented in the normalized output as
    ``reference_only`` rows. They are not added to totals. Changed cumulative
    snapshots must be monotone; additive accounting comes only from unique
    sender interval sequences. ``require_terminal`` requires both C retirement
    and F physical-link release, proving closed-socket byte coverage. It does
    not claim the relationship is ACK-settled. ``require_settled`` additionally
    requires a matching F-validated ACK prefix and release witness with K == Q.
    """

    job_snapshots: dict[
        tuple[str, str, str, int, str],
        list[tuple[tuple[int, int, int, int], tuple[str, int, int, int]]],
    ] = defaultdict(list)
    canonical_job_source: dict[
        tuple[str, str, str, int, str], tuple[str, int, int, int]
    ] = {}
    source_refs: dict[
        tuple[str, int, int, int], tuple[str, str, str, int, str]
    ] = {}
    links: dict[tuple[str, str, str, int, int], dict[str, Any]] = {}
    canonical_intervals: dict[
        tuple[tuple[str, str, str], int], dict[str, Any]
    ] = {}
    canonical_interval_source: dict[
        tuple[tuple[str, str, str], int], Any
    ] = {}
    interval_rows = []
    interval_references = []
    duplicate_job_snapshots = 0
    duplicate_intervals = 0
    pending_references: list[tuple[tuple[str, int, int, int], tuple[str, str, str, int, str]]] = []
    unavailable_snapshots = 0

    def add_interval(interval: dict[str, Any], source_ref: Any, where: str) -> None:
        link_key = interval["link"]
        # CompletionLog sequence is monotonic per logical sender and does not
        # reset when the same relationship opens a new physical link.
        sender_key = link_key[:3]
        sequence_key = (sender_key, interval["sequence"])
        previous = canonical_intervals.get(sequence_key)
        if previous is not None:
            if previous != interval:
                raise R2WireTraceError(f"{where}: conflicting duplicate link interval")
            nonlocal duplicate_intervals
            duplicate_intervals += 1
            interval_references.append(
                {
                    "interval_sequence": interval["sequence"],
                    "link": link_key,
                    "reference_only": True,
                    "reference_to": canonical_interval_source[sequence_key],
                    "source_result": source_ref,
                }
            )
            return
        canonical_intervals[sequence_key] = interval
        canonical_interval_source[sequence_key] = source_ref
        interval_rows.append(
            {
                "interval_sequence": interval["sequence"],
                "link": link_key,
                "reference_only": False,
                "source_result": source_ref,
            }
        )
        link_state = links.setdefault(
            link_key,
            {
                "intervals": {},
                "totals": [0, 0],
                "shared": [0, 0],
                "job_deltas": defaultdict(lambda: [0, 0, 0, 0]),
            },
        )
        link_state["intervals"][interval["sequence"]] = interval
        for direction in (0, 1):
            link_state["totals"][direction] += interval["totals"][direction]
            link_state["shared"][direction] += interval["shared"][direction]
        for key, delta in interval["job_deltas"].items():
            aggregate = link_state["job_deltas"][key]
            for counter in range(4):
                aggregate[counter] += delta[counter]

    for index, source in enumerate(source_results):
        if not isinstance(source, Mapping):
            raise R2WireTraceError(f"source_results[{index}]: expected an object")
        row_id = (
            _guid(source.get("c_store_guid"), f"source_results[{index}].c_store_guid"),
            _uint(source.get("wire_job_id"), f"source_results[{index}].wire_job_id", positive=True),
            _uint(source.get("assignment_epoch"), f"source_results[{index}].assignment_epoch", positive=True),
            _uint(source.get("assignment_nonce"), f"source_results[{index}].assignment_nonce", positive=True),
        )
        if row_id in source_refs:
            raise R2WireTraceError(f"source_results[{index}]: duplicate source-result identity")
        row_reference = source.get("r2_accounting_reference", False)
        if type(row_reference) is not bool:
            raise R2WireTraceError(
                f"source_results[{index}].r2_accounting_reference: expected a boolean"
            )
        outer_key_value = source.get("r2_accounting_key")
        outer_key = (
            _job_key(outer_key_value, f"source_results[{index}].r2_accounting_key")
            if outer_key_value is not None
            else None
        )
        accounting_value = source.get("r2_wire_accounting")
        if accounting_value is None:
            if row_reference:
                if outer_key is None:
                    raise R2WireTraceError(
                        f"source_results[{index}]: reference row lacks its exact R2 job key"
                    )
                pending_references.append((row_id, outer_key))
            else:
                unavailable_snapshots += 1
            continue
        if row_reference:
            raise R2WireTraceError(
                f"source_results[{index}]: reference row duplicates an R2 accounting payload"
            )
        accounting = _validate_accounting(
            accounting_value, f"source_results[{index}].r2_wire_accounting"
        )
        job_key = accounting["job_key"]
        if outer_key is not None and outer_key != job_key:
            raise R2WireTraceError(
                f"source_results[{index}]: outer R2 accounting key differs from payload"
            )
        source_c_guid = source.get("c_store_guid")
        source_raw_digest = source.get("raw_digest")
        source_tu_seq = source.get("tu_seq")
        if (
            source_c_guid != job_key[0]
            or source_raw_digest != job_key[4]
            or source_tu_seq != job_key[3]
        ):
            raise R2WireTraceError(
                f"source_results[{index}]: R2 job key differs from the source-result identity"
            )
        counters = accounting["counters"]
        job_snapshots[job_key].append((counters, row_id))
        canonical_job_source.setdefault(job_key, row_id)
        source_refs[row_id] = job_key

        for interval in accounting["intervals"]:
            add_interval(interval, row_id, f"source_results[{index}]")

    for index, event in enumerate(c_interval_events):
        interval = _validate_interval_event(event, f"c_interval_events[{index}]")
        add_interval(interval, None, f"c_interval_events[{index}]")

    events = [_validate_link_event(event, f"f_link_events[{i}]")
              for i, event in enumerate(f_link_events)]
    events_by_physical_link: dict[tuple[str, str, str, int], list[dict[str, Any]]] = defaultdict(list)
    for event in events:
        events_by_physical_link[_physical_link_key(event["link"])].append(event)

    latest_jobs: dict[tuple[str, str, str, int, str], tuple[int, int, int, int]] = {}
    for job_key, snapshots in job_snapshots.items():
        vectors = [snapshot for snapshot, _ in snapshots]
        for left in vectors:
            for right in vectors:
                if not all(a <= b for a, b in zip(left, right)) and not all(
                    b <= a for a, b in zip(left, right)
                ):
                    raise R2WireTraceError(
                        "cumulative R2 job snapshots are not monotonically comparable"
                    )
        latest = max(vectors, key=lambda row: sum(row))
        latest_jobs[job_key] = latest
        duplicate_job_snapshots += len(vectors) - len(set(vectors))

    all_job_deltas: dict[tuple[str, str, str, int, str], list[int]] = defaultdict(
        lambda: [0, 0, 0, 0]
    )
    for state in links.values():
        for job_key, delta in state["job_deltas"].items():
            aggregate = all_job_deltas[job_key]
            for counter in range(4):
                aggregate[counter] += delta[counter]

    for row_id, reference_key in pending_references:
        if reference_key not in canonical_job_source:
            raise R2WireTraceError(
                f"R2 accounting reference {row_id} has no exact measured canonical key"
            )

    # CompletionLog's interval sequence belongs to the logical sender, not a
    # physical generation. Validate adjacent observed sequence numbers without
    # constructing range(1, N). Structural parsing accepts a captured suffix;
    # a full numeric-conservation claim additionally requires the trace to
    # include the initial interval (which may contain the HELLO bytes).
    sender_sequences: dict[tuple[str, str, str], set[int]] = defaultdict(set)
    for sender_link, sequence in canonical_intervals:
        sender_sequences[sender_link].add(sequence)
    for sequences in sender_sequences.values():
        ordered = sorted(sequences)
        if require_job_conservation and ordered[0] != 1:
            raise R2WireTraceError(
                "full R2 byte conservation requires the initial sender interval"
            )
        if any(right != left + 1 for left, right in zip(ordered, ordered[1:])):
            raise R2WireTraceError("R2 sender interval sequence has a gap")

    output_links = []
    terminal_by_physical_link: dict[tuple[str, str, str, int], bool] = {}
    settled_by_physical_link: dict[tuple[str, str, str, int], bool] = {}
    for link_key, state in sorted(links.items()):
        intervals = state["intervals"]
        sequences = sorted(intervals)
        physical_key = _physical_link_key(link_key)
        physical_intervals = [
            interval
            for candidate_key, candidate_state in links.items()
            if _physical_link_key(candidate_key) == physical_key
            for interval in candidate_state["intervals"].values()
        ]
        physical_sequences = sorted(interval["sequence"] for interval in physical_intervals)
        ack_checkpoints = [
            interval["ack_prefix"]
            for interval in sorted(physical_intervals, key=lambda row: row["sequence"])
            if interval["end"] == "DrainedAckCheckpoint"
        ]
        if any(right < left for left, right in zip(ack_checkpoints, ack_checkpoints[1:])):
            raise R2WireTraceError("R2 drained ACK prefix decreased within a physical link")
        # WindowPressure and PhysicalLinkRetired intentionally carry zero in
        # this field; neither resets the latest completed ACK-write checkpoint.
        final_ack_prefix = ack_checkpoints[-1] if ack_checkpoints else 0
        retirement_sequences = [
            interval["sequence"]
            for interval in physical_intervals
            if interval["end"] == "PhysicalLinkRetired"
        ]
        if len(retirement_sequences) > 1 or (
            retirement_sequences and retirement_sequences[0] != physical_sequences[-1]
        ):
            raise R2WireTraceError("R2 physical-link retirement is not the final interval")

        link_jobs = state["job_deltas"]

        link_events = events_by_physical_link.get(physical_key, [])
        f_ack_prefixes = [
            event["prefix"]
            for event in link_events
            if event["event"] == "ack_validated"
        ]
        if any(right < left for left, right in zip(f_ack_prefixes, f_ack_prefixes[1:])):
            raise R2WireTraceError("F-side validated ACK prefix decreased within a link")
        released = [event for event in link_events if event["event"] == "link_released"]
        if len(released) > 1:
            raise R2WireTraceError("R2 link has duplicate F-side release events")
        physical_complete = bool(retirement_sequences and released)
        relationship_settled = False
        if physical_complete:
            release = released[0]
            if f_ack_prefixes and f_ack_prefixes[-1] > release["acknowledged"]:
                raise R2WireTraceError(
                    "F-side ACK validation exceeds the release's acknowledged prefix"
                )
            relationship_settled = (
                bool(f_ack_prefixes)
                and f_ack_prefixes[-1] == release["acknowledged"]
                and release["acknowledged"] == final_ack_prefix
                and release["committed"] == release["acknowledged"]
            )
        terminal_by_physical_link[physical_key] = physical_complete
        settled_by_physical_link[physical_key] = relationship_settled
        if require_terminal and not physical_complete:
            raise R2WireTraceError("R2 link accounting lacks C retirement/F physical-release evidence")
        if require_settled and not relationship_settled:
            raise R2WireTraceError("R2 relationship lacks matching F ACK-settlement evidence")

        output_links.append(
            {
                "ack_write_prefix": final_ack_prefix if ack_checkpoints else None,
                "c_to_f_bytes": state["totals"][0],
                "physical_complete": physical_complete,
                "relationship_settled": relationship_settled,
                "f_to_c_bytes": state["totals"][1],
                "interval_count": len(sequences),
                "job_counters": {
                    key: tuple(value) for key, value in sorted(link_jobs.items())
                },
                "link": link_key,
                "reference_only": False,
                "sequence_start": sequences[0] if sequences else None,
                "sequence_end": sequences[-1] if sequences else None,
                "shared_c_to_f_bytes": state["shared"][0],
                "shared_f_to_c_bytes": state["shared"][1],
            }
        )

    if require_terminal:
        event_links = set(events_by_physical_link)
        traced_physical_links = {_physical_link_key(link) for link in links}
        if event_links != traced_physical_links:
            raise R2WireTraceError("F-side R2 link events do not exactly cover traced links")

    # Compare a cumulative source snapshot with additive interval deltas only
    # when every observed physical generation for its logical relationship is
    # physically closed. Unacknowledged receipts on a retired socket remain
    # valid recovery evidence and do not invalidate byte conservation.
    closed_relationships = {
        logical_key
        for logical_key in {key[:3] for key in links}
        if all(
            terminal_by_physical_link[physical_key]
            for physical_key in terminal_by_physical_link
            if physical_key[:3] == logical_key
        )
    }
    for job_key, snapshot in latest_jobs.items():
        if not require_job_conservation and job_key[:3] not in closed_relationships:
            continue
        deltas = tuple(all_job_deltas.get(job_key, (0, 0, 0, 0)))
        if snapshot != deltas:
            raise R2WireTraceError(
                "R2 cumulative per-job snapshot differs from additive interval deltas"
            )

    return {
        "duplicate_intervals_reference_only": duplicate_intervals,
        "duplicate_job_snapshots_reference_only": duplicate_job_snapshots,
        "unavailable_job_snapshots": unavailable_snapshots,
        "jobs": {
            key: {"counters": counters, "reference_to": canonical_job_source[key]}
            for key, counters in sorted(latest_jobs.items())
        },
        "links": output_links,
        "closed_relationships": sorted(closed_relationships),
        "settled_relationships": sorted(
            logical_key
            for logical_key in closed_relationships
            if all(
                settled_by_physical_link[physical_key]
                for physical_key in settled_by_physical_link
                if physical_key[:3] == logical_key
            )
        ),
    }
