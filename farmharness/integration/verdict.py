"""Pure, fail-closed verdicts for controlled-farm acceptance bundles.

The product verdict consumes only immutable bundle data.  It does not inspect
the live farm, infer success from a process exit code, or trust a precomputed
``exact``/``wedges`` summary when the underlying rows are available.
"""

from __future__ import annotations

import hashlib
import json
import re
import struct
from collections import Counter, defaultdict
from collections.abc import Mapping, Sequence
from typing import Any

try:
    from .netem import NetemPlanError, validate_receipt as validate_netem_receipt
except ImportError:  # Direct execution from this directory.
    from netem import NetemPlanError, validate_receipt as validate_netem_receipt


BUNDLE_SCHEMA = "icefarm-bundle-v1"
ROW_SCHEMA = "icecream-newgen-farm-acceptance-v1"
VERDICT_SCHEMA = "icefarm-verdict-v1"
CONTROL_VERDICT_SCHEMA = "icefarm-control-verdict-v1"
HEADER_EDIT_SCHEMA = "icefarm-header-edit-v1"
DISK_FILL_SCHEMA = "icefarm-disk-fill-v1"
CACHE_DISK_FAULT_PATH = "/var/cache/icecream"
CACHE_DISK_FAULT_FILE = "/var/cache/icecream/.icefarm-disk-fill"
CACHE_DISK_FAULT_BYTES = 128 * 1024 * 1024
CACHE_DISK_FAULT_MIN_HEADROOM_BYTES = 8 * 1024 * 1024
DISK_FILL_WATCHDOG_S = 30
STALL_LIMIT_MS = 120_000
F_INIT_SCHEMA = "icefarm-f-init-v1"
F_INIT_LAUNCH_CONTRACT = "icefarm-f-init-launch-v1"
HISTORICAL_LAUNCH_CONTRACT = "icefarm-pre-f-init-v0"
TRANSITION_LAUNCH_CONTRACT = "icefarm-f-init-transition-v0"

ROW_FIELDS = frozenset(
    {
        "schema",
        "job_id",
        "tu",
        "client_version",
        "client_instance",
        "cs",
        "cs_version",
        "tail_present",
        "tail_profile",
        "session_outcome",
        "reuse",
        "c_to_f_bytes",
        "f_to_c_bytes",
        "object_sha_remote",
        "object_sha_local",
        "exact",
        "wall_ms",
        "retries",
        "event_epoch",
    }
)
PROFILES = frozenset(("P29V1", "ZSTD_TU", "ZSTD_ROUTE"))
S70_B4_SCHEDULER_ENGAGEMENT = "s70-b4-scheduler-restart"
S70_B4_ACTIVE_LOSS_ENGAGEMENT = "s70-b4-scheduler-active-loss"
S70_B4_CLIENT_ENGAGEMENT = "s70-b4-client-route-restart"
S70_B4_WORKER_ENGAGEMENT = "s70-b4-worker-bounces"
S70_B5_ENGAGEMENT = "s70-b5-interner-downgrade"
S70_B6_ENGAGEMENT = "s70-b6-drained-kill-switch-cycle"
S70_B7_ROLLBACK_ENGAGEMENT = "s70-b7-rollback"
S70_B7_ROLLFORWARD_ENGAGEMENT = "s70-b7-rollforward"
S90_REVISION_REFUSAL_ENGAGEMENT = "s90-revision-refusal-retry"
S95_DISK_FILL_ENGAGEMENT = "s95-cache-disk-full"
P29_FAULT_ENV = "ICECC_P50_FAULT_INJECTION"
P29_FAULT_ENV_VALUE = "P29_INTERNER_FAIL_ONCE"
P29_FAULT_SCHEMA = "icecream-p50-fault-v1"
P29_FAULT_NAME = "p29-interner-fail-once"
SESSION_OUTCOMES = frozenset(("committed", "refused", "fallback", "none"))
TERMINAL_KINDS = frozenset(
    ("completion", "fallback", "cancellation", "process-loss-recovery")
)
SHA256_RE = re.compile(r"^[0-9a-f]{64}$")


def _is_int(value: object, *, minimum: int = 0) -> bool:
    return type(value) is int and value >= minimum


def _job_id(value: object, fallback: str) -> str:
    if isinstance(value, str) and value:
        return value
    if _is_int(value, minimum=1):
        return str(value)
    return fallback


def _sorted_ids(values: Sequence[str] | set[str]) -> list[str]:
    return sorted(set(values), key=lambda item: (len(item), item))


def _transition_protocol(label: object) -> int | None:
    if not isinstance(label, str):
        return None
    match = re.match(r"^p(43|44|50)(?:s[0-9]+)?(?:-|$)", label, re.IGNORECASE)
    return int(match.group(1)) if match else None


def _clause(
    identifier: str,
    passed: bool,
    detail: str,
    jobs: Sequence[str] | set[str] = (),
) -> dict[str, Any]:
    return {
        "detail": detail,
        "id": identifier,
        "offending_job_ids": [] if passed else _sorted_ids(jobs),
        "status": "PASS" if passed else "FAIL",
    }


def _row_errors(row: object) -> list[str]:
    if not isinstance(row, Mapping):
        return ["row is not an object"]
    errors: list[str] = []
    fields = frozenset(row)
    if fields != ROW_FIELDS:
        missing = sorted(ROW_FIELDS - fields)
        extra = sorted(fields - ROW_FIELDS)
        errors.append(f"field set mismatch missing={missing!r} extra={extra!r}")
    if row.get("schema") != ROW_SCHEMA:
        errors.append("wrong row schema")
    if not (
        (isinstance(row.get("job_id"), str) and bool(row["job_id"]))
        or _is_int(row.get("job_id"), minimum=1)
    ):
        errors.append("job_id must be a nonempty string or positive integer")
    for field in ("tu", "client_instance", "cs"):
        if not isinstance(row.get(field), str) or not row[field]:
            errors.append(f"{field} must be a nonempty string")
    for field in ("client_version", "cs_version"):
        if not _is_int(row.get(field), minimum=1):
            errors.append(f"{field} must be a positive integer")
    if type(row.get("tail_present")) is not bool:
        errors.append("tail_present must be boolean")
    if row.get("tail_profile") is not None and row.get("tail_profile") not in PROFILES:
        errors.append("tail_profile is not a product profile or null")
    if row.get("session_outcome") not in SESSION_OUTCOMES:
        errors.append("session_outcome is invalid")
    if row.get("reuse") is not None and type(row.get("reuse")) is not bool:
        errors.append("reuse must be boolean or null")
    for field in ("c_to_f_bytes", "f_to_c_bytes", "retries", "event_epoch"):
        if not _is_int(row.get(field)):
            errors.append(f"{field} must be a non-negative integer")
    if not _is_int(row.get("wall_ms"), minimum=1):
        errors.append("wall_ms must be a positive integer")
    for field in ("object_sha_remote", "object_sha_local"):
        if (
            not isinstance(row.get(field), str)
            or SHA256_RE.fullmatch(row[field]) is None
        ):
            errors.append(f"{field} must be a lowercase SHA-256")
    if type(row.get("exact")) is not bool:
        errors.append("exact must be boolean")
    return errors


def _instance_protocol(
    instance: Mapping[str, Any], scenario: Mapping[str, Any]
) -> int | None:
    version = instance.get("version")
    if _is_int(version, minimum=1):
        return version
    alias = instance.get("image")
    images = scenario.get("images")
    label = images.get(alias) if isinstance(images, Mapping) else alias
    return _transition_protocol(label)


def _scenario_profile_at_epoch(
    scenario: Mapping[str, Any], event_epoch: object
) -> tuple[str | None, str | None]:
    """Resolve the scheduler profile after exactly ``event_epoch`` events."""

    if not _is_int(event_epoch) or not isinstance(scenario.get("timeline"), list):
        return None, "scenario has an invalid event epoch or timeline"
    timeline = scenario["timeline"]
    if event_epoch > len(timeline):
        return None, f"event epoch {event_epoch} exceeds timeline length {len(timeline)}"
    instances = scenario.get("instances")
    if not isinstance(instances, list):
        return None, "scenario.instances is absent or invalid"
    schedulers = [
        instance
        for instance in instances
        if isinstance(instance, Mapping) and instance.get("role") == "S"
    ]
    if len(schedulers) != 1:
        return None, f"scenario must resolve one scheduler, got {len(schedulers)}"
    scheduler = schedulers[0]
    scheduler_name = scheduler.get("name")
    protocol = _instance_protocol(scheduler, scenario)
    if protocol is None:
        return None, "scheduler image has no protocol generation"
    raw_environment = scheduler.get("env", {})
    if not isinstance(raw_environment, Mapping):
        return None, "scheduler environment is invalid"
    environment = dict(raw_environment)

    for index, event in enumerate(timeline[:event_epoch]):
        if not isinstance(event, Mapping):
            return None, f"timeline event {index} is invalid"
        if event.get("instance") != scheduler_name:
            continue
        action = event.get("action")
        if action in {"upgrade", "downgrade"}:
            alias = event.get("image")
            images = scenario.get("images")
            label = images.get(alias) if isinstance(images, Mapping) else None
            target_protocol = _transition_protocol(label)
            if target_protocol is None:
                return None, f"scheduler transition {index} has no protocol generation"
            protocol = target_protocol
            if protocol == 50:
                environment.setdefault("ICECC_P50_PROFILE", "P29V1")
            else:
                environment.pop("ICECC_P50_PROFILE", None)
        elif action == "env_set":
            update = event.get("env")
            if not isinstance(update, Mapping):
                return None, f"scheduler env_set {index} is invalid"
            environment.update(update)

    if protocol != 50:
        return None, None
    selected = environment.get("ICECC_P50_PROFILE", "P29V1")
    if selected == "OFF":
        return None, None
    if selected not in PROFILES:
        return None, f"scheduler selected unknown profile {selected!r}"
    return str(selected), None


def _scenario_profile(scenario: Mapping[str, Any]) -> tuple[str | None, str | None]:
    timeline = scenario.get("timeline")
    if not isinstance(timeline, list):
        return None, "scenario.timeline is absent or invalid"
    initial, error = _scenario_profile_at_epoch(scenario, 0)
    if error is not None:
        return None, error
    for epoch in range(1, len(timeline) + 1):
        _profile, error = _scenario_profile_at_epoch(scenario, epoch)
        if error is not None:
            return None, error
    return initial, None


def expected_profile(
    row: Mapping[str, Any],
    selected_profile: str | None,
    wire_revisions: Mapping[str, Any] | None = None,
) -> str | None:
    """Return the configured pair-law result for one dispatch row."""

    same_revision = True
    if wire_revisions is not None:
        c_revision = wire_revisions.get(row.get("client_instance"))
        f_revision = wire_revisions.get(row.get("cs"))
        same_revision = (
            _is_int(c_revision, minimum=1)
            and _is_int(f_revision, minimum=1)
            and c_revision == f_revision
        )
    if (
        selected_profile in PROFILES
        and row.get("client_version") == 50
        and row.get("cs_version") == 50
        and same_revision
    ):
        return selected_profile
    return None


def _s60_transition_epoch_errors(
    scenario: Mapping[str, Any],
    rows: Sequence[Mapping[str, Any]],
    observations: Mapping[str, Any],
    event_log: object,
) -> set[str]:
    """Bind S60 pair-law rows to the authenticated transition boundary."""

    scenario_id = scenario.get("id")
    if not isinstance(scenario_id, str) or not scenario_id.startswith("S60-"):
        return set()
    marker = "@s60:transition-epoch"
    timeline = scenario.get("timeline")
    if (
        not isinstance(timeline, list)
        or len(timeline) != 1
        or not isinstance(timeline[0], Mapping)
        or timeline[0].get("action") not in {"upgrade", "downgrade"}
        or not isinstance(event_log, list)
        or len(event_log) != 1
        or not isinstance(event_log[0], Mapping)
    ):
        return {marker}
    expected_event = timeline[0]
    observed_event = event_log[0]
    fired_ms = observed_event.get("fired_ms")
    if (
        not _is_int(fired_ms)
        or observed_event.get("event_epoch") != 1
        or observed_event.get("action") != expected_event.get("action")
        or observed_event.get("instance") != expected_event.get("instance")
        or observed_event.get("trigger") != expected_event.get("trigger")
    ):
        return {marker}

    instances = scenario.get("instances")
    if not isinstance(instances, list):
        return {marker}
    named = {
        item.get("name"): item
        for item in instances
        if isinstance(item, Mapping) and isinstance(item.get("name"), str)
    }
    target = named.get(expected_event.get("instance"))
    images = scenario.get("images")
    target_label = (
        images.get(expected_event.get("image"))
        if isinstance(images, Mapping)
        else None
    )
    target_protocol = _transition_protocol(target_label)
    if not isinstance(target, Mapping) or target_protocol is None:
        return {marker}

    lifecycle = observations.get("job_lifecycle")
    if not isinstance(lifecycle, list):
        return {marker}
    lifecycle_by_job: dict[str, Mapping[str, Any]] = {}
    for item in lifecycle:
        if not isinstance(item, Mapping) or not isinstance(item.get("job_id"), str):
            return {marker}
        job_id = item["job_id"]
        if job_id in lifecycle_by_job:
            return {marker}
        lifecycle_by_job[job_id] = item

    bad: set[str] = set()
    epochs: set[int] = set()
    target_epochs: set[int] = set()
    for row in rows:
        identifier = _job_id(row.get("job_id"), "@row")
        epoch = row.get("event_epoch")
        life = lifecycle_by_job.get(str(row.get("job_id")))
        dispatch_ms = life.get("final_dispatch_ms") if isinstance(life, Mapping) else None
        if epoch not in {0, 1} or not _is_int(dispatch_ms):
            bad.add(identifier)
            continue
        expected_epoch = int(dispatch_ms >= fired_ms)
        if epoch != expected_epoch:
            bad.add(identifier)
        epochs.add(epoch)

        for role, name_field, version_field in (
            ("C", "client_instance", "client_version"),
            ("F", "cs", "cs_version"),
        ):
            instance = named.get(row.get(name_field))
            initial = (
                _instance_protocol(instance, scenario)
                if isinstance(instance, Mapping) and instance.get("role") == role
                else None
            )
            expected_version = (
                target_protocol
                if target.get("role") == role
                and row.get(name_field) == target.get("name")
                and epoch == 1
                else initial
            )
            if expected_version is None or row.get(version_field) != expected_version:
                bad.add(identifier)
        if target.get("role") == "S" or (
            target.get("role") == "C"
            and row.get("client_instance") == target.get("name")
        ) or (
            target.get("role") == "F" and row.get("cs") == target.get("name")
        ):
            target_epochs.add(epoch)
    if set(lifecycle_by_job) != {str(row.get("job_id")) for row in rows}:
        bad.add(marker)
    if epochs != {0, 1} or target_epochs != {0, 1}:
        bad.add(marker)
    return bad


def _lifecycle_wedges(
    observations: Mapping[str, Any], row_ids: set[str]
) -> tuple[set[str], list[str]]:
    raw = observations.get("job_lifecycle")
    if not isinstance(raw, list):
        return set(), ["observations.job_lifecycle is absent or invalid"]
    records: dict[str, Mapping[str, Any]] = {}
    errors: list[str] = []
    for index, item in enumerate(raw):
        if not isinstance(item, Mapping):
            errors.append(f"job_lifecycle[{index}] is not an object")
            continue
        identifier = _job_id(item.get("job_id"), f"@lifecycle:{index}")
        if identifier in records:
            errors.append(f"duplicate lifecycle job {identifier}")
            continue
        dispatch = item.get("dispatch_ms")
        first_dispatch = item.get("first_dispatch_ms")
        final_dispatch = item.get("final_dispatch_ms")
        scheduler_generation = item.get("scheduler_generation")
        scheduler_dispatch_line = item.get("scheduler_dispatch_line")
        deadline = item.get("deadline_ms")
        terminal = item.get("terminal")
        terminal_ms = item.get("terminal_ms")
        turn = item.get("turn")
        if (
            not _is_int(dispatch)
            or not _is_int(deadline, minimum=1)
            or deadline <= dispatch
            or not isinstance(turn, str)
            or not turn
            or terminal not in TERMINAL_KINDS | {None}
            or (terminal is None and terminal_ms is not None)
            or (terminal is not None and not _is_int(terminal_ms, minimum=dispatch))
            or (
                (first_dispatch is not None or final_dispatch is not None)
                and (
                    not _is_int(first_dispatch)
                    or first_dispatch != dispatch
                    or not _is_int(final_dispatch, minimum=first_dispatch)
                    or (
                        terminal_ms is not None
                        and final_dispatch > terminal_ms
                    )
                )
            )
            or (
                (scheduler_generation is not None or scheduler_dispatch_line is not None)
                and (
                    not _is_int(scheduler_generation, minimum=1)
                    or not _is_int(scheduler_dispatch_line, minimum=1)
                )
            )
        ):
            errors.append(
                f"job_lifecycle[{index}] has invalid timing or terminal fields"
            )
            continue
        records[identifier] = item
    missing = row_ids - set(records)
    extra = set(records) - row_ids
    if missing:
        errors.append(f"lifecycle missing row jobs {_sorted_ids(missing)!r}")
    if extra:
        errors.append(f"lifecycle has unknown jobs {_sorted_ids(extra)!r}")

    wedged: set[str] = set()
    by_turn: dict[str, list[tuple[str, Mapping[str, Any]]]] = defaultdict(list)
    for identifier, item in records.items():
        terminal_ms = item.get("terminal_ms")
        if terminal_ms is None or terminal_ms > item["deadline_ms"]:
            wedged.add(identifier)
        by_turn[item["turn"]].append((identifier, item))

    # Recompute the 120-second no-progress rule from dispatch/terminal times.
    for records_in_turn in by_turn.values():
        events: list[tuple[int, int, str, str]] = []
        for identifier, item in records_in_turn:
            events.append((item["dispatch_ms"], 0, "dispatch", identifier))
            if item.get("terminal_ms") is not None:
                events.append((item["terminal_ms"], 1, "terminal", identifier))
        outstanding: set[str] = set()
        progress_start: int | None = None
        for timestamp, _order, kind, identifier in sorted(events):
            if (
                outstanding
                and progress_start is not None
                and timestamp - progress_start > STALL_LIMIT_MS
            ):
                wedged.update(outstanding)
            if kind == "dispatch":
                if not outstanding:
                    progress_start = timestamp
                outstanding.add(identifier)
            else:
                outstanding.discard(identifier)
                progress_start = timestamp if outstanding else None
        if outstanding and progress_start is not None:
            last_deadline = max(
                item["deadline_ms"] for _identifier, item in records_in_turn
            )
            if last_deadline - progress_start > STALL_LIMIT_MS:
                wedged.update(outstanding)
    return wedged, errors


def _lifecycle_final_dispatch_ms(item: Mapping[str, Any]) -> Any:
    """Return the dispatch represented by the final successful assignment."""

    return item.get("final_dispatch_ms", item.get("dispatch_ms"))


def _assignment_preference_errors(
    observation: Any,
    scenario: Mapping[str, Any],
    topology: Any = None,
) -> set[str]:
    """Recompute the S50 preference counts from its decision rows."""

    if not isinstance(observation, Mapping) or set(observation) != {
        "compatibility",
        "counts",
        "decisions",
        "schema",
        "violations",
        "workers",
    }:
        return {"@observations:assignment_preference"}
    if observation.get("schema") != "icefarm-assignment-preference-v1":
        return {"@observations:assignment_preference.schema"}
    counts = observation.get("counts")
    decisions = observation.get("decisions")
    violations = observation.get("violations")
    workers = observation.get("workers")
    compatibility = observation.get("compatibility")
    instances = scenario.get("instances")
    if (
        not isinstance(counts, Mapping)
        or set(counts) != {"checks", "escapes", "preferred", "violations"}
        or not isinstance(decisions, list)
        or not isinstance(violations, list)
        or any(not isinstance(item, str) or not item for item in violations)
        or not isinstance(workers, Mapping)
        or not isinstance(compatibility, Mapping)
        or not isinstance(instances, list)
    ):
        return {"@observations:assignment_preference"}
    named = {
        item.get("name"): item
        for item in instances
        if isinstance(item, Mapping) and isinstance(item.get("name"), str)
    }
    expected_workers = {
        name: item.get("slots")
        for name, item in named.items()
        if item.get("role") == "F"
    }
    expected_clients = {
        name: item for name, item in named.items() if item.get("role") == "C"
    }
    if (
        not expected_workers
        or not expected_clients
        or any(type(value) is not int or value <= 0 for value in expected_workers.values())
        or dict(workers) != dict(sorted(expected_workers.items()))
    ):
        return {"@observations:assignment_preference.workers"}
    expected_compatibility: dict[str, list[str]] = {}
    relationships = topology.get("relationships") if isinstance(topology, Mapping) else None
    if isinstance(relationships, list):
        expected_compatibility = {client: [] for client in expected_clients}
        expected_pairs = {
            (client, worker)
            for client in expected_clients
            for worker in expected_workers
        }
        seen_pairs: set[tuple[str, str]] = set()
        for relationship in relationships:
            if not isinstance(relationship, Mapping):
                return {"@topology:relationships"}
            client = relationship.get("c")
            worker = relationship.get("f")
            pair = (client, worker)
            if (
                client not in expected_clients
                or worker not in expected_workers
                or pair in seen_pairs
                or type(relationship.get("cache_expected")) is not bool
            ):
                return {"@topology:relationships"}
            seen_pairs.add(pair)
            if relationship["cache_expected"]:
                expected_compatibility[client].append(worker)
        if seen_pairs != expected_pairs:
            return {"@topology:relationships"}
        for compatible_workers in expected_compatibility.values():
            compatible_workers.sort()
    else:
        for client, instance in sorted(expected_clients.items()):
            client_enabled = (
                _h3_instance_version(instance, scenario) == 50
                and isinstance(instance.get("env"), Mapping)
                and instance["env"].get("ICECC_P50_MODE") == "on"
            )
            expected_compatibility[client] = [
                worker
                for worker, worker_instance in sorted(
                    (
                        (name, named[name])
                        for name in expected_workers
                    ),
                    key=lambda item: item[0],
                )
                if client_enabled
                and _h3_instance_version(worker_instance, scenario) == 50
            ]
    if dict(compatibility) != expected_compatibility:
        return {"@observations:assignment_preference.compatibility"}
    if any(not _is_int(counts.get(key)) for key in ("checks", "escapes", "preferred", "violations")):
        return {"@observations:assignment_preference.counts"}
    if counts["checks"] <= 0 or len(decisions) != counts["checks"]:
        return {"@observations:assignment_preference.counts"}
    fields = {
        "client",
        "compatible_free_workers",
        "compatible_workers",
        "dispatch_line",
        "escape",
        "occupancy",
        "preferred",
        "row_job_id",
        "scheduler_job",
        "worker",
    }
    computed_violations: list[str] = []
    computed_preferred = 0
    computed_escapes = 0
    lines: list[int] = []
    for index, decision in enumerate(decisions):
        if not isinstance(decision, Mapping) or set(decision) != fields:
            return {f"@assignment-preference:{index}"}
        compatible = decision.get("compatible_workers")
        free = decision.get("compatible_free_workers")
        occupancy = decision.get("occupancy")
        if (
            not isinstance(compatible, list)
            or any(not isinstance(item, str) or not item for item in compatible)
            or len(set(compatible)) != len(compatible)
            or not isinstance(free, list)
            or any(item not in compatible for item in free)
            or len(set(free)) != len(free)
            or not isinstance(occupancy, Mapping)
            or set(occupancy) != set(expected_workers)
            # Scheduler preload may legally queue more assignments than a
            # worker has execution slots.  Slot saturation is therefore
            # ``occupancy >= slots``; an above-slot count is not malformed.
            or any(not _is_int(value) for value in occupancy.values())
            or decision.get("client") not in expected_clients
            or decision.get("worker") not in expected_workers
            or not isinstance(decision.get("row_job_id"), str)
            or not decision["row_job_id"]
            or not _is_int(decision.get("scheduler_job"), minimum=1)
            or not _is_int(decision.get("dispatch_line"), minimum=1)
            or type(decision.get("escape")) is not bool
            or type(decision.get("preferred")) is not bool
        ):
            return {f"@assignment-preference:{index}"}
        if compatible != expected_compatibility[decision["client"]]:
            return {f"@assignment-preference:{index}"}
        lines.append(decision["dispatch_line"])
        expected_free = [
            worker
            for worker in compatible
            if occupancy[worker] < expected_workers[worker]
        ]
        if free != expected_free:
            return {f"@assignment-preference:{index}"}
        expected_escape = not expected_free
        expected_preferred = bool(
            expected_free and decision["worker"] in expected_free
        )
        if decision["escape"] != expected_escape or decision["preferred"] != expected_preferred:
            return {f"@assignment-preference:{index}"}
        computed_preferred += expected_preferred
        computed_escapes += expected_escape
        if free and not expected_preferred:
            computed_violations.append(decision["row_job_id"])
    if lines != sorted(lines) or len(set(lines)) != len(lines):
        return {"@observations:assignment_preference.order"}
    if (
        counts["preferred"] != computed_preferred
        or counts["escapes"] != computed_escapes
        or counts["violations"] != len(computed_violations)
        or violations != computed_violations
    ):
        return set(computed_violations) or {"@observations:assignment_preference.counts"}
    # A self-consistent violation report is still evidence of a routing
    # violation, not a successful preference proof.  The collector is
    # expected to describe real regressions consistently; the verdict must
    # reject rather than normalize that exact document.
    return set(computed_violations)


def _header_edit_receipt_errors(
    receipt: Any, event: Mapping[str, Any], scenario: Mapping[str, Any]
) -> set[str]:
    """Recompute the S40 header mutation and F/cache readiness contract."""

    marker = "@event:header-edit"
    required = {
        "action",
        "after",
        "before",
        "cache_invalidation",
        "coordination",
        "event_epoch",
        "instance",
        "schema",
        "turn",
    }
    if not isinstance(receipt, Mapping) or set(receipt) != required:
        return {marker}
    workload = scenario.get("workload")
    instances = scenario.get("instances")
    if not isinstance(workload, Mapping) or not isinstance(instances, list):
        return {marker}
    turns = workload.get("turns")
    clients = workload.get("clients")
    timeline_event = event.get("_scenario_event")
    if (
        receipt.get("schema") != HEADER_EDIT_SCHEMA
        or receipt.get("action") != event.get("action")
        or receipt.get("instance") != event.get("instance")
        or not _is_int(receipt.get("event_epoch"), minimum=1)
        or not isinstance(turns, list)
        or receipt.get("turn") not in turns
        or not isinstance(clients, list)
        or not clients
        or not isinstance(timeline_event, Mapping)
    ):
        return {marker}
    path = timeline_event.get("path") if isinstance(timeline_event, Mapping) else None
    if (
        not isinstance(path, str)
        or not path.startswith(("/usr/include/", "/usr/local/include/"))
        or ".." in path.split("/")
    ):
        return {marker}
    managed = {
        "p29-system-source-fingerprint-v1.cache",
        "p29-system-source-fingerprint-v1.lock",
    }
    snapshots: dict[str, Mapping[str, Any]] = {}
    snapshot_fields = {
        "container_id",
        "header_path",
        "header_sha256",
        "p29_cache_files",
        "running",
        "started_at",
    }
    for side in ("before", "after"):
        value = receipt.get(side)
        cache = value.get("p29_cache_files") if isinstance(value, Mapping) else None
        if (
            not isinstance(value, Mapping)
            or set(value) != snapshot_fields
            or not isinstance(value.get("container_id"), str)
            or re.fullmatch(r"[0-9a-f]{64}", value["container_id"]) is None
            or not isinstance(value.get("started_at"), str)
            or not value["started_at"]
            or value.get("running") is not True
            or value.get("header_path") != path
            or not isinstance(value.get("header_sha256"), str)
            or SHA256_RE.fullmatch(value["header_sha256"]) is None
            or not isinstance(cache, list)
            or any(item not in managed for item in cache)
            or len(set(cache)) != len(cache)
            or (
                side == "before"
                and cache
                != [
                    "p29-system-source-fingerprint-v1.cache",
                    "p29-system-source-fingerprint-v1.lock",
                ]
            )
        ):
            return {f"{marker}:{side}"}
        snapshots[side] = value
    if (
        snapshots["before"]["container_id"] != snapshots["after"]["container_id"]
        or snapshots["before"]["started_at"] == snapshots["after"]["started_at"]
        or snapshots["before"]["header_sha256"] == snapshots["after"]["header_sha256"]
        or snapshots["after"]["p29_cache_files"]
    ):
        return {marker}
    cache_files = [
        "p29-system-source-fingerprint-v1.cache",
        "p29-system-source-fingerprint-v1.lock",
    ]
    invalidation = receipt.get("cache_invalidation")
    if (
        not isinstance(invalidation, Mapping)
        or set(invalidation) != {"directory", "files", "removed"}
        or invalidation.get("directory") != "/var/cache/icecream/p50-runtime"
        or invalidation.get("files") != cache_files
        or invalidation.get("removed") != snapshots["before"]["p29_cache_files"]
        or invalidation.get("removed") != cache_files
    ):
        return {marker}
    coordination = receipt.get("coordination")
    if not isinstance(coordination, Mapping) or set(coordination) != {
        "clients",
        "ready_ms",
        "readiness",
        "resume",
        "scheduler_rejoin",
    }:
        return {marker}
    ready_ms = coordination.get("ready_ms")
    if not _is_int(ready_ms) or ready_ms != event.get("fired_ms"):
        return {marker}
    expected_clients = set(clients)
    pauses = coordination.get("clients")
    resumes = coordination.get("resume")
    if (
        not isinstance(pauses, Mapping)
        or not isinstance(resumes, Mapping)
        or set(pauses) != expected_clients
        or set(resumes) != expected_clients
    ):
        return {marker}
    gate_fields = {
        "action",
        "active_after",
        "active_before",
        "client",
        "epoch",
        "finished_ms",
        "schema",
        "started_ms",
        "status",
        "turn",
    }
    for name in sorted(expected_clients):
        for action, status, value in (
            ("pause", "PAUSED", pauses[name]),
            ("resume", "OPEN", resumes[name]),
        ):
            if (
                not isinstance(value, Mapping)
                or set(value) != gate_fields
                or value.get("schema") != "icefarm-event-gate-v1"
                or value.get("action") != action
                or value.get("status") != status
                or value.get("client") != name
                or value.get("turn") != receipt["turn"]
                or value.get("epoch") != receipt["event_epoch"]
                or any(
                    not _is_int(value.get(field))
                    for field in ("active_after", "active_before", "finished_ms", "started_ms")
                )
                or (action in {"pause", "quiesce"} and not _is_int(value.get("active_before"), minimum=1))
                or value["finished_ms"] < value["started_ms"]
            ):
                return {f"{marker}:gate:{name}"}
        if pauses[name]["active_after"] != 0:
            return {f"{marker}:ordering:{name}"}
    target = next(
        (item for item in instances if isinstance(item, Mapping) and item.get("name") == receipt["instance"]),
        None,
    )
    readiness = coordination.get("readiness")
    if (
        not isinstance(target, Mapping)
        or target.get("role") != "F"
        or not isinstance(readiness, Mapping)
        or set(readiness) != {"cache_line", "host", "line", "log_path", "offset", "role"}
        or readiness.get("role") != "F"
        or not isinstance(readiness.get("host"), str)
        or not isinstance(readiness.get("log_path"), str)
        or not readiness["log_path"].endswith(f"/{receipt['instance']}/log/iceccd.log")
        or not isinstance(readiness.get("line"), str)
        or "ICECREAM daemon " not in readiness["line"]
        or not isinstance(readiness.get("cache_line"), str)
        or "cache sidecar adapter state=2 lifecycle=3" not in readiness["cache_line"]
        or not _is_int(readiness.get("offset"))
    ):
        return {marker}
    scheduler = next(
        (item for item in instances if isinstance(item, Mapping) and item.get("role") == "S"),
        None,
    )
    rejoin = coordination.get("scheduler_rejoin")
    expected_profile = scheduler.get("env", {}).get("ICECC_P50_PROFILE") if isinstance(scheduler, Mapping) else None
    rejoin_fields = {
        "cache_line",
        "cache_protocol",
        "host",
        "login_line",
        "log_path",
        "offset",
        "profile",
        "role_protocol",
        "scheduler",
        "target",
    }
    login_line = rejoin.get("login_line") if isinstance(rejoin, Mapping) else None
    cache_line = rejoin.get("cache_line") if isinstance(rejoin, Mapping) else None
    login = re.search(
        rf"\bRELOGIN {re.escape(receipt['instance'])}\([^)]*\):",
        cache_line,
    ) if isinstance(cache_line, str) else None
    role_login = re.search(
        rf"\blogin\s+{re.escape(receipt['instance'])}\s+protocol\s+version:\s*50\b",
        login_line,
    ) if isinstance(login_line, str) else None
    cache = re.search(
        r"\bcache=([^ ]+) cache_wire=v1 cache_protocol=1 cache_profiles=([a-z0-9_ ]+)\s*$",
        cache_line,
    ) if isinstance(cache_line, str) else None
    profile_token = expected_profile.lower() if isinstance(expected_profile, str) else None
    if (
        not isinstance(scheduler, Mapping)
        or not isinstance(rejoin, Mapping)
        or set(rejoin) != rejoin_fields
        or rejoin.get("host") != scheduler.get("host")
        or not isinstance(rejoin.get("log_path"), str)
        or not rejoin["log_path"].startswith("/")
        or not rejoin["log_path"].endswith(f"/{scheduler['name']}/log/scheduler.log")
        or rejoin.get("scheduler") != scheduler.get("name")
        or rejoin.get("target") != receipt.get("instance")
        or rejoin.get("profile") != expected_profile
        or rejoin.get("role_protocol") != 50
        or rejoin.get("cache_protocol") != 1
        or not _is_int(rejoin.get("offset"))
        or login is None
        or role_login is None
        or cache is None
        or profile_token is None
        or profile_token not in cache.group(2).split()
    ):
        return {f"{marker}:scheduler-rejoin"}
    return set()


def _disk_fill_receipt_errors(
    receipt: Any, event: Mapping[str, Any], topology: Any = None, run_id: Any = None
) -> set[str]:
    marker = "@event:disk-fill"
    if not isinstance(receipt, Mapping) or set(receipt) != {
        "action",
        "after",
        "before",
        "event_epoch",
        "fill",
        "instance",
        "schema",
    }:
        return {marker}
    if (
        receipt.get("schema") != DISK_FILL_SCHEMA
        or receipt.get("action") != "disk_fill"
        or receipt.get("action") != event.get("action")
        or receipt.get("instance") != event.get("instance")
        or receipt.get("event_epoch") != event.get("event_epoch")
    ):
        return {marker}
    expected_mount = {
        "destination": CACHE_DISK_FAULT_PATH,
        "size_bytes": CACHE_DISK_FAULT_BYTES,
        "type": "tmpfs",
    }
    snapshot_fields = {
        "container_id", "container_name", "host", "image_closure_sha256",
        "image_id", "labels", "mount", "running", "runtime_path", "started_at",
    }
    target = next(
        (
            item for item in topology.get("instances", [])
            if isinstance(item, Mapping) and item.get("name") == event.get("instance")
        ),
        None,
    ) if isinstance(topology, Mapping) else None
    expected_host = target.get("host") if isinstance(target, Mapping) else None
    expected_closure = (
        target.get("image", {}).get("closure_sha256")
        if isinstance(target, Mapping) else None
    )
    expected_run = run_id if isinstance(run_id, str) else None
    expected_name = (
        f"icefarm-{expected_run}-{event['instance']}"
        if isinstance(expected_run, str) and expected_run else None
    )
    snapshots: dict[str, Mapping[str, Any]] = {}
    for side in ("before", "after"):
        value = receipt.get(side)
        if (
            not isinstance(value, Mapping)
            or set(value) != snapshot_fields
            or not isinstance(value.get("container_id"), str)
            or SHA256_RE.fullmatch(value["container_id"]) is None
            or not isinstance(value.get("container_name"), str)
            or (expected_name is not None and value.get("container_name") != f"/{expected_name}")
            or not isinstance(value.get("host"), str)
            or (expected_host is not None and value.get("host") != expected_host)
            or not isinstance(value.get("image_closure_sha256"), str)
            or SHA256_RE.fullmatch(value["image_closure_sha256"]) is None
            or (expected_closure is not None and value.get("image_closure_sha256") != expected_closure)
            or not isinstance(value.get("image_id"), str)
            or SHA256_RE.fullmatch(value["image_id"]) is None
            or not isinstance(value.get("labels"), Mapping)
            or value["labels"].get("icefarm.instance") != event.get("instance")
            or (expected_run is not None and value["labels"].get("icefarm.run") != expected_run)
            or value.get("mount") != expected_mount
            or value.get("running") is not True
            or not isinstance(value.get("runtime_path"), str)
            or not value["runtime_path"].startswith("/")
            or not isinstance(value.get("started_at"), str)
            or not value["started_at"]
        ):
            return {f"{marker}:{side}"}
        snapshots[side] = value
    if snapshots["before"] != snapshots["after"]:
        return {marker}
    fill = receipt.get("fill")
    if (
        not isinstance(fill, Mapping)
        or set(fill)
        != {
            "available_after",
            "available_before",
            "directory_gid",
            "directory_mode",
            "directory_uid",
            "elapsed_ms",
            "errno",
            "filler_bytes",
            "filler_path",
            "limit_bytes",
            "minimum_headroom_bytes",
            "schema",
            "watchdog_s",
        }
        or fill.get("schema") != "icefarm-disk-fill-operation-v1"
        or fill.get("errno") != 28
        or fill.get("filler_path") != CACHE_DISK_FAULT_FILE
        or fill.get("limit_bytes") != CACHE_DISK_FAULT_BYTES
        or not _is_int(fill.get("filler_bytes"), minimum=1)
        or fill["filler_bytes"] > CACHE_DISK_FAULT_BYTES
        or not _is_int(fill.get("available_before"), minimum=1)
        or fill["available_before"] < CACHE_DISK_FAULT_MIN_HEADROOM_BYTES
        or fill["available_before"] > CACHE_DISK_FAULT_BYTES
        or fill["filler_bytes"] > fill["available_before"]
        or not _is_int(fill.get("available_after"))
        or fill["available_after"] >= 1024 * 1024
        or fill["available_after"] >= fill["available_before"]
        or fill.get("directory_uid") != 65534
        or fill.get("directory_gid") != 65534
        or fill.get("directory_mode") != 0o700
        or not _is_int(fill.get("elapsed_ms"))
        or fill["elapsed_ms"] > DISK_FILL_WATCHDOG_S * 1000
        or fill.get("minimum_headroom_bytes")
        != CACHE_DISK_FAULT_MIN_HEADROOM_BYTES
        or fill.get("watchdog_s") != DISK_FILL_WATCHDOG_S
    ):
        return {marker}
    return set()


def _scheduler_restart_receipt_errors(
    receipt: Any, event: Mapping[str, Any], scenario: Mapping[str, Any]
) -> set[str]:
    marker = "@event:scheduler-restart-coordination"
    if not isinstance(receipt, Mapping) or set(receipt) != {
        "action",
        "after",
        "before",
        "coordination",
        "event_epoch",
        "instance",
        "schema",
        "turn",
    }:
        return {marker}
    workload = scenario.get("workload")
    instances = scenario.get("instances")
    if not isinstance(workload, Mapping) or not isinstance(instances, list):
        return {marker}
    turns = workload.get("turns")
    client_names = workload.get("clients")
    if (
        receipt.get("schema") != "icefarm-scheduler-restart-v1"
        or receipt.get("action") != event.get("action")
        or receipt.get("instance") != event.get("instance")
        or receipt.get("event_epoch") != event.get("event_epoch")
        or not isinstance(turns, list)
        or receipt.get("turn") not in turns
        or not isinstance(client_names, list)
        or not client_names
    ):
        return {marker}
    for side in ("before", "after"):
        snapshot = receipt.get(side)
        if (
            not isinstance(snapshot, Mapping)
            or set(snapshot) != {"container_id", "started_at"}
            or not isinstance(snapshot.get("container_id"), str)
            or re.fullmatch(r"[0-9a-f]{64}", snapshot["container_id"]) is None
            or not isinstance(snapshot.get("started_at"), str)
            or not snapshot["started_at"]
        ):
            return {marker}
    if (
        receipt["before"]["container_id"] != receipt["after"]["container_id"]
        or receipt["before"]["started_at"] == receipt["after"]["started_at"]
    ):
        return {marker}
    coordination = receipt.get("coordination")
    if not isinstance(coordination, Mapping) or set(coordination) != {
        "client_readiness",
        "clients",
        "ready_ms",
        "resume",
        "scheduler_snapshot",
        "scheduler_startup",
        "workers",
        "worker_snapshot",
    }:
        return {marker}
    ready_ms = coordination.get("ready_ms")
    if type(ready_ms) is not int or ready_ms < 0 or ready_ms != event.get("fired_ms"):
        return {marker}
    pauses = coordination.get("clients")
    resumes = coordination.get("resume")
    client_readiness = coordination.get("client_readiness")
    expected_clients = set(client_names)
    if (
        not isinstance(pauses, Mapping)
        or not isinstance(resumes, Mapping)
        or not isinstance(client_readiness, Mapping)
        or set(pauses) != expected_clients
        or set(resumes) != expected_clients
        or set(client_readiness) != expected_clients
    ):
        return {marker}
    gate_fields = {
        "action",
        "active_after",
        "active_before",
        "client",
        "epoch",
        "finished_ms",
        "schema",
        "started_ms",
        "status",
        "turn",
    }
    for name in sorted(expected_clients):
        for action, status, value in (
            ("pause", "PAUSED", pauses[name]),
            ("resume", "OPEN", resumes[name]),
        ):
            if (
                not isinstance(value, Mapping)
                or set(value) != gate_fields
                or value.get("schema") != "icefarm-event-gate-v1"
                or value.get("action") != action
                or value.get("status") != status
                or value.get("client") != name
                or value.get("turn") != receipt["turn"]
                or value.get("epoch") != receipt["event_epoch"]
                or any(
                    type(value.get(field)) is not int or value[field] < 0
                    for field in (
                        "active_after",
                        "active_before",
                        "finished_ms",
                        "started_ms",
                    )
                )
                or (action in {"pause", "quiesce"} and not _is_int(value.get("active_before"), minimum=1))
                or value["finished_ms"] < value["started_ms"]
            ):
                return {marker}
        if pauses[name]["active_after"] != 0:
            return {marker}
        client = next(
            (
                item
                for item in instances
                if isinstance(item, Mapping) and item.get("name") == name
            ),
            None,
        )
        witness = client_readiness[name]
        log_path = witness.get("log_path") if isinstance(witness, Mapping) else None
        cache_required = (
            isinstance(client, Mapping)
            and isinstance(client.get("env"), Mapping)
            and client["env"].get("ICECC_P50_MODE") == "on"
        )
        if (
            not isinstance(client, Mapping)
            or client.get("role") != "C"
            or not isinstance(witness, Mapping)
            or set(witness)
            != {
                "cache_line",
                "cache_required",
                "connected_line",
                "host",
                "log_path",
                "offset",
            }
            or witness.get("host") != client.get("host")
            or not isinstance(log_path, str)
            or not log_path.startswith("/")
            or not log_path.endswith(f"/{name}/log/client-daemon.log")
            or not _is_int(witness.get("offset"))
            or witness.get("cache_required") is not cache_required
            or not isinstance(witness.get("connected_line"), str)
            or "Connected to scheduler (I am known as "
            not in witness["connected_line"]
            or (
                cache_required
                and (
                    not isinstance(witness.get("cache_line"), str)
                    or re.search(
                        r"cache sidecar adapter state=2 lifecycle=3",
                        witness["cache_line"],
                    )
                    is None
                )
            )
            or (not cache_required and witness.get("cache_line") is not None)
        ):
            return {marker}
    expected_workers = sorted(
        item.get("name")
        for item in instances
        if isinstance(item, Mapping)
        and item.get("role") == "F"
        and isinstance(item.get("name"), str)
    )
    worker_snapshot = coordination.get("worker_snapshot")
    startup = coordination.get("scheduler_startup")
    if (
        not expected_workers
        or coordination.get("workers") != expected_workers
        or not isinstance(coordination.get("scheduler_snapshot"), str)
        or not coordination["scheduler_snapshot"]
        or not isinstance(worker_snapshot, str)
        or any(
            re.search(rf"(^|\s){re.escape(name)}(\s|$)", worker_snapshot, re.MULTILINE)
            is None
            for name in expected_workers
        )
        or not isinstance(startup, Mapping)
        or set(startup) != {"host", "line", "log_path", "offset", "role"}
        or startup.get("role") != "S"
        or type(startup.get("offset")) is not int
        or startup["offset"] < 0
        or not isinstance(startup.get("line"), str)
        or re.search(
            r"ICECREAM scheduler .* starting up, port [0-9]+", startup["line"]
        )
        is None
    ):
        return {marker}
    return set()


def _scheduler_active_loss_receipt_errors(
    receipt: Any, event: Mapping[str, Any], scenario: Mapping[str, Any]
) -> set[str]:
    marker = "@event:scheduler-active-loss"
    required = {"action", "after", "before", "compiler", "event_epoch", "instance", "lost_scheduler_generation", "lost_scheduler_job", "pre_fault", "quiescence", "schema", "turn"}
    if not isinstance(receipt, Mapping) or set(receipt) != required or receipt.get("schema") != "icefarm-scheduler-active-loss-v1":
        return {marker}
    compiler = receipt.get("compiler")
    assignment = compiler.get("assignment") if isinstance(compiler, Mapping) else None
    parent = compiler.get("daemon") if isinstance(compiler, Mapping) else None
    leader = compiler.get("leader") if isinstance(compiler, Mapping) else None
    stopped = compiler.get("stopped") if isinstance(compiler, Mapping) else None
    if (receipt.get("action") != event.get("action")
            or receipt.get("instance") != event.get("instance")
            or not _is_int(receipt.get("lost_scheduler_generation"), minimum=1)
            or not _is_int(receipt.get("lost_scheduler_job"), minimum=1)
            or receipt.get("lost_scheduler_job") != event.get("last_dispatched_job")
            or not isinstance(leader, Mapping) or not isinstance(stopped, Mapping)
            or not isinstance(parent, Mapping)
            or parent.get("pid") == leader.get("pid")
            or leader.get("ppid") != parent.get("pid")
            or not isinstance(parent.get("exe"), str)
            or not parent["exe"].endswith("/iceccd")
            or leader.get("pid") != leader.get("pgid")
            or leader.get("pid") != stopped.get("pid")
            or leader.get("start_ticks") != stopped.get("start_ticks")
            or compiler.get("group_gone", {}).get("gone") is not True
            or not isinstance(compiler.get("worker_before"), Mapping)
            or not isinstance(compiler.get("worker_after"), Mapping)
            or set(compiler["worker_before"]) != {"container_id", "started_at"}
            or set(compiler["worker_after"]) != {"container_id", "started_at"}
            or compiler["worker_before"] != compiler["worker_after"]
            or not isinstance(compiler["worker_before"].get("container_id"), str)
            or re.fullmatch(r"[0-9a-f]{64}", compiler["worker_before"]["container_id"]) is None
            or not isinstance(assignment, Mapping)
            or set(assignment) != {"child", "client", "listener", "schema"}
            or assignment.get("schema") != "icefarm-compiler-assignment-v1"
            or assignment.get("child", {}).get("pid") != leader.get("pid")
            or assignment.get("child", {}).get("pgid") != leader.get("pgid")
            or assignment.get("child", {}).get("generation") != receipt.get("lost_scheduler_generation")
            or assignment.get("client", {}).get("scheduler_job_id") != receipt.get("lost_scheduler_job")
            or assignment.get("client", {}).get("job_id") != receipt.get("lost_scheduler_job")
            or assignment.get("child", {}).get("owning_client_id") != assignment.get("client", {}).get("client_id")
            or set(assignment.get("child", {})) != {"generation", "kind", "owning_client_id", "pgid", "pid"}
            or assignment.get("child", {}).get("kind") != 0
            or set(assignment.get("client", {})) != {"client_id", "job_id", "scheduler_job_id"}
            or not all(_is_int(assignment.get("child", {}).get(key), minimum=1) for key in ("generation", "owning_client_id", "pgid", "pid"))
            or not all(_is_int(assignment.get("client", {}).get(key), minimum=1) for key in ("client_id", "job_id", "scheduler_job_id"))
            or assignment.get("listener") != {"host": "127.0.0.1", "port": 8765}
            or not isinstance(receipt.get("quiescence"), Mapping)
            or set(receipt["quiescence"]) != {"client_readiness", "client_routes", "scheduler_snapshot", "scheduler_startup", "worker_snapshot"}
            or not isinstance(receipt["quiescence"].get("scheduler_startup"), Mapping)
            or not isinstance(receipt["quiescence"]["scheduler_startup"].get("line"), str)
            or not isinstance(receipt["quiescence"].get("scheduler_snapshot"), str)
            or not isinstance(receipt["quiescence"].get("worker_snapshot"), str)
            or not receipt["quiescence"]["scheduler_snapshot"].strip()
            or not receipt["quiescence"]["worker_snapshot"].strip()
            or set(receipt["quiescence"].get("client_readiness", {}))
            != set(scenario.get("workload", {}).get("clients", []))
            or any(
                not isinstance(witness, Mapping)
                or set(witness) != {"bytes", "cache_line", "cache_required", "connected_line", "host", "log_path", "offset"}
                or type(witness.get("bytes")) is not int or witness["bytes"] < 1
                or type(witness.get("cache_required")) is not bool
                or not isinstance(witness.get("connected_line"), str)
                or "Connected to scheduler (I am known as " not in witness["connected_line"]
                or not isinstance(witness.get("host"), str) or not witness["host"]
                or not isinstance(witness.get("log_path"), str) or not witness["log_path"].startswith("/")
                or not _is_int(witness.get("offset"), minimum=0)
                for witness in receipt["quiescence"].get("client_readiness", {}).values()
            )
            or not isinstance(receipt["quiescence"].get("client_routes"), Mapping)
            or set(receipt["quiescence"]["client_routes"]) != set(scenario.get("workload", {}).get("clients", []))
            or any(
                not isinstance(pair, Mapping) or set(pair) != {"before", "after"}
                or not isinstance(pair["before"], Mapping) or not isinstance(pair["after"], Mapping)
                or set(pair["before"]) != {"container", "daemon", "route_owner"}
                or set(pair["after"]) != {"container", "daemon", "route_owner"}
                or pair["before"] != pair["after"]
                or not isinstance(pair["before"].get("container"), Mapping)
                or set(pair["before"]["container"]) != {"container_id", "started_at", "running"}
                or not SHA256_RE.fullmatch(str(pair["before"]["container"].get("container_id", "")))
                or not isinstance(pair["before"]["container"].get("started_at"), str)
                or not pair["before"]["container"]["started_at"]
                or pair["before"]["container"].get("running") is not True
                or not _valid_route_process(pair["before"].get("daemon"), "/opt/icecream/sbin/iceccd")
                or not _valid_route_process(pair["before"].get("route_owner"), "/opt/icecream/sbin/icecc-cache-service")
                or pair["before"]["route_owner"].get("ppid") != pair["before"]["daemon"].get("pid")
                or pair["before"]["route_owner"].get("uid") != pair["before"]["daemon"].get("uid")
                for pair in receipt["quiescence"]["client_routes"].values()
            )):
        return {marker}
    for side in ("before", "after"):
        snapshot = receipt.get(side)
        if (not isinstance(snapshot, Mapping)
                or set(snapshot) != {"container_id", "started_at"}
                or not isinstance(snapshot.get("container_id"), str)
                or re.fullmatch(r"[0-9a-f]{64}", snapshot["container_id"]) is None
                or not isinstance(snapshot.get("started_at"), str)
                or not snapshot["started_at"]):
            return {marker}
    if (receipt["before"]["container_id"] != receipt["after"]["container_id"]
            or receipt["before"]["started_at"] == receipt["after"]["started_at"]):
        return {marker}
    return set()


def _worker_restart_receipt_errors(
    receipt: Any, event: Mapping[str, Any], scenario: Mapping[str, Any]
) -> set[str]:
    marker = "@event:worker-restart"
    if not isinstance(receipt, Mapping) or set(receipt) != {
        "action",
        "after",
        "before",
        "coordination",
        "event_epoch",
        "instance",
        "schema",
        "turn",
    }:
        return {marker}
    workload = scenario.get("workload")
    instances = scenario.get("instances")
    target = next(
        (
            item
            for item in instances
            if isinstance(item, Mapping)
            and item.get("name") == receipt.get("instance")
        ),
        None,
    ) if isinstance(instances, list) else None
    if (
        receipt.get("schema") != "icefarm-worker-restart-v1"
        or receipt.get("action") != "restart"
        or receipt.get("action") != event.get("action")
        or receipt.get("instance") != event.get("instance")
        or receipt.get("event_epoch") != event.get("event_epoch")
        or not isinstance(workload, Mapping)
        or receipt.get("turn") not in workload.get("turns", [])
        or not isinstance(target, Mapping)
        or target.get("role") != "F"
    ):
        return {marker}
    snapshots: dict[str, Mapping[str, Any]] = {}
    for side in ("before", "after"):
        snapshot = receipt.get(side)
        if (
            not isinstance(snapshot, Mapping)
            or set(snapshot) != {"container_id", "started_at"}
            or not isinstance(snapshot.get("container_id"), str)
            or SHA256_RE.fullmatch(snapshot["container_id"]) is None
            or not isinstance(snapshot.get("started_at"), str)
            or not snapshot["started_at"]
        ):
            return {marker}
        snapshots[side] = snapshot
    if (
        snapshots["before"]["container_id"]
        != snapshots["after"]["container_id"]
        or snapshots["before"]["started_at"] == snapshots["after"]["started_at"]
    ):
        return {marker}

    coordination = receipt.get("coordination")
    if not isinstance(coordination, Mapping) or set(coordination) != {
        "ready_ms",
        "readiness",
        "scheduler_rejoin",
        "worker_snapshot",
        "workers",
    }:
        return {marker}
    if (
        not _is_int(coordination.get("ready_ms"))
        or coordination.get("ready_ms") != event.get("fired_ms")
    ):
        return {marker}
    expected_workers = sorted(
        item.get("name")
        for item in instances
        if isinstance(item, Mapping)
        and item.get("role") == "F"
        and isinstance(item.get("name"), str)
    )
    worker_snapshot = coordination.get("worker_snapshot")
    if (
        not expected_workers
        or coordination.get("workers") != expected_workers
        or not isinstance(worker_snapshot, str)
        or not worker_snapshot
        or any(
            re.search(
                rf"(^|\s){re.escape(name)}(\s|$)", worker_snapshot, re.MULTILINE
            )
            is None
            for name in expected_workers
        )
    ):
        return {marker}
    readiness = coordination.get("readiness")
    readiness_path = (
        readiness.get("log_path") if isinstance(readiness, Mapping) else None
    )
    if (
        not isinstance(readiness, Mapping)
        or set(readiness)
        != {"cache_line", "host", "line", "log_path", "offset", "role"}
        or readiness.get("host") != target.get("host")
        or readiness.get("role") != "F"
        or not isinstance(readiness_path, str)
        or not readiness_path.startswith("/")
        or not readiness_path.endswith(f"/{target['name']}/log/iceccd.log")
        or not _is_int(readiness.get("offset"))
        or not isinstance(readiness.get("line"), str)
        or re.search(r"ICECREAM daemon .* starting up", readiness["line"])
        is None
        or not isinstance(readiness.get("cache_line"), str)
        or "cache sidecar adapter state=2 lifecycle=3"
        not in readiness["cache_line"]
    ):
        return {marker}

    scheduler = next(
        (
            item
            for item in instances
            if isinstance(item, Mapping) and item.get("role") == "S"
        ),
        None,
    )
    expected_profile = (
        scheduler.get("env", {}).get("ICECC_P50_PROFILE")
        if isinstance(scheduler, Mapping)
        else None
    )
    rejoin = coordination.get("scheduler_rejoin")
    login_line = rejoin.get("login_line") if isinstance(rejoin, Mapping) else None
    cache_line = rejoin.get("cache_line") if isinstance(rejoin, Mapping) else None
    cache = (
        re.search(
            r"\bcache=([^ ]+) cache_wire=v1 cache_protocol=1 "
            r"cache_profiles=([a-z0-9_ ]+)\s*$",
            cache_line,
        )
        if isinstance(cache_line, str)
        else None
    )
    if (
        not isinstance(scheduler, Mapping)
        or not isinstance(rejoin, Mapping)
        or set(rejoin)
        != {
            "cache_line",
            "cache_protocol",
            "bytes",
            "host",
            "login_line",
            "log_path",
            "loss_job_ids",
            "offset",
            "profile",
            "role_protocol",
            "scheduler",
            "sha256",
            "target",
        }
        or rejoin.get("host") != scheduler.get("host")
        or not isinstance(rejoin.get("log_path"), str)
        or not rejoin["log_path"].startswith("/")
        or not rejoin["log_path"].endswith(
            f"/{scheduler.get('name')}/log/scheduler.log"
        )
        or rejoin.get("scheduler") != scheduler.get("name")
        or rejoin.get("target") != target.get("name")
        or rejoin.get("profile") != expected_profile
        or rejoin.get("role_protocol") != 50
        or rejoin.get("cache_protocol") != 1
        or not _is_int(rejoin.get("offset"))
        or not _is_int(rejoin.get("bytes"), minimum=1)
        or not isinstance(rejoin.get("sha256"), str)
        or re.fullmatch(r"[0-9a-f]{64}", rejoin["sha256"]) is None
        or not isinstance(login_line, str)
        or re.search(
            rf"\blogin\s+{re.escape(str(target['name']))}\s+protocol\s+version:\s*50\b",
            login_line,
        )
        is None
        or cache is None
        or not isinstance(expected_profile, str)
        or expected_profile.lower() not in cache.group(2).split()
        or not isinstance(rejoin.get("loss_job_ids"), list)
        or len(rejoin["loss_job_ids"]) != len(set(rejoin["loss_job_ids"]))
        or any(type(item) is not int or item < 1 for item in rejoin["loss_job_ids"])
    ):
        return {marker}
    return set()


def _valid_route_process(value: Any, executable: str) -> bool:
    return (
        isinstance(value, Mapping)
        and set(value)
        == {"argv", "exe", "exe_evidence", "pid", "ppid", "start_ticks", "uid"}
        and value.get("exe") == executable
        and value.get("exe_evidence")
        in {"proc-exe", "argv0-after-proc-exe-eacces"}
        and isinstance(value.get("argv"), list)
        and bool(value["argv"])
        and value["argv"][0] == executable
        and all(isinstance(item, str) and "\0" not in item for item in value["argv"])
        and all(
            _is_int(value.get(field), minimum=minimum)
            for field, minimum in (
                ("pid", 1),
                ("ppid", 0),
                ("start_ticks", 1),
                ("uid", 0),
            )
        )
    )


def _client_route_restart_receipt_errors(
    receipt: Any, event: Mapping[str, Any], scenario: Mapping[str, Any]
) -> set[str]:
    marker = "@event:client-route-owner-restart"
    if not isinstance(receipt, Mapping) or set(receipt) != {
        "action",
        "after",
        "before",
        "coordination",
        "event_epoch",
        "instance",
        "schema",
        "turn",
    }:
        return {marker}
    workload = scenario.get("workload")
    instances = scenario.get("instances")
    if not isinstance(workload, Mapping) or not isinstance(instances, list):
        return {marker}
    turns = workload.get("turns")
    client_names = workload.get("clients")
    target = next(
        (
            item
            for item in instances
            if isinstance(item, Mapping) and item.get("name") == receipt.get("instance")
        ),
        None,
    )
    if (
        receipt.get("schema") != "icefarm-client-route-restart-v1"
        or receipt.get("action") != event.get("action")
        or receipt.get("instance") != event.get("instance")
        or receipt.get("event_epoch") != event.get("event_epoch")
        or not isinstance(turns, list)
        or receipt.get("turn") not in turns
        or not isinstance(client_names, list)
        or not client_names
        or receipt.get("instance") not in client_names
        or not isinstance(target, Mapping)
        or target.get("role") != "C"
    ):
        return {marker}

    snapshots: dict[str, Mapping[str, Any]] = {}
    fields = {"container_id", "container_started_at", "daemon", "route_owner"}
    for side in ("before", "after"):
        value = receipt.get(side)
        if (
            not isinstance(value, Mapping)
            or set(value) != fields
            or not isinstance(value.get("container_id"), str)
            or re.fullmatch(r"[0-9a-f]{64}", value["container_id"]) is None
            or not isinstance(value.get("container_started_at"), str)
            or not value["container_started_at"]
            or not _valid_route_process(
                value.get("daemon"), "/opt/icecream/sbin/iceccd"
            )
            or not _valid_route_process(
                value.get("route_owner"),
                "/opt/icecream/sbin/icecc-cache-service",
            )
            or value["route_owner"]["pid"] <= 1
            or value["route_owner"]["ppid"] != value["daemon"]["pid"]
            or value["route_owner"]["uid"] != value["daemon"]["uid"]
        ):
            return {marker}
        snapshots[side] = value
    if (
        snapshots["before"]["container_id"] != snapshots["after"]["container_id"]
        or snapshots["before"]["container_started_at"]
        != snapshots["after"]["container_started_at"]
        or snapshots["before"]["daemon"] != snapshots["after"]["daemon"]
        or (
            snapshots["before"]["route_owner"]["pid"],
            snapshots["before"]["route_owner"]["start_ticks"],
        )
        == (
            snapshots["after"]["route_owner"]["pid"],
            snapshots["after"]["route_owner"]["start_ticks"],
        )
        or snapshots["before"]["route_owner"]["uid"]
        != snapshots["after"]["route_owner"]["uid"]
    ):
        return {marker}

    coordination = receipt.get("coordination")
    if not isinstance(coordination, Mapping) or set(coordination) != {
        "clients",
        "ready_ms",
        "readiness",
        "resume",
        "signal",
    }:
        return {marker}
    ready_ms = coordination.get("ready_ms")
    if not _is_int(ready_ms) or ready_ms != event.get("fired_ms"):
        return {marker}
    pauses = coordination.get("clients")
    resumes = coordination.get("resume")
    expected_clients = set(client_names)
    if (
        not isinstance(pauses, Mapping)
        or not isinstance(resumes, Mapping)
        or set(pauses) != expected_clients
        or set(resumes) != expected_clients
    ):
        return {marker}
    gate_fields = {
        "action",
        "active_after",
        "active_before",
        "client",
        "epoch",
        "finished_ms",
        "schema",
        "started_ms",
        "status",
        "turn",
    }
    for name in sorted(expected_clients):
        for action, status, value in (
            ("pause", "PAUSED", pauses[name]),
            ("resume", "OPEN", resumes[name]),
        ):
            if (
                not isinstance(value, Mapping)
                or set(value) != gate_fields
                or value.get("schema") != "icefarm-event-gate-v1"
                or value.get("action") != action
                or value.get("status") != status
                or value.get("client") != name
                or value.get("turn") != receipt["turn"]
                or value.get("epoch") != receipt["event_epoch"]
                or any(
                    not _is_int(value.get(field))
                    for field in (
                        "active_after",
                        "active_before",
                        "finished_ms",
                        "started_ms",
                    )
                )
                or (action in {"pause", "quiesce"} and not _is_int(value.get("active_before"), minimum=1))
                or value["finished_ms"] < value["started_ms"]
            ):
                return {marker}
        if pauses[name]["active_after"] != 0:
            return {marker}

    signal_receipt = coordination.get("signal")
    if (
        not isinstance(signal_receipt, Mapping)
        or set(signal_receipt)
        != {"daemon", "mechanism", "route_owner", "schema", "sent_ms", "signal"}
        or signal_receipt.get("schema") != "icefarm-client-route-signal-v1"
        or signal_receipt.get("mechanism") != "pidfd_send_signal"
        or signal_receipt.get("signal") != 9
        or not _is_int(signal_receipt.get("sent_ms"))
        or signal_receipt.get("daemon") != snapshots["before"]["daemon"]
        or signal_receipt.get("route_owner") != snapshots["before"]["route_owner"]
        or any(
            pauses[name]["finished_ms"] > signal_receipt["sent_ms"]
            for name in expected_clients
        )
        or signal_receipt["sent_ms"] > ready_ms
    ):
        return {marker}
    readiness = coordination.get("readiness")
    host = target.get("host")
    log_path = readiness.get("log_path") if isinstance(readiness, Mapping) else None
    if (
        not isinstance(readiness, Mapping)
        or set(readiness)
        != {"host", "lifecycle", "line", "log_path", "offset", "state"}
        or readiness.get("host") != host
        or not isinstance(log_path, str)
        or not log_path.startswith("/")
        or not log_path.endswith(f"/{receipt['instance']}/log/client-daemon.log")
        or not _is_int(readiness.get("offset"))
        or readiness.get("state") != 2
        or readiness.get("lifecycle") != 3
        or not isinstance(readiness.get("line"), str)
        or re.search(
            r"cache sidecar adapter state=2 lifecycle=3", readiness["line"]
        )
        is None
    ):
        return {marker}
    return set()


def _client_transition_receipt_errors(
    observed: Mapping[str, Any],
    expected: Mapping[str, Any],
    scenario: Mapping[str, Any],
) -> set[str]:
    marker = "@event:transition-coordination"
    receipt = observed.get("receipt")
    fields = {
        "action", "after", "before", "checkpoints", "client_readiness",
        "coordination", "event_epoch", "instance", "preflight", "readiness",
        "schema", "turn",
    }
    if not isinstance(receipt, Mapping) or set(receipt) != fields:
        return {marker}
    if receipt.get("action") != expected.get("action") or receipt.get("instance") != expected.get("instance"):
        return {marker}
    if not _is_int(receipt.get("event_epoch"), minimum=1) or receipt.get("event_epoch") != observed.get("event_epoch"):
        return {marker}
    clients = scenario.get("workload", {}).get("clients", [])
    if not isinstance(clients, list) or not clients or not all(isinstance(item, str) for item in clients):
        return {marker}
    if receipt.get("turn") not in set(scenario.get("workload", {}).get("turns", [])):
        return {marker}
    target = next(
        (item for item in scenario.get("instances", []) if isinstance(item, Mapping) and item.get("name") == receipt.get("instance")),
        None,
    )
    if not isinstance(target, Mapping) or target.get("role") != "C":
        return {marker}
    snapshot_fields = {"container_id", "closure_sha256", "env", "image", "role_sha256"}
    snapshots = {}
    for side in ("before", "after"):
        value = receipt.get(side)
        if (
            not isinstance(value, Mapping)
            or set(value) != snapshot_fields
            or not isinstance(value.get("container_id"), str)
            or SHA256_RE.fullmatch(value["container_id"]) is None
            or not isinstance(value.get("closure_sha256"), str)
            or SHA256_RE.fullmatch(value["closure_sha256"]) is None
            or not isinstance(value.get("role_sha256"), str)
            or SHA256_RE.fullmatch(value["role_sha256"]) is None
            or not isinstance(value.get("image"), str)
            or not isinstance(value.get("env"), Mapping)
            or any(type(key) is not str or type(item) is not str for key, item in value["env"].items())
        ):
            return {marker}
        snapshots[side] = value
    if snapshots["before"]["container_id"] == snapshots["after"]["container_id"]:
        return {marker}
    preflight = receipt.get("preflight")
    if (
        not isinstance(preflight, Mapping)
        or set(preflight) != {"image_closure_sha256", "role_sha256", "runtime_path"}
        or not isinstance(preflight.get("image_closure_sha256"), str)
        or SHA256_RE.fullmatch(preflight["image_closure_sha256"]) is None
        or not isinstance(preflight.get("role_sha256"), str)
        or SHA256_RE.fullmatch(preflight["role_sha256"]) is None
        or not isinstance(preflight.get("runtime_path"), str)
        or not preflight["runtime_path"].startswith("/")
        or snapshots["after"]["closure_sha256"] != preflight["image_closure_sha256"]
        or snapshots["after"]["role_sha256"] != preflight["role_sha256"]
    ):
        return {marker}
    action = expected.get("action")
    if action in {"upgrade", "downgrade"}:
        alias = expected.get("image")
        target_label = scenario.get("images", {}).get(alias) if isinstance(alias, str) else None
        before_version = _transition_protocol(snapshots["before"]["image"])
        after_version = _transition_protocol(snapshots["after"]["image"])
        if not isinstance(target_label, str) or snapshots["after"]["image"] != target_label or before_version is None or after_version is None:
            return {marker}
        if action == "upgrade" and after_version <= before_version:
            return {marker}
        if action == "downgrade" and after_version >= before_version:
            return {marker}
    elif action == "restart" and snapshots["before"]["image"] != snapshots["after"]["image"]:
        return {marker}
    coordination = receipt.get("coordination")
    fired_ms = observed.get("fired_ms")
    if (
        not _is_int(fired_ms)
        or not isinstance(coordination, Mapping)
        or set(coordination) != {"clients", "ready_ms", "relaunch", "resume"}
        or coordination.get("ready_ms") != fired_ms
    ):
        return {marker}
    gate_fields = {"action", "active_after", "active_before", "client", "epoch", "finished_ms", "schema", "started_ms", "status", "turn"}
    pauses = coordination["clients"]
    resumes = coordination["resume"]
    if not isinstance(pauses, Mapping) or not isinstance(resumes, Mapping) or set(pauses) != set(clients) or set(resumes) != set(clients):
        return {marker}
    for name in clients:
        pause, resume = pauses[name], resumes[name]
        if (
            not isinstance(pause, Mapping) or not isinstance(resume, Mapping)
            or set(pause) != gate_fields or set(resume) != gate_fields
            or pause.get("action") != "quiesce" or pause.get("status") != "QUIESCED"
            or resume.get("action") != "resume" or resume.get("status") != "OPEN"
            or pause.get("client") != name or resume.get("client") != name
            or pause.get("turn") != receipt.get("turn") or resume.get("turn") != receipt.get("turn")
            or pause.get("epoch") != receipt.get("event_epoch") or resume.get("epoch") != receipt.get("event_epoch")
            or pause.get("active_after") != 0
            or not _is_int(pause.get("active_before"), minimum=1)
            or any(not _is_int(value.get(field)) for value in (pause, resume) for field in ("active_after", "active_before", "finished_ms", "started_ms", "epoch"))
        ):
            return {marker}
    checkpoints = receipt.get("checkpoints")
    checkpoint_fields = {"checkpoint_sha256", "client", "completed_rows", "completed_rows_sha256", "expected_jobs", "schema", "status", "turn", "worklist_sha256"}
    if not isinstance(checkpoints, Mapping) or set(checkpoints) != set(clients):
        return {marker}
    for name in clients:
        checkpoint = checkpoints[name]
        if (
            not isinstance(checkpoint, Mapping) or set(checkpoint) != checkpoint_fields
            or checkpoint.get("schema") != "icefarm-workload-checkpoint-v1"
            or checkpoint.get("status") != "QUIESCED" or checkpoint.get("client") != name
            or checkpoint.get("turn") != receipt.get("turn") or not _is_int(checkpoint.get("expected_jobs"), minimum=1)
            or not isinstance(checkpoint.get("completed_rows"), list) or not checkpoint["completed_rows"]
            or not SHA256_RE.fullmatch(str(checkpoint.get("checkpoint_sha256")))
            or not SHA256_RE.fullmatch(str(checkpoint.get("completed_rows_sha256")))
            or not SHA256_RE.fullmatch(str(checkpoint.get("worklist_sha256")))
        ):
            return {marker}
        rows = checkpoint["completed_rows"]
        seen = set()
        for row in rows:
            if (
                not isinstance(row, Mapping) or set(row) != {"index", "path", "sha256"}
                or not _is_int(row.get("index"), minimum=1) or row["index"] in seen
                or row["index"] > checkpoint["expected_jobs"]
                or row.get("path") != f"jobs/{row['index']:06d}/result.tsv"
                or not isinstance(row.get("sha256"), str) or SHA256_RE.fullmatch(row["sha256"]) is None
            ):
                return {marker}
            seen.add(row["index"])
        canonical = json.dumps(rows, sort_keys=True, separators=(",", ":")).encode()
        body = dict(checkpoint)
        digest = body.pop("checkpoint_sha256")
        if checkpoint["completed_rows_sha256"] != hashlib.sha256(canonical).hexdigest() or digest != hashlib.sha256(json.dumps(body, sort_keys=True, separators=(",", ":")).encode()).hexdigest():
            return {marker}
    relaunch = coordination["relaunch"]
    if not isinstance(relaunch, Mapping) or set(relaunch) != set(clients):
        return {marker}
    for name in clients:
        value = relaunch[name]
        if (
            not isinstance(value, Mapping) or set(value) != {"client", "expected_jobs", "failures", "jobs", "status"}
            or value.get("client") != name or value.get("status") != "COMPLETE"
            or not _is_int(value.get("expected_jobs"), minimum=1) or value.get("expected_jobs") != checkpoints[name]["expected_jobs"]
            or not _is_int(value.get("jobs"), minimum=0)
            or not _is_int(value.get("failures"), minimum=0)
            or value.get("jobs") != value.get("expected_jobs") or value.get("failures") != 0
        ):
            return {marker}
    readiness = receipt.get("readiness")
    client_readiness = receipt.get("client_readiness")
    after_version = _transition_protocol(snapshots["after"]["image"])
    if after_version is None:
        return {marker}
    cache_required = (
        after_version == 50
        and snapshots["after"]["env"].get("ICECC_P50_MODE") == "on"
    )
    if (
        not isinstance(readiness, Mapping) or set(readiness) != {"host", "line", "log_path", "offset", "role"}
        or readiness.get("role") != "C"
        or readiness.get("host") != target.get("host")
        or not isinstance(readiness.get("log_path"), str)
        or not readiness["log_path"].startswith("/")
        or readiness["log_path"].split("/")[-3:]
        != [receipt["instance"], "log", "client-daemon.log"]
        or not isinstance(readiness.get("line"), str)
        or re.search(r"ICECREAM daemon .* starting up", readiness["line"]) is None
        or not _is_int(readiness.get("offset"), minimum=0)
        or not isinstance(client_readiness, Mapping)
        or set(client_readiness) != {"cache_line", "cache_required", "connected_line", "host", "log_path", "offset"}
        or client_readiness.get("host") != target.get("host")
        or client_readiness.get("log_path") != readiness.get("log_path")
        or not isinstance(client_readiness.get("log_path"), str)
        or not client_readiness["log_path"].startswith("/")
        or client_readiness["log_path"].split("/")[-3:]
        != [receipt["instance"], "log", "client-daemon.log"]
        or not isinstance(client_readiness.get("connected_line"), str)
        or "Connected to scheduler (I am known as " not in client_readiness["connected_line"]
        or not _is_int(client_readiness.get("offset"), minimum=0)
        or client_readiness["offset"] != readiness["offset"]
        or client_readiness.get("cache_required") is not cache_required
        or (
            cache_required
            and (
                not isinstance(client_readiness.get("cache_line"), str)
                or re.search(
                    r"cache sidecar adapter state=2 lifecycle=3",
                    client_readiness["cache_line"],
                )
                is None
            )
        )
        or (not cache_required and client_readiness.get("cache_line") is not None)
    ):
        return {marker}
    return set()


def _transition_receipt_errors(
    observed: Mapping[str, Any] | None,
    expected: Mapping[str, Any],
    scenario: Mapping[str, Any],
) -> set[str]:
    marker = "@event:transition-coordination"
    if not isinstance(observed, Mapping) or observed.get("action") != expected.get("action"):
        return {marker}
    receipt = observed.get("receipt")
    if isinstance(receipt, Mapping) and receipt.get("schema") == "icefarm-client-transition-v1":
        return _client_transition_receipt_errors(observed, expected, scenario)
    if not isinstance(receipt, Mapping) or set(receipt) != {
        "action", "after", "before", "coordination", "event_epoch", "instance",
        "preflight", "readiness", "schema", "turn",
    } or receipt.get("schema") != "icefarm-transition-v2":
        return {marker}
    snapshot_fields = {"container_id", "closure_sha256", "env", "image", "role_sha256"}
    snapshots: dict[str, Mapping[str, Any]] = {}
    for side in ("before", "after"):
        value = receipt.get(side)
        if (
            not isinstance(value, Mapping)
            or set(value) != snapshot_fields
            or not isinstance(value.get("container_id"), str)
            or SHA256_RE.fullmatch(value["container_id"]) is None
            or not isinstance(value.get("closure_sha256"), str)
            or SHA256_RE.fullmatch(value["closure_sha256"]) is None
            or not isinstance(value.get("role_sha256"), str)
            or SHA256_RE.fullmatch(value["role_sha256"]) is None
            or not isinstance(value.get("image"), str)
            or not isinstance(value.get("env"), Mapping)
            or any(type(key) is not str or type(item) is not str for key, item in value["env"].items())
        ):
            return {marker}
        snapshots[side] = value
    if snapshots["before"]["container_id"] == snapshots["after"]["container_id"]:
        return {marker}
    preflight = receipt.get("preflight")
    if (
        not isinstance(preflight, Mapping)
        or set(preflight) != {"image_closure_sha256", "role_sha256", "runtime_path"}
        or not isinstance(preflight.get("image_closure_sha256"), str)
        or SHA256_RE.fullmatch(preflight["image_closure_sha256"]) is None
        or not isinstance(preflight.get("role_sha256"), str)
        or SHA256_RE.fullmatch(preflight["role_sha256"]) is None
        or not isinstance(preflight.get("runtime_path"), str)
        or not preflight["runtime_path"].startswith("/")
        or snapshots["after"]["closure_sha256"] != preflight["image_closure_sha256"]
        or snapshots["after"]["role_sha256"] != preflight["role_sha256"]
    ):
        return {marker}
    instances = scenario.get("instances")
    target = next(
        (item for item in instances if isinstance(item, Mapping) and item.get("name") == expected.get("instance")),
        None,
    ) if isinstance(instances, list) else None
    if not isinstance(target, Mapping) or target.get("role") not in {"S", "F"}:
        return {marker}
    turns = scenario.get("workload", {}).get("turns", [])
    valid_turns = set(turns) | {None} if isinstance(turns, list) else {None}
    if (
        receipt.get("instance") != expected.get("instance")
        or receipt.get("event_epoch") != observed.get("event_epoch")
        or not _is_int(receipt.get("event_epoch"), minimum=1)
        or receipt.get("turn") not in valid_turns
    ):
        return {marker}
    expected_label = (
        scenario.get("images", {}).get(expected.get("image"))
        if expected.get("action") in {"upgrade", "downgrade"}
        else snapshots["before"]["image"]
    )
    if snapshots["after"]["image"] != expected_label:
        return {marker}
    before_protocol = _transition_protocol(snapshots["before"]["image"])
    after_protocol = _transition_protocol(snapshots["after"]["image"])
    if before_protocol is None or after_protocol is None:
        return {marker}
    if expected.get("action") == "upgrade" and after_protocol <= before_protocol:
        return {marker}
    if expected.get("action") == "downgrade" and after_protocol >= before_protocol:
        return {marker}
    if expected.get("action") == "env_set":
        update = expected.get("env")
        if not isinstance(update, Mapping) or any(snapshots["after"]["env"].get(key) != value for key, value in update.items()):
            return {marker}
    fired_ms = observed.get("fired_ms")
    coordination = receipt.get("coordination")
    if (
        not _is_int(fired_ms)
        or not isinstance(coordination, Mapping)
        or not _is_int(coordination.get("ready_ms"))
        or coordination.get("ready_ms") != fired_ms
    ):
        return {marker}
    expected_workers = sorted(
        item.get("name") for item in instances
        if isinstance(item, Mapping) and item.get("role") == "F" and isinstance(item.get("name"), str)
    )
    if (
        not isinstance(coordination.get("workers"), list)
        or coordination["workers"] != expected_workers
        or not isinstance(coordination.get("scheduler_snapshot"), str)
        or not coordination["scheduler_snapshot"]
        or not isinstance(coordination.get("worker_snapshot"), str)
        or any(re.search(rf"(^|\s){re.escape(name)}(\s|$)", coordination["worker_snapshot"], re.MULTILINE) is None for name in expected_workers)
    ):
        return {marker}
    expected_clients = set(scenario.get("workload", {}).get("clients", [])) if receipt.get("turn") else set()
    pauses = coordination.get("clients")
    resumes = coordination.get("resume")
    if (
        not isinstance(pauses, Mapping)
        or not isinstance(resumes, Mapping)
        or set(pauses) != expected_clients
        or set(resumes) != expected_clients
    ):
        return {marker}
    for name in sorted(expected_clients):
        pause = pauses[name]
        resume = resumes[name]
        if (
            not isinstance(pause, Mapping)
            or not isinstance(resume, Mapping)
            or pause.get("schema") != "icefarm-event-gate-v1"
            or resume.get("schema") != "icefarm-event-gate-v1"
            or pause.get("action") != "pause"
            or resume.get("action") != "resume"
            or pause.get("status") != "PAUSED"
            or resume.get("status") != "OPEN"
            or pause.get("client") != name
            or resume.get("client") != name
            or pause.get("active_after") != 0
            or not _is_int(pause.get("active_before"), minimum=1)
            or not all(_is_int(value) for value in (pause.get("finished_ms"), resume.get("started_ms")))
        ):
            return {marker}
    readiness = receipt.get("readiness")
    if (
        not isinstance(readiness, Mapping)
        or readiness.get("role") != target.get("role")
        or not isinstance(readiness.get("host"), str)
        or not readiness["host"]
        or not isinstance(readiness.get("log_path"), str)
        or not readiness["log_path"].startswith("/")
        or not isinstance(readiness.get("line"), str)
        or not _is_int(readiness.get("offset"))
    ):
        return {marker}
    if target["role"] == "S":
        if re.search(r"ICECREAM scheduler .* starting up, port [0-9]+", readiness["line"]) is None:
            return {marker}
        startup = coordination.get("scheduler_startup")
        client_readiness = coordination.get("client_readiness")
        if startup != readiness or not isinstance(client_readiness, Mapping) or set(client_readiness) != expected_clients:
            return {marker}
        for name in expected_clients:
            witness = client_readiness[name]
            client = next((item for item in instances if isinstance(item, Mapping) and item.get("name") == name), None)
            cache_required = isinstance(client, Mapping) and client.get("env", {}).get("ICECC_P50_MODE") == "on"
            if (
                not isinstance(witness, Mapping)
                or witness.get("cache_required") is not cache_required
                or not isinstance(witness.get("host"), str)
                or not witness["host"]
                or not isinstance(witness.get("log_path"), str)
                or not witness["log_path"].startswith("/")
                or not _is_int(witness.get("offset"))
                or not isinstance(witness.get("connected_line"), str)
                or "Connected to scheduler (I am known as " not in witness["connected_line"]
                or (cache_required and (not isinstance(witness.get("cache_line"), str) or "cache sidecar adapter state=2 lifecycle=3" not in witness["cache_line"]))
                or (not cache_required and witness.get("cache_line") is not None)
            ):
                return {marker}
    else:
        if re.search(r"ICECREAM daemon .* starting up", readiness["line"]) is None:
            return {marker}
        rejoin = coordination.get("scheduler_worker_rejoin")
        target_alias = expected.get("image")
        target_label = scenario.get("images", {}).get(target_alias) if isinstance(target_alias, str) else None
        protocol = _transition_protocol(target_label)
        if (
            not isinstance(rejoin, Mapping)
            or protocol is None
            or rejoin.get("target") != target.get("name")
            or rejoin.get("role_protocol") != protocol
            or not isinstance(rejoin.get("host"), str)
            or not rejoin["host"]
            or not isinstance(rejoin.get("log_path"), str)
            or not rejoin["log_path"].startswith("/")
            or not _is_int(rejoin.get("offset"))
            or not _is_int(rejoin.get("bytes"), minimum=1)
            or not isinstance(rejoin.get("line"), str)
            or re.search(rf"\blogin\s+{re.escape(str(target['name']))}\s+protocol\s+version:\s*{protocol}\b", rejoin["line"]) is None
        ):
            return {marker}
    return set()


def _shape_clauses(
    scenario: Mapping[str, Any],
    rows: list[Mapping[str, Any]],
    observations: Mapping[str, Any],
    selected_profile: str | None,
    event_log: Any,
    topology: Any = None,
    run_id: Any = None,
) -> list[dict[str, Any]]:
    shape = scenario.get("shape")
    clauses: list[dict[str, Any]] = []
    instances = scenario.get("instances", [])
    workers = {
        item.get("name")
        for item in instances
        if isinstance(item, Mapping) and item.get("role") == "F"
    }
    clients = {
        item.get("name")
        for item in instances
        if isinstance(item, Mapping) and item.get("role") == "C"
    }
    schedulers = {
        item.get("name")
        for item in instances
        if isinstance(item, Mapping) and item.get("role") == "S"
    }
    timeline = scenario.get("timeline")
    if isinstance(timeline, list):
        for index, expected_event in enumerate(timeline):
            if not isinstance(expected_event, Mapping) or expected_event.get("action") not in {
                "upgrade", "downgrade", "env_set", "restart"
            }:
                continue
            target = next(
                (item for item in instances if isinstance(item, Mapping) and item.get("name") == expected_event.get("instance")),
                None,
            )
            # Scheduler, client-route, and B4 worker restarts have stricter
            # dedicated receipt validators below.  Other F restarts use the
            # generic transition-v2 contract here.
            if expected_event.get("action") == "restart" and (
                not isinstance(target, Mapping)
                or target.get("role") in {"S", "C"}
                or scenario.get("expect", {}).get("engagement")
                == S70_B4_WORKER_ENGAGEMENT
            ):
                continue
            observed_event = (
                event_log[index]
                if isinstance(event_log, list) and len(event_log) == len(timeline)
                else None
            )
            transition_bad = _transition_receipt_errors(
                observed_event, expected_event, scenario
            )
            clauses.append(
                _clause(
                    f"event.transition-{index}",
                    not transition_bad,
                    "transitions prove checkpointed pause/drain and role-specific fresh readiness",
                    transition_bad,
                )
            )
    logins = observations.get("logins")
    sidecars = observations.get("sidecars")
    logins = logins if isinstance(logins, list) else []
    sidecars = sidecars if isinstance(sidecars, Mapping) else {}

    def login_for(name: object) -> list[Mapping[str, Any]]:
        return [
            item
            for item in logins
            if isinstance(item, Mapping) and item.get("instance") == name
        ]

    def sidecar(name: object) -> Mapping[str, Any]:
        value = sidecars.get(name)
        return value if isinstance(value, Mapping) else {}

    if shape in ("SCF", "S'CF", "S'FC'"):
        offenders = {
            _job_id(row.get("job_id"), "@row")
            for row in rows
            if row.get("tail_present") is True
        }
        bad_workers = {
            str(worker)
            for worker in workers
            if not login_for(worker)
            or any(
                item.get("cache_profiles") not in ([], None)
                for item in login_for(worker)
            )
            or sidecar(worker).get("process_count") != 0
            or sidecar(worker).get("cache_ports") not in ([], None)
            or sidecar(worker).get("sessions") != 0
        }
        clauses.append(
            _clause(
                "shape.legacy-surface",
                not offenders and not bad_workers,
                "legacy workers have cache=off, no tails, ports, processes, or sessions",
                offenders | {f"@instance:{name}" for name in bad_workers},
            )
        )
    if shape in ("SCF", "S'CF"):
        bad_logins: set[str] = set()
        for index, item in enumerate(logins):
            if not isinstance(item, Mapping):
                bad_logins.add(f"@login:{index}")
                continue
            if not _is_int(item.get("protocol"), minimum=1) or item["protocol"] > 43:
                bad_logins.add(f"@instance:{item.get('instance', '?')}")
        clauses.append(
            _clause(
                "shape.negotiated-legacy",
                bool(logins) and not bad_logins,
                "all scheduler logins negotiate protocol 43 or older",
                bad_logins or ({"@observations:logins"} if not logins else set()),
            )
        )
    if shape == "S'CF" and scenario.get("timeline"):
        timeline = scenario.get("timeline")
        expected_event = (
            timeline[0]
            if isinstance(timeline, list) and len(timeline) == 1
            else None
        )
        observed_event = (
            event_log[0]
            if isinstance(event_log, list) and len(event_log) == 1
            else None
        )
        trigger_match = (
            re.fullmatch(r"job ([1-9][0-9]*)", expected_event.get("trigger", ""))
            if isinstance(expected_event, Mapping)
            else None
        )
        required_dispatches = int(trigger_match.group(1)) if trigger_match else None
        coordination_bad = (
            _scheduler_restart_receipt_errors(
                observed_event.get("receipt"), observed_event, scenario
            )
            if isinstance(observed_event, Mapping)
            else {"@event:scheduler-restart-coordination"}
        )
        restart_bound = (
            isinstance(expected_event, Mapping)
            and expected_event.get("action") == "restart"
            and expected_event.get("instance") in schedulers
            and isinstance(observed_event, Mapping)
            and observed_event.get("action") == expected_event.get("action")
            and observed_event.get("instance") == expected_event.get("instance")
            and observed_event.get("trigger") == expected_event.get("trigger")
            and observed_event.get("event_index") == 0
            and observed_event.get("event_epoch") == 1
            and _is_int(observed_event.get("fired_ms"))
            and _is_int(observed_event.get("last_dispatched_job"), minimum=1)
            and required_dispatches is not None
            and _is_int(
                observed_event.get("workload_dispatch_count"),
                minimum=required_dispatches,
            )
            and not coordination_bad
        )
        epochs = {row.get("event_epoch") for row in rows}
        clauses.append(
            _clause(
                "shape.scheduler-first-restart",
                restart_bound and 0 in epochs and 1 in epochs,
                "the new scheduler restarts mid-build and old peers remain active before and after",
                set()
                if restart_bound and 0 in epochs and 1 in epochs
                else {"@event:scheduler-restart"} | coordination_bad,
            )
        )
    if shape == "S'FC'":
        bad_clients = {
            str(client) for client in clients if sidecar(client).get("sessions") != 0
        }
        clauses.append(
            _clause(
                "shape.client-zero-sessions",
                not bad_clients,
                "new clients open no P50 session against old workers",
                {f"@instance:{name}" for name in bad_clients},
            )
        )
        timeline = scenario.get("timeline")
        if isinstance(timeline, list) and timeline:
            expected_event = timeline[0] if len(timeline) == 1 else None
            observed_event = (
                event_log[0]
                if isinstance(event_log, list) and len(event_log) == 1
                else None
            )
            trigger_match = (
                re.fullmatch(r"job ([1-9][0-9]*)", expected_event.get("trigger", ""))
                if isinstance(expected_event, Mapping)
                else None
            )
            required_dispatches = int(trigger_match.group(1)) if trigger_match else None
            receipt_bad = (
                _client_route_restart_receipt_errors(
                    observed_event.get("receipt"), observed_event, scenario
                )
                if isinstance(observed_event, Mapping)
                else {"@event:client-route-owner-restart"}
            )
            restart_bound = (
                isinstance(expected_event, Mapping)
                and expected_event.get("action") == "restart"
                and expected_event.get("instance") in clients
                and isinstance(observed_event, Mapping)
                and observed_event.get("action") == expected_event.get("action")
                and observed_event.get("instance") == expected_event.get("instance")
                and observed_event.get("trigger") == expected_event.get("trigger")
                and observed_event.get("event_index") == 0
                and observed_event.get("event_epoch") == 1
                and _is_int(observed_event.get("fired_ms"))
                and _is_int(observed_event.get("last_dispatched_job"), minimum=1)
                and required_dispatches is not None
                and _is_int(
                    observed_event.get("workload_dispatch_count"),
                    minimum=required_dispatches,
                )
                and not receipt_bad
            )
            epochs = {row.get("event_epoch") for row in rows}
            clauses.append(
                _clause(
                    "shape.client-route-owner-restart",
                    restart_bound and 0 in epochs and 1 in epochs,
                    "the client route owner restarts mid-build while its container and daemon survive",
                    set()
                    if restart_bound and 0 in epochs and 1 in epochs
                    else {"@event:client-route-owner-restart"} | receipt_bad,
                )
            )
    if shape == "S'C'F'":
        refusal_workers = {
            item.get("name")
            for item in instances
            if scenario.get("id") == "S30-mutant-f-refusal"
            and isinstance(item, Mapping)
            and item.get("role") == "F"
            and item.get("image") == "mutant"
        }
        bad_workers = {
            str(worker)
            for worker in workers
            if not login_for(worker)
            or any(
                selected_profile not in (item.get("cache_profiles") or [])
                for item in login_for(worker)
            )
            or (
                worker in refusal_workers
                and sidecar(worker).get("sessions") != 0
            )
            or (
                worker not in refusal_workers
                and not _is_int(sidecar(worker).get("sessions"), minimum=1)
            )
        }
        clauses.append(
            _clause(
                "shape.full-newgen-engagement",
                bool(workers) and not bad_workers,
                "every new worker advertises the selected profile; normal workers "
                "record a session and S30 refusal workers record none",
                {f"@instance:{name}" for name in bad_workers}
                or ({"@scenario:workers"} if not workers else set()),
            )
        )
        timeline = scenario.get("timeline")
        if isinstance(timeline, list) and len(timeline) == 1 and timeline[0].get("action") == "header_edit":
            expected_event = timeline[0]
            observed_event = event_log[0] if isinstance(event_log, list) and len(event_log) == 1 else None
            bound_observed = dict(observed_event) if isinstance(observed_event, Mapping) else {}
            bound_observed["_scenario_event"] = expected_event
            receipt_bad = _header_edit_receipt_errors(
                bound_observed.get("receipt"), bound_observed, scenario
            )
            clauses.append(
                _clause(
                    "shape.header-edit",
                    isinstance(observed_event, Mapping) and not receipt_bad,
                    "header edit changes only the targeted F fingerprint and proves fresh cache readiness",
                    receipt_bad or {"@event:header-edit"},
                )
            )
        if scenario.get("expect", {}).get("engagement") == S95_DISK_FILL_ENGAGEMENT:
            disk_bad: set[str] = set()
            workload = scenario.get("workload")
            named = {
                item.get("name"): item
                for item in instances
                if isinstance(item, Mapping) and isinstance(item.get("name"), str)
            }
            expected_event = {
                "action": "disk_fill",
                "instance": "F1",
                "trigger": "job 12",
            }
            if (
                schedulers != {"S1"}
                or workers != {"F1", "F2"}
                or clients != {"C1"}
                or set(named) != {"S1", "F1", "F2", "C1"}
                or any(named[name].get("slots") != 1 for name in ("F1", "F2"))
                or scenario.get("timeline") != [expected_event]
                or not isinstance(workload, Mapping)
                or workload.get("driver") != "tu-manifest"
                or workload.get("corpus") != "fmt-100"
                or workload.get("turns") != ["A"]
                or workload.get("jobs") != 2
                or workload.get("clients") != ["C1"]
                or workload.get("repeat") != 2
            ):
                disk_bad.add("@scenario:s95-cache-disk-full")
            observed_event = (
                event_log[0]
                if isinstance(event_log, list) and len(event_log) == 1
                else None
            )
            if (
                not isinstance(observed_event, Mapping)
                or observed_event.get("action") != "disk_fill"
                or observed_event.get("instance") != "F1"
                or observed_event.get("trigger") != "job 12"
                or observed_event.get("event_index") != 0
                or observed_event.get("event_epoch") != 1
                or not _is_int(
                    observed_event.get("workload_dispatch_count"), minimum=12
                )
                or not _is_int(
                    observed_event.get("last_dispatched_job"), minimum=1
                )
            ):
                disk_bad.add("@event:disk-fill")
            elif _disk_fill_receipt_errors(
                observed_event.get("receipt"), observed_event, topology,
                run_id,
            ):
                disk_bad.add("@event:disk-fill")
            if observations.get("local_fallback_job_ids") != []:
                disk_bad.add("@observations:local_fallback_job_ids")
            clauses.append(
                _clause(
                    "s95.cache-disk-full",
                    not disk_bad,
                    "one F proves bounded container-local ENOSPC while exact remote work continues without wedges",
                    disk_bad,
                )
            )
    if shape == "S'[FF'][CC']":
        preference_bad = _assignment_preference_errors(
            observations.get("assignment_preference"), scenario, topology
        )
        clauses.append(
            _clause(
                "shape.compatible-free-preference",
                not preference_bad,
                "every mixed-pool dispatch prefers a compatible free worker or records a saturated escape",
                preference_bad,
            )
        )
        old_workers = {row.get("cs") for row in rows if row.get("cs_version") != 50}
        bad_old_workers = {
            str(worker)
            for worker in old_workers
            if sidecar(worker).get("sessions") != 0
        }
        bad_commits = {
            _job_id(row.get("job_id"), "@row")
            for row in rows
            if row.get("session_outcome") == "committed"
            and (row.get("client_version") != 50 or row.get("cs_version") != 50)
        }
        clauses.append(
            _clause(
                "shape.mixed-pair-law",
                not bad_old_workers and not bad_commits,
                "old workers have zero sessions and commits belong only to 50/50 pairs",
                bad_commits | {f"@instance:{name}" for name in bad_old_workers},
            )
        )
    if shape == "S'[F'F''][C']":
        bad: set[str] = set()
        workload = scenario.get("workload")
        named_instances = {
            item.get("name"): item
            for item in instances
            if isinstance(item, Mapping) and isinstance(item.get("name"), str)
        }
        if (
            len(schedulers) != 1
            or len(workers) != 2
            or len(clients) != 1
            or scenario.get("timeline") != []
            or not isinstance(workload, Mapping)
            or workload.get("driver") != "tu-manifest"
            or workload.get("corpus") != "fmt-100"
            or workload.get("turns") != ["A"]
            or workload.get("jobs") != 2
            or workload.get("clients") != sorted(clients)
            or workload.get("repeat") != 1
            or any(
                not isinstance(named_instances.get(worker), Mapping)
                or named_instances[worker].get("slots") != 1
                for worker in workers
            )
        ):
            bad.add("@scenario:revision-skew")
        topology_instances = (
            topology.get("instances") if isinstance(topology, Mapping) else None
        )
        relationships = (
            topology.get("relationships") if isinstance(topology, Mapping) else None
        )
        if not isinstance(topology_instances, list) or not isinstance(relationships, list):
            bad.add("@topology:revision-skew")
            expected_revisions: dict[str, int] = {}
        else:
            expected_revisions = {
                str(item["name"]): item["cache_wire_revision"]
                for item in topology_instances
                if isinstance(item, Mapping)
                and item.get("role") in {"C", "F"}
                and isinstance(item.get("name"), str)
                and _is_int(item.get("cache_wire_revision"), minimum=1)
                and item["cache_wire_revision"] <= 0xFFFF
            }
            expected_names = {
                str(item.get("name"))
                for item in topology_instances
                if isinstance(item, Mapping) and item.get("role") in {"C", "F"}
            }
            if set(expected_revisions) != expected_names:
                bad.add("@topology:wire-revisions")
        revisions = observations.get("wire_revisions")
        for name, revision in expected_revisions.items():
            if not isinstance(revisions, Mapping) or revisions.get(name) != revision:
                bad.add(f"@instance:{name}")
        worker_revisions = {
            name: expected_revisions.get(name)
            for name in workers
            if name in expected_revisions
        }
        client_revisions = {
            name: expected_revisions.get(name)
            for name in clients
            if name in expected_revisions
        }
        compatible_pairs = {
            (relationship.get("c"), relationship.get("f"))
            for relationship in relationships or []
            if isinstance(relationship, Mapping)
            and relationship.get("cache_expected") is True
        }
        incompatible_pairs = {
            (client, worker)
            for client in clients
            for worker in workers
            if (client, worker) not in compatible_pairs
        }
        if (
            not compatible_pairs
            or not incompatible_pairs
            or set(worker_revisions.values()) != {1, 2}
            or set(client_revisions.values()) != {1}
        ):
            bad.add("@topology:revision-skew")
        for worker, revision in worker_revisions.items():
            worker_logins = login_for(worker)
            if (
                len(worker_logins) != 1
                or worker_logins[0].get("cache_protocol") != revision
                or selected_profile
                not in (worker_logins[0].get("cache_profiles") or [])
            ):
                bad.add(f"@instance:{worker}")
        compatible_rows = []
        incompatible_rows = []
        for row in rows:
            identifier = _job_id(row.get("job_id"), "@row")
            pair = (row.get("client_instance"), row.get("cs"))
            if pair in compatible_pairs:
                compatible_rows.append(row)
                if (
                    row.get("tail_present") is not True
                    or row.get("tail_profile") != selected_profile
                    or row.get("session_outcome") != "committed"
                    or row.get("retries") != 0
                ):
                    bad.add(identifier)
            elif pair in incompatible_pairs:
                incompatible_rows.append(row)
                if (
                    row.get("tail_present") is not False
                    or row.get("tail_profile") is not None
                    or row.get("session_outcome") != "none"
                    or row.get("reuse") is not None
                    or row.get("retries") != 0
                ):
                    bad.add(identifier)
            else:
                bad.add(identifier)
        if not compatible_rows:
            bad.add("@rows:compatible-revision")
        if not incompatible_rows:
            bad.add("@rows:incompatible-revision")
        preference = observations.get("assignment_preference")
        preference_bad = _assignment_preference_errors(preference, scenario, topology)
        bad.update(preference_bad)
        decisions = preference.get("decisions") if isinstance(preference, Mapping) else None
        if not isinstance(decisions, list) or not any(
            isinstance(item, Mapping)
            and item.get("preferred") is True
            and (item.get("client"), item.get("worker")) in compatible_pairs
            for item in decisions
        ):
            bad.add("@assignment-preference:compatible")
        if not isinstance(decisions, list) or not any(
            isinstance(item, Mapping)
            and item.get("escape") is True
            and (item.get("client"), item.get("worker")) in incompatible_pairs
            for item in decisions
        ):
            bad.add("@assignment-preference:incompatible-escape")
        if observations.get("wire_revision_mismatches", []) != []:
            bad.add("@observations:wire_revision_mismatches")
        if observations.get("error106_job_ids") != []:
            bad.add("@observations:error106_job_ids")
        if observations.get("local_fallback_job_ids") != []:
            bad.add("@observations:local_fallback_job_ids")
        for worker, revision in worker_revisions.items():
            sessions = sidecar(worker).get("sessions")
            if revision == 1 and not _is_int(sessions, minimum=1):
                bad.add(f"@instance:{worker}:sessions")
            if revision == 2 and sessions != 0:
                bad.add(f"@instance:{worker}:sessions")
        clauses.append(
            _clause(
                "shape.revision-skew-routing",
                not bad,
                "scheduler retains both revisions, prefers the compatible worker, and uses no-tail remote legacy on a saturated incompatible escape",
                bad,
            )
        )
    if scenario.get("id") == "S90-revision-refusal-retry":
        bad: set[str] = set()
        workload = scenario.get("workload")
        named = {
            item.get("name"): item
            for item in instances
            if isinstance(item, Mapping) and isinstance(item.get("name"), str)
        }
        if (
            shape != "S'[F~][C']"
            or schedulers != {"S1"}
            or workers != {"F1"}
            or clients != {"C1"}
            or set(named) != {"S1", "F1", "C1"}
            or named["F1"].get("image") != "mutant"
            or named["F1"].get("slots") != 1
            or named["F1"].get("env", {}).get(
                "ICECC_P50_S90_ENDPOINT_WIRE_REVISION"
            )
            != "2"
            or scenario.get("timeline") != []
            or not isinstance(workload, Mapping)
            or workload.get("driver") != "tu-manifest"
            or workload.get("corpus") != "fmt-100"
            or workload.get("turns") != ["A"]
            or workload.get("jobs") != 1
            or workload.get("clients") != ["C1"]
            or workload.get("repeat") != 1
            or scenario.get("expect", {}).get("engagement")
            != S90_REVISION_REFUSAL_ENGAGEMENT
        ):
            bad.add("@scenario:s90-revision-refusal")
        f_logins = login_for("F1")
        revisions = observations.get("wire_revisions")
        if (
            len(f_logins) != 1
            or f_logins[0].get("cache_protocol") != 1
            or selected_profile not in (f_logins[0].get("cache_profiles") or [])
            or not isinstance(revisions, Mapping)
            or revisions.get("F1") != 1
            or revisions.get("C1") != 1
            or sidecar("F1").get("sessions") != 0
        ):
            bad.add("@instance:F1")
        mismatch_raw = observations.get("wire_revision_mismatches")
        mismatch_ids: set[str] = set()
        required_record_fields = {
            "advertised_worker_wire_revision",
            "client_instance",
            "client_wire_revision",
            "endpoint_wire_revision",
            "error",
            "error_code",
            "first_assignment",
            "retry_assignment",
            "row_job_id",
            "schema",
            "worker_instance",
        }
        if not isinstance(mismatch_raw, list) or not mismatch_raw:
            bad.add("@observations:wire_revision_mismatches")
        else:
            for index, record in enumerate(mismatch_raw):
                marker = f"@revision-mismatch:{index}"
                if not isinstance(record, Mapping) or set(record) != required_record_fields:
                    bad.add(marker)
                    continue
                first = record.get("first_assignment")
                retry = record.get("retry_assignment")
                identity_fields = {
                    "assignment_epoch", "assignment_nonce", "scheduler_job"
                }
                if (
                    record.get("schema") != "icefarm-wire-revision-mismatch-v1"
                    or record.get("error") != "WIRE_REVISION_MISMATCH"
                    or record.get("error_code") != 4
                    or record.get("client_instance") != "C1"
                    or record.get("worker_instance") != "F1"
                    or record.get("client_wire_revision") != 1
                    or record.get("advertised_worker_wire_revision") != 1
                    or record.get("endpoint_wire_revision") != 2
                    or not isinstance(first, Mapping)
                    or set(first) != identity_fields
                    or not isinstance(retry, Mapping)
                    or set(retry) != identity_fields
                    or any(
                        not _is_int(identity.get(field), minimum=1)
                        for identity in (first, retry)
                        for field in identity_fields
                    )
                    or first.get("scheduler_job") == retry.get("scheduler_job")
                    or (
                        first.get("assignment_epoch"),
                        first.get("assignment_nonce"),
                    )
                    == (
                        retry.get("assignment_epoch"),
                        retry.get("assignment_nonce"),
                    )
                    or not isinstance(record.get("row_job_id"), str)
                    or not record["row_job_id"]
                ):
                    bad.add(marker)
                    continue
                mismatch_ids.add(_job_id(record["row_job_id"], marker))
        row_ids = {_job_id(row.get("job_id"), "@row") for row in rows}
        fallback_ids = {
            _job_id(row.get("job_id"), "@row")
            for row in rows
            if row.get("session_outcome") == "fallback"
        }
        error106_raw = observations.get("error106_job_ids")
        local_raw = observations.get("local_fallback_job_ids")
        legacy = observations.get("legacy_wire")
        legacy_records = legacy.get("records") if isinstance(legacy, Mapping) else None
        legacy_ids = {
            _job_id(item.get("job_id"), "@legacy")
            for item in legacy_records
            if isinstance(item, Mapping)
        } if isinstance(legacy_records, list) else set()
        if mismatch_ids != fallback_ids or not mismatch_ids or mismatch_ids - row_ids:
            bad.add("@observations:wire_revision_mismatch_binding")
        error106_ids = (
            {_job_id(item, "@error106") for item in error106_raw}
            if isinstance(error106_raw, list)
            else set()
        )
        if (
            not isinstance(error106_raw, list)
            or len(error106_ids) != len(error106_raw)
            or error106_ids != mismatch_ids
        ):
            bad.add("@observations:error106_job_ids")
        if local_raw != []:
            bad.add("@observations:local_fallback_job_ids")
        if (
            not isinstance(legacy, Mapping)
            or legacy.get("record_count") != len(rows)
            or legacy_ids != row_ids
        ):
            bad.add("@observations:legacy_wire")
        clauses.append(
            _clause(
                "s90.named-refusal-fresh-remote-retry",
                not bad,
                "a real wire-revision mismatch names error 4 and each affected tail receives one distinct remote legacy assignment",
                bad,
            )
        )
    return clauses


def _network_shaping_errors(
    scenario: Mapping[str, Any], observations: Mapping[str, Any], plan: object
) -> set[str]:
    requests = scenario.get("network", {}).get("shaping")
    observed = observations.get("network_shaping")
    plan_network = plan.get("network_shaping") if isinstance(plan, Mapping) else None
    plan_bindings = (
        plan_network.get("bindings") if isinstance(plan_network, Mapping) else None
    )
    if not requests:
        if observed is not None or plan_bindings:
            return {"@observations:network_shaping:unexpected"}
        return set()
    if not isinstance(plan, Mapping):
        return {"@plan:network_shaping"}
    try:
        validate_netem_receipt(scenario, plan, observed)
    except NetemPlanError:
        return {"@observations:network_shaping"}
    return set()


def _f_init_errors(bundle: Mapping[str, Any]) -> set[str]:
    """Require retained, typed Init=true inspection evidence for every F."""

    if bundle.get("mode") == "preflight-refusal":
        return set()
    topology = bundle.get("topology")
    if not isinstance(topology, Mapping):
        plan = bundle.get("plan")
        topology = plan.get("topology") if isinstance(plan, Mapping) else None
    instances = topology.get("instances") if isinstance(topology, Mapping) else None
    if not isinstance(instances, list):
        return set()
    expected = {
        item.get("name"): item.get("host")
        for item in instances
        if isinstance(item, Mapping) and item.get("role") == "F"
    }
    raw = bundle.get("observations", {}).get("f_init")
    if not isinstance(raw, Mapping) or set(raw) != {"instances", "schema"}:
        return {"@observations:f_init"}
    if raw.get("schema") != F_INIT_SCHEMA or not isinstance(raw.get("instances"), list):
        return {"@observations:f_init"}
    errors: set[str] = set()
    seen: set[str] = set()
    for item in raw["instances"]:
        if not isinstance(item, Mapping) or set(item) != {
            "host",
            "init",
            "inspect_sha256",
            "instance",
        }:
            errors.add("@observations:f_init")
            continue
        name = item.get("instance")
        if (
            not isinstance(name, str)
            or name in seen
            or name not in expected
            or item.get("host") != expected.get(name)
            or item.get("init") is not True
            or not isinstance(item.get("inspect_sha256"), str)
            or SHA256_RE.fullmatch(item["inspect_sha256"]) is None
        ):
            errors.add(f"@observations:f_init:{name or 'unknown'}")
        else:
            seen.add(name)
    if seen != set(expected):
        errors.add("@observations:f_init")
    return errors


def _launch_contract(bundle: Mapping[str, Any]) -> tuple[str, str | None]:
    """Select current, transition, or the exact pre-init launch boundary."""

    plan = bundle.get("plan")
    if not isinstance(plan, Mapping):
        # Small pure-verdict fixtures have no launch plan; their topology is
        # already an explicit current-contract fixture.
        return F_INIT_LAUNCH_CONTRACT, None
    declared = plan.get("launch_contract")
    bundle_declared = bundle.get("launch_contract")
    if declared == F_INIT_LAUNCH_CONTRACT and bundle_declared == F_INIT_LAUNCH_CONTRACT:
        return F_INIT_LAUNCH_CONTRACT, None
    if declared is not None or bundle_declared is not None:
        return F_INIT_LAUNCH_CONTRACT, "launch contract marker is invalid"
    commands = plan.get("commands")
    if not isinstance(commands, list):
        return F_INIT_LAUNCH_CONTRACT, "launch command contract is absent"
    f_starts = [
        command
        for command in commands
        if isinstance(command, Mapping)
        and command.get("phase") == "up.start-f"
    ]
    if f_starts and all(
        isinstance(command.get("argv"), list)
        and "--init" not in command["argv"]
        for command in f_starts
    ):
        return HISTORICAL_LAUNCH_CONTRACT, None
    if not f_starts:
        return HISTORICAL_LAUNCH_CONTRACT, None
    if all(
        isinstance(command.get("argv"), list)
        and "--init" in command["argv"]
        for command in f_starts
    ):
        return TRANSITION_LAUNCH_CONTRACT, None
    return TRANSITION_LAUNCH_CONTRACT, "mixed F launch contract is ambiguous"


def _has_authenticated_f(bundle: Mapping[str, Any]) -> bool:
    topology = bundle.get("topology")
    if not isinstance(topology, Mapping):
        plan = bundle.get("plan")
        topology = plan.get("topology") if isinstance(plan, Mapping) else None
    instances = topology.get("instances") if isinstance(topology, Mapping) else None
    return isinstance(instances, list) and any(
        isinstance(item, Mapping) and item.get("role") == "F" for item in instances
    )


def evaluate_bundle(bundle: Mapping[str, Any]) -> dict[str, Any]:
    """Evaluate an already-loaded bundle without consulting external state."""

    clauses: list[dict[str, Any]] = []
    if not isinstance(bundle, Mapping):
        clauses.append(
            _clause("bundle.schema", False, "bundle is not an object", {"@bundle"})
        )
        return _finish(clauses)
    scenario = bundle.get("scenario")
    observations = bundle.get("observations")
    rows_raw = bundle.get("rows")
    bundle_ok = (
        bundle.get("schema") == BUNDLE_SCHEMA
        and isinstance(scenario, Mapping)
        and isinstance(observations, Mapping)
        and isinstance(rows_raw, list)
    )
    clauses.append(
        _clause(
            "bundle.schema",
            bundle_ok,
            "bundle, scenario, observations, and rows use the acceptance contract",
            () if bundle_ok else {"@bundle"},
        )
    )
    if not bundle_ok:
        return _finish(clauses)

    launch_contract, launch_contract_error = _launch_contract(bundle)
    has_launch_data = isinstance(bundle.get("topology"), Mapping) or isinstance(
        bundle.get("plan"), Mapping
    )
    if has_launch_data and (
        launch_contract == F_INIT_LAUNCH_CONTRACT
        or launch_contract_error is not None
    ):
        clauses.append(
            _clause(
                "launch.contract",
                launch_contract_error is None,
                "current F Docker --init launch contract is authenticated",
                () if launch_contract_error is None else {"@plan:launch_contract"},
            )
        )
    if has_launch_data and launch_contract in (
        F_INIT_LAUNCH_CONTRACT,
        TRANSITION_LAUNCH_CONTRACT,
    ) and _has_authenticated_f(bundle):
        f_init_errors = _f_init_errors(bundle)
        clauses.append(
            _clause(
                "launch.f-init",
                not f_init_errors,
                "every F retained Docker inspect proves HostConfig.Init=true",
                f_init_errors,
            )
        )

    network_relevant = bool(scenario.get("network", {}).get("shaping")) or (
        "network_shaping" in observations
    )
    plan = bundle.get("plan")
    if isinstance(plan, Mapping):
        plan_network = plan.get("network_shaping")
        if isinstance(plan_network, Mapping):
            network_relevant = network_relevant or bool(plan_network.get("bindings"))
    if network_relevant:
        network_errors = _network_shaping_errors(scenario, observations, plan)
        clauses.append(
            _clause(
                "network.shaping",
                not network_errors,
                "the shaped F has one immutable F-egress 100mbit/2ms bridge qdisc witness",
                network_errors,
            )
        )

    rows_present = bool(rows_raw)
    clauses.append(
        _clause(
            "rows.present",
            rows_present,
            "at least one dispatched job row is present",
            () if rows_present else {"@rows"},
        )
    )
    row_issues: dict[str, list[str]] = {}
    valid_rows: list[Mapping[str, Any]] = []
    for index, row in enumerate(rows_raw):
        identifier = _job_id(
            row.get("job_id") if isinstance(row, Mapping) else None, f"@row:{index}"
        )
        errors = _row_errors(row)
        if errors:
            row_issues[identifier] = errors
        elif isinstance(row, Mapping):
            valid_rows.append(row)
    clauses.append(
        _clause(
            "rows.schema",
            not row_issues,
            "every row has the exact typed acceptance-v1 field set"
            if not row_issues
            else "; ".join(
                f"{job}: {', '.join(errors)}"
                for job, errors in sorted(row_issues.items())
            ),
            set(row_issues),
        )
    )
    if row_issues or not rows_present:
        return _finish(clauses)

    identifiers = [_job_id(row["job_id"], "@row") for row in valid_rows]
    duplicates = {
        identifier for identifier, count in Counter(identifiers).items() if count > 1
    }
    clauses.append(
        _clause(
            "rows.unique",
            not duplicates,
            "job ids are unique",
            duplicates,
        )
    )
    selected_profile, profile_error = _scenario_profile(scenario)
    s30_mutant = scenario.get("id") == "S30-mutant-f-refusal"
    clauses.append(
        _clause(
            "scenario.profile",
            profile_error is None,
            "scenario resolves one product profile"
            if profile_error is None
            else profile_error,
            () if profile_error is None else {"@scenario:profile"},
        )
    )

    revisions = observations.get("wire_revisions")
    p50_instances = {
        str(row[field])
        for row in valid_rows
        for version_field, field in (
            ("client_version", "client_instance"),
            ("cs_version", "cs"),
        )
        if row["tail_present"] is True and row[version_field] == 50
    }
    revision_bad = {
        f"@instance:{instance}"
        for instance in p50_instances
        if not isinstance(revisions, Mapping)
        or not _is_int(revisions.get(instance), minimum=1)
    }
    clauses.append(
        _clause(
            "observations.wire-revisions",
            not revision_bad,
            "every Protocol-50 endpoint is bound to a positive wire revision",
            revision_bad,
        )
    )

    identity_bad = {
        _job_id(row["job_id"], "@row")
        for row in valid_rows
        if row["object_sha_remote"] != row["object_sha_local"]
        or row["exact"] != (row["object_sha_remote"] == row["object_sha_local"])
    }
    clauses.append(
        _clause(
            "objects.identity",
            not identity_bad,
            "remote and local object SHA-256 values agree with the exact flag",
            identity_bad,
        )
    )

    for field, identifier, detail in (
        (
            "compile_failure_job_ids",
            "jobs.compile-success",
            "every workload compiler process exits successfully",
        ),
        (
            "local_fallback_job_ids",
            "jobs.remote-only",
            "every workload object is produced by an assigned farm worker",
        ),
    ):
        raw_failures = observations.get(field)
        failures = (
            {_job_id(item, f"@observations:{field}") for item in raw_failures}
            if isinstance(raw_failures, list)
            else {f"@observations:{field}"}
        )
        clauses.append(
            _clause(
                identifier,
                isinstance(raw_failures, list) and not raw_failures,
                detail,
                failures,
            )
        )

    expect = scenario.get("expect")
    if not isinstance(expect, Mapping):
        clauses.append(
            _clause(
                "scenario.expect",
                False,
                "scenario.expect is invalid",
                {"@scenario:expect"},
            )
        )
        return _finish(clauses)
    exact_bad: set[str] = set()
    if expect.get("exact") == "all":
        exact_bad = {
            _job_id(row["job_id"], "@row")
            for row in valid_rows
            if row["exact"] is not True
        }
    elif expect.get("exact") == "none":
        exact_bad = {
            _job_id(row["job_id"], "@row")
            for row in valid_rows
            if row["exact"] is not False
        }
    clauses.append(
        _clause(
            "exact",
            not exact_bad,
            f"exactness expectation is {expect.get('exact')!r}",
            exact_bad,
        )
    )

    coherence_bad = {
        _job_id(row["job_id"], "@row")
        for row in valid_rows
        if row["tail_present"] != (row["tail_profile"] is not None)
    }
    clauses.append(
        _clause(
            "tail.profile-coherence",
            not coherence_bad,
            "tail presence and profile are coherent",
            coherence_bad,
        )
    )

    raw_recovered = observations.get("process_loss_recovery_job_ids")
    recovered_ids = (
        {
            _job_id(item, "@observations:process_loss_recovery_job_ids")
            for item in raw_recovered
        }
        if isinstance(raw_recovered, list)
        else set()
    )

    def authenticated_recovery_legacy(row: Mapping[str, Any]) -> bool:
        return (
            _job_id(row["job_id"], "@row") in recovered_ids
            and row["retries"] >= 1
            and row["tail_present"] is False
            and row["tail_profile"] is None
            and row["session_outcome"] in {"none", "fallback"}
        )

    engagement_mode = expect.get("engagement")
    engagement_bad: set[str] = set()
    if engagement_mode == "expected(c,f)" and profile_error is None:
        for row in valid_rows:
            row_profile, row_profile_error = _scenario_profile_at_epoch(
                scenario, row["event_epoch"]
            )
            expected = expected_profile(
                row,
                row_profile,
                revisions if isinstance(revisions, Mapping) else {},
            )
            if (
                row_profile_error is not None
                or row["tail_profile"] != expected
                and not authenticated_recovery_legacy(row)
                and not (
                    s30_mutant
                    and row["tail_present"] is False
                    and row["session_outcome"] in {"fallback", "none"}
                )
            ):
                engagement_bad.add(_job_id(row["job_id"], "@row"))
    elif engagement_mode == S70_B4_WORKER_ENGAGEMENT:
        epochs = {
            epoch: [row for row in valid_rows if row["event_epoch"] == epoch]
            for epoch in range(4)
        }
        for epoch, epoch_rows in epochs.items():
            if not epoch_rows:
                engagement_bad.add(f"@rows:epoch-{epoch}")
        recovered_raw = observations.get("process_loss_recovery_job_ids")
        recovered = (
            {_job_id(item, "@recovery") for item in recovered_raw}
            if isinstance(recovered_raw, list)
            else set()
        )
        if (
            not isinstance(recovered_raw, list)
            or len(recovered_raw) != len(recovered)
        ):
            engagement_bad.add("@observations:process_loss_recovery_job_ids")
        for row in valid_rows:
            identifier = _job_id(row["job_id"], "@row")
            if identifier in recovered:
                valid = (
                    row["tail_present"] is False
                    and row["tail_profile"] is None
                    and row["session_outcome"] in {"none", "fallback"}
                    and row["reuse"] is None
                    and row["retries"] == 1
                )
            else:
                valid = (
                    row["tail_present"] is True
                    and row["tail_profile"] == "P29V1"
                    and row["session_outcome"] == "committed"
                    and row["retries"] == 0
                )
            if row["event_epoch"] not in epochs or not valid:
                engagement_bad.add(identifier)
        error106_raw = observations.get("error106_job_ids")
        error106_ids = (
            {_job_id(item, "@error106") for item in error106_raw}
            if isinstance(error106_raw, list)
            else {"@observations:error106_job_ids"}
        )
        if not isinstance(error106_raw, list) or not error106_ids <= recovered:
            engagement_bad.update(error106_ids - recovered)
    elif engagement_mode == S70_B4_SCHEDULER_ENGAGEMENT:
        epochs = {
            epoch: [row for row in valid_rows if row["event_epoch"] == epoch]
            for epoch in range(2)
        }
        for epoch, epoch_rows in epochs.items():
            if not epoch_rows:
                engagement_bad.add(f"@rows:epoch-{epoch}")
        for row in valid_rows:
            if (
                row["event_epoch"] not in epochs
                or row["tail_present"] is not True
                or row["tail_profile"] != "P29V1"
                or row["session_outcome"] != "committed"
                or row["retries"] != 0
            ):
                engagement_bad.add(_job_id(row["job_id"], "@row"))
        error106_raw = observations.get("error106_job_ids")
        if not isinstance(error106_raw, list) or error106_raw:
            engagement_bad.add("@observations:error106_job_ids")
    elif engagement_mode == S70_B4_ACTIVE_LOSS_ENGAGEMENT:
        lifecycle = observations.get("assignment_lifecycle")
        event_log = bundle.get("event_log")
        receipt = event_log[0].get("receipt") if isinstance(event_log, list) and event_log else None
        lost_job = receipt.get("lost_scheduler_job") if isinstance(receipt, Mapping) else None
        lost_generation = receipt.get("lost_scheduler_generation") if isinstance(receipt, Mapping) else None
        affected_ids = {
            item.get("job_id") for item in lifecycle
            if isinstance(item, Mapping) and isinstance(item.get("attempts"), list)
            and item["attempts"]
            and item["attempts"][0].get("scheduler_job") == lost_job
            and item["attempts"][0].get("generation") == lost_generation
        } if isinstance(lifecycle, list) else set()
        fallback = [
            row for row in valid_rows
            if row["session_outcome"] == "fallback"
            and row["tail_present"] is False and row["tail_profile"] is None
            and row["retries"] == 1 and row["exact"] is True
            and row["job_id"] in affected_ids
        ]
        if len(affected_ids) != 1 or len(fallback) != 1:
            engagement_bad.add("@rows:exactly-one-active-loss-fallback")
        if not any(
            row["event_epoch"] == 1 and row["tail_present"] is True
            and row["tail_profile"] == "P29V1" and row["session_outcome"] == "committed"
            and row["retries"] == 0 for row in valid_rows
        ):
            engagement_bad.add("@rows:post-active-loss-p29")
        for row in valid_rows:
            if row in fallback:
                continue
            if not (row["tail_present"] is True and row["tail_profile"] == "P29V1"
                    and row["session_outcome"] == "committed" and row["retries"] == 0):
                engagement_bad.add(_job_id(row["job_id"], "@row"))
        if observations.get("local_fallback_job_ids") != []:
            engagement_bad.add("@observations:local_fallback_job_ids")
    elif engagement_mode == S70_B4_CLIENT_ENGAGEMENT:
        epochs = {
            epoch: [row for row in valid_rows if row["event_epoch"] == epoch]
            for epoch in range(2)
        }
        for epoch, epoch_rows in epochs.items():
            if not epoch_rows:
                engagement_bad.add(f"@rows:epoch-{epoch}")
        for row in valid_rows:
            if (
                row["event_epoch"] not in epochs
                or row["tail_present"] is not True
                or row["tail_profile"] != "P29V1"
                or row["session_outcome"] != "committed"
                or row["retries"] != 0
            ):
                engagement_bad.add(_job_id(row["job_id"], "@row"))
        error106_raw = observations.get("error106_job_ids")
        if not isinstance(error106_raw, list) or error106_raw:
            engagement_bad.add("@observations:error106_job_ids")
    elif engagement_mode == S70_B5_ENGAGEMENT:
        pre_event = [row for row in valid_rows if row["event_epoch"] == 0]
        post_event = [row for row in valid_rows if row["event_epoch"] == 1]
        error106_raw = observations.get("error106_job_ids")
        error106_ids = (
            {_job_id(item, "@error106") for item in error106_raw}
            if isinstance(error106_raw, list)
            else {"@observations:error106_job_ids"}
        )
        controlled = [
            row
            for row in post_event
            if _job_id(row["job_id"], "@row") in error106_ids
        ]
        if not pre_event:
            engagement_bad.add("@rows:pre-event")
        if not post_event:
            engagement_bad.add("@rows:post-event")
        for row in pre_event:
            if (
                row["tail_profile"] != "P29V1"
                or row["tail_present"] is not True
                or row["session_outcome"] != "committed"
                or row["retries"] != 0
            ):
                engagement_bad.add(_job_id(row["job_id"], "@row"))
        if (
            not isinstance(error106_raw, list)
            or len(error106_raw) != 1
            or len(error106_ids) != 1
            or len(controlled) != 1
        ):
            engagement_bad.update(error106_ids or {"@observations:error106_job_ids"})
        else:
            failure = controlled[0]
            if (
                failure["tail_present"] is not False
                or failure["tail_profile"] is not None
                or failure["session_outcome"] != "none"
                or failure["retries"] != 1
                or failure["exact"] is not True
            ):
                engagement_bad.add(_job_id(failure["job_id"], "@row"))
        later = [
            row
            for row in post_event
            if not controlled or row is not controlled[0]
        ]
        if not later:
            engagement_bad.add("@rows:post-failure-zstd-tu")
        for row in later:
            if (
                row["tail_profile"] != "ZSTD_TU"
                or row["tail_present"] is not True
                or row["session_outcome"] != "committed"
                or row["retries"] != 0
                or row["reuse"] is not None
            ):
                engagement_bad.add(_job_id(row["job_id"], "@row"))
        engagement_bad.update(
            _job_id(row["job_id"], "@row")
            for row in valid_rows
            if row["event_epoch"] not in {0, 1}
        )
        lifecycle = observations.get("job_lifecycle")
        dispatches = {
            _job_id(item.get("job_id"), "@lifecycle"): _lifecycle_final_dispatch_ms(item)
            for item in lifecycle
            if isinstance(item, Mapping)
        } if isinstance(lifecycle, list) else {}
        if len(controlled) == 1:
            failure_id = _job_id(controlled[0]["job_id"], "@row")
            failure_dispatch = dispatches.get(failure_id)
            for row in later:
                identifier = _job_id(row["job_id"], "@row")
                if (
                    not _is_int(failure_dispatch)
                    or not _is_int(dispatches.get(identifier))
                    or dispatches[identifier] <= failure_dispatch
                ):
                    engagement_bad.add(identifier)
    elif engagement_mode == S70_B6_ENGAGEMENT:
        epochs = {
            epoch: [row for row in valid_rows if row["event_epoch"] == epoch]
            for epoch in range(3)
        }
        for epoch, epoch_rows in epochs.items():
            if not epoch_rows:
                engagement_bad.add(f"@rows:epoch-{epoch}")
        engagement_bad.update(
            _job_id(row["job_id"], "@row")
            for row in valid_rows
            if row["event_epoch"] not in epochs
        )
        for epoch in (0, 2):
            for row in epochs[epoch]:
                if (
                    row["tail_present"] is not True
                    or row["tail_profile"] != "P29V1"
                    or row["session_outcome"] != "committed"
                    or row["retries"] != 0
                ):
                    engagement_bad.add(_job_id(row["job_id"], "@row"))
        for row in epochs[1]:
            if (
                row["tail_present"] is not False
                or row["tail_profile"] is not None
                or row["session_outcome"] != "none"
                or row["reuse"] is not None
                or row["retries"] != 0
            ):
                engagement_bad.add(_job_id(row["job_id"], "@row"))
        error106_raw = observations.get("error106_job_ids")
        if not isinstance(error106_raw, list) or error106_raw:
            engagement_bad.add("@observations:error106_job_ids")
    elif engagement_mode in {
        S70_B7_ROLLBACK_ENGAGEMENT,
        S70_B7_ROLLFORWARD_ENGAGEMENT,
    }:
        epochs = {
            epoch: [row for row in valid_rows if row["event_epoch"] == epoch]
            for epoch in range(4)
        }
        for epoch, epoch_rows in epochs.items():
            if not epoch_rows:
                engagement_bad.add(f"@rows:epoch-{epoch}")
        engagement_bad.update(
            _job_id(row["job_id"], "@row")
            for row in valid_rows
            if row["event_epoch"] not in epochs
        )
        rollback_versions = {
            0: (50, 50, True),
            1: (50, 50, False),
            2: (50, 43, False),
            3: (43, 43, False),
        }
        rollforward_versions = {
            0: (43, 43, False),
            1: (43, 43, False),
            2: (43, 50, False),
            3: (50, 50, True),
        }
        expected_versions = (
            rollback_versions
            if engagement_mode == S70_B7_ROLLBACK_ENGAGEMENT
            else rollforward_versions
        )
        for epoch, epoch_rows in epochs.items():
            client_version, worker_version, p29 = expected_versions[epoch]
            for row in epoch_rows:
                valid = (
                    row["client_version"] == client_version
                    and row["cs_version"] == worker_version
                    and row["retries"] == 0
                    and (
                        (
                            row["tail_present"] is True
                            and row["tail_profile"] == "P29V1"
                            and row["session_outcome"] == "committed"
                        )
                        if p29
                        else (
                            row["tail_present"] is False
                            and row["tail_profile"] is None
                            and row["session_outcome"] == "none"
                            and row["reuse"] is None
                        )
                    )
                )
                if not valid:
                    engagement_bad.add(_job_id(row["job_id"], "@row"))
        error106_raw = observations.get("error106_job_ids")
        if not isinstance(error106_raw, list) or error106_raw:
            engagement_bad.add("@observations:error106_job_ids")
    elif engagement_mode == S95_DISK_FILL_ENGAGEMENT:
        pre_event = [row for row in valid_rows if row["event_epoch"] == 0]
        post_event = [row for row in valid_rows if row["event_epoch"] == 1]
        if not pre_event:
            engagement_bad.add("@rows:s95-pre-event")
        if not post_event:
            engagement_bad.add("@rows:s95-post-event")
        engagement_bad.update(
            _job_id(row["job_id"], "@row")
            for row in valid_rows
            if row["event_epoch"] not in {0, 1}
        )
        for row in pre_event:
            if (
                row["tail_present"] is not True
                or row["tail_profile"] != "P29V1"
                or row["session_outcome"] != "committed"
                or row["retries"] != 0
            ):
                engagement_bad.add(_job_id(row["job_id"], "@row"))
        affected_post = []
        fallback_ids: set[str] = set()
        for row in post_event:
            identifier = _job_id(row["job_id"], "@row")
            if row["cs"] == "F1":
                affected_post.append(row)
            committed = (
                row["tail_present"] is True
                and row["tail_profile"] == "P29V1"
                and row["session_outcome"] == "committed"
                and row["retries"] == 0
            )
            fallback = (
                row["tail_present"] is False
                and row["tail_profile"] is None
                and row["session_outcome"] in {"none", "fallback"}
                and row["reuse"] is None
                and row["retries"] == 1
            )
            if fallback:
                fallback_ids.add(identifier)
            if not (committed or fallback):
                engagement_bad.add(identifier)
        if not affected_post:
            engagement_bad.add("@rows:s95-affected-worker-post-event")
        error106_raw = observations.get("error106_job_ids")
        error106_ids = (
            {_job_id(item, "@error106") for item in error106_raw}
            if isinstance(error106_raw, list)
            else {"@observations:error106_job_ids"}
        )
        assignment_raw = observations.get("assignment_lifecycle")
        assignment_by_job = {
            item.get("job_id"): item
            for item in assignment_raw
            if isinstance(item, Mapping)
        } if isinstance(assignment_raw, list) else {}
        assignment_bad: set[str] = set()
        row_job_ids = {row["job_id"] for row in valid_rows}
        assignment_identities: list[tuple[int, int]] = []
        for row in valid_rows:
            identifier = _job_id(row["job_id"], "@row")
            record = assignment_by_job.get(row["job_id"])
            attempts = record.get("attempts") if isinstance(record, Mapping) else None
            malformed = (
                not isinstance(attempts, list)
                or len(attempts) != row["retries"] + 1
                or any(
                    not isinstance(attempt, Mapping)
                    or set(attempt) != {"generation", "scheduler_job", "terminal", "worker"}
                    or not _is_int(attempt.get("generation"), minimum=1)
                    or not _is_int(attempt.get("scheduler_job"), minimum=1)
                    or not isinstance(attempt.get("terminal"), str)
                    or not isinstance(attempt.get("worker"), str)
                    for attempt in (attempts if isinstance(attempts, list) else [])
                )
                or len({(item["generation"], item["scheduler_job"]) for item in attempts})
                != len(attempts)
            )
            if malformed:
                assignment_bad.add(identifier)
                continue
            assignment_identities.extend(
                (attempt["generation"], attempt["scheduler_job"])
                for attempt in attempts
            )
            if (
                attempts[-1]["terminal"] != "completion"
                or attempts[-1]["worker"] != row["cs"]
                or (
                    row["retries"] == 0
                    and attempts[0]["terminal"] != "completion"
                )
                or (
                    row["retries"] == 1
                    and (
                        attempts[0]["terminal"] != "cancellation"
                        or attempts[1]["terminal"] != "completion"
                    )
                )
            ):
                assignment_bad.add(identifier)
        if (
            not isinstance(error106_raw, list)
            or len(error106_ids) != len(error106_raw)
            or error106_ids != fallback_ids
            or len(fallback_ids) > len(post_event)
            or len(assignment_by_job) != len(assignment_raw or [])
            or set(assignment_by_job) != row_job_ids
            or len(set(assignment_identities)) != len(assignment_identities)
            or assignment_bad
        ):
            engagement_bad.update(
                assignment_bad
                or (error106_ids - fallback_ids)
                or {"@observations:s95-fallback-bound"}
            )
    elif engagement_mode == S90_REVISION_REFUSAL_ENGAGEMENT:
        mismatch_raw = observations.get("wire_revision_mismatches")
        mismatch_ids = {
            _job_id(item.get("row_job_id"), "@revision-mismatch")
            for item in mismatch_raw
            if isinstance(item, Mapping)
        } if isinstance(mismatch_raw, list) else set()
        if not mismatch_ids:
            engagement_bad.add("@observations:wire_revision_mismatches")
        for row in valid_rows:
            identifier = _job_id(row["job_id"], "@row")
            expected_outcome = "fallback" if identifier in mismatch_ids else "none"
            expected_retries = 1 if identifier in mismatch_ids else 0
            if (
                row["tail_present"] is not False
                or row["tail_profile"] is not None
                or row["session_outcome"] != expected_outcome
                or row["reuse"] is not None
                or row["retries"] != expected_retries
                or row["exact"] is not True
            ):
                engagement_bad.add(identifier)
        error106_raw = observations.get("error106_job_ids")
        error106_ids = (
            {_job_id(item, "@error106") for item in error106_raw}
            if isinstance(error106_raw, list)
            else set()
        )
        if (
            not isinstance(error106_raw, list)
            or len(error106_ids) != len(error106_raw)
            or error106_ids != mismatch_ids
        ):
            engagement_bad.add("@observations:error106_job_ids")
        if observations.get("local_fallback_job_ids") != []:
            engagement_bad.add("@observations:local_fallback_job_ids")
    else:
        engagement_bad.add("@expect:engagement")
    clauses.append(
        _clause(
            "engagement.expected",
            not engagement_bad,
            "each row matches the configured engagement law",
            engagement_bad,
        )
    )
    s60_epoch_bad = _s60_transition_epoch_errors(
        scenario, valid_rows, observations, bundle.get("event_log")
    )
    if isinstance(scenario.get("id"), str) and scenario["id"].startswith("S60-"):
        clauses.append(
            _clause(
                "s60.transition-pair-law",
                not s60_epoch_bad,
                "S60 rows cross the authenticated dispatch boundary and only the transitioned endpoint changes generation",
                s60_epoch_bad,
            )
        )

    if engagement_mode == S70_B4_WORKER_ENGAGEMENT:
        b4_bad: set[str] = set()
        authenticated_restart_losses: set[tuple[str, int]] = set()
        instances = scenario.get("instances")
        instances = instances if isinstance(instances, list) else []
        workers = [
            item
            for item in instances
            if isinstance(item, Mapping) and item.get("role") == "F"
        ]
        target = next(
            (item for item in workers if item.get("name") == "F1"), None
        )
        other_workers = {
            str(item["name"])
            for item in workers
            if item.get("name") != "F1" and isinstance(item.get("name"), str)
        }
        workload = scenario.get("workload")
        expected_timeline = [
            {"trigger": f"t+{seconds}", "action": "restart", "instance": "F1"}
            for seconds in (30, 60, 90)
        ]
        if (
            not isinstance(target, Mapping)
            or len(workers) != 2
            or len(other_workers) != 1
            or scenario.get("timeline") != expected_timeline
            or not isinstance(workload, Mapping)
            or workload.get("corpus") != "firefox-1000"
            or workload.get("turns") != ["A"]
            or workload.get("jobs") != 36
            or workload.get("repeat") != 6
            or not isinstance(workload.get("clients"), list)
            or len(workload["clients"]) != 1
            or any(
                row.get("client_instance") != workload["clients"][0]
                for row in valid_rows
            )
        ):
            b4_bad.add("@scenario:s70-b4-worker")

        event_log = bundle.get("event_log")
        observed_events = event_log if isinstance(event_log, list) else []
        fired: list[int] = []
        if len(observed_events) != 3:
            b4_bad.add("@event:s70-b4-worker-count")
        else:
            previous_after: Mapping[str, Any] | None = None
            for index, (observed_event, expected_event) in enumerate(
                zip(observed_events, expected_timeline, strict=True)
            ):
                if (
                    not isinstance(observed_event, Mapping)
                    or observed_event.get("action") != "restart"
                    or observed_event.get("instance") != "F1"
                    or observed_event.get("trigger") != expected_event["trigger"]
                    or observed_event.get("event_index") != index
                    or observed_event.get("event_epoch") != index + 1
                    or _worker_restart_receipt_errors(
                        observed_event.get("receipt"), observed_event, scenario
                    )
                ):
                    b4_bad.add(f"@event:s70-b4-worker-{index}")
                    continue
                fired_ms = observed_event.get("fired_ms")
                if not _is_int(fired_ms):
                    b4_bad.add(f"@event:s70-b4-worker-{index}")
                    continue
                fired.append(fired_ms)
                receipt = observed_event["receipt"]
                rejoin = receipt["coordination"]["scheduler_rejoin"]
                authenticated_restart_losses.update(
                    ("F1", scheduler_job)
                    for scheduler_job in rejoin["loss_job_ids"]
                )
                before = receipt["before"]
                after = receipt["after"]
                if previous_after is not None and before != previous_after:
                    b4_bad.add(f"@event:s70-b4-worker-chain-{index}")
                previous_after = after
            if len(fired) == 3 and not (fired[0] < fired[1] < fired[2]):
                b4_bad.add("@event:s70-b4-worker-order")

        lifecycle = observations.get("job_lifecycle")
        dispatch_by_job = {
            _job_id(item.get("job_id"), "@lifecycle"): _lifecycle_final_dispatch_ms(item)
            for item in lifecycle
            if isinstance(item, Mapping)
        } if isinstance(lifecycle, list) else {}

        recovered_raw = observations.get("process_loss_recovery_job_ids")
        recovered = (
            {_job_id(item, "@recovery") for item in recovered_raw}
            if isinstance(recovered_raw, list)
            else set()
        )
        bindings_raw = observations.get("process_loss_recovery_bindings")
        bound_jobs: set[str] = set()
        seen_bindings: set[tuple[str, int, int, str]] = set()
        if (
            not isinstance(bindings_raw, list)
            or not isinstance(recovered_raw, list)
            or len(recovered_raw) != len(recovered)
        ):
            b4_bad.add("@observations:process-loss-recovery-bindings")
        else:
            for index, binding in enumerate(bindings_raw):
                marker = f"@observations:process-loss-recovery-binding-{index}"
                if (
                    not isinstance(binding, Mapping)
                    or set(binding)
                    != {"attempt_index", "job_id", "scheduler_job", "worker"}
                    or not _is_int(binding.get("attempt_index"))
                    or not _is_int(binding.get("scheduler_job"), minimum=1)
                    or binding.get("worker") != "F1"
                ):
                    b4_bad.add(marker)
                    continue
                job_id = _job_id(binding.get("job_id"), marker)
                identity = (
                    job_id,
                    binding["attempt_index"],
                    binding["scheduler_job"],
                    binding["worker"],
                )
                if (
                    identity in seen_bindings
                    or (binding["worker"], binding["scheduler_job"])
                    not in authenticated_restart_losses
                ):
                    b4_bad.add(marker)
                seen_bindings.add(identity)
                bound_jobs.add(job_id)
            if bound_jobs != recovered:
                b4_bad.add("@observations:process-loss-recovery-bindings")
        epoch_rows: dict[int, list[Mapping[str, Any]]] = defaultdict(list)
        ordered_target: dict[int, list[tuple[int, Mapping[str, Any]]]] = defaultdict(list)
        if len(fired) == 3:
            for row in valid_rows:
                identifier = _job_id(row["job_id"], "@row")
                dispatch_ms = dispatch_by_job.get(identifier)
                epoch = row["event_epoch"]
                expected_epoch = (
                    sum(boundary <= dispatch_ms for boundary in fired)
                    if _is_int(dispatch_ms)
                    else None
                )
                if expected_epoch != epoch:
                    b4_bad.add(identifier)
                    continue
                epoch_rows[epoch].append(row)
                if row["cs"] == "F1" and row["session_outcome"] == "committed":
                    ordered_target[epoch].append((dispatch_ms, row))

        for epoch in range(4):
            if not epoch_rows[epoch]:
                b4_bad.add(f"@rows:s70-b4-worker-epoch-{epoch}")
        for epoch in (1, 2, 3):
            for worker in other_workers:
                if not any(
                    row["cs"] == worker
                    and row["tail_profile"] == "P29V1"
                    and row["session_outcome"] == "committed"
                    for row in epoch_rows[epoch]
                ):
                    b4_bad.add(f"@worker:{worker}:epoch-{epoch}")

        target_history: dict[str, list[Mapping[str, Any]]] = defaultdict(list)
        for row in epoch_rows[0]:
            if row["cs"] == "F1" and row["session_outcome"] == "committed":
                target_history[str(row["tu"])].append(row)
        warm_route = any(
            len(rows) >= 2
            and min(row["c_to_f_bytes"] for row in rows)
            < max(row["c_to_f_bytes"] for row in rows)
            for rows in target_history.values()
        )
        if not warm_route:
            b4_bad.add("@rows:s70-b4-worker-warm-precondition")
        for epoch in (1, 2, 3):
            candidates = ordered_target[epoch]
            if not candidates:
                b4_bad.add(f"@worker:F1:epoch-{epoch}")
                continue
            first = min(candidates, key=lambda item: item[0])[1]
            references = target_history.get(str(first["tu"]), [])
            cold_bytes = max(
                (
                    (row["c_to_f_bytes"], row["f_to_c_bytes"])
                    for row in references
                ),
                default=None,
            )
            if cold_bytes != (first["c_to_f_bytes"], first["f_to_c_bytes"]):
                b4_bad.add(_job_id(first["job_id"], "@row"))
            for row in epoch_rows[epoch]:
                if row["cs"] == "F1" and row["session_outcome"] == "committed":
                    target_history[str(row["tu"])].append(row)
        clauses.append(
            _clause(
                "s70.b4-worker-bounces",
                not b4_bad,
                "one P50 worker restarts every 30 seconds, rejoins cold, and the other worker keeps committing",
                b4_bad,
            )
        )

    if engagement_mode == S70_B4_SCHEDULER_ENGAGEMENT:
        b4_bad: set[str] = set()
        instances = scenario.get("instances")
        instances = instances if isinstance(instances, list) else []
        schedulers = [
            item
            for item in instances
            if isinstance(item, Mapping) and item.get("role") == "S"
        ]
        scheduler = schedulers[0] if len(schedulers) == 1 else None
        scheduler_name = (
            scheduler.get("name") if isinstance(scheduler, Mapping) else None
        )
        expected_event = {
            "trigger": "job 50",
            "action": "restart",
            "instance": scheduler_name,
        }
        workload = scenario.get("workload")
        if (
            scheduler is None
            or not isinstance(scheduler.get("env"), Mapping)
            or scheduler["env"].get("ICECC_P50_PROFILE") != "P29V1"
            or scenario.get("timeline") != [expected_event]
            or not isinstance(workload, Mapping)
            or workload.get("corpus") != "fmt-100"
            or workload.get("jobs") != 24
            or not isinstance(workload.get("clients"), list)
            or len(workload["clients"]) != 1
            or any(
                row.get("client_instance") != workload["clients"][0]
                for row in valid_rows
            )
        ):
            b4_bad.add("@scenario:s70-b4-scheduler")

        event_log = bundle.get("event_log")
        observed_event = (
            event_log[0]
            if isinstance(event_log, list) and len(event_log) == 1
            else None
        )
        if (
            not isinstance(observed_event, Mapping)
            or observed_event.get("action") != "restart"
            or observed_event.get("instance") != scheduler_name
            or observed_event.get("trigger") != "job 50"
            or observed_event.get("event_index") != 0
            or observed_event.get("event_epoch") != 1
            or not _is_int(observed_event.get("last_dispatched_job"), minimum=1)
            or not _is_int(
                observed_event.get("workload_dispatch_count"), minimum=50
            )
            or _scheduler_restart_receipt_errors(
                observed_event.get("receipt"), observed_event, scenario
            )
        ):
            b4_bad.add("@event:s70-b4-scheduler")
        else:
            receipt = observed_event["receipt"]
            coordination = receipt.get("coordination")
            clients = (
                coordination.get("clients")
                if isinstance(coordination, Mapping)
                else None
            )
            selected_client = (
                workload["clients"][0]
                if isinstance(workload, Mapping)
                and isinstance(workload.get("clients"), list)
                and len(workload["clients"]) == 1
                else None
            )
            pause = (
                clients.get(selected_client)
                if isinstance(clients, Mapping)
                and isinstance(selected_client, str)
                else None
            )
            if (
                not isinstance(pause, Mapping)
                or not _is_int(pause.get("active_before"), minimum=1)
            ):
                b4_bad.add("@event:s70-b4-scheduler-inflight")

            fired_ms = observed_event.get("fired_ms")
            lifecycle = observations.get("job_lifecycle")
            dispatch_by_job = {
                _job_id(item.get("job_id"), "@lifecycle"): _lifecycle_final_dispatch_ms(item)
                for item in lifecycle
                if isinstance(item, Mapping)
            } if isinstance(lifecycle, list) else {}
            for row in valid_rows:
                identifier = _job_id(row["job_id"], "@row")
                dispatch_ms = dispatch_by_job.get(identifier)
                if not (
                    _is_int(fired_ms)
                    and _is_int(dispatch_ms)
                    and (
                        (row["event_epoch"] == 0 and dispatch_ms < fired_ms)
                        or (row["event_epoch"] == 1 and dispatch_ms >= fired_ms)
                    )
                ):
                    b4_bad.add(identifier)
        clauses.append(
            _clause(
                "s70.b4-scheduler-restart",
                not b4_bad,
                "a mid-build scheduler restart drains in-flight work and resumes committed P29 dispatches",
                b4_bad,
            )
        )

    if engagement_mode == S70_B4_ACTIVE_LOSS_ENGAGEMENT:
        active_bad: set[str] = set()
        instances = scenario.get("instances") if isinstance(scenario.get("instances"), list) else []
        schedulers = [item for item in instances if isinstance(item, Mapping) and item.get("role") == "S"]
        scheduler_name = schedulers[0].get("name") if len(schedulers) == 1 else None
        timeline = scenario.get("timeline")
        expected = timeline[0] if isinstance(timeline, list) and len(timeline) == 1 else None
        observed = event_log[0] if isinstance(event_log, list) and len(event_log) == 1 else None
        if (scheduler_name is None or not isinstance(expected, Mapping)
                or expected.get("action") != "scheduler-loss-active"
                or expected.get("instance") != scheduler_name
                or not isinstance(observed, Mapping)
                or observed.get("action") != "scheduler-loss-active"
                or observed.get("instance") != scheduler_name
                or observed.get("event_index") != 0
                or observed.get("event_epoch") != 1
                or not _is_int(observed.get("workload_dispatch_count"), minimum=2)):
            active_bad.add("@event:s70-b4-scheduler-active-loss")
        else:
            receipt = observed.get("receipt")
            if (_scheduler_active_loss_receipt_errors(receipt, observed, scenario)
                    or not isinstance(receipt, Mapping)
                    or not isinstance(receipt.get("before"), Mapping)
                    or not isinstance(receipt.get("after"), Mapping)
                    or receipt["before"].get("container_id") != receipt["after"].get("container_id")
                    or receipt["before"].get("started_at") == receipt["after"].get("started_at")):
                active_bad.add("@event:s70-b4-active-loss-receipt")
        if not any(row.get("event_epoch") == 1 for row in valid_rows):
            active_bad.add("@rows:post-rejoin-dispatch")
        if observations.get("local_fallback_job_ids") != []:
            active_bad.add("@observations:local_fallback_job_ids")
        lifecycle = observations.get("assignment_lifecycle")
        lost_job = receipt.get("lost_scheduler_job") if isinstance(receipt, Mapping) else None
        affected = [
            item for item in lifecycle
            if isinstance(item, Mapping)
            and isinstance(item.get("attempts"), list)
            and item["attempts"]
            and item["attempts"][0].get("scheduler_job") == lost_job
        ] if isinstance(lifecycle, list) else []
        if len(affected) != 1 or len(affected[0]["attempts"]) != 2:
            active_bad.add("@retry:active-loss-boundary")
        elif (affected[0]["attempts"][0].get("scheduler_job") != lost_job
              or affected[0]["attempts"][0].get("generation") != receipt.get("lost_scheduler_generation")
              or affected[0]["attempts"][0].get("terminal") != "scheduler-loss"
              or affected[0]["attempts"][1].get("terminal") != "completion"):
            active_bad.add("@retry:active-loss-terminals")
        if isinstance(lifecycle, list):
            for item in lifecycle:
                if affected and item is affected[0]:
                    continue
                attempts = item.get("attempts") if isinstance(item, Mapping) else None
                if not isinstance(attempts, list) or len(attempts) != 1 or attempts[0].get("terminal") != "completion":
                    active_bad.add("@retry:unexpected-additional-retry")
        clauses.append(_clause(
            "s70.b4-scheduler-active-loss",
            not active_bad,
            "an active compiler group is authenticated, scheduler loss/restart re-joins all peers, and later exact work remains remote",
            active_bad,
        ))

    if engagement_mode == S70_B4_CLIENT_ENGAGEMENT:
        b4_bad: set[str] = set()
        instances = scenario.get("instances")
        instances = instances if isinstance(instances, list) else []
        workload = scenario.get("workload")
        workload_clients = (
            workload.get("clients") if isinstance(workload, Mapping) else None
        )
        target_name = (
            workload_clients[0]
            if isinstance(workload_clients, list) and len(workload_clients) == 1
            else None
        )
        target = next(
            (
                item
                for item in instances
                if isinstance(item, Mapping) and item.get("name") == target_name
            ),
            None,
        )
        expected_event = {
            "trigger": "job 75",
            "action": "restart",
            "instance": target_name,
        }
        if (
            not isinstance(target, Mapping)
            or target.get("role") != "C"
            or not isinstance(target.get("env"), Mapping)
            or target["env"].get("ICECC_P50_MODE") != "on"
            or scenario.get("timeline") != [expected_event]
            or not isinstance(workload, Mapping)
            or workload.get("corpus") != "fmt-100"
            or workload.get("jobs") != 1
            or any(row.get("client_instance") != target_name for row in valid_rows)
        ):
            b4_bad.add("@scenario:s70-b4-client")

        event_log = bundle.get("event_log")
        observed_event = (
            event_log[0]
            if isinstance(event_log, list) and len(event_log) == 1
            else None
        )
        if (
            not isinstance(observed_event, Mapping)
            or observed_event.get("action") != "restart"
            or observed_event.get("instance") != target_name
            or observed_event.get("trigger") != "job 75"
            or observed_event.get("event_index") != 0
            or observed_event.get("event_epoch") != 1
            or not _is_int(observed_event.get("last_dispatched_job"), minimum=1)
            or not _is_int(
                observed_event.get("workload_dispatch_count"), minimum=75
            )
            or _client_route_restart_receipt_errors(
                observed_event.get("receipt"), observed_event, scenario
            )
        ):
            b4_bad.add("@event:s70-b4-client")
        else:
            receipt = observed_event["receipt"]
            coordination = receipt.get("coordination")
            pauses = (
                coordination.get("clients")
                if isinstance(coordination, Mapping)
                else None
            )
            pause = (
                pauses.get(target_name)
                if isinstance(pauses, Mapping) and isinstance(target_name, str)
                else None
            )
            if (
                not isinstance(pause, Mapping)
                or not _is_int(pause.get("active_before"), minimum=1)
            ):
                b4_bad.add("@event:s70-b4-client-inflight")

            lifecycle = observations.get("job_lifecycle")
            dispatch_by_job = {
                _job_id(item.get("job_id"), "@lifecycle"): _lifecycle_final_dispatch_ms(item)
                for item in lifecycle
                if isinstance(item, Mapping)
            } if isinstance(lifecycle, list) else {}
            fired_ms = observed_event.get("fired_ms")
            ordered_rows: list[tuple[int, Mapping[str, Any]]] = []
            for row in valid_rows:
                identifier = _job_id(row["job_id"], "@row")
                dispatch_ms = dispatch_by_job.get(identifier)
                if not (
                    _is_int(fired_ms)
                    and _is_int(dispatch_ms)
                    and (
                        (row["event_epoch"] == 0 and dispatch_ms < fired_ms)
                        or (row["event_epoch"] == 1 and dispatch_ms >= fired_ms)
                    )
                ):
                    b4_bad.add(identifier)
                elif row["event_epoch"] == 1:
                    ordered_rows.append((dispatch_ms, row))

            pre_by_tu: dict[str, list[Mapping[str, Any]]] = defaultdict(list)
            for row in valid_rows:
                if row["event_epoch"] == 0:
                    pre_by_tu[str(row["tu"])].append(row)
            warm_route = any(
                len(tu_rows) >= 2
                and min(row["c_to_f_bytes"] for row in tu_rows)
                < max(row["c_to_f_bytes"] for row in tu_rows)
                for tu_rows in pre_by_tu.values()
            )
            if not warm_route:
                b4_bad.add("@rows:s70-b4-client-warm-precondition")
            if not ordered_rows:
                b4_bad.add("@rows:s70-b4-client-post")
            else:
                first_post = min(ordered_rows, key=lambda item: item[0])[1]
                reference_rows = pre_by_tu.get(str(first_post["tu"]), [])
                cold_bytes = (
                    max(
                        (
                            (row["c_to_f_bytes"], row["f_to_c_bytes"])
                            for row in reference_rows
                        ),
                        default=None,
                    )
                )
                if cold_bytes != (
                    first_post["c_to_f_bytes"],
                    first_post["f_to_c_bytes"],
                ):
                    b4_bad.add(_job_id(first_post["job_id"], "@row"))
        clauses.append(
            _clause(
                "s70.b4-client-route-restart",
                not b4_bad,
                "a warm client route-owner restart preserves the daemon and makes the next P29 TU cold",
                b4_bad,
            )
        )

    if engagement_mode == S70_B5_ENGAGEMENT:
        b5_bad: set[str] = set()
        timeline = scenario.get("timeline")
        expected_event = (
            timeline[0]
            if isinstance(timeline, list) and len(timeline) == 1
            else None
        )
        instances = scenario.get("instances")
        instances = instances if isinstance(instances, list) else []
        target = next(
            (
                item
                for item in instances
                if isinstance(item, Mapping)
                and isinstance(expected_event, Mapping)
                and item.get("name") == expected_event.get("instance")
            ),
            None,
        )
        target_env = target.get("env") if isinstance(target, Mapping) else None
        target_env = target_env if isinstance(target_env, Mapping) else None
        workload = scenario.get("workload")
        event_env = (
            expected_event.get("env")
            if isinstance(expected_event, Mapping)
            else None
        )
        target_label = (
            scenario.get("images", {}).get(target.get("image"))
            if isinstance(target, Mapping)
            and isinstance(scenario.get("images"), Mapping)
            else None
        )
        expected_update = {P29_FAULT_ENV: P29_FAULT_ENV_VALUE}
        if (
            not isinstance(expected_event, Mapping)
            or not isinstance(event_env, Mapping)
            or set(event_env) != {P29_FAULT_ENV}
            or event_env != expected_update
            or expected_event.get("action") != "env_set"
            or not isinstance(target, Mapping)
            or target.get("role") != "C"
            or target_env is None
            or target_env.get("ICECC_P50_MODE") != "on"
            or _transition_protocol(target_label) != 50
            or not isinstance(workload, Mapping)
            or workload.get("jobs") != 1
            or workload.get("clients")
            != ([target.get("name")] if isinstance(target, Mapping) else None)
            or any(
                row.get("client_instance") != target.get("name")
                for row in valid_rows
            )
        ):
            b5_bad.add("@scenario:s70-b5-event")
        schedulers = [
            item
            for item in instances
            if isinstance(item, Mapping) and item.get("role") == "S"
        ]
        if (
            len(schedulers) != 1
            or not isinstance(schedulers[0].get("env"), Mapping)
            or "ICECC_P50_PROFILE" in schedulers[0]["env"]
        ):
            b5_bad.add("@scenario:s70-b5-scheduler-default")
        observed_event = (
            bundle.get("event_log")[0]
            if isinstance(bundle.get("event_log"), list)
            and len(bundle["event_log"]) == 1
            else None
        )
        if (
            not isinstance(expected_event, Mapping)
            or not isinstance(observed_event, Mapping)
            or observed_event.get("action") != "env_set"
            or observed_event.get("instance") != expected_event.get("instance")
            or observed_event.get("trigger") != expected_event.get("trigger")
            or observed_event.get("event_index") != 0
            or observed_event.get("event_epoch") != 1
            or _transition_receipt_errors(observed_event, expected_event, scenario)
        ):
            b5_bad.add("@event:s70-b5")
        else:
            receipt = observed_event.get("receipt")
            before = receipt.get("before") if isinstance(receipt, Mapping) else None
            after = receipt.get("after") if isinstance(receipt, Mapping) else None
            initial_env = target_env
            if (
                not isinstance(before, Mapping)
                or not isinstance(after, Mapping)
                or before.get("env") != initial_env
                or after.get("env")
                != ({**initial_env, **expected_update} if isinstance(initial_env, Mapping) else None)
            ):
                b5_bad.add("@event:s70-b5-env")
        faults = observations.get("p29_interner_faults")
        if (
            not isinstance(faults, list)
            or len(faults) != 1
            or not isinstance(faults[0], Mapping)
            or set(faults[0]) != {"client_instance", "schema", "fault", "outcome"}
            or faults[0].get("client_instance")
            != (target.get("name") if isinstance(target, Mapping) else None)
            or faults[0].get("schema") != P29_FAULT_SCHEMA
            or faults[0].get("fault") != P29_FAULT_NAME
            or faults[0].get("outcome") != "fired"
        ):
            b5_bad.add("@observations:p29_interner_faults")
        clauses.append(
            _clause(
                "s70.b5-interner-downgrade",
                not b5_bad,
                "one authenticated P29 interner fault causes a sticky client downgrade to ZSTD_TU",
                b5_bad,
            )
        )

    if engagement_mode == S70_B6_ENGAGEMENT:
        b6_bad: set[str] = set()
        instances = scenario.get("instances")
        instances = instances if isinstance(instances, list) else []
        schedulers = [
            item
            for item in instances
            if isinstance(item, Mapping) and item.get("role") == "S"
        ]
        scheduler = schedulers[0] if len(schedulers) == 1 else None
        scheduler_env = (
            scheduler.get("env") if isinstance(scheduler, Mapping) else None
        )
        scheduler_name = (
            scheduler.get("name") if isinstance(scheduler, Mapping) else None
        )
        workload = scenario.get("workload")
        expected_timeline = [
            {
                "trigger": "job 1",
                "action": "env_set",
                "instance": scheduler_name,
                "env": {"ICECC_P50_PROFILE": "OFF"},
            },
            {
                "trigger": "job 3",
                "action": "env_set",
                "instance": scheduler_name,
                "env": {"ICECC_P50_PROFILE": "P29V1"},
            },
        ]
        timeline = scenario.get("timeline")
        if (
            scheduler is None
            or not isinstance(scheduler_env, Mapping)
            or scheduler_env.get("ICECC_P50_PROFILE") != "P29V1"
            or timeline != expected_timeline
            or not isinstance(workload, Mapping)
            or workload.get("jobs") != 1
            or not isinstance(workload.get("clients"), list)
            or len(workload["clients"]) != 1
            or any(
                row.get("client_instance") != workload["clients"][0]
                for row in valid_rows
            )
        ):
            b6_bad.add("@scenario:s70-b6-cycle")

        event_log = bundle.get("event_log")
        observed_events = event_log if isinstance(event_log, list) else []
        if len(observed_events) != 2:
            b6_bad.add("@event:s70-b6-count")
        else:
            for index, (observed_event, expected_event) in enumerate(
                zip(observed_events, expected_timeline, strict=True)
            ):
                dispatch_number = 1 if index == 0 else 3
                if (
                    not isinstance(observed_event, Mapping)
                    or observed_event.get("action") != "env_set"
                    or observed_event.get("instance") != scheduler_name
                    or observed_event.get("trigger") != expected_event["trigger"]
                    or observed_event.get("event_index") != index
                    or observed_event.get("event_epoch") != index + 1
                    or not _is_int(
                        observed_event.get("last_dispatched_job"), minimum=1
                    )
                    or not _is_int(
                        observed_event.get("workload_dispatch_count"),
                        minimum=dispatch_number,
                    )
                    or _transition_receipt_errors(
                        observed_event, expected_event, scenario
                    )
                ):
                    b6_bad.add(f"@event:s70-b6-{index}")

            first = observed_events[0]
            second = observed_events[1]
            first_receipt = first.get("receipt") if isinstance(first, Mapping) else None
            second_receipt = second.get("receipt") if isinstance(second, Mapping) else None
            first_before = (
                first_receipt.get("before")
                if isinstance(first_receipt, Mapping)
                else None
            )
            first_after = (
                first_receipt.get("after")
                if isinstance(first_receipt, Mapping)
                else None
            )
            second_before = (
                second_receipt.get("before")
                if isinstance(second_receipt, Mapping)
                else None
            )
            second_after = (
                second_receipt.get("after")
                if isinstance(second_receipt, Mapping)
                else None
            )
            off_env = (
                {**scheduler_env, "ICECC_P50_PROFILE": "OFF"}
                if isinstance(scheduler_env, Mapping)
                else None
            )
            restored_env = (
                {**scheduler_env, "ICECC_P50_PROFILE": "P29V1"}
                if isinstance(scheduler_env, Mapping)
                else None
            )
            if (
                not isinstance(first_before, Mapping)
                or not isinstance(first_after, Mapping)
                or not isinstance(second_before, Mapping)
                or not isinstance(second_after, Mapping)
                or first_before.get("env") != scheduler_env
                or first_after.get("env") != off_env
                or second_before.get("env") != off_env
                or second_after.get("env") != restored_env
                or second_before.get("container_id")
                != first_after.get("container_id")
            ):
                b6_bad.add("@event:s70-b6-env-progression")

            lifecycle = observations.get("job_lifecycle")
            scheduler_order_by_job = {
                _job_id(item.get("job_id"), "@lifecycle"): (
                    item.get("scheduler_generation"),
                    item.get("scheduler_dispatch_line"),
                )
                for item in lifecycle
                if isinstance(item, Mapping)
            } if isinstance(lifecycle, list) else {}
            first_count = observed_events[0].get("workload_dispatch_count")
            second_count = observed_events[1].get("workload_dispatch_count")
            if (
                not _is_int(first_count, minimum=1)
                or not _is_int(second_count, minimum=1)
                or second_count <= first_count
            ):
                b6_bad.add("@event:s70-b6-no-post-off-dispatch")
            for row in valid_rows:
                identifier = _job_id(row["job_id"], "@row")
                epoch = row["event_epoch"]
                generation, dispatch_line = scheduler_order_by_job.get(
                    identifier, (None, None)
                )
                if (
                    not _is_int(generation, minimum=1)
                    or not _is_int(dispatch_line, minimum=1)
                    or generation != epoch + 1
                ):
                    b6_bad.add(identifier)
        clauses.append(
            _clause(
                "s70.b6-drained-kill-switch-cycle",
                not b6_bad,
                "a drained scheduler replacement suppresses tails under OFF and restores them under P29V1",
                b6_bad,
            )
        )

    if engagement_mode in {
        S70_B7_ROLLBACK_ENGAGEMENT,
        S70_B7_ROLLFORWARD_ENGAGEMENT,
    }:
        rollback = engagement_mode == S70_B7_ROLLBACK_ENGAGEMENT
        clause_id = "s70.b7-rollback" if rollback else "s70.b7-rollforward"
        b7_bad: set[str] = set()
        instances = scenario.get("instances")
        instances = instances if isinstance(instances, list) else []
        role_instances = {
            role: [
                item
                for item in instances
                if isinstance(item, Mapping) and item.get("role") == role
            ]
            for role in ("S", "F", "C")
        }
        singular = all(len(role_instances[role]) == 1 for role in role_instances)
        names = {
            role: role_instances[role][0].get("name")
            if len(role_instances[role]) == 1
            else None
            for role in role_instances
        }
        initial_alias = "new" if rollback else "old"
        target_alias = "old" if rollback else "new"
        triggers = (75, 100, 125) if rollback else (25, 50, 75)
        action = "downgrade" if rollback else "upgrade"
        expected_timeline = [
            {
                "trigger": f"job {trigger}",
                "action": action,
                "instance": names[role],
                "image": target_alias,
            }
            for trigger, role in zip(triggers, ("S", "F", "C"), strict=True)
        ]
        workload = scenario.get("workload")
        if (
            not singular
            or scenario.get("shape") != "mixed"
            or scenario.get("timeline") != expected_timeline
            or not isinstance(workload, Mapping)
            or workload.get("corpus") != "fmt-100"
            or workload.get("turns") != ["A"]
            or workload.get("jobs") != 1
            or workload.get("repeat") != 4
            or workload.get("clients") != [names["C"]]
            or any(
                item.get("image") != initial_alias
                for role in role_instances.values()
                for item in role
            )
        ):
            b7_bad.add(f"@scenario:{clause_id}")

        event_log = bundle.get("event_log")
        observed_events = event_log if isinstance(event_log, list) else []
        fired: list[int] = []
        if len(observed_events) != 3:
            b7_bad.add(f"@event:{clause_id}-count")
        else:
            for index, (observed_event, expected_event, trigger) in enumerate(
                zip(observed_events, expected_timeline, triggers, strict=True)
            ):
                if (
                    not isinstance(observed_event, Mapping)
                    or observed_event.get("action") != action
                    or observed_event.get("instance") != expected_event["instance"]
                    or observed_event.get("trigger") != expected_event["trigger"]
                    or observed_event.get("event_index") != index
                    or observed_event.get("event_epoch") != index + 1
                    or not _is_int(
                        observed_event.get("last_dispatched_job"), minimum=1
                    )
                    or not _is_int(
                        observed_event.get("workload_dispatch_count"),
                        minimum=trigger,
                    )
                    or _transition_receipt_errors(
                        observed_event, expected_event, scenario
                    )
                ):
                    b7_bad.add(f"@event:{clause_id}-{index}")
                    continue
                fired_ms = observed_event.get("fired_ms")
                receipt = observed_event.get("receipt")
                before = (
                    receipt.get("before") if isinstance(receipt, Mapping) else None
                )
                after = (
                    receipt.get("after") if isinstance(receipt, Mapping) else None
                )
                clients = (
                    receipt.get("coordination", {}).get("clients")
                    if isinstance(receipt, Mapping)
                    and isinstance(receipt.get("coordination"), Mapping)
                    else None
                )
                pause = (
                    clients.get(names["C"])
                    if isinstance(clients, Mapping)
                    and isinstance(names["C"], str)
                    else None
                )
                role = ("S", "F", "C")[index]
                before_env = (
                    {"ICECC_P50_PROFILE": "P29V1"}
                    if rollback and role == "S"
                    else {"ICECC_P50_MODE": "on"}
                    if rollback and role == "C"
                    else {}
                )
                after_env = (
                    {"ICECC_P50_PROFILE": "P29V1"}
                    if not rollback and role == "S"
                    else {"ICECC_P50_MODE": "on"}
                    if not rollback and role == "C"
                    else {}
                )
                if (
                    not _is_int(fired_ms)
                    or not isinstance(before, Mapping)
                    or not isinstance(after, Mapping)
                    or before.get("image")
                    != scenario.get("images", {}).get(initial_alias)
                    or after.get("image")
                    != scenario.get("images", {}).get(target_alias)
                    or before.get("env") != before_env
                    or after.get("env") != after_env
                    or not isinstance(pause, Mapping)
                    or not _is_int(pause.get("active_before"), minimum=1)
                ):
                    b7_bad.add(f"@event:{clause_id}-{index}")
                    continue
                fired.append(fired_ms)
            if len(fired) == 3 and not (fired[0] < fired[1] < fired[2]):
                b7_bad.add(f"@event:{clause_id}-order")

        lifecycle = observations.get("job_lifecycle")
        dispatch_by_job = {
            _job_id(item.get("job_id"), "@lifecycle"): _lifecycle_final_dispatch_ms(item)
            for item in lifecycle
            if isinstance(item, Mapping)
        } if isinstance(lifecycle, list) else {}
        epoch_rows: dict[int, list[tuple[int, Mapping[str, Any]]]] = defaultdict(list)
        if len(fired) == 3:
            for row in valid_rows:
                identifier = _job_id(row["job_id"], "@row")
                dispatch_ms = dispatch_by_job.get(identifier)
                expected_epoch = (
                    sum(boundary <= dispatch_ms for boundary in fired)
                    if _is_int(dispatch_ms)
                    else None
                )
                if expected_epoch != row["event_epoch"]:
                    b7_bad.add(identifier)
                elif _is_int(dispatch_ms):
                    epoch_rows[row["event_epoch"]].append((dispatch_ms, row))
        for epoch in range(4):
            if not epoch_rows[epoch]:
                b7_bad.add(f"@rows:{clause_id}-epoch-{epoch}")

        if rollback:
            by_tu: dict[str, list[tuple[int, Mapping[str, Any]]]] = defaultdict(list)
            for item in epoch_rows[0]:
                by_tu[str(item[1]["tu"])].append(item)
            warm_precondition = any(
                len(items) >= 2
                and min(row["c_to_f_bytes"] for _, row in items[1:])
                < items[0][1]["c_to_f_bytes"]
                for items in (
                    sorted(items, key=lambda entry: entry[0])
                    for items in by_tu.values()
                )
            )
            if not warm_precondition:
                b7_bad.add("@rows:s70.b7-warm-precondition")
        else:
            post = sorted(epoch_rows[3], key=lambda item: item[0])
            if not post:
                b7_bad.add("@rows:s70.b7-cold-restart")
            else:
                first = post[0][1]
                later = [
                    row for _, row in post[1:] if row["tu"] == first["tu"]
                ]
                if (
                    not later
                    or not any(
                        row["c_to_f_bytes"] < first["c_to_f_bytes"]
                        for row in later
                    )
                ):
                    b7_bad.add(_job_id(first["job_id"], "@row"))
        clauses.append(
            _clause(
                clause_id,
                not b7_bad,
                (
                    "a warm P29 farm rolls back S then F then C to exact legacy"
                    if rollback
                    else "an all-old farm rolls S then F then C forward and the new P29 route starts cold"
                ),
                b7_bad,
            )
        )

    session_bad = {
        _job_id(row["job_id"], "@row")
        for row in valid_rows
        if (row["session_outcome"] == "committed") != row["tail_present"]
    }
    clauses.append(
        _clause(
            "session.committed-iff-tail",
            not session_bad,
            "a session is committed if and only if a tail is present",
            session_bad,
        )
    )

    bounce_actions = {"restart", "kill -9", "upgrade", "downgrade"}
    timeline = scenario.get("timeline", [])
    bounce = isinstance(timeline, list) and any(
        isinstance(item, Mapping) and item.get("action") in bounce_actions
        for item in timeline
    )
    # The client protocol permits one fresh legacy remote assignment after a
    # failed P50 assignment.  A timeline transition can exercise that path;
    # steady-state cells may not retry at all.
    retry_limit = (
        1
        if bounce
        or s30_mutant
        or engagement_mode in {
            S70_B5_ENGAGEMENT,
            S70_B4_ACTIVE_LOSS_ENGAGEMENT,
            S90_REVISION_REFUSAL_ENGAGEMENT,
            S95_DISK_FILL_ENGAGEMENT,
        }
        else 0
    )
    retry_bad = {
        _job_id(row["job_id"], "@row")
        for row in valid_rows
        if retry_limit < 0 or row["retries"] > retry_limit
    }
    clauses.append(
        _clause(
            "retries.bounded",
            not retry_bad,
            f"per-job retries are bounded by {retry_limit}",
            retry_bad,
        )
    )

    if s30_mutant:
        s30 = observations.get("s30_mutant_f")
        records = s30.get("records") if isinstance(s30, Mapping) else None
        record_count = len(records) if isinstance(records, list) else None
        canary_records = (
            s30.get("canary_records") if isinstance(s30, Mapping) else None
        )
        fallback_ids = {
            _job_id(row["job_id"], "@row")
            for row in valid_rows
            if row["session_outcome"] == "fallback"
        }
        trace_bad: set[str] = set()
        if not isinstance(s30, Mapping) or s30.get("schema") != "icefarm-s30-mutant-f-refusal-v1":
            trace_bad.add("@observations:s30_mutant_f")
        if (
            not isinstance(records, list)
            or not records
            or len(records) > len(fallback_ids)
        ):
            trace_bad.add("@observations:s30_mutant_f.records")
        else:
            for index, record in enumerate(records):
                if (
                    not isinstance(record, Mapping)
                    or set(record) != {"schema", "refusal", "wire_revision", "supported_profiles"}
                    or record.get("schema") != "icefarm-s30-mutant-f-refusal-v1"
                    or record.get("refusal") != "p50-session-refused"
                    or not _is_int(record.get("wire_revision"), minimum=1)
                    or record.get("wire_revision") != 1
                    or record.get("supported_profiles") != 1
                ):
                    trace_bad.add(f"@s30-refusal:{index}")
        if (
            not isinstance(canary_records, list)
            or not canary_records
            or not isinstance(s30, Mapping)
            or s30.get("canary_refusal_count") != len(canary_records)
            or any(
                not isinstance(record, Mapping)
                or set(record)
                != {"schema", "refusal", "wire_revision", "supported_profiles"}
                or record.get("schema") != "icefarm-s30-mutant-f-refusal-v1"
                or record.get("refusal") != "p50-session-refused"
                or record.get("wire_revision") != 1
                or record.get("supported_profiles") != 1
                for record in canary_records
            )
        ):
            trace_bad.add("@observations:s30_mutant_f.canary_records")
        if (
            not isinstance(s30, Mapping)
            or s30.get("refusal_count") != record_count
        ):
            trace_bad.add("@observations:s30_mutant_f.refusal_count")
        if (
            not isinstance(s30, Mapping)
            or set(s30.get("fallback_job_ids", ())) != fallback_ids
            or not fallback_ids
            or s30.get("fresh_legacy_assignment_count") != len(fallback_ids)
            or s30.get("local_fallback_job_ids") != []
        ):
            trace_bad.add("@observations:s30_mutant_f.binding")
        legacy = observations.get("legacy_wire")
        legacy_records = legacy.get("records") if isinstance(legacy, Mapping) else None
        legacy_ids = {
            _job_id(item.get("job_id"), "@observations:legacy_wire")
            for item in legacy_records
            if isinstance(item, Mapping)
        } if isinstance(legacy_records, list) else set()
        if (
            not isinstance(legacy, Mapping)
            or legacy.get("record_count") != len(valid_rows)
            or not isinstance(legacy_records, list)
            or legacy_ids != set(identifiers)
            or any(
                not isinstance(item, Mapping)
                or item.get("c_to_f_bytes", 0) <= 0
                or item.get("f_to_c_bytes", 0) <= 0
                for item in legacy_records
            )
        ):
            trace_bad.add("@observations:s30_mutant_f.legacy_wire")
        clauses.append(
            _clause(
                "s30.mutant-refusal-evidence",
                not trace_bad,
                "authenticated post-hello refusal(s) bind bounded conserved legacy retries",
                trace_bad,
            )
        )
        s30_rows_bad = {
            _job_id(row["job_id"], "@row")
            for row in valid_rows
            if not (
                (row["session_outcome"] == "fallback" and row["retries"] == 1)
                or (row["session_outcome"] == "none" and row["retries"] == 0)
            )
            or row["tail_present"] is not False
            or row["tail_profile"] is not None
            or row["exact"] is not True
            or row["client_version"] != 50
            or row["cs_version"] != 50
        }
        clauses.append(
            _clause(
                "s30.one-refusal-one-retry",
                not s30_rows_bad,
                "each affected S30 job has exactly one refused P50 attempt and one fresh legacy assignment",
                s30_rows_bad,
            )
        )

    row_ids = set(identifiers)
    wedges, lifecycle_errors = _lifecycle_wedges(observations, row_ids)
    clauses.append(
        _clause(
            "job-lifecycle.schema",
            not lifecycle_errors,
            "job lifecycle records bind every row"
            if not lifecycle_errors
            else "; ".join(lifecycle_errors),
            () if not lifecycle_errors else {"@observations:job_lifecycle"},
        )
    )
    expected_wedges = expect.get("wedges")
    clauses.append(
        _clause(
            "wedges",
            _is_int(expected_wedges) and len(wedges) == expected_wedges,
            f"observed {len(wedges)} wedged jobs; expected {expected_wedges!r}",
            wedges or ({"@expect:wedges"} if not _is_int(expected_wedges) else set()),
        )
    )

    error106 = observations.get("error106_job_ids")
    error106_ids = (
        {_job_id(item, "@error106") for item in error106}
        if isinstance(error106, list)
        else {"@observations:error106_job_ids"}
    )
    unrecovered_error106_ids = error106_ids - recovered_ids
    error106_max = expect.get("error106_max")
    error106_ok = (
        isinstance(error106, list)
        and _is_int(error106_max)
        and len(unrecovered_error106_ids) <= error106_max
    )
    clauses.append(
        _clause(
            "error106.max",
            error106_ok,
            "observed "
            f"{len(unrecovered_error106_ids) if isinstance(error106, list) else 'invalid'} "
            f"unrecovered Error-106 jobs; maximum {error106_max!r}",
            unrecovered_error106_ids,
        )
    )

    incapable = {
        _job_id(row["job_id"], "@row")
        for row in valid_rows
        if row["tail_present"]
        and (row["client_version"] != 50 or row["cs_version"] != 50)
    }
    h3_mutant = observations.get("h3_mutant")
    if isinstance(h3_mutant, Mapping) and h3_mutant.get("authenticated") is True:
        record_count = h3_mutant.get("record_count")
        if _is_int(record_count) and record_count > 0:
            incapable.update(
                f"@h3-mutant:{index}"
                for index in range(1, record_count + 1)
            )
    tail_limit = expect.get("tail_to_incapable")
    clauses.append(
        _clause(
            "tail.incapable",
            _is_int(tail_limit) and len(incapable) <= tail_limit,
            f"observed {len(incapable)} tails to incapable peers; maximum {tail_limit!r}",
            incapable
            or ({"@expect:tail_to_incapable"} if not _is_int(tail_limit) else set()),
        )
    )

    reuse_bad: set[str] = set()
    reuse_expectation = expect.get("reuse")
    pair_expectation = expect.get("reuse_pairs")
    reuse_expectation_valid = reuse_expectation in {
        "all-true-when-p29v1",
        "all-false-when-p29v1",
        "none-when-legacy",
    }
    if pair_expectation is not None:
        pair_bad: set[str] = set()
        seen_pairs: set[str] = set()
        if not isinstance(pair_expectation, Mapping) or not pair_expectation:
            pair_bad.add("@expect:reuse_pairs")
        else:
            header_event = any(
                isinstance(item, Mapping) and item.get("action") == "header_edit"
                for item in scenario.get("timeline", [])
            ) if isinstance(scenario.get("timeline"), list) else False
            post_event_pairs: set[str] = set()
            for pair, expected_reuse in pair_expectation.items():
                if (
                    not isinstance(pair, str)
                    or pair.count("/") != 1
                    or not all(pair.split("/"))
                    or type(expected_reuse) is not bool
                ):
                    pair_bad.add("@expect:reuse_pairs")
            for row in valid_rows:
                if row["tail_profile"] != "P29V1":
                    continue
                pair = f"{row['client_instance']}/{row['cs']}"
                expected_reuse = pair_expectation.get(pair)
                if type(expected_reuse) is not bool:
                    pair_bad.add(_job_id(row["job_id"], "@row"))
                elif expected_reuse is False and header_event and row.get("event_epoch", 0) == 0:
                    # A header mutation may deliberately occur mid-build: the
                    # warm pre-event rows are not evidence against the new
                    # fingerprint, but a post-event row is mandatory below.
                    seen_pairs.add(pair)
                    continue
                elif row["reuse"] is not expected_reuse:
                    pair_bad.add(_job_id(row["job_id"], "@row"))
                else:
                    seen_pairs.add(pair)
                if expected_reuse is False and header_event and row.get("event_epoch", 0) > 0:
                    post_event_pairs.add(pair)
            if isinstance(pair_expectation, Mapping):
                pair_bad.update(
                    f"@pair:{pair}"
                    for pair in pair_expectation
                    if pair not in seen_pairs
                )
                if header_event:
                    pair_bad.update(
                        f"@pair:{pair}:post-event"
                        for pair, expected_reuse in pair_expectation.items()
                        if expected_reuse is False and pair not in post_event_pairs
                    )
        reuse_bad = pair_bad
        reuse_expectation_valid = not pair_bad
    elif reuse_expectation == "all-true-when-p29v1":
        reuse_bad = {
            _job_id(row["job_id"], "@row")
            for row in valid_rows
            if row["tail_profile"] == "P29V1" and row["reuse"] is not True
        }
    elif reuse_expectation == "all-false-when-p29v1":
        reuse_bad = {
            _job_id(row["job_id"], "@row")
            for row in valid_rows
            if row["tail_profile"] == "P29V1" and row["reuse"] is not False
        }
    elif reuse_expectation == "none-when-legacy":
        reuse_bad = {
            _job_id(row["job_id"], "@row")
            for row in valid_rows
            if row["tail_profile"] != "P29V1" and row["reuse"] is not None
        }
    clauses.append(
        _clause(
            "reuse",
            reuse_expectation_valid and not reuse_bad,
            f"reuse expectation is {pair_expectation!r}" if pair_expectation is not None else f"reuse expectation is {reuse_expectation!r}",
            reuse_bad or (() if reuse_expectation_valid else {"@expect:reuse"}),
        )
    )

    oracle = observations.get("oracle")
    oracle_bad: set[str] = set()
    oracle_ok = isinstance(oracle, Mapping)
    if oracle_ok:
        sample_total = oracle.get("sample_total")
        mismatches = oracle.get("sample_mismatch_job_ids")
        oracle_ok = (
            _is_int(sample_total, minimum=1)
            and isinstance(mismatches, list)
            and not mismatches
        )
        if isinstance(mismatches, list):
            oracle_bad = {_job_id(item, "@oracle") for item in mismatches}
    clauses.append(
        _clause(
            "oracle.sample",
            oracle_ok,
            "the deterministic local-SHA cache sample was recompiled without mismatch",
            oracle_bad or (() if oracle_ok else {"@observations:oracle"}),
        )
    )

    incomplete = observations.get("incomplete_turns")
    incomplete_values = (
        {f"@turn:{item}" for item in incomplete}
        if isinstance(incomplete, list)
        else {"@observations:incomplete_turns"}
    )
    clauses.append(
        _clause(
            "turns.complete",
            isinstance(incomplete, list) and not incomplete,
            "every requested turn completed before its deadline",
            incomplete_values,
        )
    )

    wall_max = expect.get("wall_s_max")
    wall_bad: set[str] = set()
    wall_ok = True
    if wall_max is not None:
        cell_wall = observations.get("cell_wall_ms")
        wall_ok = (
            isinstance(wall_max, (int, float))
            and not isinstance(wall_max, bool)
            and _is_int(cell_wall)
        )
        if wall_ok and cell_wall > wall_max * 1000:
            wall_ok = False
        if not wall_ok:
            wall_bad.add("@observations:cell_wall_ms")
    clauses.append(
        _clause(
            "wall.max",
            wall_ok,
            "cell wall is reported only when a maximum is configured"
            if wall_max is None
            else f"cell wall is at most {wall_max} seconds",
            wall_bad,
        )
    )

    clauses.extend(
        _shape_clauses(
            scenario,
            valid_rows,
            observations,
            selected_profile,
            bundle.get("event_log"),
            bundle.get("topology"),
            bundle.get("run_id"),
        )
    )
    return _finish(clauses)


def _finish(clauses: list[dict[str, Any]]) -> dict[str, Any]:
    failed = [item for item in clauses if item["status"] == "FAIL"]
    return {
        "clauses": clauses,
        "offending_job_ids": _sorted_ids(
            {identifier for item in failed for identifier in item["offending_job_ids"]}
        ),
        "schema": VERDICT_SCHEMA,
        "status": "FAIL" if failed else "PASS",
    }


def _h3_instance_version(instance: Mapping[str, Any], scenario: Mapping[str, Any]) -> int | None:
    value = instance.get("version")
    if type(value) is int:
        return value
    alias = instance.get("image")
    images = scenario.get("images")
    label = images.get(alias) if isinstance(images, Mapping) else alias
    if not isinstance(label, str):
        return None
    if alias == "old" or label.lower().startswith("p43"):
        return 43
    if alias in {"new", "mutant"} or label.lower().startswith("p50"):
        return 50
    return None


def _h3_failure_is_authenticated(bundle: Mapping[str, Any], failure: Any) -> bool:
    """Recompute H3 bindings from the sealed bundle; do not trust flags."""

    if not isinstance(failure, Mapping):
        return False
    scenario = bundle.get("scenario")
    if not isinstance(scenario, Mapping):
        return False
    instances = scenario.get("instances")
    if not isinstance(instances, list):
        topology = bundle.get("topology")
        instances = topology.get("instances") if isinstance(topology, Mapping) else None
    if not isinstance(instances, list):
        return False
    named = [item for item in instances if isinstance(item, Mapping)]
    clients = {
        str(item["name"])
        for item in named
        if item.get("role") == "C" and _h3_instance_version(item, scenario) is not None
    }
    old_clients = {
        str(item["name"])
        for item in named
        if item.get("role") == "C"
        and (version := _h3_instance_version(item, scenario)) is not None
        and version < 50
    }
    current_workers = {
        str(item["name"])
        for item in named
        if item.get("role") == "F" and _h3_instance_version(item, scenario) == 50
    }
    schedulers = {
        str(item["name"]) for item in named if item.get("role") == "S"
    }
    if len(old_clients) != 1 or not current_workers or len(schedulers) != 1:
        return False
    old_client = next(iter(old_clients))
    scheduler = next(iter(schedulers))

    dispatches = failure.get("dispatches")
    emission = failure.get("emission")
    records = emission.get("records") if isinstance(emission, Mapping) else None
    rejections = failure.get("rejections")
    failed_jobs = failure.get("failed_jobs")
    if not all(isinstance(value, list) for value in (dispatches, records, rejections, failed_jobs)):
        return False
    dispatch_count = len(dispatches)
    if dispatch_count <= 0 or any(
        len(value) != dispatch_count for value in (records, rejections)
    ):
        return False
    if failure.get("scheduler_dispatch_count") != dispatch_count:
        return False
    workload_job_count = failure.get("workload_job_count")
    workload_failure_count = failure.get("workload_failure_count")
    if (
        type(workload_job_count) is not int
        or workload_job_count <= 0
        or workload_failure_count != workload_job_count
        or len(failed_jobs) != workload_job_count
    ):
        return False

    dispatch_keys: set[tuple[int, str, str]] = set()
    for dispatch in dispatches:
        if not isinstance(dispatch, Mapping):
            return False
        job = dispatch.get("scheduler_job")
        client = dispatch.get("client")
        worker = dispatch.get("worker")
        if (
            type(job) is not int
            or job <= 0
            or not isinstance(client, str)
            or not isinstance(worker, str)
            or client != old_client
            or client not in clients
            or worker not in current_workers
            or dispatch.get("terminal")
            not in {"completion", "cancellation", "process-loss-recovery"}
            or type(dispatch.get("terminal_ms")) is not int
            or type(dispatch.get("dispatch_ms")) is not int
            or dispatch["dispatch_ms"] > dispatch["terminal_ms"]
        ):
            return False
        key = (job, client, worker)
        if key in dispatch_keys:
            return False
        dispatch_keys.add(key)

    trace_fields = {
        "assignment_epoch",
        "assignment_nonce",
        "client_instance",
        "cache_port",
        "cache_profile_mask",
        "cache_protocol",
        "emission",
        "schema",
        "scheduler_instance",
        "scheduler_job",
        "tail_bytes",
        "tail_hex",
        "worker_instance",
    }
    seen_records: set[tuple[int, str, str]] = set()
    for record in records:
        if not isinstance(record, Mapping) or set(record) != trace_fields:
            return False
        key = (
            record.get("scheduler_job"),
            record.get("client_instance"),
            record.get("worker_instance"),
        )
        if (
            type(key[0]) is not int
            or not isinstance(key[1], str)
            or not isinstance(key[2], str)
            or key not in dispatch_keys
            or key in seen_records
            or record.get("scheduler_instance") != scheduler
            or record.get("schema") != "icefarm-scheduler-mutant-trace-v1"
            or record.get("emission") != "protocol-50-tail-sent"
            or type(record.get("assignment_epoch")) is not int
            or record["assignment_epoch"] <= 0
            or type(record.get("assignment_nonce")) is not int
            or record["assignment_nonce"] <= 0
            or record.get("tail_bytes") != 12
        ):
            return False
        values = tuple(record.get(field) for field in ("cache_port", "cache_protocol", "cache_profile_mask"))
        if any(type(value) is not int or not 1 <= value <= 65535 for value in values):
            return False
        if record.get("tail_hex") != struct.pack(">III", *values).hex():
            return False
        seen_records.add(key)

    rejection_lines: set[int] = set()
    for rejection in rejections:
        if not isinstance(rejection, Mapping):
            return False
        message_size = rejection.get("message_size")
        bytes_read = rejection.get("bytes_read")
        if (
            rejection.get("schema") != "icefarm-h3-client-rejection-v1"
            or rejection.get("client_instance") != old_client
            or type(message_size) is not int
            or type(bytes_read) is not int
            or message_size < 0
            or bytes_read < 0
            or message_size - bytes_read != 12
            or type(rejection.get("line")) is not int
            or rejection["line"] <= 0
            or rejection["line"] in rejection_lines
        ):
            return False
        rejection_lines.add(rejection["line"])

    failed_indexes: set[int] = set()
    for failed in failed_jobs:
        if not isinstance(failed, Mapping):
            return False
        scheduler_job = failed.get("scheduler_job")
        worker = failed.get("worker")
        missing_assignment = (
            isinstance(scheduler_job, str)
            and scheduler_job.startswith("missing-")
            and worker == "UNKNOWN"
        )
        local_assignment = (
            isinstance(scheduler_job, str)
            and scheduler_job.isdigit()
            and int(scheduler_job) > 0
            and worker == "127.0.0.1:0"
        )
        if (
            type(failed.get("index")) is not int
            or failed["index"] <= 0
            or type(failed.get("compile_rc")) is not int
            or failed["compile_rc"] < 0
            or type(failed.get("exact")) is not bool
            or failed.get("remote") is not False
            or not (missing_assignment or local_assignment)
            or not isinstance(failed.get("turn"), str)
            or not failed["turn"]
            or failed["index"] in failed_indexes
        ):
            return False
        failed_indexes.add(failed["index"])
    return seen_records == dispatch_keys


def evaluate_control(control_id: str, bundle: Mapping[str, Any]) -> dict[str, Any]:
    """Judge whether an H1--H5 mutation produced its designed observation."""

    verdict = evaluate_bundle(bundle)
    observations = bundle.get("observations", {}) if isinstance(bundle, Mapping) else {}
    fault = observations.get("fault", {}) if isinstance(observations, Mapping) else {}
    failed_ids = {item["id"] for item in verdict["clauses"] if item["status"] == "FAIL"}
    passed = False
    detail = "unknown control"
    if control_id == "H1":
        refusal = (
            observations.get("preflight_refusal", {})
            if isinstance(observations, Mapping)
            else {}
        )
        bindings = ("farm_digest", "scenario_digest", "topology_digest")
        passed = (
            isinstance(refusal, Mapping)
            and bundle.get("mode") == "preflight-refusal"
            and refusal.get("schema") == "icefarm-preflight-refusal-v1"
            and refusal.get("reason_code") == "role-hash-mismatch"
            and refusal.get("jobs_started") == 0
            and refusal.get("persistent_start_attempted") is False
            and bundle.get("rows") == []
            and all(
                isinstance(bundle.get(field), str)
                and SHA256_RE.fullmatch(bundle[field]) is not None
                and refusal.get(field) == bundle[field]
                for field in bindings
            )
        )
        detail = "wrong role hash is refused before any job starts"
    elif control_id == "H2":
        scenario = bundle.get("scenario", {}) if isinstance(bundle, Mapping) else {}
        instances = scenario.get("instances", []) if isinstance(scenario, Mapping) else []
        workload = scenario.get("workload", {}) if isinstance(scenario, Mapping) else {}
        workload_clients = (
            workload.get("clients", []) if isinstance(workload, Mapping) else []
        )
        disabled_clients = {
            instance.get("name")
            for instance in instances
            if isinstance(instance, Mapping)
            and instance.get("role") == "C"
            and isinstance(instance.get("env"), Mapping)
            and instance["env"].get("ICECC_P50_MODE") == "off"
        }
        rows = bundle.get("rows", []) if isinstance(bundle, Mapping) else []
        passed = (
            fault.get("client_kill_switch") is True
            and verdict["status"] == "FAIL"
            and failed_ids == {"shape.full-newgen-engagement"}
            and isinstance(workload_clients, list)
            and bool(workload_clients)
            and set(workload_clients) == disabled_clients
            and isinstance(rows, list)
            and bool(rows)
            and all(
                isinstance(row, Mapping)
                and row.get("client_instance") in disabled_clients
                and row.get("tail_present") is False
                and row.get("tail_profile") is None
                and row.get("session_outcome") == "none"
                and row.get("reuse") is None
                for row in rows
            )
        )
        detail = "client kill switch produces only the designed zero-session shape failure"
    elif control_id == "H3":
        failure = (
            observations.get("h3_control_failure")
            if isinstance(observations, Mapping)
            else None
        )
        passed = (
            bundle.get("mode") == "control-failure"
            and verdict["status"] == "FAIL"
            and bundle.get("rows") == []
            and isinstance(failure, Mapping)
            and failure.get("schema") == "icefarm-h3-control-failure-v1"
            and failure.get("successful_product_rows") == 0
            and _h3_failure_is_authenticated(bundle, failure)
        )
        detail = "scheduler emission and independent legacy-client unread-tail rejection are authenticated"
    elif control_id == "H4":
        corruption = (
            observations.get("object_corruption")
            if isinstance(observations, Mapping)
            else None
        )
        scenario = bundle.get("scenario", {}) if isinstance(bundle, Mapping) else {}
        descriptor = scenario.get("fault", {}) if isinstance(scenario, Mapping) else {}
        before = (
            corruption.get("before_sha256") if isinstance(corruption, Mapping) else None
        )
        after = (
            corruption.get("after_sha256") if isinstance(corruption, Mapping) else None
        )
        row_job_id = (
            corruption.get("row_job_id") if isinstance(corruption, Mapping) else None
        )
        rows = bundle.get("rows", []) if isinstance(bundle, Mapping) else []
        bound_rows = (
            [
                row
                for row in rows
                if isinstance(row, Mapping)
                and row.get("job_id") == row_job_id
                and row.get("client_instance") == corruption.get("client")
                and row.get("object_sha_local") == before
                and row.get("object_sha_remote") == after
                and row.get("exact") is False
            ]
            if isinstance(rows, list) and isinstance(corruption, Mapping)
            else []
        )
        passed = (
            fault.get("corrupt_object") is True
            and isinstance(corruption, Mapping)
            and isinstance(descriptor, Mapping)
            and descriptor.get("kind") == "corrupt-object"
            and corruption.get("client") == descriptor.get("client")
            and corruption.get("job") == descriptor.get("job")
            and isinstance(before, str)
            and SHA256_RE.fullmatch(before) is not None
            and isinstance(after, str)
            and SHA256_RE.fullmatch(after) is not None
            and before != after
            and (
                (isinstance(row_job_id, str) and bool(row_job_id))
                or _is_int(row_job_id, minimum=1)
            )
            and len(bound_rows) == 1
            and "objects.identity" in failed_ids
        )
        detail = "corrupted object makes the product verdict fail identity"
    elif control_id == "H5":
        killed = (
            {_job_id(item, "@fault") for item in fault.get("worker_killed_job_ids", [])}
            if isinstance(fault, Mapping)
            and isinstance(fault.get("worker_killed_job_ids"), list)
            else set()
        )
        recovered = (
            {
                _job_id(item, "@recovery")
                for item in observations.get("process_loss_recovery_job_ids", [])
            }
            if isinstance(observations, Mapping)
            and isinstance(observations.get("process_loss_recovery_job_ids"), list)
            else set()
        )
        passed = bool(killed) and killed <= recovered and verdict["status"] == "PASS"
        detail = "worker loss is recorded as recovery and never as a wedge"
    clause = _clause(
        f"control.{control_id}",
        passed,
        detail,
        () if passed else {f"@control:{control_id}"},
    )
    return {
        "clause": clause,
        "control": control_id,
        "product_verdict": verdict,
        "schema": CONTROL_VERDICT_SCHEMA,
        "status": "PASS" if passed else "FAIL",
    }
