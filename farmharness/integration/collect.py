"""Immutable evidence collection and fail-closed acceptance-row parsing."""

from __future__ import annotations

import hashlib
import json
import os
import re
import shutil
from collections import Counter, defaultdict
from collections.abc import Mapping
from pathlib import Path
from typing import Any

try:
    from .farm_spec import FarmSpec
    from .images import CommandFactory, RecordingTransport
    from .lifecycle import bundle_root, collect_diagnostics
    from .remote import PlannedCommand, RemoteError, docker_argv
    from .scenario_spec import ScenarioSpec
    from .schema_validation import canonical_bytes
    from .verdict import BUNDLE_SCHEMA, ROW_SCHEMA
except ImportError:  # Direct execution from this directory.
    from farm_spec import FarmSpec
    from images import CommandFactory, RecordingTransport
    from lifecycle import bundle_root, collect_diagnostics
    from remote import PlannedCommand, RemoteError, docker_argv
    from scenario_spec import ScenarioSpec
    from schema_validation import canonical_bytes
    from verdict import BUNDLE_SCHEMA, ROW_SCHEMA


SOURCE_RESULT_SCHEMA = "icecream-p50-source-result-v1"
COLLECT_SCHEMA = "icefarm-collect-v1"
SHA256_RE = re.compile(r"^[0-9a-f]{64}$")
PROFILE_RE = re.compile(
    r"\b(P29V1|ZSTD_TU|ZSTD_ROUTE) source committed for P50 CompileFile: "
    r"([0-9]+) exact bytes, TU sequence ([0-9]+)\b"
)
ATTACH_RE = re.compile(
    r"\bP50 CompileFile attached exact (P29V1|ZSTD_TU|ZSTD_ROUTE) input for job ([0-9]+)\b"
)
LOGIN_RE = re.compile(r"\bRELOGIN ([A-Za-z0-9][A-Za-z0-9._-]*)\([^)]*\):.*$")
CACHE_LOGIN_RE = re.compile(
    r"\bcache=([^ ]+) cache_wire=v([0-9]+) cache_protocol=([0-9]+) "
    r"cache_profiles=([a-z0-9_ ]+)\s*$"
)
SOURCE_RESULT_FIELDS = frozenset(
    {
        "schema",
        "wire_job_id",
        "logical_job",
        "assignment_epoch",
        "assignment_nonce",
        "c_store_guid",
        "profile",
        "status",
        "attempts",
        "tu_seq",
        "raw_bytes",
        "c_to_f_bytes",
        "f_to_c_bytes",
        "system_source_reuse",
    }
)
PROFILE_LABELS = {
    "p29v1": "P29V1",
    "zstd_tu": "ZSTD_TU",
    "zstd_route": "ZSTD_ROUTE",
}
GENERATED_ROOT_FILES = frozenset(
    ("bundle.json", "SHA256SUMS", "verdict.json", "EVIDENCE.md", "witness.json", "down.json")
)


class CollectError(RuntimeError):
    """Evidence is absent, ambiguous, mutable, or internally inconsistent."""


def _atomic_bytes(path: Path, value: bytes) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(path.name + f".tmp-{os.getpid()}")
    try:
        temporary.write_bytes(value)
        os.replace(temporary, path)
    finally:
        temporary.unlink(missing_ok=True)


def _atomic_json(path: Path, value: Any) -> None:
    _atomic_bytes(path, canonical_bytes(value))


def _sha256(path: Path) -> str:
    digest = hashlib.sha256()
    try:
        with path.open("rb") as handle:
            for block in iter(lambda: handle.read(1024 * 1024), b""):
                digest.update(block)
    except OSError as exc:
        raise CollectError(f"cannot hash {path}: {exc}") from exc
    return digest.hexdigest()


def _strict_object(text: str, source: str) -> dict[str, Any]:
    def pairs(values: list[tuple[str, Any]]) -> dict[str, Any]:
        result: dict[str, Any] = {}
        for key, value in values:
            if key in result:
                raise CollectError(f"{source}: duplicate JSON key {key!r}")
            result[key] = value
        return result

    try:
        value = json.loads(text, object_pairs_hook=pairs)
    except (json.JSONDecodeError, UnicodeError) as exc:
        raise CollectError(f"{source}: malformed JSON: {exc}") from exc
    if not isinstance(value, dict):
        raise CollectError(f"{source}: JSON value is not an object")
    return value


def _read_json(path: Path) -> dict[str, Any]:
    try:
        return _strict_object(path.read_text(encoding="utf-8"), str(path))
    except OSError as exc:
        raise CollectError(f"cannot read {path}: {exc}") from exc


def _read_jsonl(path: Path, *, required: bool = False) -> list[dict[str, Any]]:
    if not path.exists():
        if required:
            raise CollectError(f"required JSONL evidence is absent: {path}")
        return []
    try:
        lines = path.read_text(encoding="utf-8").splitlines()
    except OSError as exc:
        raise CollectError(f"cannot read {path}: {exc}") from exc
    if required and not lines:
        raise CollectError(f"required JSONL evidence is empty: {path}")
    return [
        _strict_object(line, f"{path}:{index}")
        for index, line in enumerate(lines, start=1)
        if line
    ]


def _copy_tree(source: Path, destination: Path) -> None:
    if not source.is_dir() or source.is_symlink():
        raise CollectError(f"evidence directory is absent or unsafe: {source}")
    if destination.exists():
        raise CollectError(f"immutable evidence destination already exists: {destination}")
    try:
        shutil.copytree(source, destination, symlinks=True)
    except OSError as exc:
        raise CollectError(f"cannot snapshot {source}: {exc}") from exc


def _validate_regular_tree(root: Path) -> None:
    if not root.is_dir() or root.is_symlink():
        raise CollectError(f"evidence root is absent or unsafe: {root}")
    for path in root.rglob("*"):
        if path.is_symlink():
            raise CollectError(f"symlink is forbidden in evidence: {path}")
        if not path.is_dir() and not path.is_file():
            raise CollectError(f"non-regular evidence object is forbidden: {path}")


def _command(
    factory: CommandFactory,
    *,
    phase: str,
    host: str,
    transport: str,
    timeout_s: int,
    argv: tuple[str, ...],
) -> PlannedCommand:
    return factory.make(
        phase=phase,
        host=host,
        transport=transport,
        timeout_s=timeout_s,
        argv=argv,
    )


def _docker_transport(farm: FarmSpec, host_name: str) -> str:
    return "docker-context" if farm.hosts[host_name].get("docker_context") else "ssh-docker"


def _snapshot_live_evidence(
    farm: FarmSpec,
    plan: dict[str, Any],
    destination: Path,
    recorder: RecordingTransport,
) -> None:
    factory = CommandFactory()
    diagnostics = destination / "diagnostics"
    timeout_s = plan["timeouts"]["collect_s"]
    container_ids: dict[str, str] = {}
    for instance in plan["topology"]["instances"]:
        host = instance["host"]
        name = instance["name"]
        container = f"icefarm-{plan['run_id']}-{name}"
        try:
            authenticated = recorder.invoke(
                _command(
                    factory,
                    phase="collect.authenticate",
                    host=host,
                    transport=_docker_transport(farm, host),
                    timeout_s=timeout_s,
                    argv=docker_argv(
                        farm,
                        host,
                        ("container", "inspect", "--format", "{{json .}}", container),
                    ),
                )
            )
            document = _strict_object(
                authenticated.stdout.strip(), f"container inspect for {host}:{name}"
            )
            labels = document.get("Config", {}).get("Labels")
            container_id = document.get("Id")
            if (
                not isinstance(labels, dict)
                or labels.get("icefarm.run") != plan["run_id"]
                or labels.get("icefarm.instance") != name
                or not isinstance(container_id, str)
                or re.fullmatch(r"[0-9a-f]{64}", container_id) is None
            ):
                raise CollectError(f"container identity is unauthenticated: {host}:{name}")
            container_ids[name] = container_id
            host_diagnostics = diagnostics / host
            host_diagnostics.mkdir(parents=True, exist_ok=True)
            for kind, args in (
                ("top", ("container", "top", container_id, "-eo", "pid,comm,args")),
                (
                    "stats",
                    (
                        "container",
                        "stats",
                        "--no-stream",
                        "--format",
                        "{{json .}}",
                        container_id,
                    ),
                ),
            ):
                result = recorder.invoke(
                    _command(
                        factory,
                        phase=f"collect.{kind}",
                        host=host,
                        transport=_docker_transport(farm, host),
                        timeout_s=min(timeout_s, 30),
                        argv=docker_argv(farm, host, args),
                    )
                )
                _atomic_bytes(
                    host_diagnostics / f"{name}.{kind}",
                    (result.stdout + result.stderr).encode("utf-8"),
                )
        except RemoteError as exc:
            raise CollectError(f"cannot collect {host}:{name}: {exc}") from exc

    # Freeze all writers before copying their logs and result trees.  The
    # authenticated container IDs close the name-replacement gap and the C/F/S
    # order prevents new work from entering while workers and the scheduler
    # drain.  `down` remains responsible for labelled removal and scratch
    # cleanup after offline verification.
    role_order = {"C": 0, "F": 1, "S": 2}
    for instance in sorted(
        plan["topology"]["instances"],
        key=lambda item: (role_order[item["role"]], item["name"]),
    ):
        host = instance["host"]
        name = instance["name"]
        try:
            recorder.invoke(
                _command(
                    factory,
                    phase="collect.stop",
                    host=host,
                    transport=_docker_transport(farm, host),
                    timeout_s=min(timeout_s, 30),
                    argv=docker_argv(
                        farm,
                        host,
                        ("container", "stop", "--time", "10", container_ids[name]),
                    ),
                )
            )
        except RemoteError as exc:
            raise CollectError(f"cannot freeze {host}:{name}: {exc}") from exc

    problems = collect_diagnostics(
        farm,
        plan,
        recorder,
        factory,
        diagnostics,
        container_ids=container_ids,
    )
    if problems:
        raise CollectError("diagnostic collection failed: " + "; ".join(problems))

    for instance in plan["topology"]["instances"]:
        host = instance["host"]
        name = instance["name"]
        result_dir = destination / "instances" / name / "results"
        result_dir.mkdir(parents=True, exist_ok=False)
        try:
            recorder.invoke(
                _command(
                    factory,
                    phase="collect.results",
                    host=host,
                    transport=_docker_transport(farm, host),
                    timeout_s=timeout_s,
                    argv=docker_argv(
                        farm,
                        host,
                        (
                            "container",
                            "cp",
                            f"{container_ids[name]}:/results/.",
                            str(result_dir),
                        ),
                    ),
                )
            )
        except RemoteError as exc:
            raise CollectError(f"cannot collect {host}:{name}: {exc}") from exc


def _snapshot_existing_evidence(root: Path, destination: Path, plan: dict[str, Any]) -> None:
    diagnostics = root / "diagnostics"
    if diagnostics.is_dir():
        _copy_tree(diagnostics, destination / "diagnostics")
    else:
        raise CollectError("existing collection has no diagnostics directory")
    for instance in plan["topology"]["instances"]:
        name = instance["name"]
        candidates = (
            root / f"{name}.results",
            root / "instances" / name / "results",
        )
        source = next((item for item in candidates if item.is_dir()), None)
        if source is None:
            raise CollectError(f"existing collection has no results for {name}")
        _copy_tree(source, destination / "instances" / name / "results")


def _load_receipts(root: Path, plan: dict[str, Any]) -> dict[str, dict[str, Any]]:
    result = {
        name: _read_json(root / f"{name}.json")
        for name in ("preflight", "lifecycle", "workload")
    }
    expected = {
        "farm_digest": plan["farm_digest"],
        "run_id": plan["run_id"],
        "scenario_digest": plan["scenario_digest"],
        "topology_digest": plan["topology_digest"],
    }
    for name, receipt in result.items():
        for field, value in expected.items():
            if receipt.get(field) != value:
                raise CollectError(f"{name}.json does not bind {field}")
    if result["lifecycle"].get("status") != "UP":
        down = root / "down.json"
        if not down.exists() or _read_json(down).get("status") != "DOWN":
            raise CollectError("collection requires an authenticated UP or completed DOWN run")
    if result["lifecycle"].get("plan") != plan:
        raise CollectError("lifecycle receipt plan differs from the current immutable plan")
    if not str(result["workload"].get("status", "")).startswith("COMPLETE"):
        raise CollectError("workload receipt is not terminal")
    return result


def _stage_evidence(
    farm: FarmSpec,
    scenario: ScenarioSpec,
    plan: dict[str, Any],
    root: Path,
    receipts: dict[str, dict[str, Any]],
    *,
    recorder: RecordingTransport | None,
    sync_remote: bool,
) -> Path:
    evidence = root / "evidence"
    if evidence.exists():
        _validate_regular_tree(evidence)
        return evidence
    temporary = root / f".evidence.tmp-{os.getpid()}"
    if temporary.exists():
        raise CollectError(f"stale collection staging directory exists: {temporary}")
    temporary.mkdir(parents=True)
    try:
        if sync_remote:
            _snapshot_live_evidence(
                farm, plan, temporary, recorder or RecordingTransport()
            )
        else:
            _snapshot_existing_evidence(root, temporary, plan)
        specs = temporary / "specs"
        _atomic_json(specs / "farm.json", farm.data)
        _atomic_json(specs / "scenario.json", scenario.data)
        _atomic_json(specs / "plan.json", plan)
        _atomic_json(specs / "topology.json", plan["topology"])
        receipt_dir = temporary / "receipts"
        for name, receipt in receipts.items():
            _atomic_json(receipt_dir / f"{name}.json", receipt)
        _validate_regular_tree(temporary)
        os.replace(temporary, evidence)
    except Exception:
        shutil.rmtree(temporary, ignore_errors=True)
        raise
    return evidence


def _instance_results(evidence: Path, name: str) -> Path:
    result = evidence / "instances" / name / "results"
    if not result.is_dir() or result.is_symlink():
        raise CollectError(f"results for {name} are absent or unsafe")
    return result


def _source_results(path: Path) -> dict[int, dict[str, Any]]:
    records: dict[int, dict[str, Any]] = {}
    for index, item in enumerate(_read_jsonl(path), start=1):
        if frozenset(item) != SOURCE_RESULT_FIELDS or item.get("schema") != SOURCE_RESULT_SCHEMA:
            raise CollectError(f"{path}:{index}: source-result schema mismatch")
        integers = (
            "wire_job_id",
            "logical_job",
            "assignment_epoch",
            "assignment_nonce",
            "status",
            "attempts",
            "tu_seq",
            "raw_bytes",
            "c_to_f_bytes",
            "f_to_c_bytes",
        )
        if any(type(item.get(field)) is not int or item[field] < 0 for field in integers):
            raise CollectError(f"{path}:{index}: source-result integer is invalid")
        if item["wire_job_id"] == 0 or item["logical_job"] == 0:
            raise CollectError(f"{path}:{index}: source-result job identity is zero")
        if item["status"] > 7 or item["attempts"] > 2:
            raise CollectError(f"{path}:{index}: source-result status/attempt count is invalid")
        if item.get("profile") not in PROFILE_LABELS.values():
            raise CollectError(f"{path}:{index}: source-result profile is invalid")
        if not isinstance(item.get("c_store_guid"), str) or re.fullmatch(
            r"[0-9a-f]{32}", item["c_store_guid"]
        ) is None or item["c_store_guid"] == "0" * 32:
            raise CollectError(f"{path}:{index}: source-result C GUID is invalid")
        reuse = item.get("system_source_reuse")
        if reuse is not None and type(reuse) is not bool:
            raise CollectError(f"{path}:{index}: source-result reuse is invalid")
        if item["profile"] != "P29V1" and reuse is not None:
            raise CollectError(f"{path}:{index}: non-P29V1 source-result has reuse evidence")
        if item["status"] == 0:
            if item["attempts"] == 0:
                raise CollectError(f"{path}:{index}: committed source-result has no attempt")
            if item["c_to_f_bytes"] == 0 or item["f_to_c_bytes"] == 0:
                raise CollectError(f"{path}:{index}: committed source-result has no wire bytes")
            if item["profile"] == "P29V1" and type(reuse) is not bool:
                raise CollectError(
                    f"{path}:{index}: committed P29V1 result has no reuse witness"
                )
        key = item["wire_job_id"]
        if key in records:
            raise CollectError(f"{path}: duplicate source result for scheduler job {key}")
        records[key] = item
    return records


def _action_commits(path: Path, actions: frozenset[str]) -> set[tuple[str, int]]:
    result: set[tuple[str, int]] = set()
    for index, item in enumerate(_read_jsonl(path), start=1):
        if item.get("action") not in actions:
            continue
        guid = item.get("c_store_guid")
        tu_seq = item.get("tu_seq")
        if not isinstance(guid, str) or re.fullmatch(r"[0-9a-f]{32}", guid) is None:
            raise CollectError(f"{path}:{index}: action C GUID is invalid")
        if type(tu_seq) is not int or tu_seq < 0:
            raise CollectError(f"{path}:{index}: action TU sequence is invalid")
        key = (guid, tu_seq)
        if key in result:
            raise CollectError(f"{path}: duplicate terminal action for {key}")
        result.add(key)
    return result


def _one_role_log(evidence: Path, instance: Mapping[str, Any]) -> Path | None:
    role_leaf = {"S": "scheduler.log", "C": "client-daemon.log", "F": "iceccd.log"}[
        str(instance["role"])
    ]
    root = evidence / "diagnostics" / str(instance["host"])
    matches = [path for path in root.rglob(role_leaf) if path.is_file() and not path.is_symlink()]
    if len(matches) > 1:
        raise CollectError(f"ambiguous {instance['name']} log: {matches!r}")
    return matches[0] if matches else None


def _text(path: Path | None) -> str:
    if path is None:
        return ""
    try:
        return path.read_text(encoding="utf-8", errors="replace")
    except OSError as exc:
        raise CollectError(f"cannot read {path}: {exc}") from exc


def _worker_attachments(
    evidence: Path, topology_instances: list[dict[str, Any]]
) -> dict[tuple[str, int], str]:
    result: dict[tuple[str, int], str] = {}
    for instance in topology_instances:
        if instance["role"] != "F":
            continue
        for profile, job in ATTACH_RE.findall(_text(_one_role_log(evidence, instance))):
            key = (instance["name"], int(job))
            if key in result and result[key] != profile:
                raise CollectError(f"worker attachment changed profile for {key}")
            result[key] = profile
    return result


def _event_log(evidence: Path) -> list[dict[str, Any]]:
    path = evidence / "events" / "events.json"
    if not path.exists():
        return []
    value = _read_json(path)
    events = value.get("events")
    if not isinstance(events, list):
        raise CollectError("events.json has no event list")
    previous = -1
    for index, event in enumerate(events):
        if not isinstance(event, dict) or type(event.get("fired_ms")) is not int:
            raise CollectError(f"events.json event {index} is invalid")
        if event["fired_ms"] < previous:
            raise CollectError("events.json is not chronological")
        previous = event["fired_ms"]
    return events


def _epoch_at(events: list[dict[str, Any]], dispatch_ms: int) -> int:
    return sum(event["fired_ms"] <= dispatch_ms for event in events)


def _job_result(path: Path) -> dict[str, Any]:
    try:
        lines = path.read_text(encoding="utf-8").splitlines()
    except OSError as exc:
        raise CollectError(f"cannot read {path}: {exc}") from exc
    if len(lines) != 1:
        raise CollectError(f"{path}: result must contain exactly one row")
    fields = lines[0].split("\t")
    if len(fields) != 14:
        raise CollectError(f"{path}: result has {len(fields)} fields, expected 14")
    (
        index,
        turn,
        occurrence,
        relative,
        scheduler_job,
        worker,
        started,
        finished,
        compile_rc,
        remote_sha,
        local_sha,
        exact,
        remote,
        retries,
    ) = fields
    integers: dict[str, int] = {}
    for name, value in (
        ("index", index),
        ("occurrence", occurrence),
        ("started", started),
        ("finished", finished),
        ("compile_rc", compile_rc),
        ("exact", exact),
        ("remote", remote),
        ("retries", retries),
    ):
        if not value.isdigit():
            raise CollectError(f"{path}: {name} is not a non-negative integer")
        integers[name] = int(value)
    if integers["index"] < 1 or integers["finished"] <= integers["started"]:
        raise CollectError(f"{path}: job index/timestamps are invalid")
    if integers["exact"] not in (0, 1) or integers["remote"] not in (0, 1):
        raise CollectError(f"{path}: exact/remote flag is invalid")
    if SHA256_RE.fullmatch(remote_sha) is None or SHA256_RE.fullmatch(local_sha) is None:
        raise CollectError(f"{path}: object digest is invalid")
    recomputed_exact = integers["compile_rc"] == 0 and remote_sha == local_sha
    if bool(integers["exact"]) != recomputed_exact:
        raise CollectError(f"{path}: exact flag disagrees with rc/object hashes")
    if not turn or not relative or not scheduler_job or not worker:
        raise CollectError(f"{path}: job identity is incomplete")
    return {
        **integers,
        "turn": turn,
        "relative": relative,
        "scheduler_job": scheduler_job,
        "worker": worker,
        "remote_sha": remote_sha,
        "local_sha": local_sha,
    }


def _profile_marker(job_dir: Path) -> tuple[str, int, int] | None:
    content = _text(job_dir / "client-debug.log") + "\n" + _text(
        job_dir / "client-output.log"
    )
    matches = PROFILE_RE.findall(content)
    unique = {(profile, int(raw), int(tu)) for profile, raw, tu in matches}
    if len(unique) > 1:
        raise CollectError(f"{job_dir}: conflicting P50 commit markers")
    return next(iter(unique)) if unique else None


def _endpoint_workers(plan: dict[str, Any]) -> dict[str, dict[str, Any]]:
    result: dict[str, dict[str, Any]] = {}
    for instance in plan["topology"]["instances"]:
        if instance["role"] != "F":
            continue
        endpoint = f"{instance['address']}:{plan['ports']['instances'][instance['name']]}"
        if endpoint in result:
            raise CollectError(f"duplicate worker endpoint {endpoint}")
        result[endpoint] = instance
    return result


def _parse_rows(
    scenario: ScenarioSpec,
    plan: dict[str, Any],
    evidence: Path,
    events: list[dict[str, Any]],
) -> tuple[list[dict[str, Any]], dict[str, Any]]:
    topology = plan["topology"]["instances"]
    by_name = {item["name"]: item for item in topology}
    workers = _endpoint_workers(plan)
    attachments = _worker_attachments(evidence, topology)
    f_commits = {
        item["name"]: _action_commits(
            _instance_results(evidence, item["name"]) / "f-action.jsonl",
            frozenset(("INPUT_COMMITTED",)),
        )
        for item in topology
        if item["role"] == "F"
    }
    rows: list[dict[str, Any]] = []
    raw_jobs: list[dict[str, Any]] = []
    compile_failures: list[str] = []
    local_fallbacks: list[str] = []
    error106: list[str] = []

    for client_name in scenario.data["workload"]["clients"]:
        client = by_name[client_name]
        results = _instance_results(evidence, client_name)
        source_results = _source_results(results / "source-result.jsonl")
        c_commits = _action_commits(
            results / "c-action.jsonl",
            frozenset(("COMMIT_ACCEPTED", "LOST_COMMIT_ACCEPTED")),
        )
        job_paths = sorted((results / "workload" / "jobs").glob("*/result.tsv"))
        if not job_paths:
            raise CollectError(f"client {client_name} has no workload job rows")
        for path in job_paths:
            raw = _job_result(path)
            raw_jobs.append({"client": client_name, **raw})
            if not raw["scheduler_job"].isdigit():
                raise CollectError(f"{path}: scheduler job id is not numeric")
            scheduler_job = int(raw["scheduler_job"])
            worker = workers.get(raw["worker"])
            if worker is None:
                raise CollectError(f"{path}: assignment names unknown worker {raw['worker']!r}")
            job_id = f"{client_name}:{scheduler_job}"
            job_dir = path.parent
            log_text = _text(job_dir / "client-debug.log") + "\n" + _text(
                job_dir / "client-output.log"
            )
            marker = _profile_marker(job_dir)
            source = source_results.get(scheduler_job)
            if marker is not None and source is None:
                raise CollectError(
                    f"{job_id}: P50 commit marker has no exact source-result witness"
                )
            tail_present = source is not None
            profile = source["profile"] if source is not None else None
            outcome = "none"
            reuse: bool | None = None
            c_to_f = 0
            f_to_c = 0
            transfer_retries = 0
            if source is not None:
                if marker is None:
                    outcome = "refused"
                else:
                    marker_profile, marker_raw, marker_tu = marker
                    key = (source["c_store_guid"], source["tu_seq"])
                    attached = attachments.get((worker["name"], scheduler_job))
                    committed = (
                        source["status"] == 0
                        and source["attempts"] >= 1
                        and marker_profile == profile
                        and marker_raw == source["raw_bytes"]
                        and marker_tu == source["tu_seq"]
                        and key in c_commits
                        and key in f_commits[worker["name"]]
                        and attached == profile
                    )
                    outcome = "committed" if committed else "refused"
                reuse = source["system_source_reuse"] if profile == "P29V1" else None
                c_to_f = source["c_to_f_bytes"]
                f_to_c = source["f_to_c_bytes"]
                transfer_retries = max(0, source["attempts"] - 1)
            if raw["compile_rc"] != 0:
                compile_failures.append(job_id)
            if raw["remote"] != 1:
                local_fallbacks.append(job_id)
            if "Error 106" in log_text or "Error106" in log_text:
                error106.append(job_id)
            rows.append(
                {
                    "c_to_f_bytes": c_to_f,
                    "client_instance": client_name,
                    "client_version": client["version"],
                    "cs": worker["name"],
                    "cs_version": worker["version"],
                    "event_epoch": _epoch_at(events, raw["started"]),
                    "exact": bool(raw["exact"]),
                    "f_to_c_bytes": f_to_c,
                    "job_id": job_id,
                    "object_sha_local": raw["local_sha"],
                    "object_sha_remote": raw["remote_sha"],
                    "retries": max(raw["retries"], transfer_retries),
                    "reuse": reuse,
                    "schema": ROW_SCHEMA,
                    "session_outcome": outcome,
                    "tail_present": tail_present,
                    "tail_profile": profile,
                    "tu": raw["relative"],
                    "wall_ms": raw["finished"] - raw["started"],
                }
            )
    identifiers = [row["job_id"] for row in rows]
    duplicates = [job for job, count in Counter(identifiers).items() if count > 1]
    if duplicates:
        raise CollectError(f"acceptance job ids are not unique: {sorted(duplicates)!r}")
    return rows, {
        "compile_failure_job_ids": sorted(compile_failures),
        "error106_job_ids": sorted(error106),
        "local_fallback_job_ids": sorted(local_fallbacks),
        "raw_jobs": raw_jobs,
    }


def _parse_logins(
    evidence: Path, plan: dict[str, Any]
) -> tuple[list[dict[str, Any]], dict[str, int], dict[str, list[int]]]:
    topology = plan["topology"]["instances"]
    scheduler = next(item for item in topology if item["role"] == "S")
    log = _text(_one_role_log(evidence, scheduler))
    by_name = {item["name"]: item for item in topology}
    latest: dict[str, dict[str, Any]] = {}
    revisions: dict[str, int] = {}
    ports: dict[str, list[int]] = defaultdict(list)
    for line in log.splitlines():
        login = LOGIN_RE.search(line)
        if login is None or login.group(1) not in by_name:
            continue
        name = login.group(1)
        instance = by_name[name]
        cache = CACHE_LOGIN_RE.search(line)
        if cache is None:
            if instance["version"] < 50 or "cache=off" in line:
                # A clean cache-service shutdown causes the daemon to publish
                # a final cache=off RELOGIN while collection freezes C/F/S.
                # Preserve any earlier authenticated capability login; for a
                # genuinely legacy worker, a pre-50 RELOGIN naturally has no
                # cache fields and is normalized to the same empty surface.
                latest.setdefault(
                    name,
                    {
                        "cache_profiles": [],
                        "instance": name,
                        "protocol": instance["version"],
                    },
                )
            continue
        endpoint, revision, _cache_protocol, profile_text = cache.groups()
        profiles = []
        for raw in profile_text.split():
            profile = PROFILE_LABELS.get(raw)
            if profile is None:
                raise CollectError(f"scheduler login names unknown profile {raw!r}")
            profiles.append(profile)
        latest[name] = {
            "cache_profiles": profiles,
            "instance": name,
            "protocol": instance["version"],
        }
        revisions[name] = int(revision)
        port_text = endpoint.rsplit(":", 1)[-1]
        if not port_text.isdigit() or not (1 <= int(port_text) <= 65535):
            raise CollectError(f"scheduler login has invalid cache endpoint {endpoint!r}")
        ports[name] = [int(port_text)]
    return [latest[name] for name in sorted(latest)], revisions, dict(ports)


def _oracle(evidence: Path, scenario: ScenarioSpec) -> dict[str, Any]:
    sample_total = 0
    mismatches: list[str] = []
    for client in scenario.data["workload"]["clients"]:
        root = _instance_results(evidence, client) / "workload"
        summary: dict[str, int] = {}
        try:
            lines = (root / "oracle-summary.tsv").read_text(encoding="utf-8").splitlines()
        except OSError as exc:
            raise CollectError(f"cannot read oracle summary for {client}: {exc}") from exc
        for line in lines:
            fields = line.split("\t")
            if len(fields) != 2 or not fields[1].isdigit() or fields[0] in summary:
                raise CollectError(f"oracle summary for {client} is malformed")
            summary[fields[0]] = int(fields[1])
        if set(summary) != {"sample_total", "sample_mismatches"}:
            raise CollectError(f"oracle summary for {client} has the wrong fields")
        sample_total += summary["sample_total"]
        try:
            samples = (root / "oracle-samples.tsv").read_text(encoding="utf-8").splitlines()
        except OSError as exc:
            raise CollectError(f"cannot read oracle samples for {client}: {exc}") from exc
        observed_mismatches = 0
        for index, line in enumerate(samples, start=1):
            fields = line.split("\t")
            if (
                len(fields) != 4
                or SHA256_RE.fullmatch(fields[1]) is None
                or SHA256_RE.fullmatch(fields[2]) is None
                or fields[3] not in ("0", "1")
            ):
                raise CollectError(f"oracle sample {client}:{index} is malformed")
            exact = fields[1] == fields[2]
            if (fields[3] == "1") != exact:
                raise CollectError(f"oracle sample {client}:{index} exact flag disagrees")
            if not exact:
                observed_mismatches += 1
                mismatches.append(f"oracle:{client}:{fields[0]}")
        if len(samples) != summary["sample_total"] or observed_mismatches != summary["sample_mismatches"]:
            raise CollectError(f"oracle summary for {client} disagrees with samples")
    return {"sample_mismatch_job_ids": sorted(mismatches), "sample_total": sample_total}


def _turn_completeness(
    scenario: ScenarioSpec, plan: dict[str, Any], raw_jobs: list[dict[str, Any]]
) -> list[str]:
    authority = plan["topology"]["corpus_authority"]
    per_turn = authority["tus"] * authority.get("repeat", 1) * scenario.data["workload"]["repeat"]
    observed = Counter((item["client"], item["turn"]) for item in raw_jobs)
    incomplete: list[str] = []
    for client in scenario.data["workload"]["clients"]:
        for turn in scenario.data["workload"]["turns"]:
            if observed[(client, turn)] != per_turn:
                incomplete.append(f"{client}:{turn}")
    extras = set(observed) - {
        (client, turn)
        for client in scenario.data["workload"]["clients"]
        for turn in scenario.data["workload"]["turns"]
    }
    incomplete.extend(f"unexpected:{client}:{turn}" for client, turn in sorted(extras))
    return incomplete


def _process_count(evidence: Path, instance: Mapping[str, Any], sessions: int) -> int:
    path = evidence / "diagnostics" / str(instance["host"]) / f"{instance['name']}.top"
    text = _text(path if path.exists() else None)
    matches = 0
    for line in text.splitlines():
        fields = line.split(maxsplit=2)
        if len(fields) >= 2 and fields[1] in (
            "icecc-cache-ser",
            "icecc-cache-service",
            "p50cacheservice",
        ):
            matches += 1
    if matches:
        return matches
    # An F action SESSION_OPENED can only be emitted by the cache-service
    # endpoint, and remains an exact process-presence witness for older bundles
    # which predate the `docker top` sample.
    return 1 if sessions > 0 else 0


def _observations(
    farm: FarmSpec,
    scenario: ScenarioSpec,
    plan: dict[str, Any],
    evidence: Path,
    rows: list[dict[str, Any]],
    row_facts: dict[str, Any],
) -> dict[str, Any]:
    logins, revisions, cache_ports = _parse_logins(evidence, plan)
    topology = plan["topology"]["instances"]
    for row in rows:
        if row["tail_present"]:
            worker_revision = revisions.get(row["cs"])
            if worker_revision is not None:
                previous = revisions.setdefault(row["client_instance"], worker_revision)
                if previous != worker_revision:
                    raise CollectError(
                        f"client {row['client_instance']} negotiated multiple wire revisions"
                    )
    sidecars: dict[str, dict[str, Any]] = {}
    for instance in topology:
        result_root = _instance_results(evidence, instance["name"])
        sessions = 0
        if instance["role"] == "F":
            sessions = sum(
                item.get("action") == "SESSION_OPENED"
                for item in _read_jsonl(result_root / "f-action.jsonl")
            )
        sidecars[instance["name"]] = {
            "cache_ports": cache_ports.get(instance["name"], []),
            "process_count": _process_count(evidence, instance, sessions),
            "sessions": sessions,
        }
    raw_jobs = row_facts.pop("raw_jobs")
    lifecycle = []
    row_by_identity = {
        (row["client_instance"], row["job_id"].split(":", 1)[1]): row for row in rows
    }
    for raw in raw_jobs:
        row = row_by_identity[(raw["client"], raw["scheduler_job"])]
        terminal = "completion" if raw["compile_rc"] == 0 else "cancellation"
        lifecycle.append(
            {
                "deadline_ms": raw["started"] + scenario.data["timeouts"]["turn_s"] * 1000,
                "dispatch_ms": raw["started"],
                "job_id": row["job_id"],
                "terminal": terminal,
                "terminal_ms": raw["finished"],
                "turn": raw["turn"],
            }
        )
    starts = [item["started"] for item in raw_jobs]
    finishes = [item["finished"] for item in raw_jobs]
    preflight = _read_json(evidence / "receipts" / "preflight.json")
    return {
        "cell_wall_ms": max(finishes) - min(starts),
        **row_facts,
        "incomplete_turns": _turn_completeness(scenario, plan, raw_jobs),
        "job_lifecycle": lifecycle,
        "logins": logins,
        "oracle": _oracle(evidence, scenario),
        "protected_before": {
            host: facts.get("protected", {}) for host, facts in preflight["hosts"].items()
        },
        "sidecars": sidecars,
        "wire_revisions": revisions,
    }


def _write_derived(
    evidence: Path,
    rows: list[dict[str, Any]],
    observations: dict[str, Any],
    events: list[dict[str, Any]],
) -> None:
    derived = evidence / "derived"
    rows_bytes = b"".join(canonical_bytes(row) for row in rows)
    values = {
        derived / "rows.jsonl": rows_bytes,
        derived / "observations.json": canonical_bytes(observations),
        derived / "events.json": canonical_bytes({"events": events}),
    }
    for path, value in values.items():
        if path.exists() and path.read_bytes() != value:
            raise CollectError(f"immutable derived evidence changed: {path}")
        if not path.exists():
            _atomic_bytes(path, value)


def _evidence_artifacts(root: Path) -> dict[str, str]:
    evidence = root / "evidence"
    _validate_regular_tree(evidence)
    result: dict[str, str] = {}
    for path in sorted(item for item in evidence.rglob("*") if item.is_file()):
        relative = path.relative_to(root).as_posix()
        if "\n" in relative or "\r" in relative or "  " in relative:
            raise CollectError(f"unsafe checksum path {relative!r}")
        result[relative] = _sha256(path)
    if not result:
        raise CollectError("evidence tree contains no artifacts")
    return result


def _write_checksums(root: Path, artifacts: Mapping[str, str]) -> str:
    content = "".join(f"{digest}  {path}\n" for path, digest in sorted(artifacts.items()))
    _atomic_bytes(root / "SHA256SUMS", content.encode("utf-8"))
    return hashlib.sha256(content.encode("utf-8")).hexdigest()


def collect_bundle(
    farm: FarmSpec,
    scenario: ScenarioSpec,
    plan: dict[str, Any],
    *,
    recorder: RecordingTransport | None = None,
    sync_remote: bool = True,
) -> dict[str, Any]:
    """Collect one run into an immutable, offline-verifiable bundle."""

    root = bundle_root(farm, plan["run_id"])
    root.mkdir(parents=True, exist_ok=True)
    if (root / "bundle.json").exists():
        bundle = load_verified_bundle(root)
        if (
            bundle.get("farm_digest") != farm.digest
            or bundle.get("scenario_digest") != scenario.digest
            or bundle.get("topology_digest") != plan["topology_digest"]
        ):
            raise CollectError("existing immutable bundle belongs to another plan")
        return bundle
    receipts = _load_receipts(root, plan)
    evidence = _stage_evidence(
        farm,
        scenario,
        plan,
        root,
        receipts,
        recorder=recorder,
        sync_remote=sync_remote,
    )
    events = _event_log(evidence)
    rows, row_facts = _parse_rows(scenario, plan, evidence, events)
    observations = _observations(farm, scenario, plan, evidence, rows, row_facts)
    _write_derived(evidence, rows, observations, events)
    artifacts = _evidence_artifacts(root)
    sums_sha = _write_checksums(root, artifacts)
    preflight = receipts["preflight"]
    bundle = {
        "artifacts": artifacts,
        "checksum_policy": {
            "root": "evidence/",
            "sha256sums_sha256": sums_sha,
        },
        "event_log": events,
        "farm": farm.data,
        "farm_digest": farm.digest,
        "images": preflight["images"],
        "instances": plan["topology"]["instances"],
        "observations": observations,
        "plan": plan,
        "rows": rows,
        "run_id": plan["run_id"],
        "scenario": scenario.data,
        "scenario_digest": scenario.digest,
        "schema": BUNDLE_SCHEMA,
        "topology": plan["topology"],
        "topology_digest": plan["topology_digest"],
    }
    _atomic_json(root / "bundle.json", bundle)
    return load_verified_bundle(root)


def _manifest(path: Path) -> dict[str, str]:
    try:
        lines = path.read_text(encoding="utf-8").splitlines()
    except OSError as exc:
        raise CollectError(f"cannot read checksum manifest: {exc}") from exc
    result: dict[str, str] = {}
    for index, line in enumerate(lines, start=1):
        if len(line) < 67 or line[64:66] != "  ":
            raise CollectError(f"SHA256SUMS:{index}: malformed row")
        digest, relative = line[:64], line[66:]
        if SHA256_RE.fullmatch(digest) is None or not relative.startswith("evidence/"):
            raise CollectError(f"SHA256SUMS:{index}: invalid digest/path")
        pure = Path(relative)
        if pure.is_absolute() or ".." in pure.parts or relative in result:
            raise CollectError(f"SHA256SUMS:{index}: unsafe or duplicate path")
        result[relative] = digest
    return result


def load_verified_bundle(root: Path | str) -> dict[str, Any]:
    """Verify immutable bytes and their derived bundle bindings before use."""

    path = Path(root)
    bundle = _read_json(path / "bundle.json")
    if bundle.get("schema") != BUNDLE_SCHEMA:
        raise CollectError("bundle schema is invalid")
    manifest_path = path / "SHA256SUMS"
    artifacts = _manifest(manifest_path)
    observed = _evidence_artifacts(path)
    if artifacts != observed:
        missing = sorted(set(artifacts) - set(observed))
        extra = sorted(set(observed) - set(artifacts))
        changed = sorted(
            item for item in set(artifacts) & set(observed) if artifacts[item] != observed[item]
        )
        raise CollectError(
            f"bundle checksum mismatch missing={missing!r} extra={extra!r} changed={changed!r}"
        )
    if bundle.get("artifacts") != artifacts:
        raise CollectError("bundle artifact index differs from SHA256SUMS")
    policy = bundle.get("checksum_policy")
    if not isinstance(policy, dict) or policy.get("root") != "evidence/":
        raise CollectError("bundle checksum policy is invalid")
    if policy.get("sha256sums_sha256") != _sha256(manifest_path):
        raise CollectError("bundle does not bind SHA256SUMS")
    bindings = {
        "farm": "farm.json",
        "scenario": "scenario.json",
        "plan": "plan.json",
        "topology": "topology.json",
    }
    for field, leaf in bindings.items():
        snapshot = _read_json(path / "evidence" / "specs" / leaf)
        if bundle.get(field) != snapshot:
            raise CollectError(f"bundle {field} differs from its immutable snapshot")
    plan = bundle["plan"]
    topology = bundle["topology"]
    plan_bindings = {
        "farm_digest": bundle.get("farm_digest"),
        "run_id": bundle.get("run_id"),
        "scenario_digest": bundle.get("scenario_digest"),
        "topology_digest": bundle.get("topology_digest"),
    }
    for field, expected in plan_bindings.items():
        if plan.get(field) != expected:
            raise CollectError(f"bundle {field} differs from its immutable plan")
    if plan.get("topology") != topology:
        raise CollectError("bundle topology differs from its immutable plan")
    if topology.get("topology_digest") != bundle.get("topology_digest"):
        raise CollectError("bundle topology digest differs from its topology")
    if bundle.get("instances") != topology.get("instances"):
        raise CollectError("bundle instances differ from its immutable topology")
    receipt_bindings = {
        "preflight": _read_json(path / "evidence" / "receipts" / "preflight.json"),
        "lifecycle": _read_json(path / "evidence" / "receipts" / "lifecycle.json"),
        "workload": _read_json(path / "evidence" / "receipts" / "workload.json"),
    }
    for name, receipt in receipt_bindings.items():
        for field, expected in plan_bindings.items():
            if receipt.get(field) != expected:
                raise CollectError(f"{name} receipt does not bind bundle {field}")
    if receipt_bindings["lifecycle"].get("plan") != plan:
        raise CollectError("lifecycle receipt plan differs from the immutable plan")
    if bundle.get("images") != receipt_bindings["preflight"].get("images"):
        raise CollectError("bundle images differ from the immutable preflight receipt")
    if hashlib.sha256(canonical_bytes(bundle["farm"])).hexdigest() != bundle.get("farm_digest"):
        raise CollectError("bundle farm digest is not reproducible")
    if hashlib.sha256(canonical_bytes(bundle["scenario"])).hexdigest() != bundle.get(
        "scenario_digest"
    ):
        raise CollectError("bundle scenario digest is not reproducible")
    rows = _read_jsonl(path / "evidence" / "derived" / "rows.jsonl", required=True)
    observations = _read_json(path / "evidence" / "derived" / "observations.json")
    events = _read_json(path / "evidence" / "derived" / "events.json").get("events")
    if bundle.get("rows") != rows or bundle.get("observations") != observations:
        raise CollectError("bundle derived values differ from immutable evidence")
    if bundle.get("event_log") != events:
        raise CollectError("bundle event log differs from immutable evidence")
    return bundle
