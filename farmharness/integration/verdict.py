"""Pure, fail-closed verdicts for controlled-farm acceptance bundles.

The product verdict consumes only immutable bundle data.  It does not inspect
the live farm, infer success from a process exit code, or trust a precomputed
``exact``/``wedges`` summary when the underlying rows are available.
"""

from __future__ import annotations

import re
from collections import Counter, defaultdict
from collections.abc import Mapping, Sequence
from typing import Any


BUNDLE_SCHEMA = "icefarm-bundle-v1"
ROW_SCHEMA = "icecream-newgen-farm-acceptance-v1"
VERDICT_SCHEMA = "icefarm-verdict-v1"
CONTROL_VERDICT_SCHEMA = "icefarm-control-verdict-v1"
STALL_LIMIT_MS = 120_000

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
        if not isinstance(row.get(field), str) or SHA256_RE.fullmatch(row[field]) is None:
            errors.append(f"{field} must be a lowercase SHA-256")
    if type(row.get("exact")) is not bool:
        errors.append("exact must be boolean")
    return errors


def _scenario_profile(scenario: Mapping[str, Any]) -> tuple[str | None, str | None]:
    profiles: set[str] = set()
    instances = scenario.get("instances")
    if not isinstance(instances, list):
        return None, "scenario.instances is absent or invalid"
    for instance in instances:
        if not isinstance(instance, Mapping) or instance.get("role") != "S":
            continue
        environment = instance.get("env", {})
        if not isinstance(environment, Mapping):
            return None, "scheduler environment is invalid"
        selected = environment.get("ICECC_P50_PROFILE", "P29V1")
        if selected == "OFF":
            profiles.add("OFF")
        elif selected in PROFILES:
            profiles.add(selected)
        else:
            return None, f"scheduler selected unknown profile {selected!r}"
    if len(profiles) != 1:
        return None, f"scenario must resolve one scheduler profile, got {sorted(profiles)!r}"
    profile = next(iter(profiles))
    return (None if profile == "OFF" else profile), None


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
        ):
            errors.append(f"job_lifecycle[{index}] has invalid timing or terminal fields")
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
            if outstanding and progress_start is not None and timestamp - progress_start > STALL_LIMIT_MS:
                wedged.update(outstanding)
            if kind == "dispatch":
                if not outstanding:
                    progress_start = timestamp
                outstanding.add(identifier)
            else:
                outstanding.discard(identifier)
                progress_start = timestamp if outstanding else None
        if outstanding and progress_start is not None:
            last_deadline = max(item["deadline_ms"] for _identifier, item in records_in_turn)
            if last_deadline - progress_start > STALL_LIMIT_MS:
                wedged.update(outstanding)
    return wedged, errors


def _shape_clauses(
    scenario: Mapping[str, Any],
    rows: list[Mapping[str, Any]],
    observations: Mapping[str, Any],
    selected_profile: str | None,
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
            or any(item.get("cache_profiles") not in ([], None) for item in login_for(worker))
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
            if (
                not _is_int(item.get("protocol"), minimum=1)
                or item["protocol"] > 43
            ):
                bad_logins.add(f"@instance:{item.get('instance', '?')}")
        clauses.append(
            _clause(
                "shape.negotiated-legacy",
                bool(logins) and not bad_logins,
                "all scheduler logins negotiate protocol 43 or older",
                bad_logins or ({"@observations:logins"} if not logins else set()),
            )
        )
    if shape == "S'FC'":
        bad_clients = {
            str(client)
            for client in clients
            if sidecar(client).get("sessions") != 0
        }
        clauses.append(
            _clause(
                "shape.client-zero-sessions",
                not bad_clients,
                "new clients open no P50 session against old workers",
                {f"@instance:{name}" for name in bad_clients},
            )
        )
    if shape == "S'C'F'":
        bad_workers = {
            str(worker)
            for worker in workers
            if not login_for(worker)
            or any(
                selected_profile not in (item.get("cache_profiles") or [])
                for item in login_for(worker)
            )
            or not _is_int(sidecar(worker).get("sessions"), minimum=1)
        }
        clauses.append(
            _clause(
                "shape.full-newgen-engagement",
                bool(workers) and not bad_workers,
                "every new worker advertises the selected profile and records a session",
                {f"@instance:{name}" for name in bad_workers}
                or ({"@scenario:workers"} if not workers else set()),
            )
        )
    if shape == "S'[FF'][CC']":
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
        mismatches = observations.get("wire_revision_mismatches")
        mismatches = mismatches if isinstance(mismatches, list) else []
        bad: set[str] = set()
        for index, item in enumerate(mismatches):
            if (
                not isinstance(item, Mapping)
                or item.get("error") != "WIRE_REVISION_MISMATCH"
                or item.get("fallback") != "legacy"
            ):
                bad.add(f"@wire-mismatch:{index}")
        clauses.append(
            _clause(
                "shape.revision-skew",
                bool(mismatches) and not bad,
                "revision skew is named and falls back to legacy",
                bad or ({"@observations:wire_revision_mismatches"} if not mismatches else set()),
            )
        )
    return clauses


def evaluate_bundle(bundle: Mapping[str, Any]) -> dict[str, Any]:
    """Evaluate an already-loaded bundle without consulting external state."""

    clauses: list[dict[str, Any]] = []
    if not isinstance(bundle, Mapping):
        clauses.append(_clause("bundle.schema", False, "bundle is not an object", {"@bundle"}))
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
        identifier = _job_id(row.get("job_id") if isinstance(row, Mapping) else None, f"@row:{index}")
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
            else "; ".join(f"{job}: {', '.join(errors)}" for job, errors in sorted(row_issues.items())),
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
    clauses.append(
        _clause(
            "scenario.profile",
            profile_error is None,
            "scenario resolves one product profile" if profile_error is None else profile_error,
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
        clauses.append(_clause("scenario.expect", False, "scenario.expect is invalid", {"@scenario:expect"}))
        return _finish(clauses)
    exact_bad: set[str] = set()
    if expect.get("exact") == "all":
        exact_bad = {
            _job_id(row["job_id"], "@row") for row in valid_rows if row["exact"] is not True
        }
    elif expect.get("exact") == "none":
        exact_bad = {
            _job_id(row["job_id"], "@row") for row in valid_rows if row["exact"] is not False
        }
    clauses.append(_clause("exact", not exact_bad, f"exactness expectation is {expect.get('exact')!r}", exact_bad))

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

    engagement_bad: set[str] = set()
    if expect.get("engagement") == "expected(c,f)" and profile_error is None:
        for row in valid_rows:
            expected = expected_profile(
                row,
                selected_profile,
                revisions if isinstance(revisions, Mapping) else {},
            )
            if row["tail_profile"] != expected:
                engagement_bad.add(_job_id(row["job_id"], "@row"))
    clauses.append(
        _clause(
            "engagement.expected",
            not engagement_bad,
            "each tail matches the configured client/worker pair law",
            engagement_bad,
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
        isinstance(item, Mapping) and item.get("action") in bounce_actions for item in timeline
    )
    retry_limit = observations.get("retry_limit_per_job") if bounce else 0
    retry_limit = retry_limit if _is_int(retry_limit) else -1
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
    error106_ids = {
        _job_id(item, "@error106") for item in error106
    } if isinstance(error106, list) else {"@observations:error106_job_ids"}
    error106_max = expect.get("error106_max")
    error106_ok = (
        isinstance(error106, list)
        and _is_int(error106_max)
        and len(error106_ids) <= error106_max
    )
    clauses.append(
        _clause(
            "error106.max",
            error106_ok,
            f"observed {len(error106_ids) if isinstance(error106, list) else 'invalid'} Error-106 jobs; maximum {error106_max!r}",
            error106_ids,
        )
    )

    incapable = {
        _job_id(row["job_id"], "@row")
        for row in valid_rows
        if row["tail_present"] and (row["client_version"] != 50 or row["cs_version"] != 50)
    }
    tail_limit = expect.get("tail_to_incapable")
    clauses.append(
        _clause(
            "tail.incapable",
            _is_int(tail_limit) and len(incapable) <= tail_limit,
            f"observed {len(incapable)} tails to incapable peers; maximum {tail_limit!r}",
            incapable or ({"@expect:tail_to_incapable"} if not _is_int(tail_limit) else set()),
        )
    )

    reuse_bad: set[str] = set()
    if expect.get("reuse") == "all-true-when-p29v1":
        reuse_bad = {
            _job_id(row["job_id"], "@row")
            for row in valid_rows
            if row["tail_profile"] == "P29V1" and row["reuse"] is not True
        }
    clauses.append(
        _clause(
            "reuse",
            not reuse_bad,
            f"reuse expectation is {expect.get('reuse')!r}",
            reuse_bad,
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
        wall_ok = isinstance(wall_max, (int, float)) and not isinstance(wall_max, bool) and _is_int(cell_wall)
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

    clauses.extend(_shape_clauses(scenario, valid_rows, observations, selected_profile))
    return _finish(clauses)


def _finish(clauses: list[dict[str, Any]]) -> dict[str, Any]:
    failed = [item for item in clauses if item["status"] == "FAIL"]
    return {
        "clauses": clauses,
        "offending_job_ids": _sorted_ids(
            {
                identifier
                for item in failed
                for identifier in item["offending_job_ids"]
            }
        ),
        "schema": VERDICT_SCHEMA,
        "status": "FAIL" if failed else "PASS",
    }


def evaluate_control(control_id: str, bundle: Mapping[str, Any]) -> dict[str, Any]:
    """Judge whether an H1--H5 mutation produced its designed observation."""

    verdict = evaluate_bundle(bundle)
    observations = bundle.get("observations", {}) if isinstance(bundle, Mapping) else {}
    fault = observations.get("fault", {}) if isinstance(observations, Mapping) else {}
    failed_ids = {
        item["id"] for item in verdict["clauses"] if item["status"] == "FAIL"
    }
    passed = False
    detail = "unknown control"
    if control_id == "H1":
        refusal = observations.get("preflight_refusal", {}) if isinstance(observations, Mapping) else {}
        passed = (
            isinstance(refusal, Mapping)
            and refusal.get("reason_code") == "role-hash-mismatch"
            and refusal.get("jobs_started") == 0
        )
        detail = "wrong role hash is refused before any job starts"
    elif control_id == "H2":
        passed = fault.get("client_kill_switch") is True and "engagement.expected" in failed_ids
        detail = "client kill switch makes the product verdict detect absent engagement"
    elif control_id == "H3":
        passed = fault.get("mutant_scheduler") is True and "tail.incapable" in failed_ids
        detail = "mutant scheduler makes the product verdict detect a tail to an incapable client"
    elif control_id == "H4":
        passed = fault.get("corrupt_object") is True and "objects.identity" in failed_ids
        detail = "corrupted object makes the product verdict fail identity"
    elif control_id == "H5":
        killed = {_job_id(item, "@fault") for item in fault.get("worker_killed_job_ids", [])} if isinstance(fault, Mapping) and isinstance(fault.get("worker_killed_job_ids"), list) else set()
        recovered = {_job_id(item, "@recovery") for item in observations.get("process_loss_recovery_job_ids", [])} if isinstance(observations, Mapping) and isinstance(observations.get("process_loss_recovery_job_ids"), list) else set()
        passed = bool(killed) and killed <= recovered and verdict["status"] == "PASS"
        detail = "worker loss is recorded as recovery and never as a wedge"
    clause = _clause(f"control.{control_id}", passed, detail, () if passed else {f"@control:{control_id}"})
    return {
        "clause": clause,
        "control": control_id,
        "product_verdict": verdict,
        "schema": CONTROL_VERDICT_SCHEMA,
        "status": "PASS" if passed else "FAIL",
    }
