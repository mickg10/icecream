"""Immutable evidence collection and fail-closed acceptance-row parsing."""

from __future__ import annotations

import hashlib
import ipaddress
import json
import os
import re
import shutil
from collections import Counter, defaultdict
from collections.abc import Mapping
from datetime import datetime, timezone
from pathlib import Path, PurePosixPath
from typing import Any

try:
    from .client_epoch import CLIENT_ROUTE_EPOCH_CONTRACT, client_route_epoch
    from .farm_spec import FarmSpec
    from .images import CommandFactory, RecordingTransport
    from .lifecycle import bundle_root, collect_diagnostics, f_runtime_host_config_valid
    from .layout import instance_root, runtime_root
    from .mutant import (
        MUTANT_TRACE_PATH,
        MUTANT_ARM_PATH, MUTANT_ARM_CONTRACT, h3_workload_dispatches,
        scheduler_mutant_requires_arming,
        MutantError,
        parse_h3_client_rejections,
        validate_h3_trace,
    )
    from .netem import NetemPlanError, validate_receipt as validate_netem_receipt
    from .remote import PlannedCommand, RemoteError, docker_argv
    from .retry_decision import same_endpoint_decision_valid
    from .scenario_spec import ScenarioSpec
    from .schema_validation import canonical_bytes
    from .verdict import BUNDLE_SCHEMA, ROW_SCHEMA, S70_B5_ENGAGEMENT
except ImportError:  # Direct execution from this directory.
    from client_epoch import CLIENT_ROUTE_EPOCH_CONTRACT, client_route_epoch
    from farm_spec import FarmSpec
    from images import CommandFactory, RecordingTransport
    from lifecycle import bundle_root, collect_diagnostics, f_runtime_host_config_valid
    from layout import instance_root, runtime_root
    from mutant import (
        MUTANT_TRACE_PATH,
        MUTANT_ARM_PATH, MUTANT_ARM_CONTRACT, h3_workload_dispatches,
        scheduler_mutant_requires_arming,
        MutantError,
        parse_h3_client_rejections,
        validate_h3_trace,
    )
    from netem import NetemPlanError, validate_receipt as validate_netem_receipt
    from remote import PlannedCommand, RemoteError, docker_argv
    from retry_decision import same_endpoint_decision_valid
    from scenario_spec import ScenarioSpec
    from schema_validation import canonical_bytes
    from verdict import BUNDLE_SCHEMA, ROW_SCHEMA, S70_B5_ENGAGEMENT


SOURCE_RESULT_SCHEMA = "icecream-p50-source-result-v2"
P29_INTERNER_FAULT_SCHEMA = "icecream-p50-fault-v1"
P29_INTERNER_FAULT = "p29-interner-fail-once"
P29_INTERNER_FAULT_OUTCOME = "fired"
P29_INTERNER_FAULT_FIELDS = frozenset(("schema", "fault", "outcome"))
P29_ACTION_LINEAGE_SCHEMA = "icefarm-p29-action-lineage-v1"
P29_ACTION_LINEAGE_CONTRACT = "p29-action-lineage-v1"
WORKER_REJOIN_EPOCH_CONTRACT = "icefarm-worker-rejoin-log-order-v1"
ZERO_GUID = "0" * 32
LEGACY_WIRE_SCHEMA = "icecream-p50-legacy-wire-v1"
LEGACY_WIRE_FIELDS = frozenset(
    {
        "schema",
        "role",
        "job_id",
        "assignment_epoch",
        "assignment_nonce",
        "c_guid",
        "tu_seq",
        "c_to_f_sent_bytes",
        "c_to_f_received_bytes",
        "f_to_c_sent_bytes",
        "f_to_c_received_bytes",
    }
)
COMPILE_IDENTITY_FIELDS = frozenset(
    {"record", "job_id", "assignment_epoch", "assignment_nonce", "c_guid", "tu_seq"}
)
COLLECT_SCHEMA = "icefarm-collect-v1"
SHA256_RE = re.compile(r"^[0-9a-f]{64}$")
PROFILE_RE = re.compile(
    r"\b(P29V1|ZSTD_TU|ZSTD_ROUTE) source committed for P50 CompileFile: "
    r"([0-9]+) exact bytes, TU sequence ([0-9]+)\b"
)
ATTACH_RE = re.compile(
    r"\bP50 CompileFile attached exact (P29V1|ZSTD_TU|ZSTD_ROUTE) input for job ([0-9]+)\b"
)
LEGACY_WIRE_BIND_RE = re.compile(
    r"\blegacy wire identity bound for job ([0-9]+) epoch ([0-9]+) "
    r"nonce ([0-9]+) c_guid ([0-9]+) tu_seq ([0-9]+) "
    r"origin (client-local|scheduler)\b"
)
LOGIN_RE = re.compile(r"\bRELOGIN ([A-Za-z0-9][A-Za-z0-9._-]*)\([^)]*\):.*$")
ROLE_LOGIN_RE = re.compile(
    r"\blogin\s+([A-Za-z0-9][A-Za-z0-9._-]*)\s+protocol\s+version:\s*([0-9]+)\b"
)
CACHE_LOGIN_RE = re.compile(
    r"\bcache=([^ ]+) cache_wire=v([0-9]+) cache_protocol=([0-9]+) "
    r"cache_profiles=([a-z0-9_ ]+)\s*$"
)
WARM_HINT_OVERRIDE_RE = re.compile(
    r"\bP50_WARM_HINT_OVERRIDE job=([0-9]+) warm=([0-9]+) "
    r"compatible_free=([0-9]+) idle_excluded=([0-9]+)$"
)
CLIENT_ASSIGNMENT_RE = re.compile(r"\bHave to use host ([^ ]+) - Job ID: ([0-9]+)\b")
P50_ASSIGNMENT_IDENTITY_RE = re.compile(
    r"\bP50 assignment identity bound for job ([0-9]+) epoch ([0-9]+) "
    r"nonce ([0-9]+) c_guid ([0-9]+) tu_seq ([0-9]+)\b"
)
P50_NORMALIZED_ERROR106_RE = re.compile(
    r"\bnormalizing P50 client error ([0-9]+) to Error 106 for a fresh assignment\b"
)
P50_RETRYABLE_TRANSPORT_ERRORS = frozenset((2, 6, 8, 9, 13, 14, 19, 20, 23, 107))
P50_RESOURCE_FAILURE_RE = re.compile(
    r"\bP50 worker resource failure normalized to Error 106 for job ([0-9]+) "
    r"epoch ([0-9]+) nonce ([0-9]+) c_guid ([0-9]+) tu_seq ([0-9]+)$"
)
P50_SOURCE_TRANSFER_FAILED_RE = re.compile(
    r"\b(P29V1|ZSTD_TU|ZSTD_ROUTE) cache source transfer failed closed "
    r"\(status ([0-9]+), error ([0-9]+), attempts ([0-9]+)\)\s*$"
)
P50_STRICT_RETRY_REQUEST_RE = re.compile(
    r"\bP50 assignment failed; requesting one fresh strict-P50 remote "
    r"assignment; avoiding failed endpoint ([^ ]+)\s*$"
)
P50_LEGACY_RETRY_REQUEST_RE = re.compile(
    r"\bP50 assignment failed; requesting one fresh legacy remote assignment\s*$"
)
P50_NO_CACHE_HANDOFF_RE = re.compile(
    r"\blocal build forced by remote exception: "
    r"Error 105 - strict all-P50 assignment has no cache handoff\s*$"
)
REMOTE_ONLY_REFUSAL_RE = re.compile(r"\bremote-only policy refuses local retry\s*$")
UNEXPECTED_USECS_ERROR_RE = re.compile(
    r"\bgot exception Error 1 - expected use_cs reply, but got UNKNOWN instead\b"
)
REMOTE_ONLY_CLIENT_ERROR_REFUSAL_RE = re.compile(
    r"\bremote-only policy refuses client-error fallback\s*$"
)
LOCAL_BUILD_MARKERS = ("<building_local>", "building myself, but telling localhost")
LOG_TIMESTAMP_RE = re.compile(
    r"\b([0-9]{4}-[0-9]{2}-[0-9]{2} [0-9]{2}:[0-9]{2}:[0-9]{2}):"
)
SCHEDULER_LINE_RE = re.compile(
    r"^\[[^]\r\n]+\]\s+"
    r"([0-9]{4}-[0-9]{2}-[0-9]{2} [0-9]{2}:[0-9]{2}:[0-9]{2}):\s+(.*)$"
)
SCHEDULER_START_RE = re.compile(r"^ICECREAM scheduler .* starting up, port [0-9]+$")
SCHEDULER_ACTIVE_LOSS_SCHEMA_V1 = "icefarm-scheduler-active-loss-v1"
SCHEDULER_ACTIVE_LOSS_SCHEMA_V2 = "icefarm-scheduler-active-loss-v2"
SCHEDULER_ACTIVE_LOSS_SCHEMA_V3 = "icefarm-scheduler-active-loss-v3"
SCHEDULER_ACTIVE_LOSS_SCHEMA_V4 = "icefarm-scheduler-active-loss-v4"
SCHEDULER_ACTIVE_LOSS_SCHEMA_V5 = "icefarm-scheduler-active-loss-v5"
SCHEDULER_ACTIVE_LOSS_SCHEMA = "icefarm-scheduler-active-loss-v6"
SCHEDULER_ACTIVE_LOSS_ADMISSION_SCHEMA = "icefarm-active-loss-admission-v1"
LISTENER_BINDING_EVIDENCE = "container-env+netns-listener-uid+http-child"
DAEMON_START_RE = re.compile(r"ICECREAM daemon .* starting up")
READINESS_SCHEDULER_RE = re.compile(r"ICECREAM scheduler .* starting up, port [0-9]+")
CACHE_READY_RE = re.compile(r"cache sidecar adapter state=2 lifecycle=3")
CACHE_STATE_RE = re.compile(r"cache sidecar adapter state=([0-9]+) lifecycle=([0-9]+)")
CLIENT_SCHEDULER_READINESS_SCHEMA = "icefarm-client-scheduler-readiness-v2"
SCHEDULER_NEW_RE = re.compile(r"^NEW ([0-9]+) client=([A-Za-z0-9][A-Za-z0-9._-]*)\b")
SCHEDULER_DISPATCH_RE = re.compile(
    r"^put ([0-9]+) in joblist of ([A-Za-z0-9][A-Za-z0-9._-]*)\b"
)
SCHEDULER_PREEXPOSURE_REDISPATCH_RE = re.compile(
    r"^redispatch unexposed assignment ([0-9]+) after worker loss "
    r"([A-Za-z0-9][A-Za-z0-9._-]*)$"
)
SCHEDULER_RETRY_DECISION_RE = re.compile(
    r"^P50_RETRY_DECISION job=([0-9]+) epoch=([0-9]+) nonce=([0-9]+) "
    r"failed=([A-Za-z0-9][A-Za-z0-9._-]*):([0-9]+) "
    r"selected=([A-Za-z0-9][A-Za-z0-9._-]*):([0-9]+) profile=([0-9]+) "
    r"compatible_alternative=([01])$"
)
SCHEDULER_RETRY_DECISION_PROFILES = frozenset((1, 2, 4))
SCHEDULER_BEGIN_RE = re.compile(r"^BEGIN: ([0-9]+)\b")
SCHEDULER_END_RE = re.compile(r"^END ([0-9]+) status=(-?[0-9]+)\b")
SCHEDULER_STOP_RE = re.compile(r"^STOP \((WAITFORCS|DAEMON|DAEMON2)\) FOR ([0-9]+)\b")
SCHEDULER_GENERATION_ACTIONS = frozenset(
    {"upgrade", "downgrade", "restart", "env_set", "scheduler-loss-active"}
)
SCHEDULER_DISPATCH_EPOCH_CONTRACT = "icefarm-scheduler-generation-epoch-v1"
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
        "raw_digest",
        "c_to_f_bytes",
        "f_to_c_bytes",
        "source_mutex_wait_ns",
        "source_mutex_service_ns",
        "terminal_error_code",
        "terminal_error_name",
        "system_source_reuse",
    }
)
PROFILE_LABELS = {
    "p29v1": "P29V1",
    "zstd_tu": "ZSTD_TU",
    "zstd_route": "ZSTD_ROUTE",
}
GENERATED_ROOT_FILES = frozenset(
    (
        "bundle.json",
        "SHA256SUMS",
        "verdict.json",
        "control-verdict.json",
        "EVIDENCE.md",
        "witness.json",
        "down.json",
    )
)
REFUSAL_MODE = "preflight-refusal"
CONTROL_FAILURE_MODE = "control-failure"
EVENT_GATE_SCHEMA = "icefarm-event-gate-v1"
SCHEDULER_RESTART_SCHEMA = "icefarm-scheduler-restart-v1"
CLIENT_ROUTE_RESTART_SCHEMA = "icefarm-client-route-restart-v1"
WORKER_RESTART_SCHEMA = "icefarm-worker-restart-v1"
CLIENT_ROUTE_SIGNAL_SCHEMA = "icefarm-client-route-signal-v1"
HEADER_EDIT_SCHEMA = "icefarm-header-edit-v1"
DISK_FILL_SCHEMA = "icefarm-disk-fill-v1"
CACHE_DISK_FAULT_PATH = "/var/cache/icecream"
CACHE_DISK_FAULT_FILE = "/var/cache/icecream/.icefarm-disk-fill"
CACHE_DISK_FAULT_BYTES = 128 * 1024 * 1024
CACHE_DISK_FAULT_MIN_HEADROOM_BYTES = 8 * 1024 * 1024
DISK_FILL_WATCHDOG_S = 30


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


def _f_init_observation(plan: Mapping[str, Any], evidence: Path) -> dict[str, Any]:
    """Bind every F to its retained Docker init and nofile witness."""

    records: list[dict[str, Any]] = []
    topology = plan.get("topology", {}).get("instances", [])
    if not isinstance(topology, list):
        raise CollectError("plan topology is not a list")
    for instance in topology:
        if not isinstance(instance, Mapping) or instance.get("role") != "F":
            continue
        name = instance.get("name")
        host = instance.get("host")
        if not isinstance(name, str) or not isinstance(host, str):
            raise CollectError("F init witness has invalid instance identity")
        path = evidence / "diagnostics" / host / f"{name}.inspect"
        document = _read_json(path)
        host_config = document.get("HostConfig")
        if not f_runtime_host_config_valid(host_config):
            raise CollectError(
                f"F container {host}:{name} lacks Docker Init=true or exact "
                "nofile=65536:65536"
            )
        records.append(
            {
                "host": host,
                "init": True,
                "inspect_sha256": _sha256(path),
                "instance": name,
            }
        )
    if not records:
        raise CollectError("plan has no F Init witnesses")
    return {"instances": records, "schema": "icefarm-f-init-v1"}


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
        raise CollectError(
            f"immutable evidence destination already exists: {destination}"
        )
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
    return (
        "docker-context"
        if farm.hosts[host_name].get("docker_context")
        else "ssh-docker"
    )


def _snapshot_live_evidence(
    farm: FarmSpec,
    plan: dict[str, Any],
    destination: Path,
    recorder: RecordingTransport,
    *,
    stopped_instances: frozenset[str] = frozenset(),
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
                raise CollectError(
                    f"container identity is unauthenticated: {host}:{name}"
                )
            if instance["role"] == "F":
                host_config = document.get("HostConfig")
                if not f_runtime_host_config_valid(host_config):
                    raise CollectError(
                        f"F container {host}:{name} lacks Docker Init=true or exact "
                        "nofile=65536:65536"
                    )
            container_ids[name] = container_id
            host_diagnostics = diagnostics / host
            host_diagnostics.mkdir(parents=True, exist_ok=True)
            if plan.get("worker_endpoint_contract") == "icefarm-live-bridge-endpoint-v1":
                _atomic_json(host_diagnostics / f"{name}.live-inspect", document)
            running = document.get("State", {}).get("Running")
            if name in stopped_instances:
                if running is not False:
                    raise CollectError(
                        f"event-killed container is unexpectedly running: {host}:{name}"
                    )
                _atomic_bytes(host_diagnostics / f"{name}.top", b"")
                _atomic_json(
                    host_diagnostics / f"{name}.stats",
                    {"container_running": False, "reason": "authenticated-kill-event"},
                )
                continue
            if running is False:
                raise CollectError(f"container stopped without a kill event: {host}:{name}")
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

    # Network namespace diagnostics require a live authenticated container.
    problems = collect_diagnostics(
        farm, plan, recorder, factory, diagnostics,
        container_ids=container_ids, live_only=True,
    )
    if problems:
        raise CollectError("live diagnostic collection failed: " + "; ".join(problems))

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
        if name in stopped_instances:
            continue
        try:
            recorder.invoke(
                _command(
                    factory,
                    phase="collect.stop",
                    host=host,
                    transport=_docker_transport(farm, host),
                    # The container gets only ten seconds of graceful shutdown,
                    # but a remote Docker context needs the declared collection
                    # budget to return that bounded operation reliably.
                    timeout_s=timeout_s,
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
        include_live=False,
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


def _snapshot_existing_evidence(
    root: Path, destination: Path, plan: dict[str, Any]
) -> None:
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
            raise CollectError(
                "collection requires an authenticated UP or completed DOWN run"
            )
    if result["lifecycle"].get("plan") != plan:
        raise CollectError(
            "lifecycle receipt plan differs from the current immutable plan"
        )
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
        event_source = root / "events"
        if event_source.exists():
            _copy_tree(event_source, temporary / "events")
        elif scenario.data["timeline"]:
            raise CollectError("timeline run has no event evidence")
        # A checkpointed client transition binds rows that live in the result
        # trees we have not copied yet.  Authenticate the event structure first
        # so kill events can safely determine the snapshot set, then perform a
        # second, full validation after every result tree has been frozen and
        # copied into staging.
        events = _event_log(
            temporary,
            scenario,
            farm=farm,
            plan=plan,
            preflight=receipts.get("preflight"),
            validate_client_evidence=False,
        )
        stopped_instances = frozenset(
            event["instance"] for event in events if event["action"] == "kill -9"
        )
        if sync_remote:
            _snapshot_live_evidence(
                farm,
                plan,
                temporary,
                recorder or RecordingTransport(),
                stopped_instances=stopped_instances,
            )
        else:
            _snapshot_existing_evidence(root, temporary, plan)
        _event_log(
            temporary,
            scenario,
            farm=farm,
            plan=plan,
            preflight=receipts.get("preflight"),
        )
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


def _checkpoint_result_path(
    evidence: Path, client: str, turn: str, relative: str
) -> Path | None:
    """Resolve a checkpoint result without traversing symlinked ancestors."""

    parts = relative.split("/")
    if len(parts) != 3 or parts[0] != "jobs" or parts[2] != "result.tsv":
        return None
    if evidence.is_symlink() or not evidence.is_dir():
        return None
    current = evidence
    for component in (
        "instances",
        client,
        "results",
        "workload",
        turn,
        "jobs",
        parts[1],
    ):
        current = current / component
        if current.is_symlink() or not current.is_dir():
            return None
    path = current / "result.tsv"
    if path.is_symlink() or not path.is_file():
        return None
    return path


def _source_results(path: Path) -> dict[tuple[int, int, int], dict[str, Any]]:
    records: dict[tuple[int, int, int], dict[str, Any]] = {}
    for index, item in enumerate(_read_jsonl(path), start=1):
        if (
            frozenset(item) != SOURCE_RESULT_FIELDS
            or item.get("schema") != SOURCE_RESULT_SCHEMA
        ):
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
            "source_mutex_wait_ns",
            "source_mutex_service_ns",
            "terminal_error_code",
        )
        if any(
            type(item.get(field)) is not int or item[field] < 0 for field in integers
        ):
            raise CollectError(f"{path}:{index}: source-result integer is invalid")
        if item["wire_job_id"] == 0 or item["logical_job"] == 0:
            raise CollectError(f"{path}:{index}: source-result job identity is zero")
        if item["status"] > 7 or item["attempts"] > 2:
            raise CollectError(
                f"{path}:{index}: source-result status/attempt count is invalid"
            )
        terminal_name = item.get("terminal_error_name")
        if terminal_name not in (None, "WIRE_REVISION_MISMATCH"):
            raise CollectError(f"{path}:{index}: source-result terminal error is invalid")
        if (item["terminal_error_code"] == 4) != (
            terminal_name == "WIRE_REVISION_MISMATCH"
        ):
            raise CollectError(
                f"{path}:{index}: source-result terminal error code/name disagree"
            )
        if item["terminal_error_code"] > 0xFFFF:
            raise CollectError(
                f"{path}:{index}: source-result terminal error code is invalid"
            )
        if item.get("profile") not in PROFILE_LABELS.values():
            raise CollectError(f"{path}:{index}: source-result profile is invalid")
        if (
            not isinstance(item.get("c_store_guid"), str)
            or re.fullmatch(r"[0-9a-f]{32}", item["c_store_guid"]) is None
            or item["c_store_guid"] == "0" * 32
        ):
            raise CollectError(f"{path}:{index}: source-result C GUID is invalid")
        if (
            not isinstance(item.get("raw_digest"), str)
            or re.fullmatch(r"[0-9a-f]{32}", item["raw_digest"]) is None
        ):
            raise CollectError(f"{path}:{index}: source-result raw digest is invalid")
        reuse = item.get("system_source_reuse")
        if reuse is not None and type(reuse) is not bool:
            raise CollectError(f"{path}:{index}: source-result reuse is invalid")
        if item["profile"] != "P29V1" and reuse is not None:
            raise CollectError(
                f"{path}:{index}: non-P29V1 source-result has reuse evidence"
            )
        if item["status"] == 0:
            if item["terminal_error_code"] != 0 or terminal_name is not None:
                raise CollectError(
                    f"{path}:{index}: committed source-result has a terminal error"
                )
            if item["attempts"] == 0:
                raise CollectError(
                    f"{path}:{index}: committed source-result has no attempt"
                )
            if item["raw_digest"] == "0" * 32:
                raise CollectError(
                    f"{path}:{index}: committed source-result has no raw digest"
                )
            if item["source_mutex_service_ns"] == 0:
                raise CollectError(
                    f"{path}:{index}: committed source-result has no mutex service time"
                )
            if item["c_to_f_bytes"] == 0 or item["f_to_c_bytes"] == 0:
                raise CollectError(
                    f"{path}:{index}: committed source-result has no wire bytes"
                )
            if item["profile"] == "P29V1" and type(reuse) is not bool:
                raise CollectError(
                    f"{path}:{index}: committed P29V1 result has no reuse witness"
                )
        key = (
            item["wire_job_id"],
            item["assignment_epoch"],
            item["assignment_nonce"],
        )
        if key in records:
            raise CollectError(f"{path}: duplicate source result for assignment {key}")
        records[key] = item
    return records


def _source_result_status(
    source_results: Mapping[tuple[int, int, int], Mapping[str, Any]],
    identity: tuple[int, int, int],
    *,
    context: str,
    expected_attempts: int | None = None,
    expected_profile: str | None = None,
    row_job_id: str,
) -> int | None:
    """Distinguish a committed result from a parsed failure diagnostic.

    The product writes ``source-result`` records for both committed transfers
    (status zero) and terminal noncommitted attempts (nonzero status).  The
    latter is positive failure evidence and must not be mistaken for a source
    commit.  Real records have already passed :func:`_source_results`; the
    explicit type check keeps direct helper callers fail closed as well.
    """

    if identity not in source_results:
        return None
    result = source_results[identity]
    status = result.get("status") if isinstance(result, Mapping) else None
    if type(status) is not int or not 0 <= status <= 7:
        raise CollectError(
            f"{row_job_id}: {context} source-result status is malformed"
        )
    if status != 0 and (
        expected_profile is not None
        and result.get("profile") != expected_profile
        or expected_attempts is not None
        and result.get("attempts") != expected_attempts
    ):
        raise CollectError(
            f"{row_job_id}: {context} source-result diagnostic disagrees with "
            "the client failure marker"
        )
    return status


def _source_candidates_for_assignment(
    source_results: Mapping[tuple[int, int, int], dict[str, Any]],
    scheduler_job: int,
    scheduler_version: int,
    assignment_identity: tuple[int, int, int] | None = None,
    legacy_binding_marker: tuple[int, int, int, int, int] | None = None,
) -> list[tuple[tuple[int, int, int], dict[str, Any]]]:
    """Return only source evidence the assignment's S could have selected.

    Scheduler job numbers restart after replacement.  A pre-v50 scheduler can
    never emit a P50 cache tail, so a same-number source record from an older
    capable scheduler is historical rather than a candidate for this row.

    Client log timestamps have only one-second resolution while transition
    receipts use milliseconds.  At an upgrade boundary the timestamp-derived
    scheduler version can therefore still name the old scheduler even though
    the assignment happened after replacement.  A job-local P50 assignment
    identity is stronger evidence: legacy schedulers cannot put that identity
    on the wire, and the caller additionally requires the same full identity
    in both the source-result and compile-result traces.  Permit only that
    exact candidate when the coarse timestamp and the wire identity disagree.

    An exact client-local legacy binding is likewise stronger than the coarse
    scheduler-version timestamp.  Its zero epoch/nonce proves that this
    assignment has no P50 scheduler identity, so a positive-identity source
    result with the same scheduler job number belongs to another scheduler
    generation.  Retain the overlap check when a job-local P50 assignment
    identity is also present: that combination can represent a real duplicate
    transport and must fail closed.
    """

    if (
        assignment_identity is None
        and legacy_binding_marker is not None
        and legacy_binding_marker[1:3] == (0, 0)
    ):
        return []
    if scheduler_version < 50 and assignment_identity is None:
        return []
    return [
        (key, source)
        for key, source in source_results.items()
        if key[0] == scheduler_job
        and (assignment_identity is None or key == assignment_identity)
    ]


def _p50_assignment_identity_evidence(
    text: str,
    scheduler_job: int,
    *,
    after_line: int = 0,
    before_line: int | None = None,
) -> dict[str, int] | None:
    """Return one exact, line-bound scheduler identity from a job-local log."""

    identities: dict[tuple[int, int, int, int, int], int] = {}
    for line_number, line in enumerate(text.splitlines(), start=1):
        if line_number <= after_line or (
            before_line is not None and line_number >= before_line
        ):
            continue
        match = P50_ASSIGNMENT_IDENTITY_RE.search(line)
        if match is None:
            continue
        values = tuple(int(match.group(index)) for index in range(1, 6))
        if values[0] != scheduler_job:
            continue
        if any(value <= 0 for value in values[:4]) or values[4] < 0:
            raise CollectError("client P50 assignment identity marker is invalid")
        identities.setdefault(values, line_number)
    if len(identities) > 1:
        raise CollectError("client P50 assignment identity marker is ambiguous")
    if not identities:
        return None
    values, line_number = next(iter(identities.items()))
    return {
        "assignment_epoch": values[1],
        "assignment_nonce": values[2],
        "c_guid": values[3],
        "line": line_number,
        "scheduler_job": values[0],
        "tu_seq": values[4],
    }


def _p50_assignment_identity_marker(
    text: str,
    scheduler_job: int,
    *,
    after_line: int = 0,
    before_line: int | None = None,
) -> tuple[int, int, int] | None:
    """Return one exact scheduler assignment identity from a job-local log."""

    evidence = _p50_assignment_identity_evidence(
        text,
        scheduler_job,
        after_line=after_line,
        before_line=before_line,
    )
    if evidence is None:
        return None
    return (
        evidence["scheduler_job"],
        evidence["assignment_epoch"],
        evidence["assignment_nonce"],
    )


def _legacy_wire_candidates_for_assignment(
    legacy_wires: Mapping[tuple[int, int, int, int, int], dict[str, Any]],
    scheduler_job: int,
    binding_marker: tuple[int, int, int, int, int] | None,
    compile_identities: Mapping[tuple[int, int, int], dict[str, Any]],
) -> list[tuple[tuple[int, int, int, int, int], dict[str, Any]]]:
    """Return only legacy-wire evidence bound by this job's own log.

    Scheduler job numbers restart at scheduler replacement.  In particular,
    a zero-epoch legacy scheduler record can reuse the number of an earlier
    P50 source transfer.  The current client emits the complete five-field
    binding marker before every instrumented legacy transfer, so matching by
    job number alone is both unnecessary and unsafe.
    """

    if binding_marker is None:
        return []
    return [
        (key, wire)
        for key, wire in legacy_wires.items()
        if key[0] == scheduler_job
        and key == binding_marker
        and (
            (key[1] == 0 and key[2] == 0)
            or (
                key[:3] in compile_identities
                and compile_identities[key[:3]]["c_guid"] == wire["c_guid"]
                and compile_identities[key[:3]]["tu_seq"] == wire["tu_seq"]
            )
        )
    ]


def _compile_identities(path: Path) -> dict[tuple[int, int, int], dict[str, Any]]:
    records: dict[tuple[int, int, int], dict[str, Any]] = {}
    for index, item in enumerate(_read_jsonl(path), start=1):
        if (
            frozenset(item) != COMPILE_IDENTITY_FIELDS
            or item.get("record") != "compile-result-identity"
        ):
            raise CollectError(f"{path}:{index}: compile-identity schema mismatch")
        positive = ("job_id", "assignment_epoch", "assignment_nonce", "c_guid")
        if any(
            type(item.get(field)) is not int or item[field] <= 0 for field in positive
        ):
            raise CollectError(f"{path}:{index}: compile-identity value is invalid")
        if type(item.get("tu_seq")) is not int or item["tu_seq"] < 0:
            raise CollectError(
                f"{path}:{index}: compile-identity TU sequence is invalid"
            )
        key = (item["job_id"], item["assignment_epoch"], item["assignment_nonce"])
        if key in records:
            raise CollectError(
                f"{path}: duplicate compile identity for assignment {key}"
            )
        records[key] = item
    return records


def _legacy_wire_results(
    path: Path, role: str
) -> dict[tuple[int, int, int, int, int], dict[str, Any]]:
    if role not in ("C", "F"):
        raise ValueError(f"invalid legacy-wire role {role!r}")
    records: dict[tuple[int, int, int, int, int], dict[str, Any]] = {}
    for index, item in enumerate(_read_jsonl(path), start=1):
        if (
            frozenset(item) != LEGACY_WIRE_FIELDS
            or item.get("schema") != LEGACY_WIRE_SCHEMA
            or item.get("role") != role
        ):
            raise CollectError(f"{path}:{index}: legacy-wire schema/role mismatch")
        positive = ("job_id", "c_guid")
        counters = (
            "c_to_f_sent_bytes",
            "c_to_f_received_bytes",
            "f_to_c_sent_bytes",
            "f_to_c_received_bytes",
        )
        if any(
            type(item.get(field)) is not int or item[field] <= 0 for field in positive
        ):
            raise CollectError(f"{path}:{index}: legacy-wire identity is invalid")
        assignment_epoch = item.get("assignment_epoch")
        assignment_nonce = item.get("assignment_nonce")
        # Mirror P50LegacyWireIdentity::valid(): a scheduler-wide legacy
        # fence deliberately carries no assignment identity, while a fenced
        # legacy transfer must carry both words.  A half-present identity is
        # never valid.
        if (
            type(assignment_epoch) is not int
            or type(assignment_nonce) is not int
            or not (
                (assignment_epoch == 0 and assignment_nonce == 0)
                or (assignment_epoch > 0 and assignment_nonce > 0)
            )
        ):
            raise CollectError(f"{path}:{index}: legacy-wire identity is invalid")
        if type(item.get("tu_seq")) is not int or item["tu_seq"] < 0:
            raise CollectError(f"{path}:{index}: legacy-wire TU sequence is invalid")
        if any(
            type(item.get(field)) is not int or item[field] < 0 for field in counters
        ):
            raise CollectError(f"{path}:{index}: legacy-wire byte count is invalid")
        sent_field = "c_to_f_sent_bytes" if role == "C" else "f_to_c_sent_bytes"
        received_field = (
            "f_to_c_received_bytes" if role == "C" else "c_to_f_received_bytes"
        )
        if item[sent_field] == 0 or item[received_field] == 0:
            raise CollectError(f"{path}:{index}: legacy-wire transfer is incomplete")
        key = (
            item["job_id"],
            item["assignment_epoch"],
            item["assignment_nonce"],
            item["c_guid"],
            item["tu_seq"],
        )
        if key in records:
            raise CollectError(f"{path}: duplicate legacy-wire result for {key}")
        records[key] = item
    return records


S30_MUTANT_TRACE_SCHEMA = "icefarm-s30-mutant-f-refusal-v1"
S90_REVISION_MISMATCH_SCHEMA = "icefarm-wire-revision-mismatch-v1"


def _s30_mutant_refusals(path: Path) -> list[dict[str, Any]]:
    """Read only the daemon's post-hello refusal witness.

    The witness proves that the mutant reached a decoded SessionHello.  Job,
    assignment, and wire conservation remain independently authenticated by
    the scheduler/client/F traces below; this file is never accepted on its
    own as a workload result.
    """

    fields = {"schema", "refusal", "wire_revision", "supported_profiles"}
    records = _read_jsonl(path, required=True)
    for index, item in enumerate(records, start=1):
        if (
            set(item) != fields
            or item.get("schema") != S30_MUTANT_TRACE_SCHEMA
            or item.get("refusal") != "p50-session-refused"
            or type(item.get("wire_revision")) is not int
            or item["wire_revision"] <= 0
            or type(item.get("supported_profiles")) is not int
            or item["supported_profiles"] <= 0
        ):
            raise CollectError(f"{path}:{index}: invalid S30 refusal witness")
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


def _p29_action_lineages(path: Path) -> dict[tuple[str, int], dict[str, Any]]:
    """Retain the F-authored P29 relationship identity for each transaction.

    B4 used to infer a fresh worker-side relationship from the chance that the
    scheduler sent the same translation unit to the restarted worker twice.
    The product trace already states the stronger fact directly: a new F store
    starts at session/history/relationship sequence 1/1/0.  Preserve only the
    fields needed to bind that fact to a full source-result identity.
    """

    result: dict[tuple[str, int], dict[str, Any]] = {}
    for index, item in enumerate(_read_jsonl(path), start=1):
        if item.get("action") != "TX_BEGIN":
            continue
        if item.get("actor") != "F" or item.get("profile") != "p29_v1":
            raise CollectError(f"{path}:{index}: P29 TX_BEGIN role/profile is invalid")
        c_store_guid = item.get("c_store_guid")
        f_store_guid = item.get("f_store_guid")
        previous_f_store_guid = item.get("previous_f_store_guid")
        raw_digest = item.get("raw_digest")
        tu_seq = item.get("tu_seq")
        session_serial = item.get("session_serial")
        history_nonce = item.get("history_nonce")
        rel_seq = item.get("rel_seq")
        if (
            not isinstance(c_store_guid, str)
            or re.fullmatch(r"[0-9a-f]{32}", c_store_guid) is None
            or c_store_guid == ZERO_GUID
            or not isinstance(f_store_guid, str)
            or re.fullmatch(r"[0-9a-f]{32}", f_store_guid) is None
            or f_store_guid == ZERO_GUID
            or not isinstance(previous_f_store_guid, str)
            or re.fullmatch(r"[0-9a-f]{32}", previous_f_store_guid) is None
            or not isinstance(raw_digest, str)
            or re.fullmatch(r"[0-9a-f]{32}", raw_digest) is None
            or type(tu_seq) is not int
            or tu_seq < 0
            or type(session_serial) is not int
            or session_serial < 1
            or type(history_nonce) is not int
            or history_nonce < 1
            or type(rel_seq) is not int
            or rel_seq < 0
        ):
            raise CollectError(f"{path}:{index}: P29 TX_BEGIN lineage is invalid")
        key = (c_store_guid, tu_seq)
        if key in result:
            raise CollectError(f"{path}: duplicate P29 TX_BEGIN lineage for {key}")
        result[key] = {
            "c_store_guid": c_store_guid,
            "f_store_guid": f_store_guid,
            "history_nonce": history_nonce,
            "previous_f_store_guid": previous_f_store_guid,
            "raw_digest": raw_digest,
            "rel_seq": rel_seq,
            "session_serial": session_serial,
            "tu_seq": tu_seq,
        }
    return result


def _one_role_log(evidence: Path, instance: Mapping[str, Any]) -> Path | None:
    role_leaf = {"S": "scheduler.log", "C": "client-daemon.log", "F": "iceccd.log"}[
        str(instance["role"])
    ]
    path = (
        evidence
        / "diagnostics"
        / str(instance["host"])
        / f"{instance['name']}.log"
        / role_leaf
    )
    if path.is_symlink():
        raise CollectError(f"unsafe {instance['name']} log: {path}")
    if not path.exists():
        return None
    if not path.is_file():
        raise CollectError(f"non-regular {instance['name']} log: {path}")
    return path


def _retained_log_payload(
    evidence: Path | None,
    instance: Mapping[str, Any],
    offset: Any,
)-> tuple[bool, bytes | None]:
    """Return whether a retained role log exists and its authenticated tail."""
    if evidence is None or not (evidence / "diagnostics").is_dir():
        return False, None
    if type(offset) is not int or offset < 0:
        return True, None
    path = _one_role_log(evidence, instance)
    if path is None:
        return True, None
    try:
        payload = path.read_bytes()
    except OSError:
        return True, None
    if offset > len(payload):
        return True, None
    return True, payload[offset:]


def _retained_log_witness(
    evidence: Path | None,
    instance: Mapping[str, Any],
    offset: Any,
    line: Any,
) -> bool:
    """Bind a producer readiness claim to retained post-offset log bytes."""

    present, payload = _retained_log_payload(evidence, instance, offset)
    if not present:
        return True
    return (
        payload is not None
        and isinstance(line, str)
        and line in payload.decode("utf-8", "replace").splitlines()
    )


def _retained_log_witness_exact(
    evidence: Path | None,
    instance: Mapping[str, Any],
    offset: Any,
    line: Any,
    byte_count: Any,
    sha256: Any,
) -> bool:
    """Bind the captured post-offset prefix while permitting later appends."""

    present, payload = _retained_log_payload(evidence, instance, offset)
    if not present:
        return True
    return (
        payload is not None
        and type(byte_count) is int
        and 0 < byte_count <= len(payload)
        and isinstance(sha256, str)
        and SHA256_RE.fullmatch(sha256) is not None
        and hashlib.sha256(payload[:byte_count]).hexdigest() == sha256
        and isinstance(line, str)
        and line in payload[:byte_count].decode("utf-8", "replace").splitlines()
    )


CLIENT_SCHEDULER_READINESS_FIELDS = frozenset(
    {
        "bytes",
        "cache_expected",
        "cache_fresh",
        "cache_lifecycle",
        "cache_line",
        "cache_line_offset",
        "cache_state",
        "connected_line",
        "connected_line_offset",
        "host",
        "log_path",
        "offset",
        "post_sha256",
        "route",
        "schema",
    }
)


def _valid_client_route_state(value: Any) -> bool:
    if not isinstance(value, Mapping) or set(value) != {
        "container",
        "daemon",
        "route_owner",
    }:
        return False
    container = value.get("container")
    daemon = value.get("daemon")
    owner = value.get("route_owner")
    return (
        isinstance(container, Mapping)
        and set(container) == {"container_id", "running", "started_at"}
        and isinstance(container.get("container_id"), str)
        and SHA256_RE.fullmatch(container["container_id"]) is not None
        and container.get("running") is True
        and isinstance(container.get("started_at"), str)
        and bool(container["started_at"])
        and _valid_route_process(daemon, "/opt/icecream/sbin/iceccd")
        and _valid_route_process(owner, "/opt/icecream/sbin/icecc-cache-service")
        and owner["ppid"] == daemon["pid"]
        and owner["uid"] == daemon["uid"]
    )


def _retained_client_scheduler_readiness_v2(
    evidence: Path | None,
    instance: Mapping[str, Any],
    witness: Mapping[str, Any],
) -> bool:
    """Recompute the producer's exact readiness view from its retained prefix."""

    if evidence is None or not (evidence / "diagnostics").is_dir():
        return True
    path = _one_role_log(evidence, instance)
    if path is None:
        return False
    try:
        raw = path.read_bytes()
    except OSError:
        return False
    offset = witness.get("offset")
    byte_count = witness.get("bytes")
    if (
        type(offset) is not int
        or offset < 0
        or type(byte_count) is not int
        or byte_count < 1
        or offset + byte_count > len(raw)
        or hashlib.sha256(raw[offset : offset + byte_count]).hexdigest()
        != witness.get("post_sha256")
    ):
        return False
    endpoint = offset + byte_count
    lines: list[tuple[int, str]] = []
    position = 0
    for chunk in raw[:endpoint].splitlines(keepends=True):
        line = chunk.rstrip(b"\r\n").decode("utf-8", "replace")
        lines.append((position, line))
        position += len(chunk)
    connected = [
        item
        for item in lines
        if item[0] >= offset
        and "Connected to scheduler (I am known as " in item[1]
    ]
    if not connected or connected[-1] != (
        witness.get("connected_line_offset"),
        witness.get("connected_line"),
    ):
        return False
    cache = []
    for line_offset, line in lines:
        match = CACHE_STATE_RE.search(line)
        if match is not None:
            cache.append(
                (line_offset, line, int(match.group(1)), int(match.group(2)))
            )
    if not cache:
        return all(
            witness.get(field) is None
            for field in (
                "cache_lifecycle",
                "cache_line",
                "cache_line_offset",
                "cache_state",
            )
        ) and witness.get("cache_fresh") is False
    latest = cache[-1]
    return (
        latest
        == (
            witness.get("cache_line_offset"),
            witness.get("cache_line"),
            witness.get("cache_state"),
            witness.get("cache_lifecycle"),
        )
        and witness.get("cache_fresh") is (latest[0] >= offset)
    )


def _valid_client_scheduler_readiness_v2(
    witness: Any,
    client: Mapping[str, Any],
    *,
    cache_expected: bool,
    expected_host: str,
    expected_path: str,
    evidence: Path | None,
) -> bool:
    if (
        not isinstance(witness, Mapping)
        or set(witness) != CLIENT_SCHEDULER_READINESS_FIELDS
        or witness.get("schema") != CLIENT_SCHEDULER_READINESS_SCHEMA
        or witness.get("host") != expected_host
        or witness.get("log_path") != expected_path
        or type(witness.get("offset")) is not int
        or witness["offset"] < 0
        or type(witness.get("bytes")) is not int
        or witness["bytes"] < 1
        or not isinstance(witness.get("post_sha256"), str)
        or SHA256_RE.fullmatch(witness["post_sha256"]) is None
        or witness.get("cache_expected") is not cache_expected
        or type(witness.get("cache_fresh")) is not bool
        or not isinstance(witness.get("connected_line"), str)
        or "Connected to scheduler (I am known as " not in witness["connected_line"]
        or type(witness.get("connected_line_offset")) is not int
        or witness["connected_line_offset"] < witness["offset"]
    ):
        return False
    cache_line = witness.get("cache_line")
    if cache_line is None:
        cache_consistent = all(
            witness.get(field) is None
            for field in ("cache_lifecycle", "cache_line_offset", "cache_state")
        ) and witness.get("cache_fresh") is False
    else:
        match = CACHE_STATE_RE.search(cache_line) if isinstance(cache_line, str) else None
        cache_consistent = (
            match is not None
            and type(witness.get("cache_line_offset")) is int
            and 0 <= witness["cache_line_offset"] < witness["offset"] + witness["bytes"]
            and type(witness.get("cache_state")) is int
            and type(witness.get("cache_lifecycle")) is int
            and int(match.group(1)) == witness["cache_state"]
            and int(match.group(2)) == witness["cache_lifecycle"]
            and witness["cache_fresh"] is (
                witness["cache_line_offset"] >= witness["offset"]
            )
        )
    if not cache_consistent:
        return False
    route = witness.get("route")
    if cache_expected:
        if (
            witness.get("cache_state") != 2
            or witness.get("cache_lifecycle") != 3
            or not isinstance(route, Mapping)
            or set(route) != {"after", "before"}
            or (route["before"] is not None and not _valid_client_route_state(route["before"]))
            or not _valid_client_route_state(route["after"])
            or (route["before"] != route["after"] and witness.get("cache_fresh") is not True)
        ):
            return False
    elif route is not None:
        return False
    return _retained_client_scheduler_readiness_v2(evidence, client, witness)


def _requires_client_scheduler_readiness_v2(plan: Mapping[str, Any]) -> bool:
    contract = plan.get("client_scheduler_readiness_contract")
    if contract is None:
        return False
    if contract != CLIENT_SCHEDULER_READINESS_SCHEMA:
        raise CollectError("plan has an unknown client scheduler readiness contract")
    return True


def _p29_interner_faults(
    evidence: Path, topology: list[dict[str, Any]]
) -> list[dict[str, str]]:
    """Read exact one-shot P29 fault witnesses from authenticated C logs."""

    records: list[dict[str, str]] = []
    needle = f'"schema":"{P29_INTERNER_FAULT_SCHEMA}"'
    for instance in topology:
        if instance.get("role") != "C":
            continue
        path = (
            evidence
            / "diagnostics"
            / str(instance["host"])
            / f"{instance['name']}.logs"
        )
        if path.is_symlink():
            raise CollectError(f"unsafe {instance['name']} container log: {path}")
        if not path.exists():
            continue
        if not path.is_file():
            raise CollectError(f"non-regular {instance['name']} container log: {path}")
        for line_number, line in enumerate(
            _text(path).splitlines(), start=1
        ):
            if P29_INTERNER_FAULT_SCHEMA not in line:
                continue
            if needle not in line:
                raise CollectError(
                    f"{path}:{line_number}: malformed P29 interner fault witness"
                )
            candidate = _strict_object(line, f"{path}:{line_number}")
            if (
                not isinstance(candidate, dict)
                or set(candidate) != P29_INTERNER_FAULT_FIELDS
                or candidate.get("schema") != P29_INTERNER_FAULT_SCHEMA
                or candidate.get("fault") != P29_INTERNER_FAULT
                or candidate.get("outcome") != P29_INTERNER_FAULT_OUTCOME
            ):
                raise CollectError(
                    f"{path}:{line_number}: invalid P29 interner fault witness"
                )
            records.append(
                {
                    "client_instance": str(instance["name"]),
                    **candidate,
                }
            )
    identities = [record["client_instance"] for record in records]
    if len(identities) != len(set(identities)):
        raise CollectError("duplicate P29 interner fault witness for one client")
    return sorted(records, key=lambda item: item["client_instance"])


def _text(path: Path | None) -> str:
    if path is None:
        return ""
    try:
        return path.read_text(encoding="utf-8", errors="replace")
    except OSError as exc:
        raise CollectError(f"cannot read {path}: {exc}") from exc


def _timestamp_ms(raw: str, source: str) -> int:
    try:
        value = datetime.strptime(raw, "%Y-%m-%d %H:%M:%S").replace(tzinfo=timezone.utc)
    except ValueError as exc:
        raise CollectError(f"{source}: invalid log timestamp {raw!r}") from exc
    return int(value.timestamp()) * 1000


def _client_assignments(text: str, source: str) -> list[dict[str, Any]]:
    """Parse every scheduler assignment observed by one compiler wrapper."""

    result: list[dict[str, Any]] = []
    for line_number, line in enumerate(text.splitlines(), start=1):
        match = CLIENT_ASSIGNMENT_RE.search(line)
        if match is None:
            continue
        timestamp = LOG_TIMESTAMP_RE.search(line)
        if timestamp is None:
            raise CollectError(
                f"{source}:{line_number}: assignment lacks an authenticated timestamp"
            )
        result.append(
            {
                "endpoint": match.group(1),
                "line": line_number,
                "observed_ms": _timestamp_ms(
                    timestamp.group(1), f"{source}:{line_number}"
                ),
                "scheduler_job": int(match.group(2)),
            }
        )
    return result


def _scheduler_retry_decisions(
    evidence: Path, plan: dict[str, Any]
) -> list[dict[str, Any]]:
    """Parse producer-authenticated P50 retry selection decisions.

    This is deliberately an optional witness for historical bundles: bundles
    without the marker continue through the existing collector.  When present,
    however, every marker is strict, framed, tied to a known scheduler NEW
    record, and ordered before that incarnation's put record.
    """

    scheduler = next(
        item for item in plan["topology"]["instances"] if item["role"] == "S"
    )
    path = _one_role_log(evidence, scheduler)
    if path is None:
        raise CollectError("scheduler lifecycle log is absent")
    generation = 0
    jobs: dict[tuple[int, int], dict[str, Any]] = {}
    decisions: list[dict[str, Any]] = []
    marker = "P50_RETRY_DECISION"
    for line_number, line in enumerate(_text(path).splitlines(), start=1):
        framed = SCHEDULER_LINE_RE.fullmatch(line)
        if framed is None:
            if marker in line:
                raise CollectError(
                    f"scheduler retry decision line {line_number} lacks exact timestamp frame"
                )
            continue
        timestamp_ms = _timestamp_ms(framed.group(1), f"{path}:{line_number}")
        message = framed.group(2)
        if SCHEDULER_START_RE.fullmatch(message):
            generation += 1
            continue
        new = SCHEDULER_NEW_RE.match(message)
        if new is not None:
            if generation == 0:
                raise CollectError(
                    "scheduler retry decision job appears before startup"
                )
            key = (generation, int(new.group(1)))
            if key in jobs:
                raise CollectError(f"duplicate scheduler NEW record for {key}")
            jobs[key] = {
                "new_line": line_number,
                "incarnation_start_ms": timestamp_ms,
                "dispatch_line": None,
                "pending": False,
                "pending_decision": None,
                "decision_tuples": set(),
            }
            continue
        redispatch = SCHEDULER_PREEXPOSURE_REDISPATCH_RE.match(message)
        if redispatch is not None:
            key = (generation, int(redispatch.group(1)))
            state = jobs.get(key)
            if state is None or state["dispatch_line"] is None or state["pending"]:
                raise CollectError(
                    f"invalid pre-exposure redispatch before retry decision at line {line_number}"
                )
            state["pending"] = True
            state["incarnation_start_ms"] = timestamp_ms
            continue
        if marker in message:
            parsed = SCHEDULER_RETRY_DECISION_RE.fullmatch(message)
            if parsed is None:
                raise CollectError(
                    f"malformed scheduler retry decision at line {line_number}"
                )
            if generation == 0:
                raise CollectError("scheduler retry decision appears before startup")
            job_id, epoch, nonce = (int(parsed.group(i)) for i in (1, 2, 3))
            failed_port, selected_port, profile = (
                int(parsed.group(i)) for i in (5, 7, 8)
            )
            if (
                (
                    job_id > 0xFFFFFFFF
                    or epoch > 0xFFFFFFFFFFFFFFFF
                    or nonce > 0xFFFFFFFFFFFFFFFF
                    or min(job_id, epoch, nonce) <= 0
                    or not (1 <= failed_port <= 65535)
                )
                or not (1 <= selected_port <= 65535)
                or profile not in SCHEDULER_RETRY_DECISION_PROFILES
            ):
                raise CollectError(
                    f"invalid scheduler retry decision values at line {line_number}"
                )
            key = (generation, job_id)
            state = jobs.get(key)
            if state is None:
                raise CollectError(
                    f"scheduler retry decision has no NEW record for {key}"
                )
            if state["dispatch_line"] is not None and not state["pending"]:
                raise CollectError(f"scheduler retry decision follows put for {key}")
            if state["pending_decision"] is not None:
                raise CollectError(
                    f"multiple scheduler retry decisions before put for {key}"
                )
            if timestamp_ms < state["incarnation_start_ms"]:
                raise CollectError(
                    f"scheduler retry decision precedes its current incarnation for {key}"
                )
            decision_tuple = (epoch, nonce)
            if decision_tuple in state["decision_tuples"]:
                raise CollectError(f"duplicate scheduler retry decision for {key}")
            state["decision_tuples"].add(decision_tuple)
            decision = {
                "generation": generation,
                "job": job_id,
                "epoch": epoch,
                "nonce": nonce,
                "failed_host": parsed.group(4),
                "failed_port": failed_port,
                "selected_host": parsed.group(6),
                "selected_port": selected_port,
                "profile": profile,
                "compatible_alternative": int(parsed.group(9)),
                "line": line_number,
                "source_path": str(path.relative_to(evidence)),
                "timestamp_ms": timestamp_ms,
                "new_line": state["new_line"],
            }
            state["pending_decision"] = decision
            decisions.append(decision)
            continue
        put = SCHEDULER_DISPATCH_RE.match(message)
        if put is not None:
            key = (generation, int(put.group(1)))
            state = jobs.get(key)
            if state is None:
                raise CollectError(f"scheduler put has no NEW record for {key}")
            if state["dispatch_line"] is not None and not state["pending"]:
                raise CollectError(f"duplicate scheduler put for {key}")
            pending_decision = state["pending_decision"]
            if pending_decision is not None:
                if timestamp_ms < pending_decision["timestamp_ms"]:
                    raise CollectError(
                        f"scheduler put precedes retry decision for {key}"
                    )
                pending_decision.update(
                    {
                        "put_line": line_number,
                        "put_ms": timestamp_ms,
                        "worker": put.group(2),
                    }
                )
                state["pending_decision"] = None
            state["dispatch_line"] = line_number
            state["pending"] = False
            continue
    if generation == 0:
        raise CollectError("scheduler log has no startup generation")
    for state in jobs.values():
        if state["pending_decision"] is not None:
            raise CollectError("scheduler retry decision has no corresponding put")
    return decisions


def _scheduler_jobs(
    evidence: Path,
    plan: dict[str, Any],
    *,
    allow_unterminated_job_ids: set[int] | None = None,
    allow_unterminated_generations: set[int] | None = None,
) -> list[dict[str, Any]]:
    """Parse scheduler-owned dispatch and terminal transitions from its log."""

    scheduler = next(
        item for item in plan["topology"]["instances"] if item["role"] == "S"
    )
    path = _one_role_log(evidence, scheduler)
    if path is None:
        raise CollectError("scheduler lifecycle log is absent")
    generation = 0
    generation_starts: dict[int, tuple[int, int]] = {}
    jobs: dict[tuple[int, int], dict[str, Any]] = {}
    dispatches: list[dict[str, Any]] = []
    for line_number, line in enumerate(_text(path).splitlines(), start=1):
        framed = SCHEDULER_LINE_RE.fullmatch(line)
        if framed is None:
            if any(
                marker in line
                for marker in (
                    "ICECREAM scheduler",
                    "NEW ",
                    "put ",
                    "redispatch unexposed assignment",
                    "BEGIN:",
                    "END ",
                    "STOP (",
                )
            ):
                raise CollectError(
                    f"scheduler lifecycle line {line_number} lacks the exact timestamp frame"
                )
            continue
        timestamp_ms = _timestamp_ms(framed.group(1), f"{path}:{line_number}")
        message = framed.group(2)
        if SCHEDULER_START_RE.fullmatch(message):
            generation += 1
            generation_starts[generation] = (line_number, timestamp_ms)
            continue
        matched = SCHEDULER_NEW_RE.match(message)
        if matched is not None:
            if generation == 0:
                raise CollectError(
                    "scheduler job appears before a scheduler start record"
                )
            key = (generation, int(matched.group(1)))
            if key in jobs:
                raise CollectError(f"duplicate scheduler NEW record for {key}")
            jobs[key] = {
                "client": matched.group(2),
                "generation": generation,
                "line": line_number,
                "new_ms": timestamp_ms,
                "scheduler_job": key[1],
            }
            continue
        matched = SCHEDULER_PREEXPOSURE_REDISPATCH_RE.fullmatch(message)
        if matched is not None:
            key = (generation, int(matched.group(1)))
            job = jobs.get(key)
            lost_worker = matched.group(2)
            if job is None or "dispatch_ms" not in job:
                raise CollectError(
                    f"pre-exposure redispatch has no dispatch for {key}"
                )
            if (
                "terminal_ms" in job
                or "begin_ms" in job
                or "_pending_preexposure_redispatch" in job
            ):
                raise CollectError(
                    f"pre-exposure redispatch is not an unexposed live dispatch for {key}"
                )
            if job["worker"] != lost_worker:
                raise CollectError(
                    f"pre-exposure redispatch worker mismatch for {key}"
                )
            if timestamp_ms < job["dispatch_ms"]:
                raise CollectError(
                    f"pre-exposure redispatch timestamp precedes dispatch for {key}"
                )
            job["_pending_preexposure_redispatch"] = {
                "client": job["client"],
                "generation": generation,
                "lost_dispatch_line": job["dispatch_line"],
                "lost_dispatch_ms": job["dispatch_ms"],
                "lost_worker": lost_worker,
                "marker_line": line_number,
                "marker_ms": timestamp_ms,
                "scheduler_job": key[1],
            }
            continue
        if "redispatch unexposed assignment" in message:
            raise CollectError(
                f"malformed pre-exposure redispatch record at scheduler line {line_number}"
            )
        matched = SCHEDULER_DISPATCH_RE.match(message)
        if matched is not None:
            key = (generation, int(matched.group(1)))
            job = jobs.get(key)
            if job is None:
                raise CollectError(f"scheduler dispatch has no NEW record for {key}")
            if "dispatch_ms" in job:
                pending = job.pop("_pending_preexposure_redispatch", None)
                if pending is None:
                    raise CollectError(f"duplicate scheduler dispatch for {key}")
                replacement_worker = matched.group(2)
                if replacement_worker == pending["lost_worker"]:
                    raise CollectError(
                        f"pre-exposure redispatch reused lost worker for {key}"
                    )
                if timestamp_ms < pending["marker_ms"]:
                    raise CollectError(
                        f"pre-exposure replacement precedes its marker for {key}"
                    )
                pending.update(
                    {
                        "replacement_dispatch_line": line_number,
                        "replacement_dispatch_ms": timestamp_ms,
                        "replacement_worker": replacement_worker,
                    }
                )
                job.setdefault("preexposure_redispatches", []).append(pending)
                job.update(
                    {
                        "dispatch_line": line_number,
                        "dispatch_ms": timestamp_ms,
                        "worker": replacement_worker,
                    }
                )
                continue
            job.update(
                {
                    "dispatch_line": line_number,
                    "dispatch_ms": timestamp_ms,
                    "worker": matched.group(2),
                }
            )
            dispatches.append(job)
            continue
        matched = SCHEDULER_BEGIN_RE.match(message)
        if matched is not None:
            key = (generation, int(matched.group(1)))
            job = jobs.get(key)
            if job is None or "dispatch_ms" not in job:
                raise CollectError(f"scheduler BEGIN has no dispatch for {key}")
            if "_pending_preexposure_redispatch" in job:
                raise CollectError(
                    f"pre-exposure redispatch has no replacement dispatch for {key}"
                )
            if "begin_ms" in job:
                raise CollectError(f"duplicate scheduler BEGIN for {key}")
            job["begin_ms"] = timestamp_ms
            continue
        matched = SCHEDULER_END_RE.match(message)
        if matched is not None:
            key = (generation, int(matched.group(1)))
            job = jobs.get(key)
            if job is None or "dispatch_ms" not in job:
                raise CollectError(f"scheduler END has no dispatch for {key}")
            if "_pending_preexposure_redispatch" in job:
                raise CollectError(
                    f"pre-exposure redispatch has no replacement dispatch for {key}"
                )
            if "terminal_ms" in job:
                raise CollectError(f"duplicate scheduler terminal for {key}")
            status = int(matched.group(2))
            job.update(
                {
                    "status": status,
                    "terminal": "completion" if status == 0 else "cancellation",
                    "terminal_line": line_number,
                    "terminal_ms": timestamp_ms,
                }
            )
            continue
        matched = SCHEDULER_STOP_RE.match(message)
        if matched is not None:
            key = (generation, int(matched.group(2)))
            job = jobs.get(key)
            if job is None:
                raise CollectError(f"scheduler STOP has no NEW record for {key}")
            if "dispatch_ms" not in job:
                continue
            if "_pending_preexposure_redispatch" in job:
                raise CollectError(
                    f"pre-exposure redispatch has no replacement dispatch for {key}"
                )
            if "terminal_ms" in job:
                raise CollectError(f"duplicate scheduler terminal for {key}")
            reason = matched.group(1)
            job.update(
                {
                    "status": None,
                    "terminal": (
                        "cancellation"
                        if reason == "WAITFORCS"
                        else "process-loss-recovery"
                    ),
                    "terminal_line": line_number,
                    "terminal_ms": timestamp_ms,
                }
            )
    if generation == 0:
        raise CollectError("scheduler log has no startup generation")
    pending = [
        (job["generation"], job["scheduler_job"])
        for job in jobs.values()
        if "_pending_preexposure_redispatch" in job
    ]
    if pending:
        raise CollectError(
            f"pre-exposure redispatch has no replacement dispatch for {pending[0]}"
        )
    allowed = allow_unterminated_job_ids or set()
    incomplete = [job for job in dispatches if "terminal_ms" not in job]
    # An active scheduler loss may explain exactly one dispatch.  Do not
    # authorize every generation sharing a numeric job id.
    if len(incomplete) > 1:
        raise CollectError(
            "scheduler log has multiple unterminated dispatches; refusing to "
            "attribute scheduler loss by numeric job id"
        )
    for job in dispatches:
        if "terminal_ms" not in job:
            if (
                len(incomplete) == 1
                and job is incomplete[0]
                and job["scheduler_job"] in allowed
                and (
                    not allow_unterminated_generations
                    or job["generation"] in allow_unterminated_generations
                )
            ):
                successor = generation_starts.get(job["generation"] + 1)
                if (
                    successor is None
                    or successor[0] <= job["dispatch_line"]
                    or successor[1] < job["dispatch_ms"]
                ):
                    raise CollectError(
                        "scheduler-loss dispatch has no authenticated successor "
                        "scheduler generation"
                    )
                job.update(
                    {
                        "status": None,
                        "terminal": "scheduler-loss",
                        # The exact next scheduler startup is the only retained
                        # log boundary at which this old-generation assignment
                        # is known to have ceased occupying its F slot.  Binding
                        # the synthetic loss terminal to that line preserves
                        # assignment-preference occupancy without inventing an
                        # END/STOP record.
                        "terminal_line": successor[0],
                        "terminal_ms": successor[1],
                    }
                )
                continue
            raise CollectError(
                "scheduler dispatch has no terminal: "
                f"generation={job['generation']} job={job['scheduler_job']}"
            )
        if (
            job["new_ms"] > job["dispatch_ms"]
            or job["dispatch_ms"] > job["terminal_ms"]
            or (
                "begin_ms" in job
                and not job["dispatch_ms"] <= job["begin_ms"] <= job["terminal_ms"]
            )
        ):
            raise CollectError(
                "scheduler lifecycle timestamps are not monotonic for "
                f"generation={job['generation']} job={job['scheduler_job']}"
            )
    return dispatches


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


def _event_log(
    evidence: Path,
    scenario: ScenarioSpec,
    *,
    farm: FarmSpec | None = None,
    plan: dict[str, Any] | None = None,
    preflight: Mapping[str, Any] | None = None,
    validate_client_evidence: bool = True,
) -> list[dict[str, Any]]:
    path = evidence / "events" / "events.json"
    failure_path = evidence / "events" / "failure.json"
    if failure_path.exists():
        failure = _read_json(failure_path)
        if not isinstance(failure, Mapping) or failure.get("schema") != "icefarm-event-failure-v1":
            raise CollectError("event failure descriptor is malformed")
        raise CollectError(
            "timeline event failed before a complete receipt: "
            + str(failure.get("exception", "unknown event failure"))
        )
    if not path.exists():
        if scenario.data["timeline"]:
            raise CollectError("timeline run has no events.json")
        return []
    value = _read_json(path)
    events = value.get("events")
    if not isinstance(events, list):
        raise CollectError("events.json has no event list")
    expected = scenario.data["timeline"]
    if len(events) != len(expected):
        raise CollectError("events.json does not cover the complete scenario timeline")
    previous = -1
    continuity: dict[str, dict[str, Any]] = {}
    for index, event in enumerate(events):
        fields = {
            "action",
            "event_epoch",
            "event_index",
            "fired_ms",
            "instance",
            "last_dispatched_job",
            "trigger",
            "workload_dispatch_count",
        }
        if not isinstance(event, dict) or not fields.issubset(event):
            raise CollectError(f"events.json event {index} has the wrong fields")
        transition = event.get("action") in {"upgrade", "downgrade", "env_set"}
        header_edit = event.get("action") == "header_edit"
        disk_fill = event.get("action") == "disk_fill"
        kill_interval = (
            event.get("action") == "kill -9"
            and plan is not None
            and plan.get("kill_timing_contract") == "icefarm-kill-interval-v1"
        )
        scheduler_restart = False
        scheduler_active_loss = False
        client_route_restart = False
        worker_restart = False
        client_checkpoint_transition = False
        if event.get("action") in {"restart", "scheduler-loss-active"} and plan is not None:
            target = next(
                (
                    item
                    for item in plan["topology"]["instances"]
                    if item["name"] == event.get("instance")
                ),
                None,
            )
            scheduler_restart = (
                isinstance(target, Mapping)
                and target.get("role") == "S"
                and event.get("action") == "restart"
                and isinstance(event.get("trigger"), str)
                and re.fullmatch(r"job [1-9][0-9]*", event["trigger"]) is not None
            )
            scheduler_active_loss = (
                isinstance(target, Mapping)
                and target.get("role") == "S"
                and event.get("action") == "scheduler-loss-active"
                and isinstance(event.get("trigger"), str)
                and re.fullmatch(r"job [1-9][0-9]*", event["trigger"]) is not None
            )
            client_route_restart = (
                isinstance(target, Mapping)
                and target.get("role") == "C"
                and event.get("action") == "restart"
                and isinstance(event.get("trigger"), str)
                and re.fullmatch(r"job [1-9][0-9]*", event["trigger"]) is not None
            )
            worker_restart = (
                isinstance(target, Mapping)
                and target.get("role") == "F"
                and scenario.data.get("expect", {}).get("engagement")
                == "s70-b4-worker-bounces"
            )
            client_checkpoint_transition = (
                isinstance(target, Mapping)
                and target.get("role") == "C"
                and event.get("action") in {"restart", "upgrade", "downgrade"}
                and isinstance(event.get("trigger"), str)
                and re.fullmatch(r"job [1-9][0-9]*", event["trigger"]) is not None
                and isinstance(event.get("receipt"), Mapping)
                and event["receipt"].get("schema") == "icefarm-client-transition-v1"
            )
            if client_checkpoint_transition:
                client_route_restart = False
        expected_fields = fields | (
            {"receipt"}
            if transition
            or header_edit
            or disk_fill
            or kill_interval
            or scheduler_restart
            or scheduler_active_loss
            or client_route_restart
            or worker_restart
            or client_checkpoint_transition
            else set()
        )
        if set(event) != expected_fields:
            raise CollectError(f"events.json event {index} has the wrong fields")
        if (
            type(event.get("fired_ms")) is not int
            or event["fired_ms"] < 0
            or type(event.get("event_epoch")) is not int
            or event["event_epoch"] != index + 1
            or type(event.get("event_index")) is not int
            or event["event_index"] != index
            or event.get("action") != expected[index]["action"]
            or event.get("instance") != expected[index]["instance"]
            or event.get("trigger") != expected[index]["trigger"]
            or type(event.get("workload_dispatch_count")) is not int
            or event["workload_dispatch_count"] < 0
            or (
                event.get("last_dispatched_job") is not None
                and (
                    type(event["last_dispatched_job"]) is not int
                    or event["last_dispatched_job"] < 0
                )
            )
        ):
            raise CollectError(f"events.json event {index} is invalid")
        if event["fired_ms"] < previous:
            raise CollectError("events.json is not chronological")
        if transition or client_checkpoint_transition:
            _validate_transition_receipt(
                event["receipt"],
                event,
                scenario,
                index,
                farm=farm,
                plan=plan,
                continuity=continuity,
                evidence=(
                    evidence
                    if validate_client_evidence
                    or event["receipt"].get("schema")
                    != "icefarm-client-transition-v1"
                    else None
                ),
            )
        elif header_edit:
            _validate_header_edit_receipt(
                event["receipt"], event, scenario, index, farm=farm, plan=plan
            )
        elif kill_interval:
            _validate_kill_interval(
                event, plan, evidence if validate_client_evidence else None
            )
        elif disk_fill:
            _validate_disk_fill_receipt(
                event["receipt"], event, scenario, index, plan=plan
            )
        elif scheduler_restart:
            _validate_scheduler_restart_receipt(
                event["receipt"], event, scenario, index,
                farm=farm, plan=plan, evidence=evidence,
            )
        elif scheduler_active_loss:
            _validate_scheduler_active_loss_receipt(
                event["receipt"], event, scenario, index,
                farm=farm,
                plan=plan,
                preflight=preflight,
                evidence=evidence,
                validate_retained_evidence=validate_client_evidence,
            )
        elif client_route_restart:
            _validate_client_route_restart_receipt(
                event["receipt"], event, scenario, index,
                farm=farm, plan=plan, evidence=evidence,
            )
        elif worker_restart:
            _validate_worker_restart_receipt(
                event["receipt"], event, scenario, index,
                farm=farm, plan=plan, evidence=evidence,
            )
        previous = event["fired_ms"]
    return events


def _validate_scheduler_active_loss_receipt(
    receipt: Any,
    event: Mapping[str, Any],
    scenario: ScenarioSpec,
    index: int,
    *,
    farm: FarmSpec | None,
    plan: dict[str, Any] | None,
    preflight: Mapping[str, Any] | None = None,
    evidence: Path | None,
    validate_retained_evidence: bool = True,
) -> None:
    prefix = f"events.json event {index} scheduler active loss"
    if farm is None or plan is None or evidence is None:
        raise CollectError(f"{prefix} needs authenticated farm, plan, and evidence")
    required = {"action", "after", "before", "compiler", "event_epoch", "instance", "lost_scheduler_generation", "lost_scheduler_job", "pre_fault", "quiescence", "schema", "turn"}
    schema = receipt.get("schema") if isinstance(receipt, Mapping) else None
    if schema in {
        SCHEDULER_ACTIVE_LOSS_SCHEMA_V4,
        SCHEDULER_ACTIVE_LOSS_SCHEMA_V5,
        SCHEDULER_ACTIVE_LOSS_SCHEMA,
    }:
        required.add("selection_last_dispatched_job")
    if schema in {SCHEDULER_ACTIVE_LOSS_SCHEMA_V5, SCHEDULER_ACTIVE_LOSS_SCHEMA}:
        required.add("admission_release")
    if schema == SCHEDULER_ACTIVE_LOSS_SCHEMA:
        required.add("capture_boundary")
    if (
        not isinstance(receipt, Mapping)
        or set(receipt) != required
        or receipt.get("schema")
        not in {
            SCHEDULER_ACTIVE_LOSS_SCHEMA_V1,
            SCHEDULER_ACTIVE_LOSS_SCHEMA_V2,
            SCHEDULER_ACTIVE_LOSS_SCHEMA_V3,
            SCHEDULER_ACTIVE_LOSS_SCHEMA_V4,
            SCHEDULER_ACTIVE_LOSS_SCHEMA_V5,
            SCHEDULER_ACTIVE_LOSS_SCHEMA,
        }
    ):
        raise CollectError(f"{prefix} has invalid receipt fields")
    strict = receipt.get("schema") != SCHEDULER_ACTIVE_LOSS_SCHEMA_V1
    listener_v3 = receipt.get("schema") in {
        SCHEDULER_ACTIVE_LOSS_SCHEMA_V3,
        SCHEDULER_ACTIVE_LOSS_SCHEMA_V4,
        SCHEDULER_ACTIVE_LOSS_SCHEMA_V5,
        SCHEDULER_ACTIVE_LOSS_SCHEMA,
    }
    if receipt.get("action") != event.get("action") or receipt.get("instance") != event.get("instance") or receipt.get("event_epoch") != event.get("event_epoch") or receipt.get("turn") not in scenario.data["workload"]["turns"]:
        raise CollectError(f"{prefix} is not bound to its timeline event")
    if schema in {SCHEDULER_ACTIVE_LOSS_SCHEMA_V5, SCHEDULER_ACTIVE_LOSS_SCHEMA}:
        admission = receipt.get("admission_release")
        expected_clients = scenario.data["workload"].get("clients")
        trigger = re.fullmatch(r"job ([1-9][0-9]*)", str(event.get("trigger", "")))
        clients = admission.get("clients") if isinstance(admission, Mapping) else None
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
        admission_valid = (
            isinstance(admission, Mapping)
            and set(admission) == {"clients", "schema", "serial_through"}
            and admission.get("schema") == SCHEDULER_ACTIVE_LOSS_ADMISSION_SCHEMA
            and trigger is not None
            and admission.get("serial_through") == int(trigger.group(1))
            and isinstance(expected_clients, list)
            and len(expected_clients) == 1
            and isinstance(expected_clients[0], str)
            and isinstance(clients, Mapping)
            and set(clients) == set(expected_clients)
        )
        if admission_valid:
            for name in expected_clients:
                value = clients[name]
                if (
                    not isinstance(value, Mapping)
                    or set(value) != gate_fields
                    or value.get("schema") != EVENT_GATE_SCHEMA
                    or value.get("action") != "resume"
                    or value.get("status") != "OPEN"
                    or value.get("client") != name
                    or value.get("turn") != receipt.get("turn")
                    or value.get("epoch") != receipt.get("event_epoch")
                    or value.get("active_before") != 1
                    or value.get("active_after") != 1
                    or any(
                        type(value.get(field)) is not int or value[field] < 0
                        for field in ("finished_ms", "started_ms")
                    )
                    or value["finished_ms"] < value["started_ms"]
                ):
                    admission_valid = False
                    break
        if not admission_valid:
            raise CollectError(f"{prefix} has no exact serialized admission release")
    if schema == SCHEDULER_ACTIVE_LOSS_SCHEMA:
        boundary = receipt.get("capture_boundary")
        trigger = re.fullmatch(r"job ([1-9][0-9]*)", str(event.get("trigger", "")))
        expected_clients = scenario.data["workload"].get("clients")
        arm = boundary.get("arm") if isinstance(boundary, Mapping) else None
        waited = boundary.get("wait") if isinstance(boundary, Mapping) else None
        released = boundary.get("release") if isinstance(boundary, Mapping) else None
        wait_fields = {"action", "index", "pid", "ready_ms", "schema"}
        release_fields = wait_fields | {"released_ms"}
        boundary_valid = (
            isinstance(boundary, Mapping)
            and set(boundary)
            == {
                "arm", "client", "event_epoch", "event_index", "release",
                "run_id", "schema", "serial_through", "turn", "wait",
            }
            and boundary.get("schema") == "icefarm-active-compiler-boundary-v1"
            and trigger is not None
            and boundary.get("serial_through") == int(trigger.group(1))
            and boundary.get("event_epoch") == event.get("event_epoch")
            and boundary.get("event_index") == index
            and boundary.get("run_id") == plan.get("run_id")
            and boundary.get("turn") == receipt.get("turn")
            and isinstance(expected_clients, list)
            and len(expected_clients) == 1
            and boundary.get("client") == expected_clients[0]
            and isinstance(arm, Mapping)
            and set(arm)
            == {
                "capture_mode", "daemon_pid", "daemon_start_ticks", "pid",
                "schema", "skip",
            }
            and arm.get("schema") == "icefarm-compiler-capture-arm-v2"
            and arm.get("capture_mode") == "ptrace-fork-v1"
            and type(arm.get("daemon_pid")) is int
            and arm["daemon_pid"] > 0
            and type(arm.get("daemon_start_ticks")) is int
            and arm["daemon_start_ticks"] > 0
            and type(arm.get("pid")) is int
            and arm["pid"] > 0
            and arm.get("skip") == 0
            and isinstance(waited, Mapping)
            and set(waited) == wait_fields
            and waited.get("schema")
            == "icefarm-active-compiler-boundary-control-v1"
            and waited.get("action") == "wait"
            and waited.get("index") == boundary.get("serial_through")
            and type(waited.get("pid")) is int
            and waited["pid"] > 0
            and type(waited.get("ready_ms")) is int
            and waited["ready_ms"] > 0
            and isinstance(released, Mapping)
            and set(released) == release_fields
            and released.get("schema")
            == "icefarm-active-compiler-boundary-control-v1"
            and released.get("action") == "release"
            and released.get("index") == waited.get("index")
            and released.get("pid") == waited.get("pid")
            and released.get("ready_ms") == waited.get("ready_ms")
            and type(released.get("released_ms")) is int
            and released["released_ms"] >= waited["ready_ms"]
            and event.get("workload_dispatch_count") == int(trigger.group(1))
            and receipt.get("lost_scheduler_job") == event.get("last_dispatched_job")
            and receipt.get("selection_last_dispatched_job")
            == receipt.get("lost_scheduler_job")
            and isinstance(receipt.get("compiler"), Mapping)
            and isinstance(receipt["compiler"].get("daemon"), Mapping)
            and arm.get("daemon_pid") == receipt["compiler"]["daemon"].get("pid")
            and arm.get("daemon_start_ticks")
            == receipt["compiler"]["daemon"].get("start_ticks")
        )
        if not boundary_valid:
            raise CollectError(
                f"{prefix} has no exact active compiler capture boundary"
            )
    v4_boundary_valid = (
        receipt.get("schema")
        in {
            SCHEDULER_ACTIVE_LOSS_SCHEMA_V4,
            SCHEDULER_ACTIVE_LOSS_SCHEMA_V5,
            SCHEDULER_ACTIVE_LOSS_SCHEMA,
        }
        and type(receipt.get("selection_last_dispatched_job")) is int
        and receipt["selection_last_dispatched_job"] > 0
        and receipt["selection_last_dispatched_job"]
        == event.get("last_dispatched_job")
        and type(receipt.get("lost_scheduler_job")) is int
        and receipt["lost_scheduler_job"] > 0
        and receipt.get("lost_scheduler_job")
        <= receipt["selection_last_dispatched_job"]
    )
    legacy_boundary_valid = (
        receipt.get("schema")
        not in {
            SCHEDULER_ACTIVE_LOSS_SCHEMA_V4,
            SCHEDULER_ACTIVE_LOSS_SCHEMA_V5,
            SCHEDULER_ACTIVE_LOSS_SCHEMA,
        }
        and receipt.get("lost_scheduler_job") == event.get("last_dispatched_job")
    )
    if (type(receipt.get("lost_scheduler_generation")) is not int or receipt["lost_scheduler_generation"] <= 0
            or type(receipt.get("lost_scheduler_job")) is not int or receipt["lost_scheduler_job"] <= 0
            or not (v4_boundary_valid or legacy_boundary_valid)):
        raise CollectError(f"{prefix} has no exact lost scheduler job boundary")
    snapshots = {}
    for side in ("before", "after"):
        value = receipt.get(side)
        if (not isinstance(value, Mapping) or set(value) != {"container_id", "started_at"}
                or not isinstance(value.get("container_id"), str) or SHA256_RE.fullmatch(value["container_id"]) is None
                or not isinstance(value.get("started_at"), str) or not value["started_at"]):
            raise CollectError(f"{prefix} has invalid scheduler {side} identity")
        snapshots[side] = value
    if snapshots["before"]["container_id"] != snapshots["after"]["container_id"] or snapshots["before"]["started_at"] == snapshots["after"]["started_at"]:
        raise CollectError(f"{prefix} does not prove same-container restart")
    worker = next(
        (
            item
            for item in plan["topology"]["instances"]
            if item.get("role") == "F"
        ),
        None,
    )
    if not isinstance(worker, Mapping):
        raise CollectError(f"{prefix} has no authenticated F")
    web_ports = plan.get("ports", {}).get("web")
    planned_web_port = (
        web_ports.get(worker["name"]) if isinstance(web_ports, Mapping) else None
    )
    if (planned_web_port is not None) is not strict:
        raise CollectError(f"{prefix} active-loss schema disagrees with its plan")
    expected_web_port = planned_web_port if strict else 8765
    if type(expected_web_port) is not int or not (1 <= expected_web_port <= 65535):
        raise CollectError(f"{prefix} has no valid planned F web port")
    compiler = receipt["compiler"]
    leader = compiler.get("leader") if isinstance(compiler, Mapping) else None
    stopped = compiler.get("stopped") if isinstance(compiler, Mapping) else None
    parent = compiler.get("daemon") if isinstance(compiler, Mapping) else None
    assignment = compiler.get("assignment") if isinstance(compiler, Mapping) else None
    compiler_fields = {
        "assignment",
        "container_id",
        "daemon",
        "group_gone",
        "leader",
        "stopped",
        "worker_before",
        "worker_after",
    }
    if strict:
        compiler_fields.add("listener")
    worker_fields = (
        {
            "container_id",
            "env",
            "image_id",
            "running",
            "runtime_path",
            "started_at",
        }
        if strict
        else {"container_id", "started_at"}
    )
    listener = compiler.get("listener") if isinstance(compiler, Mapping) else None
    expected_assignment_listener = {
        "host": "127.0.0.1",
        "port": expected_web_port,
    }
    if listener_v3 and isinstance(listener, Mapping):
        expected_assignment_listener.update(
            {
                "binding_evidence": LISTENER_BINDING_EVIDENCE,
                "socket_inode": listener.get("socket_inode"),
                "socket_uid": listener.get("socket_uid"),
            }
        )
    process_fields = {
        "argv",
        "comm",
        "exe",
        "exe_evidence",
        "pgid",
        "pid",
        "ppid",
        "start_ticks",
        "state",
        "uids",
    }

    def valid_process(value: Any) -> bool:
        return (
            isinstance(value, Mapping)
            and set(value) == process_fields
            and isinstance(value.get("argv"), list)
            and all(isinstance(item, str) for item in value["argv"])
            and bool(value["argv"])
            and value.get("comm") == "iceccd"
            and value.get("exe") == "/opt/icecream/sbin/iceccd"
            and value["argv"][0] == value["exe"]
            and value.get("exe_evidence") == "proc-cmdline+comm"
            and value.get("uids") == [65534, 65534, 65534, 65534]
            and all(
                type(value.get(field)) is int and value[field] >= minimum
                for field, minimum in (
                    ("pgid", 1),
                    ("pid", 1),
                    ("ppid", 0),
                    ("start_ticks", 1),
                )
            )
            and isinstance(value.get("state"), str)
            and len(value["state"]) == 1
        )

    group_gone = compiler.get("group_gone") if isinstance(compiler, Mapping) else None
    leader_state_valid = (
        leader.get("state") in {"T", "t"}
        if schema == SCHEDULER_ACTIVE_LOSS_SCHEMA and isinstance(leader, Mapping)
        else isinstance(leader, Mapping)
        and leader.get("state") not in {"T", "t", "Z", "X"}
    )
    strict_process_valid = (
        not strict
        or (
            valid_process(parent)
            and valid_process(leader)
            and valid_process(stopped)
            and all(
                leader.get(field) == stopped.get(field)
                for field in process_fields - {"state"}
            )
            and leader_state_valid
            and stopped.get("state") in {"T", "t"}
            and isinstance(group_gone, Mapping)
            and set(group_gone) == {"gone", "leader", "members", "schema"}
            and group_gone.get("schema") == "icefarm-compiler-group-gone-v1"
            and group_gone.get("gone") is True
            and group_gone.get("leader") is None
            and group_gone.get("members") == []
        )
    )
    if (not isinstance(compiler, Mapping) or set(compiler) != compiler_fields
            or not isinstance(compiler.get("container_id"), str) or SHA256_RE.fullmatch(compiler["container_id"]) is None
            or not isinstance(leader, Mapping) or not isinstance(stopped, Mapping)
            or not isinstance(parent, Mapping)
            or PurePosixPath(str(parent.get("exe", ""))).name != "iceccd"
            or parent.get("pid") == leader.get("pid")
            or leader.get("ppid") != parent.get("pid")
            or parent.get("argv") != leader.get("argv")
            or leader.get("pid") != leader.get("pgid") or leader.get("pid") != stopped.get("pid")
            or leader.get("pgid") != stopped.get("pgid") or leader.get("start_ticks") != stopped.get("start_ticks")
            or stopped.get("state") not in {"T", "t"}
            or not isinstance(compiler.get("group_gone"), Mapping)
            or compiler["group_gone"].get("gone") is not True
            or not strict_process_valid
            or not isinstance(compiler.get("worker_before"), Mapping)
            or not isinstance(compiler.get("worker_after"), Mapping)
            or set(compiler["worker_before"]) != worker_fields
            or set(compiler["worker_after"]) != worker_fields
            or compiler["worker_before"] != compiler["worker_after"]
            or not isinstance(compiler["worker_before"].get("container_id"), str)
            or SHA256_RE.fullmatch(compiler["worker_before"]["container_id"]) is None
            or (
                strict
                and (
                    compiler["container_id"]
                    != compiler["worker_before"]["container_id"]
                    or not isinstance(
                        compiler["worker_before"].get("started_at"), str
                    )
                    or not compiler["worker_before"]["started_at"]
                )
            )
            or not isinstance(assignment, Mapping)
            or set(assignment) != {"child", "client", "listener", "schema"}
            or assignment.get("schema")
            != (
                "icefarm-compiler-assignment-v2"
                if receipt.get("schema")
                in {
                    SCHEDULER_ACTIVE_LOSS_SCHEMA_V4,
                    SCHEDULER_ACTIVE_LOSS_SCHEMA_V5,
                    SCHEDULER_ACTIVE_LOSS_SCHEMA,
                }
                else "icefarm-compiler-assignment-v1"
            )
            or assignment.get("child", {}).get("pid") != leader.get("pid")
            or assignment.get("child", {}).get("pgid") != leader.get("pgid")
            or assignment.get("child", {}).get("generation") != receipt.get("lost_scheduler_generation")
            or assignment.get("client", {}).get("scheduler_job_id") != receipt.get("lost_scheduler_job")
            or assignment.get("client", {}).get("job_id") != receipt.get("lost_scheduler_job")
            or assignment.get("child", {}).get("owning_client_id") != assignment.get("client", {}).get("client_id")
            or set(assignment.get("child", {})) != {"generation", "kind", "owning_client_id", "pgid", "pid"}
            or assignment.get("child", {}).get("kind") != 0
            or set(assignment.get("client", {})) != {"client_id", "job_id", "scheduler_job_id"}
            or not all(isinstance(assignment.get("child", {}).get(key), int) and assignment["child"][key] > 0 for key in ("generation", "owning_client_id", "pgid", "pid"))
            or not all(isinstance(assignment.get("client", {}).get(key), int) and assignment["client"][key] > 0 for key in ("client_id", "job_id", "scheduler_job_id"))
            or assignment.get("listener") != expected_assignment_listener):
        raise CollectError(f"{prefix} has no exact stopped compiler-group identity")
    if strict:
        expected_env = {
            **worker.get("env", {}),
            "ICECC_WEB_HOSTPORT": f"127.0.0.1:{expected_web_port}",
        }
        expected_runtime = str(runtime_root(farm, dict(worker)))
        container_image = worker.get("container_image")
        images = preflight.get("images") if isinstance(preflight, Mapping) else None
        preflight_binding_valid = (
            isinstance(preflight, Mapping)
            and preflight.get("schema") == "icefarm-preflight-v1"
            and all(
                preflight.get(field) == plan.get(field)
                for field in (
                    "farm_digest",
                    "run_id",
                    "scenario_digest",
                    "topology_digest",
                )
            )
        )
        reference = (
            container_image.get("reference")
            if isinstance(container_image, Mapping)
            else None
        )
        closure = (
            container_image.get("closure_sha256")
            if isinstance(container_image, Mapping)
            else None
        )
        image_receipt = (
            images.get(f"container:{worker.get('host')}:{reference}")
            if isinstance(images, Mapping)
            else None
        )
        native_image_id = (
            image_receipt.get("id")
            if isinstance(image_receipt, Mapping)
            else None
        )
        expected_image_id = (
            native_image_id.removeprefix("sha256:")
            if isinstance(native_image_id, str)
            else None
        )
        container_image_authority_valid = (
            preflight_binding_valid
            and isinstance(image_receipt, Mapping)
            and image_receipt.get("reference") == reference
            and image_receipt.get("closure_sha256") == closure
            and isinstance(expected_image_id, str)
            and SHA256_RE.fullmatch(expected_image_id) is not None
        )
        listener_fields = {
            "daemon_pid",
            "daemon_start_ticks",
            "host",
            "port",
            "socket_inode",
        }
        if listener_v3:
            listener_fields.update({"binding_evidence", "socket_uid"})
        listener_valid = (
            isinstance(listener, Mapping)
            and set(listener) == listener_fields
            and listener.get("daemon_pid") == parent.get("pid")
            and listener.get("daemon_start_ticks") == parent.get("start_ticks")
            and listener.get("host") == "127.0.0.1"
            and listener.get("port") == expected_web_port
            and isinstance(listener.get("socket_inode"), str)
            and re.fullmatch(r"[1-9][0-9]*", listener["socket_inode"])
            is not None
        )
        if listener_v3:
            listener_valid = (
                listener_valid
                and listener.get("binding_evidence") == LISTENER_BINDING_EVIDENCE
                and listener.get("socket_uid") == parent.get("uids", [None, None])[1]
                and listener.get("socket_uid") == 65534
            )
        if (
            not listener_valid
            or not container_image_authority_valid
            or compiler["worker_before"].get("running") is not True
            or compiler["worker_before"].get("image_id") != expected_image_id
            or compiler["worker_before"].get("runtime_path") != expected_runtime
            or compiler["worker_before"].get("env") != expected_env
        ):
            raise CollectError(f"{prefix} has no bound F listener/runtime authority")
    pre = receipt["pre_fault"]
    if (not isinstance(pre, Mapping) or set(pre) != {"scheduler_log", "worker_log"}):
        raise CollectError(f"{prefix} has invalid pre-fault log offsets")
    offset = pre["worker_log"].get("offset") if isinstance(pre["worker_log"], Mapping) else None
    if type(offset) is not int or offset < 0:
        raise CollectError(f"{prefix} has invalid F log offset")
    quiescence = receipt["quiescence"]
    readiness_v2 = _requires_client_scheduler_readiness_v2(plan)
    client_readiness = (
        quiescence.get("client_readiness") if isinstance(quiescence, Mapping) else None
    )
    client_routes = (
        quiescence.get("client_routes") if isinstance(quiescence, Mapping) else None
    )
    client_evidence_valid = (
        isinstance(client_readiness, Mapping)
        and set(client_readiness) == set(scenario.data["workload"]["clients"])
        and isinstance(client_routes, Mapping)
        and set(client_routes) == set(scenario.data["workload"]["clients"])
    )
    if strict and not readiness_v2:
        client_evidence_valid = False
    if client_evidence_valid and readiness_v2:
        for name in scenario.data["workload"]["clients"]:
            client = next(
                item for item in plan["topology"]["instances"] if item["name"] == name
            )
            version = _planned_instance_version_at_epoch(
                scenario, client, receipt.get("event_epoch")
            )
            env = _planned_instance_env_at_epoch(
                scenario, client, receipt.get("event_epoch")
            )
            expected_host, expected_path = _transition_readiness_path(
                farm, plan, client
            )
            readiness = client_readiness[name]
            if strict:
                if (
                    not isinstance(readiness, Mapping)
                    or set(readiness) != {"ready", "witness"}
                    or readiness.get("ready") is not True
                    or not isinstance(readiness.get("witness"), Mapping)
                ):
                    client_evidence_valid = False
                    break
                witness = readiness["witness"]
            else:
                witness = readiness
            pair = client_routes[name]
            if (
                not _valid_client_scheduler_readiness_v2(
                    witness,
                    client,
                    cache_expected=(
                        version == 50 and env.get("ICECC_P50_MODE") == "on"
                    ),
                    expected_host=expected_host,
                    expected_path=expected_path,
                    evidence=evidence,
                )
                or not isinstance(pair, Mapping)
                or set(pair) != {"after", "before"}
                or pair.get("before") != pair.get("after")
                or witness.get("route") != pair
            ):
                client_evidence_valid = False
                break
    elif client_evidence_valid:
        client_evidence_valid = all(
            isinstance(witness, Mapping)
            and set(witness)
            == {"bytes", "cache_line", "cache_required", "connected_line", "host", "log_path", "offset"}
            and type(witness.get("bytes")) is int
            and witness["bytes"] >= 1
            and type(witness.get("cache_required")) is bool
            and isinstance(witness.get("connected_line"), str)
            and "Connected to scheduler (I am known as " in witness["connected_line"]
            and isinstance(witness.get("host"), str)
            and bool(witness["host"])
            and isinstance(witness.get("log_path"), str)
            and witness["log_path"].startswith("/")
            and type(witness.get("offset")) is int
            and witness["offset"] >= 0
            for witness in client_readiness.values()
        ) and all(
            isinstance(pair, Mapping)
            and set(pair) == {"before", "after"}
            and isinstance(pair["before"], Mapping)
            and isinstance(pair["after"], Mapping)
            and pair["before"] == pair["after"]
            and _valid_client_route_state(pair["before"])
            for pair in client_routes.values()
        )
    if (not isinstance(quiescence, Mapping)
            or set(quiescence) != {"client_readiness", "client_routes", "scheduler_snapshot", "scheduler_startup", "worker_snapshot"}
            or not isinstance(quiescence.get("scheduler_startup"), Mapping)
            or not isinstance(quiescence["scheduler_startup"].get("line"), str)
            or "ICECREAM scheduler" not in quiescence["scheduler_startup"]["line"]
            or not isinstance(quiescence.get("scheduler_snapshot"), str)
            or not quiescence["scheduler_snapshot"].strip()
            or not isinstance(quiescence.get("worker_snapshot"), str)
            or not quiescence["worker_snapshot"].strip()
            or not client_evidence_valid
            or not isinstance(quiescence.get("scheduler_snapshot"), str)
            or not isinstance(quiescence.get("worker_snapshot"), str)
            or any(
                re.search(rf"(^|\s){re.escape(item['name'])}(\s|$)", quiescence["worker_snapshot"], re.MULTILINE) is None
                for item in plan["topology"]["instances"] if item.get("role") == "F"
            )):
        raise CollectError(f"{prefix} lacks complete fresh scheduler/F/C rejoin evidence")
    if validate_retained_evidence:
        log = _text(_one_role_log(evidence, worker))
        tail = log.encode("utf-8")[offset:].decode("utf-8", "replace")
        pid, pgid = leader.get("pid"), leader.get("pgid")
        for phase in ("TERM", "KILL", "settled"):
            if not re.search(rf"session quiescence {phase} compiler pid={pid} pgid={pgid} generation=[0-9]+", tail):
                raise CollectError(f"{prefix} lacks post-offset F {phase} witness")


def _validate_disk_fill_receipt(
    receipt: Any,
    event: Mapping[str, Any],
    scenario: ScenarioSpec,
    index: int,
    *,
    plan: dict[str, Any] | None,
) -> None:
    prefix = f"events.json event {index} disk_fill"
    if plan is None:
        raise CollectError(f"{prefix} needs plan authority")
    target = next(
        (
            item
            for item in plan.get("topology", {}).get("instances", [])
            if isinstance(item, Mapping) and item.get("name") == event.get("instance")
        ),
        None,
    )
    if not isinstance(target, Mapping) or target.get("role") != "F":
        raise CollectError(f"{prefix} does not target a planned F instance")
    if scenario.data["timeline"][index] != {
        "action": "disk_fill",
        "instance": event.get("instance"),
        "trigger": event.get("trigger"),
    }:
        raise CollectError(f"{prefix} is not the exact declared fault")
    if not isinstance(receipt, Mapping) or set(receipt) not in ({
        "action",
        "after",
        "before",
        "event_epoch",
        "fill",
        "instance",
        "schema",
    }, {
        "action",
        "after",
        "before",
        "event_epoch",
        "fill",
        "instance",
        "schema",
        "admission_boundary",
    }):
        raise CollectError(f"{prefix} has invalid receipt fields")
    boundary = receipt.get("admission_boundary")
    if boundary is not None:
        if (
            not isinstance(boundary, Mapping)
            or set(boundary) != {"epoch", "pause", "resume", "schema", "turn"}
            or boundary.get("schema") != "icefarm-disk-fill-admission-boundary-v1"
            or not isinstance(boundary.get("epoch"), int)
            or not isinstance(boundary.get("turn"), str)
            or not isinstance(boundary.get("pause"), Mapping)
            or not isinstance(boundary.get("resume"), Mapping)
        ):
            raise CollectError(f"{prefix} has malformed admission boundary")
    if (
        receipt.get("schema") != DISK_FILL_SCHEMA
        or receipt.get("action") != "disk_fill"
        or receipt.get("action") != event.get("action")
        or receipt.get("instance") != event.get("instance")
        or receipt.get("event_epoch") != event.get("event_epoch")
    ):
        raise CollectError(f"{prefix} is not bound to the timeline")
    expected_mount = {
        "destination": CACHE_DISK_FAULT_PATH,
        "size_bytes": CACHE_DISK_FAULT_BYTES,
        "type": "tmpfs",
    }
    snapshots: dict[str, Mapping[str, Any]] = {}
    snapshot_fields = {
        "container_id", "container_name", "host", "image_closure_sha256",
        "image_id", "labels", "mount", "running", "runtime_path", "started_at",
    }
    expected_name = f"icefarm-{plan['run_id']}-{event['instance']}"
    expected_closure = target.get("image", {}).get("closure_sha256")
    if not isinstance(expected_closure, str) or SHA256_RE.fullmatch(expected_closure) is None:
        raise CollectError(f"{prefix} has no planned image closure")
    for side in ("before", "after"):
        value = receipt.get(side)
        if (
            not isinstance(value, Mapping)
            or set(value) != snapshot_fields
            or not isinstance(value.get("container_id"), str)
            or SHA256_RE.fullmatch(value["container_id"]) is None
            or value.get("container_name") != f"/{expected_name}"
            or value.get("host") != target.get("host")
            or value.get("image_closure_sha256") != expected_closure
            or not isinstance(value.get("image_id"), str)
            or SHA256_RE.fullmatch(value["image_id"]) is None
            or not isinstance(value.get("labels"), Mapping)
            or value["labels"].get("icefarm.run") != plan["run_id"]
            or value["labels"].get("icefarm.instance") != event["instance"]
            or value.get("mount") != expected_mount
            or value.get("running") is not True
            or not isinstance(value.get("runtime_path"), str)
            or not value["runtime_path"].startswith("/")
            or not isinstance(value.get("started_at"), str)
            or not value["started_at"]
        ):
            raise CollectError(f"{prefix} has invalid {side} snapshot")
        snapshots[side] = value
    if snapshots["before"] != snapshots["after"]:
        raise CollectError(f"{prefix} does not preserve the target container identity")
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
        or type(fill.get("filler_bytes")) is not int
        or not 0 < fill["filler_bytes"] <= CACHE_DISK_FAULT_BYTES
        or type(fill.get("available_before")) is not int
        or not CACHE_DISK_FAULT_MIN_HEADROOM_BYTES
        <= fill["available_before"]
        <= CACHE_DISK_FAULT_BYTES
        or fill["filler_bytes"] > fill["available_before"]
        or type(fill.get("available_after")) is not int
        or not 0 <= fill["available_after"] < 1024 * 1024
        or fill["available_after"] >= fill["available_before"]
        or fill.get("directory_uid") != 65534
        or fill.get("directory_gid") != 65534
        or fill.get("directory_mode") != 0o700
        or type(fill.get("elapsed_ms")) is not int
        or not 0 <= fill["elapsed_ms"] <= DISK_FILL_WATCHDOG_S * 1000
        or fill.get("minimum_headroom_bytes")
        != CACHE_DISK_FAULT_MIN_HEADROOM_BYTES
        or fill.get("watchdog_s") != DISK_FILL_WATCHDOG_S
        ):
            raise CollectError(f"{prefix} does not prove bounded ENOSPC")
    if (
        scenario.data.get("id") == "S95-cache-disk-full"
        and (
            type(event.get("workload_dispatch_count")) is not int
            or event["workload_dispatch_count"] < 12
            or type(event.get("last_dispatched_job")) is not int
            or event["last_dispatched_job"] < 1
        )
    ):
        raise CollectError(f"{prefix} fired before the twelfth dispatch threshold")


def _validate_header_edit_receipt(
    receipt: Any,
    event: Mapping[str, Any],
    scenario: ScenarioSpec,
    index: int,
    *,
    farm: FarmSpec | None,
    plan: dict[str, Any] | None,
) -> None:
    prefix = f"events.json event {index} header_edit"
    if farm is None or plan is None:
        raise CollectError(f"{prefix} needs farm and plan authority")
    if not isinstance(receipt, Mapping) or set(receipt) != {
        "action",
        "after",
        "before",
        "cache_invalidation",
        "coordination",
        "event_epoch",
        "instance",
        "schema",
        "turn",
    }:
        raise CollectError(f"{prefix} has invalid receipt fields")
    if (
        receipt.get("schema") != HEADER_EDIT_SCHEMA
        or receipt.get("action") != "header_edit"
        or receipt.get("action") != event.get("action")
        or receipt.get("instance") != event.get("instance")
        or receipt.get("event_epoch") != event.get("event_epoch")
        or receipt.get("turn") not in scenario.data["workload"]["turns"]
    ):
        raise CollectError(f"{prefix} is not bound to the timeline")
    expected = scenario.data["timeline"][index]
    expected_path = expected.get("path")
    if not isinstance(expected_path, str):
        raise CollectError(f"{prefix} has no declared header path")
    snapshot_fields = {
        "container_id",
        "header_path",
        "header_sha256",
        "p29_cache_files",
        "running",
        "started_at",
    }
    snapshots: dict[str, Mapping[str, Any]] = {}
    managed = {
        "p29-system-source-fingerprint-v1.cache",
        "p29-system-source-fingerprint-v1.lock",
    }
    for side in ("before", "after"):
        value = receipt.get(side)
        if not isinstance(value, Mapping) or set(value) != snapshot_fields:
            raise CollectError(f"{prefix} has invalid {side} snapshot")
        cache_files = value.get("p29_cache_files")
        if (
            not isinstance(value.get("container_id"), str)
            or SHA256_RE.fullmatch(value["container_id"]) is None
            or not isinstance(value.get("started_at"), str)
            or not value["started_at"]
            or value.get("running") is not True
            or value.get("header_path") != expected_path
            or not isinstance(value.get("header_sha256"), str)
            or SHA256_RE.fullmatch(value["header_sha256"]) is None
            or not isinstance(cache_files, list)
            or any(item not in managed for item in cache_files)
            or len(set(cache_files)) != len(cache_files)
            or (
                side == "before"
                and cache_files
                != [
                    "p29-system-source-fingerprint-v1.cache",
                    "p29-system-source-fingerprint-v1.lock",
                ]
            )
        ):
            raise CollectError(f"{prefix} has malformed {side} snapshot")
        snapshots[side] = value
    if (
        snapshots["before"]["container_id"] != snapshots["after"]["container_id"]
        or snapshots["before"]["started_at"] == snapshots["after"]["started_at"]
        or snapshots["before"]["header_sha256"] == snapshots["after"]["header_sha256"]
        or snapshots["after"]["p29_cache_files"]
    ):
        raise CollectError(f"{prefix} does not prove a fresh changed F state")
    invalidation = receipt.get("cache_invalidation")
    if (
        not isinstance(invalidation, Mapping)
        or set(invalidation) != {"directory", "files", "removed"}
        or invalidation.get("directory") != "/var/cache/icecream/p50-runtime"
        or invalidation.get("files")
        != [
            "p29-system-source-fingerprint-v1.cache",
            "p29-system-source-fingerprint-v1.lock",
        ]
        or invalidation.get("removed") != snapshots["before"]["p29_cache_files"]
        or any(item not in managed for item in invalidation.get("removed", []))
        or invalidation.get("removed")
        != [
            "p29-system-source-fingerprint-v1.cache",
            "p29-system-source-fingerprint-v1.lock",
        ]
    ):
        raise CollectError(f"{prefix} has invalid scoped cache invalidation")
    coordination = receipt.get("coordination")
    if not isinstance(coordination, Mapping) or set(coordination) != {
        "clients",
        "ready_ms",
        "readiness",
        "resume",
        "scheduler_rejoin",
    }:
        raise CollectError(f"{prefix} has invalid coordination")
    ready_ms = coordination.get("ready_ms")
    if type(ready_ms) is not int or ready_ms < 0 or ready_ms != event.get("fired_ms"):
        raise CollectError(f"{prefix} readiness is not bound to the event epoch")
    expected_clients = set(scenario.data["workload"]["clients"])
    pauses = coordination.get("clients")
    resumes = coordination.get("resume")
    if (
        not expected_clients
        or not isinstance(pauses, Mapping)
        or not isinstance(resumes, Mapping)
        or set(pauses) != expected_clients
        or set(resumes) != expected_clients
    ):
        raise CollectError(f"{prefix} does not cover every workload client")
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
                or value.get("schema") != EVENT_GATE_SCHEMA
                or value.get("action") != action
                or value.get("status") != status
                or value.get("client") != name
                or value.get("turn") != receipt["turn"]
                or value.get("epoch") != receipt["event_epoch"]
                or any(
                    type(value.get(field)) is not int or value[field] < 0
                    for field in ("active_after", "active_before", "finished_ms", "started_ms")
                )
                or (
                    action in {"pause", "quiesce"}
                    and value.get("active_before", 0) < 1
                )
                or value["finished_ms"] < value["started_ms"]
            ):
                raise CollectError(f"{prefix} has invalid {action} receipt for {name}")
        if pauses[name]["active_after"] != 0:
            raise CollectError(f"{prefix} has invalid drain/readiness ordering")
    target = next(
        (item for item in plan["topology"]["instances"] if item["name"] == event["instance"]),
        None,
    )
    if not isinstance(target, Mapping) or target.get("role") != "F":
        raise CollectError(f"{prefix} target is not a planned F")
    scheduler = next(
        (item for item in plan["topology"]["instances"] if item["role"] == "S"),
        None,
    )
    if not isinstance(scheduler, Mapping):
        raise CollectError(f"{prefix} has no planned scheduler")
    expected_host, expected_log = _transition_readiness_path(farm, plan, target)
    readiness = coordination.get("readiness")
    if (
        not isinstance(readiness, Mapping)
        or set(readiness) != {"cache_line", "host", "line", "log_path", "offset", "role"}
        or readiness.get("host") != expected_host
        or readiness.get("log_path") != expected_log
        or readiness.get("role") != "F"
        or not isinstance(readiness.get("line"), str)
        or "ICECREAM daemon " not in readiness["line"]
        or not isinstance(readiness.get("cache_line"), str)
        or CACHE_READY_RE.search(readiness["cache_line"]) is None
        or type(readiness.get("offset")) is not int
        or readiness["offset"] < 0
    ):
        raise CollectError(f"{prefix} has invalid fresh F/cache READY evidence")
    expected_scheduler_host, expected_scheduler_log = _transition_readiness_path(
        farm, plan, scheduler
    )
    expected_profile = scheduler.get("env", {}).get("ICECC_P50_PROFILE")
    rejoin = coordination.get("scheduler_rejoin")
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
    role_login = ROLE_LOGIN_RE.search(login_line) if isinstance(login_line, str) else None
    cache_relogin = LOGIN_RE.search(cache_line) if isinstance(cache_line, str) else None
    cache_login = CACHE_LOGIN_RE.search(cache_line) if isinstance(cache_line, str) else None
    profile_token = expected_profile.lower() if isinstance(expected_profile, str) else None
    advertised = cache_login.group(4).split() if cache_login is not None else []
    if (
        not isinstance(rejoin, Mapping)
        or set(rejoin) != rejoin_fields
        or rejoin.get("host") != expected_scheduler_host
        or rejoin.get("log_path") != expected_scheduler_log
        or rejoin.get("scheduler") != scheduler["name"]
        or rejoin.get("target") != target["name"]
        or rejoin.get("profile") != expected_profile
        or rejoin.get("role_protocol") != 50
        or rejoin.get("cache_protocol") != 1
        or type(rejoin.get("offset")) is not int
        or rejoin["offset"] < 0
        or role_login is None
        or role_login.group(1) != target["name"]
        or role_login.group(2) != "50"
        or cache_relogin is None
        or cache_relogin.group(1) != target["name"]
        or cache_login is None
        or cache_login.group(2) != "1"
        or cache_login.group(3) != "1"
        or profile_token is None
        or profile_token not in advertised
    ):
        raise CollectError(f"{prefix} has invalid fresh scheduler rejoin evidence")


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
            type(value.get(field)) is int and value[field] >= minimum
            for field, minimum in (
                ("pid", 1),
                ("ppid", 0),
                ("start_ticks", 1),
                ("uid", 0),
            )
        )
    )


def _validate_client_route_restart_receipt(
    receipt: Any,
    event: Mapping[str, Any],
    scenario: ScenarioSpec,
    index: int,
    *,
    farm: FarmSpec | None,
    plan: dict[str, Any] | None,
    evidence: Path | None,
) -> None:
    prefix = f"events.json event {index} client route-owner restart"
    if farm is None or plan is None:
        raise CollectError(f"{prefix} needs farm and plan authority")
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
        raise CollectError(f"{prefix} has an invalid receipt")
    if (
        receipt.get("schema") != CLIENT_ROUTE_RESTART_SCHEMA
        or receipt.get("action") != "restart"
        or receipt.get("action") != event.get("action")
        or receipt.get("instance") != event.get("instance")
        or receipt.get("event_epoch") != event.get("event_epoch")
        or receipt.get("turn") not in scenario.data["workload"]["turns"]
    ):
        raise CollectError(f"{prefix} receipt is not bound")
    target = next(
        (
            item
            for item in plan["topology"]["instances"]
            if item["name"] == receipt["instance"]
        ),
        None,
    )
    if not isinstance(target, Mapping) or target.get("role") != "C":
        raise CollectError(f"{prefix} does not target a client")

    snapshots: dict[str, Mapping[str, Any]] = {}
    snapshot_fields = {
        "container_id",
        "container_started_at",
        "daemon",
        "route_owner",
    }
    for side in ("before", "after"):
        value = receipt.get(side)
        if (
            not isinstance(value, Mapping)
            or set(value) != snapshot_fields
            or not isinstance(value.get("container_id"), str)
            or SHA256_RE.fullmatch(value["container_id"]) is None
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
            raise CollectError(f"{prefix} has an invalid {side} snapshot")
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
        raise CollectError(f"{prefix} does not prove only a fresh route owner")

    coordination = receipt.get("coordination")
    if not isinstance(coordination, Mapping) or set(coordination) != {
        "clients",
        "ready_ms",
        "readiness",
        "resume",
        "signal",
    }:
        raise CollectError(f"{prefix} has invalid coordination")
    ready_ms = coordination.get("ready_ms")
    if type(ready_ms) is not int or ready_ms < 0 or ready_ms != event.get("fired_ms"):
        raise CollectError(f"{prefix} readiness is not epoch-bound")
    expected_clients = set(scenario.data["workload"]["clients"])
    pauses = coordination.get("clients")
    resumes = coordination.get("resume")
    if (
        not expected_clients
        or receipt["instance"] not in expected_clients
        or not isinstance(pauses, Mapping)
        or not isinstance(resumes, Mapping)
        or set(pauses) != expected_clients
        or set(resumes) != expected_clients
    ):
        raise CollectError(f"{prefix} does not cover every workload client")
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
                or value.get("schema") != EVENT_GATE_SCHEMA
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
                or (
                    action in {"pause", "quiesce"}
                    and value.get("active_before", 0) < 1
                )
                or value["finished_ms"] < value["started_ms"]
            ):
                raise CollectError(f"{prefix} has invalid {action} receipt for {name}")
        if pauses[name]["active_after"] != 0:
            raise CollectError(f"{prefix} has invalid drain/readiness ordering")

    signal_receipt = coordination.get("signal")
    if (
        not isinstance(signal_receipt, Mapping)
        or set(signal_receipt)
        != {"daemon", "mechanism", "route_owner", "schema", "sent_ms", "signal"}
        or signal_receipt.get("schema") != CLIENT_ROUTE_SIGNAL_SCHEMA
        or signal_receipt.get("mechanism") != "pidfd_send_signal"
        or signal_receipt.get("signal") != 9
        or type(signal_receipt.get("sent_ms")) is not int
        or signal_receipt["sent_ms"] < 0
        or signal_receipt.get("daemon") != snapshots["before"]["daemon"]
        or signal_receipt.get("route_owner") != snapshots["before"]["route_owner"]
        or any(
            pauses[name]["finished_ms"] > signal_receipt["sent_ms"]
            for name in expected_clients
        )
        or signal_receipt["sent_ms"] > ready_ms
    ):
        raise CollectError(f"{prefix} has an invalid exact signal receipt")

    readiness = coordination.get("readiness")
    expected_host, expected_path = _transition_readiness_path(farm, plan, target)
    if (
        not isinstance(readiness, Mapping)
        or set(readiness)
        != {"host", "lifecycle", "line", "log_path", "offset", "state"}
        or readiness.get("host") != expected_host
        or readiness.get("log_path") != expected_path
        or type(readiness.get("offset")) is not int
        or readiness["offset"] < 0
        or readiness.get("state") != 2
        or readiness.get("lifecycle") != 3
        or not isinstance(readiness.get("line"), str)
        or CACHE_READY_RE.search(readiness["line"]) is None
        or not _retained_log_witness(
            evidence, target, readiness["offset"], readiness["line"]
        )
    ):
        raise CollectError(f"{prefix} has invalid fresh READY evidence")


def _validate_scheduler_restart_receipt(
    receipt: Any,
    event: Mapping[str, Any],
    scenario: ScenarioSpec,
    index: int,
    *,
    farm: FarmSpec | None,
    plan: dict[str, Any] | None,
    evidence: Path | None,
) -> None:
    if farm is None or plan is None:
        raise CollectError(
            f"events.json event {index} scheduler restart needs farm and plan authority"
        )
    if not isinstance(receipt, dict) or set(receipt) != {
        "action",
        "after",
        "before",
        "coordination",
        "event_epoch",
        "instance",
        "schema",
        "turn",
    }:
        raise CollectError(f"events.json event {index} has invalid scheduler-restart receipt")
    if (
        receipt.get("schema") != SCHEDULER_RESTART_SCHEMA
        or receipt.get("action") != "restart"
        or receipt.get("action") != event.get("action")
        or receipt.get("instance") != event.get("instance")
        or receipt.get("event_epoch") != event.get("event_epoch")
        or receipt.get("turn") not in scenario.data["workload"]["turns"]
    ):
        raise CollectError(f"events.json event {index} scheduler-restart receipt is not bound")
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
            raise CollectError(
                f"events.json event {index} has invalid scheduler {side} snapshot"
            )
        snapshots[side] = snapshot
    if (
        snapshots["before"]["container_id"] != snapshots["after"]["container_id"]
        or snapshots["before"]["started_at"] == snapshots["after"]["started_at"]
    ):
        raise CollectError(
            f"events.json event {index} does not prove an in-place scheduler restart"
        )

    coordination = receipt.get("coordination")
    required = {
        "client_readiness",
        "clients",
        "ready_ms",
        "resume",
        "scheduler_snapshot",
        "scheduler_startup",
        "workers",
        "worker_snapshot",
    }
    if not isinstance(coordination, Mapping) or set(coordination) != required:
        raise CollectError(
            f"events.json event {index} has invalid restart coordination"
        )
    ready_ms = coordination.get("ready_ms")
    if type(ready_ms) is not int or ready_ms < 0 or event.get("fired_ms") != ready_ms:
        raise CollectError(
            f"events.json event {index} restart readiness is not epoch-bound"
        )
    expected_clients = set(scenario.data["workload"]["clients"])
    pauses = coordination.get("clients")
    resumes = coordination.get("resume")
    client_readiness = coordination.get("client_readiness")
    if (
        not isinstance(pauses, Mapping)
        or not isinstance(resumes, Mapping)
        or not isinstance(client_readiness, Mapping)
        or set(pauses) != expected_clients
        or set(resumes) != expected_clients
        or set(client_readiness) != expected_clients
        or not expected_clients
    ):
        raise CollectError(
            f"events.json event {index} restart does not cover every workload client"
        )
    scheduler = next(
        item for item in plan["topology"]["instances"] if item["role"] == "S"
    )
    scheduler_cache_capable = (
        _planned_instance_version_at_epoch(
            scenario, scheduler, receipt.get("event_epoch")
        )
        == 50
    )
    readiness_v2 = _requires_client_scheduler_readiness_v2(plan)
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
                or value.get("schema") != EVENT_GATE_SCHEMA
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
                or (
                    action in {"pause", "quiesce"}
                    and value.get("active_before", 0) < 1
                )
                or value["finished_ms"] < value["started_ms"]
            ):
                raise CollectError(
                    f"events.json event {index} has invalid {action} receipt for {name}"
                )
        if pauses[name]["active_after"] != 0:
            raise CollectError(
                f"events.json event {index} resumed {name} before active jobs drained"
            )
        client = next(
            item
            for item in plan["topology"]["instances"]
            if item["name"] == name
        )
        witness = client_readiness[name]
        expected_host, expected_path = _transition_readiness_path(farm, plan, client)
        client_version = _planned_instance_version_at_epoch(
            scenario, client, receipt.get("event_epoch")
        )
        client_env = _planned_instance_env_at_epoch(
            scenario, client, receipt.get("event_epoch")
        )
        cache_expected = (
            client_version == 50 and client_env.get("ICECC_P50_MODE") == "on"
        )
        valid_readiness = _valid_client_scheduler_readiness_v2(
            witness,
            client,
            cache_expected=cache_expected,
            expected_host=expected_host,
            expected_path=expected_path,
            evidence=evidence,
        ) if readiness_v2 else (
            isinstance(witness, Mapping)
            and set(witness) == {
                "cache_line", "cache_required", "connected_line", "host", "log_path", "offset"
            }
            and witness.get("host") == expected_host
            and witness.get("log_path") == expected_path
            and type(witness.get("offset")) is int
            and witness["offset"] >= 0
            and witness.get("cache_required")
            is (scheduler_cache_capable and cache_expected)
            and isinstance(witness.get("connected_line"), str)
            and "Connected to scheduler (I am known as " in witness["connected_line"]
            and (
                not (scheduler_cache_capable and cache_expected)
                or (
                    isinstance(witness.get("cache_line"), str)
                    and CACHE_READY_RE.search(witness["cache_line"]) is not None
                )
            )
            and (
                (scheduler_cache_capable and cache_expected)
                or witness.get("cache_line") is None
            )
            and _retained_log_witness(
                evidence, client, witness.get("offset"), witness.get("connected_line")
            )
            and (
                not (scheduler_cache_capable and cache_expected)
                or _retained_log_witness(
                    evidence, client, witness.get("offset"), witness.get("cache_line")
                )
            )
        )
        if not valid_readiness:
            raise CollectError(
                f"events.json event {index} has invalid fresh scheduler readiness for {name}"
            )

    startup = coordination.get("scheduler_startup")
    expected_host, expected_path = _transition_readiness_path(farm, plan, scheduler)
    if (
        not isinstance(startup, Mapping)
        or set(startup) != {"host", "line", "log_path", "offset", "role"}
        or startup.get("host") != expected_host
        or startup.get("log_path") != expected_path
        or startup.get("role") != "S"
        or type(startup.get("offset")) is not int
        or startup["offset"] < 0
        or not isinstance(startup.get("line"), str)
        or READINESS_SCHEDULER_RE.search(startup["line"]) is None
        or not _retained_log_witness(
            evidence, scheduler, startup["offset"], startup["line"]
        )
    ):
        raise CollectError(
            f"events.json event {index} has invalid fresh scheduler readiness"
        )
    expected_workers = sorted(
        item["name"] for item in plan["topology"]["instances"] if item["role"] == "F"
    )
    scheduler_snapshot = coordination.get("scheduler_snapshot")
    worker_snapshot = coordination.get("worker_snapshot")
    if (
        coordination.get("workers") != expected_workers
        or not isinstance(scheduler_snapshot, str)
        or not scheduler_snapshot
        or not isinstance(worker_snapshot, str)
        or not worker_snapshot
        or any(
            re.search(rf"(^|\s){re.escape(name)}(\s|$)", worker_snapshot, re.MULTILINE)
            is None
            for name in expected_workers
        )
    ):
        raise CollectError(
            f"events.json event {index} does not prove every planned worker rejoined"
        )


def _validate_worker_restart_receipt(
    receipt: Any,
    event: Mapping[str, Any],
    scenario: ScenarioSpec,
    index: int,
    *,
    farm: FarmSpec | None,
    plan: dict[str, Any] | None,
    evidence: Path | None,
) -> None:
    prefix = f"events.json event {index} worker restart"
    if farm is None or plan is None:
        raise CollectError(f"{prefix} needs farm and plan authority")
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
        raise CollectError(f"{prefix} has an invalid receipt")
    if (
        receipt.get("schema") != WORKER_RESTART_SCHEMA
        or receipt.get("action") != "restart"
        or receipt.get("action") != event.get("action")
        or receipt.get("instance") != event.get("instance")
        or receipt.get("event_epoch") != event.get("event_epoch")
        or receipt.get("turn") not in scenario.data["workload"]["turns"]
    ):
        raise CollectError(f"{prefix} receipt is not bound")
    target = next(
        (
            item
            for item in plan["topology"]["instances"]
            if item["name"] == receipt["instance"]
        ),
        None,
    )
    if not isinstance(target, Mapping) or target.get("role") != "F":
        raise CollectError(f"{prefix} target is not a planned F")
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
            raise CollectError(f"{prefix} has an invalid {side} snapshot")
        snapshots[side] = snapshot
    if (
        snapshots["before"]["container_id"]
        != snapshots["after"]["container_id"]
        or snapshots["before"]["started_at"] == snapshots["after"]["started_at"]
    ):
        raise CollectError(f"{prefix} does not prove an in-place fresh restart")

    coordination = receipt.get("coordination")
    if not isinstance(coordination, Mapping) or set(coordination) != {
        "ready_ms",
        "readiness",
        "scheduler_rejoin",
        "worker_snapshot",
        "workers",
    }:
        raise CollectError(f"{prefix} has invalid coordination")
    ready_ms = coordination.get("ready_ms")
    if type(ready_ms) is not int or ready_ms < 0 or ready_ms != event.get("fired_ms"):
        raise CollectError(f"{prefix} readiness is not epoch-bound")
    expected_workers = sorted(
        item["name"]
        for item in plan["topology"]["instances"]
        if item["role"] == "F"
    )
    worker_snapshot = coordination.get("worker_snapshot")
    if (
        coordination.get("workers") != expected_workers
        or not isinstance(worker_snapshot, str)
        or not worker_snapshot
        or any(
            re.search(
                rf"(^|\s){re.escape(name)}(\s|$)",
                worker_snapshot,
                re.MULTILINE,
            )
            is None
            for name in expected_workers
        )
    ):
        raise CollectError(f"{prefix} does not prove every worker ready")

    expected_host, expected_log = _transition_readiness_path(farm, plan, target)
    readiness = coordination.get("readiness")
    if (
        not isinstance(readiness, Mapping)
        or set(readiness)
        != {"cache_line", "host", "line", "log_path", "offset", "role"}
        or readiness.get("host") != expected_host
        or readiness.get("log_path") != expected_log
        or readiness.get("role") != "F"
        or not isinstance(readiness.get("line"), str)
        or DAEMON_START_RE.search(readiness["line"]) is None
        or not isinstance(readiness.get("cache_line"), str)
        or CACHE_READY_RE.search(readiness["cache_line"]) is None
        or type(readiness.get("offset")) is not int
        or readiness["offset"] < 0
    ):
        raise CollectError(f"{prefix} has invalid fresh F/cache readiness")

    scheduler = next(
        item for item in plan["topology"]["instances"] if item["role"] == "S"
    )
    expected_scheduler_host, expected_scheduler_log = _transition_readiness_path(
        farm, plan, scheduler
    )
    expected_profile = scheduler.get("env", {}).get("ICECC_P50_PROFILE")
    rejoin = coordination.get("scheduler_rejoin")
    login_line = rejoin.get("login_line") if isinstance(rejoin, Mapping) else None
    cache_line = rejoin.get("cache_line") if isinstance(rejoin, Mapping) else None
    role_login = (
        ROLE_LOGIN_RE.search(login_line) if isinstance(login_line, str) else None
    )
    cache_relogin = LOGIN_RE.search(cache_line) if isinstance(cache_line, str) else None
    cache_login = (
        CACHE_LOGIN_RE.search(cache_line) if isinstance(cache_line, str) else None
    )
    advertised = cache_login.group(4).split() if cache_login is not None else []
    profile_token = (
        expected_profile.lower() if isinstance(expected_profile, str) else None
    )
    if (
        not isinstance(rejoin, Mapping)
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
        or rejoin.get("host") != expected_scheduler_host
        or rejoin.get("log_path") != expected_scheduler_log
        or rejoin.get("scheduler") != scheduler["name"]
        or rejoin.get("target") != target["name"]
        or rejoin.get("profile") != expected_profile
        or rejoin.get("role_protocol") != 50
        or rejoin.get("cache_protocol") != 1
        or type(rejoin.get("offset")) is not int
        or rejoin["offset"] < 0
        or type(rejoin.get("bytes")) is not int
        or rejoin["bytes"] < 1
        or not isinstance(rejoin.get("sha256"), str)
        or SHA256_RE.fullmatch(rejoin["sha256"]) is None
        or role_login is None
        or role_login.group(1) != target["name"]
        or role_login.group(2) != "50"
        or cache_relogin is None
        or cache_relogin.group(1) != target["name"]
        or cache_login is None
        or cache_login.group(2) != "1"
        or cache_login.group(3) != "1"
        or profile_token is None
        or profile_token not in advertised
        or not isinstance(rejoin.get("loss_job_ids"), list)
        or len(rejoin["loss_job_ids"]) != len(set(rejoin["loss_job_ids"]))
        or any(type(item) is not int or item < 1 for item in rejoin["loss_job_ids"])
        or not _retained_log_witness(
            evidence, target, readiness["offset"], readiness["line"]
        )
        or not _retained_log_witness(
            evidence, target, readiness["offset"], readiness["cache_line"]
        )
        or not _retained_log_witness_exact(
            evidence,
            scheduler,
            rejoin["offset"],
            rejoin["login_line"],
            rejoin["bytes"],
            rejoin["sha256"],
        )
        or not _retained_log_witness(
            evidence, scheduler, rejoin["offset"], rejoin["cache_line"]
        )
    ):
        raise CollectError(f"{prefix} has invalid fresh scheduler rejoin")


def _event_role_hash(farm: FarmSpec, label: str, role: str) -> str:
    match = re.match(r"^p(43|44|50)(?:s[0-9]+)?(?:-|$)", label.rsplit(":", 1)[-1], re.IGNORECASE)
    if match is None:
        raise CollectError(f"image {label!r} has no protocol generation")
    authority = farm.data["authority"]["images"].get(label)
    if not isinstance(authority, dict):
        raise CollectError(f"image {label!r} is absent from the authority")
    role_key = {"S": "scheduler", "C": "client", "F": "daemon"}[role]
    if authority.get("kind") == "scheduler-mutant" and role == "S":
        override = authority.get("role_overrides", {}).get("scheduler")
        digest = override.get("sha256") if isinstance(override, dict) else None
    elif authority.get("kind") == "daemon-mutant" and role == "F":
        override = authority.get("role_overrides", {}).get("daemon")
        digest = override.get("sha256") if isinstance(override, dict) else None
    else:
        store = farm.data["authority"]["role_stores"].get(match.group(1), {})
        entry = store.get(role_key) if isinstance(store, dict) else None
        digest = entry.get("sha256") if isinstance(entry, dict) else None
    if not isinstance(digest, str) or SHA256_RE.fullmatch(digest) is None:
        raise CollectError(f"image {label!r} has no authenticated {role_key} hash")
    return digest


def _event_image_version(label: str) -> int:
    match = re.match(r"^p(43|44|50)(?:s[0-9]+)?(?:-|$)", label.rsplit(":", 1)[-1], re.IGNORECASE)
    if match is None:
        raise CollectError(f"image {label!r} has no protocol generation")
    return int(match.group(1))


def _planned_instance_version_at_epoch(
    scenario: ScenarioSpec,
    instance: Mapping[str, Any],
    event_epoch: object,
) -> int:
    """Resolve a planned endpoint generation after an authenticated event epoch."""

    version = instance.get("version")
    timeline = scenario.data.get("timeline")
    images = scenario.data.get("images")
    name = instance.get("name")
    if (
        type(version) is not int
        or version < 1
        or type(event_epoch) is not int
        or event_epoch < 0
        or not isinstance(timeline, list)
        or event_epoch > len(timeline)
        or not isinstance(images, Mapping)
        or not isinstance(name, str)
    ):
        raise CollectError("cannot resolve planned instance generation at event epoch")
    for event in timeline[:event_epoch]:
        if (
            not isinstance(event, Mapping)
            or event.get("instance") != name
            or event.get("action") not in {"upgrade", "downgrade"}
        ):
            continue
        alias = event.get("image")
        label = images.get(alias) if isinstance(alias, str) else None
        if not isinstance(label, str):
            raise CollectError("timeline generation transition has no image authority")
        version = _event_image_version(label)
    return version


def _planned_instance_env_at_epoch(
    scenario: ScenarioSpec,
    instance: Mapping[str, Any],
    event_epoch: object,
) -> dict[str, str]:
    """Resolve a planned endpoint environment after an authenticated epoch."""

    env = instance.get("env")
    timeline = scenario.data.get("timeline")
    images = scenario.data.get("images")
    name = instance.get("name")
    role = instance.get("role")
    image = instance.get("image")
    label = image.get("label") if isinstance(image, Mapping) else None
    if (
        not isinstance(env, Mapping)
        or any(type(key) is not str or type(value) is not str for key, value in env.items())
        or type(event_epoch) is not int
        or event_epoch < 0
        or not isinstance(timeline, list)
        or event_epoch > len(timeline)
        or not isinstance(images, Mapping)
        or not isinstance(name, str)
        or role not in {"S", "C", "F"}
        or not isinstance(label, str)
    ):
        raise CollectError("cannot resolve planned instance environment at event epoch")
    result = dict(env)
    current_label = label
    for event in timeline[:event_epoch]:
        if not isinstance(event, Mapping) or event.get("instance") != name:
            continue
        action = event.get("action")
        if action not in {"upgrade", "downgrade", "env_set"}:
            continue
        if action in {"upgrade", "downgrade"}:
            alias = event.get("image")
            target_label = images.get(alias) if isinstance(alias, str) else None
            if not isinstance(target_label, str):
                raise CollectError("timeline environment transition has no image authority")
            current_label = target_label
        result = _transition_target_env(
            {"env": result}, event, str(role), current_label
        )
    return result


def _transition_target_env(
    before: Mapping[str, Any], event: Mapping[str, Any], role: str, target_label: str
) -> dict[str, str]:
    result = dict(before["env"])
    if event["action"] == "env_set":
        update = event.get("env")
        if not isinstance(update, dict) or any(
            type(key) is not str or type(value) is not str for key, value in update.items()
        ):
            raise CollectError("env_set timeline has malformed environment")
        result.update(update)
        return result
    version = _event_image_version(target_label)
    if role == "S":
        if version == 50:
            result.setdefault("ICECC_P50_PROFILE", "P29V1")
        else:
            result.pop("ICECC_P50_PROFILE", None)
    elif role == "C":
        if version == 50:
            result.setdefault("ICECC_P50_MODE", "on")
        else:
            result.pop("ICECC_P50_MODE", None)
            result.pop("ICECC_P50_FAULT_INJECTION", None)
    return result


def _transition_readiness_path(
    farm: FarmSpec, plan: dict[str, Any], instance: Mapping[str, Any]
) -> tuple[str, str]:
    leaf = {"S": "scheduler.log", "C": "client-daemon.log", "F": "iceccd.log"}[instance["role"]]
    return instance["host"], str(
        instance_root(farm, instance["host"], plan["run_id"], instance["name"])
        / "log"
        / leaf
    )


def _validate_transition_coordination(
    coordination: Mapping[str, Any],
    receipt: Mapping[str, Any],
    event: Mapping[str, Any],
    instance: Mapping[str, Any],
    scenario: ScenarioSpec,
    farm: FarmSpec,
    plan: dict[str, Any],
    evidence: Path | None,
) -> bool:
    role = instance["role"]
    readiness_v2 = _requires_client_scheduler_readiness_v2(plan)
    scheduler_cache_capable = False
    if role == "S":
        after = receipt.get("after")
        if not isinstance(after, Mapping) or not isinstance(after.get("image"), str):
            return False
        try:
            scheduler_cache_capable = _event_image_version(after["image"]) == 50
        except CollectError:
            return False
    common = {
        "clients",
        "ready_ms",
        "resume",
        "scheduler_snapshot",
        "worker_snapshot",
        "workers",
    }
    role_fields = (
        {"client_readiness", "scheduler_startup"}
        if role == "S"
        else {"scheduler_worker_rejoin"}
    )
    if set(coordination) != common | role_fields:
        return False
    ready_ms = coordination.get("ready_ms")
    if type(ready_ms) is not int or ready_ms < 0 or ready_ms != event.get("fired_ms"):
        return False
    expected_workers = sorted(
        item["name"] for item in plan["topology"]["instances"] if item["role"] == "F"
    )
    if (
        coordination.get("workers") != expected_workers
        or not isinstance(coordination.get("scheduler_snapshot"), str)
        or not coordination["scheduler_snapshot"]
        or not isinstance(coordination.get("worker_snapshot"), str)
        or any(
            re.search(rf"(^|\s){re.escape(name)}(\s|$)", coordination["worker_snapshot"], re.MULTILINE)
            is None
            for name in expected_workers
        )
    ):
        return False
    expected_clients = set(scenario.data["workload"]["clients"]) if receipt.get("turn") else set()
    pauses = coordination.get("clients")
    resumes = coordination.get("resume")
    if (
        not isinstance(pauses, Mapping)
        or not isinstance(resumes, Mapping)
        or set(pauses) != expected_clients
        or set(resumes) != expected_clients
    ):
        return False
    gate_fields = {
        "action", "active_after", "active_before", "client", "epoch",
        "finished_ms", "schema", "started_ms", "status", "turn",
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
                or value.get("turn") != receipt.get("turn")
                or value.get("epoch") != receipt.get("event_epoch")
                or any(type(value.get(field)) is not int or value[field] < 0 for field in (
                    "active_after", "active_before", "finished_ms", "started_ms"
                ))
                or (
                    action in {"pause", "quiesce"}
                    and value.get("active_before", 0) < 1
                )
                or value["finished_ms"] < value["started_ms"]
            ):
                return False
        if pauses[name]["active_after"] != 0:
            return False

    readiness = receipt.get("readiness")
    expected_host, expected_path = _transition_readiness_path(farm, plan, instance)
    if (
        not isinstance(readiness, Mapping)
        or readiness.get("host") != expected_host
        or readiness.get("log_path") != expected_path
        or readiness.get("role") != role
        or not isinstance(readiness.get("offset"), int)
        or readiness["offset"] < 0
        or not isinstance(readiness.get("line"), str)
    ):
        return False
    if role == "S":
        if READINESS_SCHEDULER_RE.search(readiness["line"]) is None:
            return False
        startup = coordination.get("scheduler_startup")
        clients = coordination.get("client_readiness")
        if startup != readiness or not isinstance(clients, Mapping) or set(clients) != expected_clients:
            return False
        for name in sorted(expected_clients):
            client = next((item for item in plan["topology"]["instances"] if item["name"] == name), None)
            witness = clients[name]
            if not isinstance(client, Mapping) or client.get("role") != "C":
                return False
            expected_client_host, expected_client_path = _transition_readiness_path(
                farm, plan, client
            )
            client_version = _planned_instance_version_at_epoch(
                scenario, client, receipt.get("event_epoch")
            )
            client_env = _planned_instance_env_at_epoch(
                scenario, client, receipt.get("event_epoch")
            )
            cache_expected = (
                client_version == 50 and client_env.get("ICECC_P50_MODE") == "on"
            )
            if readiness_v2:
                if not _valid_client_scheduler_readiness_v2(
                    witness,
                    client,
                    cache_expected=cache_expected,
                    expected_host=expected_client_host,
                    expected_path=expected_client_path,
                    evidence=evidence,
                ):
                    return False
            else:
                required_cache = scheduler_cache_capable and cache_expected
                if (
                    not isinstance(witness, Mapping)
                    or set(witness) != {"cache_line", "cache_required", "connected_line", "host", "log_path", "offset"}
                    or witness.get("host") != expected_client_host
                    or witness.get("cache_required") is not required_cache
                    or witness.get("log_path") != expected_client_path
                    or not isinstance(witness.get("offset"), int)
                    or not isinstance(witness.get("connected_line"), str)
                    or "Connected to scheduler (I am known as " not in witness["connected_line"]
                    or (required_cache and (not isinstance(witness.get("cache_line"), str) or CACHE_READY_RE.search(witness["cache_line"]) is None))
                    or (not required_cache and witness.get("cache_line") is not None)
                    or not _retained_log_witness(
                        evidence, client, witness.get("offset"), witness.get("connected_line")
                    )
                    or (
                        required_cache
                        and not _retained_log_witness(
                            evidence, client, witness.get("offset"), witness.get("cache_line")
                        )
                    )
                ):
                    return False
    else:
        if DAEMON_START_RE.search(readiness["line"]) is None:
            return False
        rejoin = coordination.get("scheduler_worker_rejoin")
        scheduler = next((item for item in plan["topology"]["instances"] if item["role"] == "S"), None)
        event_index = event.get("event_index")
        target_alias = (
            scenario.data["timeline"][event_index].get("image")
            if type(event_index) is int and 0 <= event_index < len(scenario.data["timeline"])
            else None
        )
        target_label = scenario.data["images"].get(target_alias) if isinstance(target_alias, str) else None
        target_protocol = _event_image_version(target_label) if isinstance(target_label, str) else None
        if (
            not isinstance(scheduler, Mapping)
            or not isinstance(rejoin, Mapping)
            or set(rejoin) != {"bytes", "host", "line", "log_path", "offset", "role_protocol", "target"}
            or rejoin.get("host") != scheduler.get("host")
            or not rejoin["log_path"].endswith(f"/{scheduler['name']}/log/scheduler.log")
            or rejoin.get("target") != instance.get("name")
            or rejoin.get("role_protocol") != target_protocol
            or not isinstance(rejoin.get("offset"), int)
            or not isinstance(rejoin.get("line"), str)
            or re.search(rf"\blogin\s+{re.escape(instance['name'])}\s+protocol\s+version:\s*{target_protocol}\b", rejoin["line"]) is None
            or not _retained_log_witness(
                evidence,
                scheduler,
                rejoin.get("offset"),
                rejoin.get("line"),
            )
        ):
            return False
    return True


def _validate_client_transition_coordination(
    coordination: Mapping[str, Any],
    receipt: Mapping[str, Any],
    event: Mapping[str, Any],
    instance: Mapping[str, Any],
    scenario: ScenarioSpec,
    farm: FarmSpec,
    plan: dict[str, Any],
    evidence: Path | None,
) -> bool:
    expected_clients = set(scenario.data["workload"]["clients"])
    if set(coordination) != {"clients", "ready_ms", "relaunch", "resume"}:
        return False
    ready_ms = coordination.get("ready_ms")
    if type(ready_ms) is not int or ready_ms < 0 or ready_ms != event.get("fired_ms"):
        return False
    pauses = coordination.get("clients")
    resumes = coordination.get("resume")
    if (
        not isinstance(pauses, Mapping)
        or not isinstance(resumes, Mapping)
        or set(pauses) != expected_clients
        or set(resumes) != expected_clients
    ):
        return False
    gate_fields = {
        "action", "active_after", "active_before", "client", "epoch",
        "finished_ms", "schema", "started_ms", "status", "turn",
    }
    for name in sorted(expected_clients):
        pause = pauses[name]
        resume = resumes[name]
        if (
            not isinstance(pause, Mapping)
            or not isinstance(resume, Mapping)
            or set(pause) != gate_fields
            or set(resume) != gate_fields
            or pause.get("schema") != "icefarm-event-gate-v1"
            or resume.get("schema") != "icefarm-event-gate-v1"
            or pause.get("action") != "quiesce"
            or pause.get("status") != "QUIESCED"
            or resume.get("action") != "resume"
            or resume.get("status") != "OPEN"
            or pause.get("client") != name
            or resume.get("client") != name
            or pause.get("turn") != receipt.get("turn")
            or resume.get("turn") != receipt.get("turn")
            or pause.get("epoch") != receipt.get("event_epoch")
            or resume.get("epoch") != receipt.get("event_epoch")
            or pause.get("active_after") != 0
            or any(
                type(value.get(field)) is not int or value[field] < 0
                for value in (pause, resume)
                for field in ("active_after", "active_before", "finished_ms", "started_ms", "epoch")
            )
            or pause.get("active_before", 0) < 1
        ):
            return False
    checkpoints = receipt.get("checkpoints")
    if not isinstance(checkpoints, Mapping) or set(checkpoints) != expected_clients:
        return False
    checkpoint_fields = {
        "checkpoint_sha256", "client", "completed_rows", "completed_rows_sha256",
        "expected_jobs", "schema", "status", "turn", "worklist_sha256",
    }
    for name in sorted(expected_clients):
        checkpoint = checkpoints[name]
        if (
            not isinstance(checkpoint, Mapping)
            or set(checkpoint) != checkpoint_fields
            or checkpoint.get("schema") != "icefarm-workload-checkpoint-v1"
            or checkpoint.get("status") != "QUIESCED"
            or checkpoint.get("client") != name
            or checkpoint.get("turn") != receipt.get("turn")
            or type(checkpoint.get("expected_jobs")) is not int
            or checkpoint["expected_jobs"] < 1
            or not SHA256_RE.fullmatch(str(checkpoint.get("checkpoint_sha256")))
            or not SHA256_RE.fullmatch(str(checkpoint.get("completed_rows_sha256")))
            or not SHA256_RE.fullmatch(str(checkpoint.get("worklist_sha256")))
            or not isinstance(checkpoint.get("completed_rows"), list)
            or not checkpoint["completed_rows"]
        ):
            return False
        rows = checkpoint["completed_rows"]
        seen: set[int] = set()
        for row in rows:
            if (
                not isinstance(row, Mapping)
                or set(row) != {"index", "path", "sha256"}
                or type(row.get("index")) is not int
                or not 1 <= row["index"] <= checkpoint["expected_jobs"]
                or row["index"] in seen
                or not isinstance(row.get("path"), str)
                or row["path"] != f"jobs/{row['index']:06d}/result.tsv"
                or not isinstance(row.get("sha256"), str)
                or SHA256_RE.fullmatch(row["sha256"]) is None
            ):
                return False
            seen.add(row["index"])
            if evidence is not None:
                path = _checkpoint_result_path(
                    evidence, name, receipt["turn"], row["path"]
                )
                if path is None:
                    return False
                if hashlib.sha256(path.read_bytes()).hexdigest() != row["sha256"]:
                    return False
        canonical = json.dumps(rows, sort_keys=True, separators=(",", ":")).encode()
        body = dict(checkpoint)
        digest = body.pop("checkpoint_sha256")
        if (
            checkpoint["completed_rows_sha256"] != hashlib.sha256(canonical).hexdigest()
            or digest
            != hashlib.sha256(
                json.dumps(body, sort_keys=True, separators=(",", ":")).encode()
            ).hexdigest()
        ):
            return False
    relaunch = coordination.get("relaunch")
    if not isinstance(relaunch, Mapping) or set(relaunch) != expected_clients:
        return False
    for name in sorted(expected_clients):
        value = relaunch[name]
        if (
            not isinstance(value, Mapping)
            or set(value) != {"client", "expected_jobs", "failures", "jobs", "status"}
            or value.get("client") != name
            or value.get("status") != "COMPLETE"
            or type(value.get("expected_jobs")) is not int
            or type(value.get("jobs")) is not int
            or type(value.get("failures")) is not int
            or value["expected_jobs"] != checkpoints[name]["expected_jobs"]
            or value["jobs"] != value["expected_jobs"]
            or value["failures"] != 0
        ):
            return False
    return True


def _validate_transition_receipt(
    receipt: Any,
    event: Mapping[str, Any],
    scenario: ScenarioSpec,
    index: int,
    *,
    farm: FarmSpec | None = None,
    plan: dict[str, Any] | None = None,
    continuity: dict[str, dict[str, Any]] | None = None,
    evidence: Path | None = None,
) -> None:
    coordinated = isinstance(receipt, dict) and receipt.get("schema") in {
        "icefarm-transition-v2",
        "icefarm-client-transition-v1",
    }
    client_coordinated = isinstance(receipt, dict) and receipt.get("schema") == "icefarm-client-transition-v1"
    expected_fields = {
        "action",
        "instance",
        "before",
        "after",
        "preflight",
        "readiness",
    }
    if coordinated:
        expected_fields |= {"coordination", "event_epoch", "schema", "turn"}
    if client_coordinated:
        expected_fields |= {"checkpoints", "client_readiness"}
    if not isinstance(receipt, dict) or set(receipt) != expected_fields:
        raise CollectError(f"events.json event {index} has invalid transition receipt")
    if receipt["action"] != event["action"] or receipt["instance"] != event["instance"]:
        raise CollectError(f"events.json event {index} transition receipt is not bound")
    instance = None
    if coordinated:
        if receipt.get("event_epoch") != event.get("event_epoch"):
            raise CollectError(f"events.json event {index} transition epoch is not bound")
        if farm is None or plan is None:
            raise CollectError(f"events.json event {index} coordinated transition needs farm and plan")
        instance = next(
            (item for item in plan["topology"]["instances"] if item["name"] == event["instance"]),
            None,
        )
        expected_roles = {"C"} if client_coordinated else {"S", "F"}
        if not isinstance(instance, Mapping) or instance.get("role") not in expected_roles:
            raise CollectError(f"events.json event {index} coordinated transition has unsupported role")
        coordination = receipt.get("coordination")
        valid_coordination = (
            _validate_client_transition_coordination(
                coordination, receipt, event, instance, scenario, farm, plan, evidence
            )
            if client_coordinated
            else _validate_transition_coordination(
                coordination,
                receipt,
                event,
                instance,
                scenario,
                farm,
                plan,
                evidence,
            )
        )
        if not isinstance(coordination, Mapping) or not valid_coordination:
            raise CollectError(f"events.json event {index} has invalid transition coordination")
    preflight = receipt["preflight"]
    if not isinstance(preflight, dict) or set(preflight) != {
        "image_closure_sha256",
        "role_sha256",
        "runtime_path",
    }:
        raise CollectError(f"events.json event {index} has invalid preflight receipt")
    for key in ("image_closure_sha256", "role_sha256"):
        value = preflight[key]
        if (
            type(value) is not str
            or len(value) != 64
            or any(character not in "0123456789abcdef" for character in value)
        ):
            raise CollectError(f"events.json event {index} has invalid preflight digest")
    if (
        type(preflight["runtime_path"]) is not str
        or not preflight["runtime_path"].startswith("/")
        or ".." in preflight["runtime_path"].split("/")
    ):
        raise CollectError(f"events.json event {index} has invalid runtime path")
    readiness = receipt["readiness"]
    if not isinstance(readiness, dict) or set(readiness) != {
        "host",
        "line",
        "log_path",
        "offset",
        "role",
    }:
        raise CollectError(f"events.json event {index} has invalid readiness receipt")
    if (
        type(readiness["host"]) is not str
        or not readiness["host"]
        or type(readiness["line"]) is not str
        or not readiness["line"]
        or type(readiness["log_path"]) is not str
        or not readiness["log_path"].startswith("/")
        or type(readiness["offset"]) is not int
        or readiness["offset"] < 0
        or readiness["role"] not in {"S", "C", "F"}
    ):
        raise CollectError(f"events.json event {index} has malformed readiness receipt")
    if client_coordinated:
        target_host, target_path = _transition_readiness_path(farm, plan, instance)
        if (
            readiness["role"] != "C"
            or readiness["host"] != target_host
            or readiness["log_path"] != target_path
        ):
            raise CollectError(
                f"events.json event {index} readiness witness is not bound to the client"
            )
        client_readiness = receipt.get("client_readiness")
        after_snapshot = receipt.get("after")
        if (
            not isinstance(after_snapshot, Mapping)
            or not isinstance(after_snapshot.get("image"), str)
            or not isinstance(after_snapshot.get("env"), Mapping)
        ):
            raise CollectError(f"events.json event {index} has invalid client after state")
        try:
            after_version = _event_image_version(after_snapshot["image"])
        except CollectError as exc:
            raise CollectError(
                f"events.json event {index} has invalid client image version"
            ) from exc
        cache_expected = (
            after_version == 50
            and after_snapshot["env"].get("ICECC_P50_MODE") == "on"
        )
        if _requires_client_scheduler_readiness_v2(plan):
            valid_client_readiness = _valid_client_scheduler_readiness_v2(
                client_readiness,
                instance,
                cache_expected=cache_expected,
                expected_host=target_host,
                expected_path=target_path,
                evidence=evidence,
            ) and client_readiness.get("offset") == readiness["offset"]
        else:
            legacy_cache_required = (
                cache_expected
                and _planned_instance_version_at_epoch(
                    scenario,
                    next(
                        item
                        for item in plan["topology"]["instances"]
                        if item["role"] == "S"
                    ),
                    receipt.get("event_epoch"),
                )
                == 50
            )
            valid_client_readiness = (
                isinstance(client_readiness, Mapping)
                and set(client_readiness) == {
                    "cache_line", "cache_required", "connected_line", "host", "log_path", "offset"
                }
                and client_readiness.get("host") == target_host
                and client_readiness.get("log_path") == target_path
                and type(client_readiness.get("offset")) is int
                and client_readiness["offset"] >= 0
                and client_readiness["offset"] == readiness["offset"]
                and isinstance(client_readiness.get("connected_line"), str)
                and "Connected to scheduler (I am known as " in client_readiness["connected_line"]
                and client_readiness.get("cache_required") is legacy_cache_required
                and (
                    not legacy_cache_required
                    or (
                        isinstance(client_readiness.get("cache_line"), str)
                        and CACHE_READY_RE.search(client_readiness["cache_line"]) is not None
                    )
                )
                and (legacy_cache_required or client_readiness.get("cache_line") is None)
                and _retained_log_witness(
                    evidence, instance, client_readiness.get("offset"), client_readiness.get("connected_line")
                )
                and (
                    not legacy_cache_required
                    or _retained_log_witness(
                        evidence, instance, client_readiness.get("offset"), client_readiness.get("cache_line")
                    )
                )
            )
        if not valid_client_readiness:
            raise CollectError(f"events.json event {index} has invalid client readiness")
    readiness_pattern = (
        READINESS_SCHEDULER_RE if readiness["role"] == "S" else DAEMON_START_RE
    )
    if readiness_pattern.search(readiness["line"]) is None:
        raise CollectError(f"events.json event {index} readiness witness is not a startup observation")
    snapshot_fields = {"container_id", "closure_sha256", "env", "image", "role_sha256"}
    for side in ("before", "after"):
        snapshot = receipt[side]
        if not isinstance(snapshot, dict) or set(snapshot) != snapshot_fields:
            raise CollectError(f"events.json event {index} has invalid {side} snapshot")
        for key in ("closure_sha256", "role_sha256"):
            value = snapshot[key]
            if (
                type(value) is not str
                or len(value) != 64
                or any(character not in "0123456789abcdef" for character in value)
            ):
                raise CollectError(f"events.json event {index} has invalid {side} digest")
        if (
            type(snapshot["container_id"]) is not str
            or re.fullmatch(r"[0-9a-f]{64}", snapshot["container_id"]) is None
            or type(snapshot["image"]) is not str
            or not snapshot["image"]
            or type(snapshot["env"]) is not dict
            or any(type(key) is not str or type(value) is not str for key, value in snapshot["env"].items())
        ):
            raise CollectError(f"events.json event {index} has invalid {side} snapshot")
    before = receipt["before"]
    after = receipt["after"]
    if (
        preflight["image_closure_sha256"] != after["closure_sha256"]
        or preflight["role_sha256"] != after["role_sha256"]
        or not preflight["runtime_path"].endswith(
            f"/runtimes/{after['closure_sha256']}/root"
        )
    ):
        raise CollectError(f"events.json event {index} preflight receipt is not bound to after state")
    if event["action"] in {"upgrade", "downgrade"}:
        image_alias = scenario.data["timeline"][index].get("image")
        if not isinstance(image_alias, str):
            raise CollectError(f"events.json event {index} lacks an image alias")
        expected_label = scenario.data["images"].get(image_alias)
        if after["image"] != expected_label:
            raise CollectError(f"events.json event {index} after image is not bound to the timeline")
        before_version = _event_image_version(before["image"])
        after_version = _event_image_version(after["image"])
        if event["action"] == "upgrade" and after_version <= before_version:
            raise CollectError(f"events.json event {index} upgrade did not increase protocol generation")
        if event["action"] == "downgrade" and after_version >= before_version:
            raise CollectError(f"events.json event {index} downgrade did not decrease protocol generation")
    elif event["action"] == "env_set":
        update = scenario.data["timeline"][index].get("env")
        if (
            before["image"] != after["image"]
            or before["closure_sha256"] != after["closure_sha256"]
            or before["role_sha256"] != after["role_sha256"]
            or not isinstance(update, dict)
            or any(after["env"].get(key) != value for key, value in update.items())
        ):
            raise CollectError(f"events.json event {index} env_set receipt is not bound")
    if farm is None or plan is None or continuity is None:
        return
    if instance is None:
        instance = next(
            (item for item in plan["topology"]["instances"] if item["name"] == event["instance"]),
            None,
        )
    if instance is None:
        raise CollectError(f"events.json event {index} targets an unknown planned instance")
    expected_ready_host, expected_ready_path = _transition_readiness_path(farm, plan, instance)
    if (
        readiness["role"] != instance["role"]
        or readiness["host"] != expected_ready_host
        or readiness["log_path"] != expected_ready_path
        or not _retained_log_witness(
            evidence,
            instance,
            readiness["offset"],
            readiness["line"],
        )
    ):
        raise CollectError(f"events.json event {index} readiness witness is not bound to the instance")
    previous = continuity.get(event["instance"])
    if previous is None:
        expected_before = {
            "closure_sha256": instance["image"]["closure_sha256"],
            "env": dict(instance.get("env", {})),
            "image": instance["image"]["label"],
            "role_sha256": instance["sha256"],
        }
        if any(before[key] != value for key, value in expected_before.items()):
            raise CollectError(f"events.json event {index} before snapshot differs from the planned instance")
    elif any(before[key] != previous[key] for key in ("closure_sha256", "env", "image", "role_sha256", "container_id")):
        raise CollectError(f"events.json event {index} before snapshot breaks transition continuity")
    if after["container_id"] == before["container_id"]:
        raise CollectError(f"events.json event {index} did not create a fresh container")
    if event["action"] in {"upgrade", "downgrade"}:
        label = scenario.data["images"][scenario.data["timeline"][index]["image"]]
    else:
        label = before["image"]
    authority = farm.data["authority"]["images"].get(label)
    if not isinstance(authority, dict) or after["closure_sha256"] != authority.get("closure_sha256"):
        raise CollectError(f"events.json event {index} after closure is not authority-bound")
    if after["role_sha256"] != _event_role_hash(farm, label, instance["role"]):
        raise CollectError(f"events.json event {index} after role hash is not authority-bound")
    if event["action"] == "env_set" and after["image"] != before["image"]:
        raise CollectError(f"events.json event {index} env_set changed the image identity")
    if after["env"] != _transition_target_env(before, scenario.data["timeline"][index], instance["role"], label):
        raise CollectError(f"events.json event {index} after environment is not bound to the transition")
    continuity[event["instance"]] = after


def _authenticated_rejoin_line(raw: bytes, rejoin: Mapping[str, Any]) -> int:
    """Locate a unique cache rejoin in its checksum-bound, complete log window.

    Role/container/worker identity must already have passed restart receipt
    validation. Never select a matching line outside the captured window.
    Returned positions use the scheduler lifecycle parser's 1-based lines.
    """
    offset, size = rejoin.get("offset"), rejoin.get("bytes")
    needle = rejoin.get("cache_line")
    digest = rejoin.get("sha256")
    if (
        type(offset) is not int or offset < 0
        or type(size) is not int or size < 1
        or offset + size > len(raw)
        or (offset and raw[offset - 1:offset] != b"\n")
        or not isinstance(needle, str) or not needle
        or "\n" in needle or "\r" in needle
        or not isinstance(digest, str)
    ):
        raise CollectError("worker rejoin has invalid log window")
    window = raw[offset:offset + size]
    if not window.endswith(b"\n") or hashlib.sha256(window).hexdigest() != digest:
        raise CollectError("worker rejoin log window is incomplete or unauthenticated")
    matches = [index for index, line in enumerate(window.splitlines(), start=1)
               if line == needle.encode("utf-8")]
    if len(matches) != 1:
        raise CollectError("worker rejoin log boundary is missing or ambiguous")
    return len(raw[:offset].splitlines()) + matches[0]


def _worker_rejoin_boundaries(
    farm: FarmSpec, scenario: ScenarioSpec, plan: dict[str, Any],
    evidence: Path, events: list[dict[str, Any]],
) -> list[dict[str, Any]]:
    """Authenticate S70's three worker incarnations independently of store GUIDs."""
    if (scenario.data.get("expect", {}).get("engagement") != "s70-b4-worker-bounces"
            or len(events) != 3):
        raise CollectError("worker rejoin boundaries require the complete bounce timeline")
    scheduler = next(item for item in plan["topology"]["instances"] if item["role"] == "S")
    path = _one_role_log(evidence, scheduler)
    if path is None:
        raise CollectError("worker rejoin scheduler log is absent")
    raw = path.read_bytes()
    # This contract is deliberately limited to a stable scheduler incarnation.
    if sum("ICECREAM scheduler" in line for line in raw.decode("utf-8").splitlines()) != 1:
        raise CollectError("worker rejoin requires exactly one scheduler incarnation")
    boundaries: list[dict[str, Any]] = []
    for index, event in enumerate(events):
        if (event.get("action") != "restart" or event.get("instance") != "F1"
                or event.get("event_epoch") != index + 1):
            raise CollectError("worker rejoin timeline identity is invalid")
        receipt = event["receipt"]
        _validate_worker_restart_receipt(
            receipt, event, scenario, index, farm=farm, plan=plan, evidence=evidence,
        )
        rejoin = receipt["coordination"]["scheduler_rejoin"]
        line = _authenticated_rejoin_line(raw, rejoin)
        if boundaries and line <= boundaries[-1]["scheduler_rejoin_line"]:
            raise CollectError("worker rejoin boundaries are not strictly ordered")
        boundaries.append({
            "event_epoch": index + 1,
            "worker_instance": event["instance"],
            "scheduler_generation": 1,
            "scheduler_rejoin_line": line,
            "container_id": receipt["after"]["container_id"],
            "started_at": receipt["after"]["started_at"],
            "rejoin_sha256": rejoin["sha256"],
        })
    return boundaries


def _epoch_at(events: list[dict[str, Any]], dispatch_ms: int) -> int:
    return sum(event["fired_ms"] <= dispatch_ms for event in events)


def _active_scheduler_started_ms(event: Mapping[str, Any]) -> int | None:
    """Return the authenticated replacement start bound for an active loss.

    ``fired_ms`` records completion of the active-loss event, after scheduler
    restart, readiness, and admission release.  A retry may therefore be
    dispatched by the replacement scheduler before that completion timestamp.
    Docker's retained ``after.started_at`` identity is the exact earlier bound.
    """

    if event.get("action") != "scheduler-loss-active":
        return None
    receipt = event.get("receipt")
    after = receipt.get("after") if isinstance(receipt, Mapping) else None
    started_at = after.get("started_at") if isinstance(after, Mapping) else None
    fired_ms = event.get("fired_ms")
    if not isinstance(started_at, str) or type(fired_ms) is not int:
        return None
    try:
        parsed = datetime.fromisoformat(started_at.replace("Z", "+00:00"))
    except ValueError:
        return None
    if parsed.tzinfo is None:
        return None
    started_ms = int(parsed.timestamp() * 1000)
    return started_ms if 0 <= started_ms <= fired_ms else None


def _scheduler_transition_allows_early_dispatch(
    event: Mapping[str, Any], dispatch_ms: int
) -> bool:
    fired_ms = event["fired_ms"]
    if fired_ms // 1000 == dispatch_ms // 1000:
        return True
    started_ms = _active_scheduler_started_ms(event)
    return started_ms is not None and started_ms <= dispatch_ms < fired_ms


def _scheduler_dispatch_epoch(
    events: list[dict[str, Any]],
    scheduler_name: str,
    dispatch_ms: int,
    scheduler_generation: int,
) -> int:
    """Resolve a dispatch epoch using the scheduler's exact incarnation.

    Scheduler logs have whole-second timestamps while transition receipts have
    millisecond timestamps.  A dispatch from a freshly started scheduler can
    therefore appear a few milliseconds before the event that started it.
    Only the exact scheduler-log generation may resolve that same-second
    ambiguity.  Active-loss completion is additionally allowed to lag a retry
    dispatch, but only back to the retained replacement-container start.
    Larger timestamp disagreements and impossible generations fail closed.
    """

    if (
        not isinstance(scheduler_name, str)
        or not scheduler_name
        or type(dispatch_ms) is not int
        or dispatch_ms < 0
        or type(scheduler_generation) is not int
        or scheduler_generation < 1
    ):
        raise CollectError("scheduler dispatch epoch has invalid identity")
    replacement_epochs: list[int] = []
    for index, event in enumerate(events):
        fired_ms = event.get("fired_ms")
        if type(fired_ms) is not int or fired_ms < 0:
            raise CollectError("event has no valid fired timestamp")
        if (
            event.get("instance") == scheduler_name
            and event.get("action") in SCHEDULER_GENERATION_ACTIONS
        ):
            replacement_epochs.append(index + 1)

    if not replacement_epochs:
        return _epoch_at(events, dispatch_ms)

    generation_index = scheduler_generation - 1
    if generation_index > len(replacement_epochs):
        raise CollectError(
            "scheduler log generation exceeds declared scheduler transitions"
        )
    lower_epoch = (
        replacement_epochs[generation_index - 1] if generation_index else 0
    )
    upper_epoch = (
        replacement_epochs[generation_index] - 1
        if generation_index < len(replacement_epochs)
        else len(events)
    )
    epoch = _epoch_at(events, dispatch_ms)
    if epoch < lower_epoch:
        skipped = events[epoch:lower_epoch]
        if not skipped or any(
            not _scheduler_transition_allows_early_dispatch(event, dispatch_ms)
            for event in skipped
        ):
            raise CollectError(
                "scheduler generation disagrees with the dispatch timestamp"
            )
        epoch = lower_epoch
    if epoch > upper_epoch:
        raise CollectError(
            "scheduler generation precedes the dispatch event epoch"
        )
    return epoch


def _instance_version_at(
    instance: Mapping[str, Any],
    events: list[dict[str, Any]],
    dispatch_ms: int,
) -> int:
    """Resolve one endpoint generation at an authenticated dispatch boundary."""

    version = instance.get("version")
    if type(version) is not int or version < 1:
        raise CollectError("planned instance has no valid protocol generation")
    name = instance.get("name")
    for event in events:
        fired_ms = event.get("fired_ms")
        if type(fired_ms) is not int:
            raise CollectError("event has no valid fired timestamp")
        if fired_ms > dispatch_ms:
            break
        if (
            event.get("instance") != name
            or event.get("action") not in {"upgrade", "downgrade"}
        ):
            continue
        receipt = event.get("receipt")
        after = receipt.get("after") if isinstance(receipt, Mapping) else None
        label = after.get("image") if isinstance(after, Mapping) else None
        if not isinstance(label, str):
            raise CollectError(
                f"transition for {name!r} has no authenticated after image"
            )
        version = _event_image_version(label)
    return version


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
    if (
        SHA256_RE.fullmatch(remote_sha) is None
        or SHA256_RE.fullmatch(local_sha) is None
    ):
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


def _profile_markers(job_dir: Path) -> list[dict[str, Any]]:
    content = (
        _text(job_dir / "client-debug.log")
        + "\n"
        + _text(job_dir / "client-output.log")
    )
    matches = list(PROFILE_RE.finditer(content))
    unique: dict[tuple[str, int, int], dict[str, Any]] = {}
    for match in matches:
        key = (match.group(1), int(match.group(2)), int(match.group(3)))
        unique.setdefault(
            key,
            {
                "line": content.count("\n", 0, match.start()) + 1,
                "profile": key[0],
                "raw_bytes": key[1],
                "tu_seq": key[2],
            },
        )
    return sorted(unique.values(), key=lambda item: item["line"])


def _profile_markers_by_assignment(
    markers: list[dict[str, Any]],
    assignments: list[dict[str, Any]],
    source: str,
) -> list[dict[str, Any] | None]:
    """Bind each commit marker to its immediately preceding assignment."""

    bound: list[list[dict[str, Any]]] = [[] for _ in assignments]
    for marker in markers:
        owners = [
            index
            for index, assignment in enumerate(assignments)
            if assignment["line"] < marker["line"]
        ]
        if not owners:
            raise CollectError(f"{source}: P50 commit marker precedes every assignment")
        bound[owners[-1]].append(marker)
    if any(len(items) > 1 for items in bound):
        raise CollectError(f"{source}: assignment has conflicting P50 commit markers")
    return [items[0] if items else None for items in bound]


def _bind_same_endpoint_decision(
    record: dict[str, Any], decisions: list[dict[str, Any]], kind: str
) -> dict[str, Any]:
    if record["failed_endpoint"] != record["retry_endpoint"]:
        return record
    matches = [
        decision
        for decision in decisions
        if (decision["job"], decision["epoch"], decision["nonce"])
        == (
            record["retry_scheduler_job"],
            record["retry_assignment_epoch"],
            record["retry_assignment_nonce"],
        )
    ]
    if len(matches) != 1:
        raise CollectError(
            f"{record['row_job_id']}: {kind} lacks one exact same-endpoint decision"
        )
    record["same_endpoint_decision"] = matches[0]
    if not same_endpoint_decision_valid(record):
        raise CollectError(
            f"{record['row_job_id']}: {kind} has invalid same-endpoint decision"
        )
    return record


def _source_transfer_failure_observation(
    *,
    log_text: str,
    assignment: Mapping[str, Any],
    retry_assignment: Mapping[str, Any],
    assignment_identity: Mapping[str, int] | None,
    retry_identity: Mapping[str, int] | None,
    source_results: Mapping[tuple[int, int, int], Mapping[str, Any]],
    compile_identities: Mapping[tuple[int, int, int], Mapping[str, Any]],
    row_job_id: str,
    attempt_index: int,
    retry_decisions: list[dict[str, Any]] | None = None,
) -> dict[str, Any] | None:
    """Authenticate a retry caused before a P50 source result exists.

    The producer's fail-closed transfer line and wrapper retry request must be
    unique and ordered inside one exact assignment window.  The failed
    identity must have produced no source-result or compile-result evidence,
    and a strict retry must authenticate endpoint selection. Normal legacy
    retries retain their distinct mode and require a final legacy wire witness
    at the caller; they never qualify as successful strict retries.
    Cache action commits are
    deliberately not constrained: a lost acknowledgement can fail the client
    closed after the remote cache has already committed the input.
    """

    assignment_line = assignment.get("line")
    retry_assignment_line = retry_assignment.get("line")
    if type(assignment_line) is not int or type(retry_assignment_line) is not int:
        raise CollectError(f"{row_job_id}: source-transfer assignment line is invalid")
    scoped_lines = [
        (line_number, line)
        for line_number, line in enumerate(log_text.splitlines(), start=1)
        if assignment_line < line_number < retry_assignment_line
    ]
    failures = [
        (line_number, match)
        for line_number, line in scoped_lines
        if (match := P50_SOURCE_TRANSFER_FAILED_RE.search(line)) is not None
    ]
    retries = [
        (line_number, match)
        for line_number, line in scoped_lines
        if (match := P50_STRICT_RETRY_REQUEST_RE.search(line)) is not None
    ]
    legacy_retries = [
        (line_number, match)
        for line_number, line in scoped_lines
        if (match := P50_LEGACY_RETRY_REQUEST_RE.search(line)) is not None
    ]
    legacy_retry = bool(legacy_retries)
    retries += legacy_retries
    has_failure_claim = any(
        "cache source transfer failed closed" in line for _, line in scoped_lines
    )
    has_retry_claim = any(
        "requesting one fresh strict-P50 remote assignment" in line
        for _, line in scoped_lines
    )
    if not has_failure_claim and not has_retry_claim:
        return None
    retry_claims = sum(
        line.count("P50 assignment failed; requesting one fresh")
        for _, line in scoped_lines
    )
    if len(failures) != 1 or len(retries) != 1 or retry_claims != 1:
        raise CollectError(
            f"{row_job_id}: source-transfer loss markers are absent, malformed, or ambiguous"
        )
    if assignment_identity is None or retry_identity is None:
        raise CollectError(
            f"{row_job_id}: source-transfer loss lacks exact assignment identities"
        )

    def identity_occurrences(evidence: Mapping[str, int]) -> int:
        expected = (
            evidence["scheduler_job"],
            evidence["assignment_epoch"],
            evidence["assignment_nonce"],
            evidence["c_guid"],
            evidence["tu_seq"],
        )
        return sum(
            tuple(int(match.group(index)) for index in range(1, 6)) == expected
            for match in P50_ASSIGNMENT_IDENTITY_RE.finditer(log_text)
        )

    if (
        identity_occurrences(assignment_identity) != 1
        or identity_occurrences(retry_identity) != 1
    ):
        raise CollectError(
            f"{row_job_id}: source-transfer loss assignment identity is ambiguous"
        )

    failure_line, failure = failures[0]
    retry_line, retry = retries[0]
    identity_line = assignment_identity["line"]
    retry_identity_line = retry_identity["line"]
    status = int(failure.group(2))
    error = int(failure.group(3))
    transfer_attempts = int(failure.group(4))
    control_endings = [
        (number, line)
        for number, line in scoped_lines
        if "P50 cache control operation ended" in line
    ]
    control_reply_lost = bool(control_endings)
    if control_reply_lost and not (
        len(control_endings) == 1
        and control_endings[0][1].count("P50 cache control operation ended") == 1
        and control_endings[0][1].endswith(
            "P50 cache control operation ended disconnected"
        )
        and assignment_line < control_endings[0][0] < failure_line
        and (status, error, transfer_attempts) == (2, 7, 0)
    ):
        raise CollectError(f"{row_job_id}: source-transfer loss control disconnect is ambiguous")
    failed_endpoint = assignment.get("endpoint") if legacy_retry else retry.group(1)
    identity = (
        assignment_identity["scheduler_job"],
        assignment_identity["assignment_epoch"],
        assignment_identity["assignment_nonce"],
    )
    worker = str(assignment.get("worker", ""))
    source_result_status = _source_result_status(
        source_results,
        identity,
        context="source-transfer loss",
        # The local error-7 fallback carries no daemon attempt knowledge.
        # A received reply still requires exact agreement between observers.
        expected_attempts=None if control_reply_lost else transfer_attempts,
        expected_profile=failure.group(1),
        row_job_id=row_job_id,
    )
    if (
        assignment_identity["scheduler_job"] != assignment.get("scheduler_job")
        or retry_identity["scheduler_job"] != retry_assignment.get("scheduler_job")
        or retry_identity["c_guid"] != assignment_identity["c_guid"]
        or (
            retry_identity["scheduler_job"],
            retry_identity["assignment_epoch"],
            retry_identity["assignment_nonce"],
        )
        == identity
        or status < 1
        or error < 1
        or not (
            identity_line
            < assignment_line
            < failure_line
            < retry_line
            < retry_identity_line
            < retry_assignment_line
        )
        or failed_endpoint != assignment.get("endpoint")
        or not worker
        or source_result_status == 0
        or identity in compile_identities
    ):
        raise CollectError(
            f"{row_job_id}: source-transfer loss does not bind one uncommitted "
            "failed assignment to a distinct retry"
        )
    record = {
        "assignment_epoch": assignment_identity["assignment_epoch"],
        "assignment_identity_line": identity_line,
        "assignment_line": assignment_line,
        "assignment_nonce": assignment_identity["assignment_nonce"],
        "attempt_index": attempt_index,
        "c_guid": assignment_identity["c_guid"],
        "compile_identity_present": False,
        "error": error,
        "failed_endpoint": failed_endpoint,
        "failure_line": failure_line,
        "profile": failure.group(1),
        "retry_assignment_epoch": retry_identity["assignment_epoch"],
        "retry_assignment_identity_line": retry_identity_line,
        "retry_assignment_line": retry_assignment_line,
        "retry_assignment_nonce": retry_identity["assignment_nonce"],
        "retry_c_guid": retry_identity["c_guid"],
        "retry_endpoint": retry_assignment["endpoint"],
        "retry_line": retry_line,
        "retry_scheduler_job": retry_assignment["scheduler_job"],
        "retry_tu_seq": retry_identity["tu_seq"],
        "row_job_id": row_job_id,
        "scheduler_job": assignment["scheduler_job"],
        "source_result_present": False,
        "source_result_status": source_result_status,
        "status": status,
        "transfer_attempts": transfer_attempts,
        "tu_seq": assignment_identity["tu_seq"],
        "worker": worker,
    }
    if control_reply_lost:
        observed_attempts = source_results.get(identity, {}).get("attempts")
        if identity in source_results and (
            type(observed_attempts) is not int or not 0 <= observed_attempts <= 2
        ):
            raise CollectError(f"{row_job_id}: source-transfer loss daemon attempts are malformed")
        record["control_result_received"] = False
        record["control_disconnect_line"] = control_endings[0][0]
        record["source_result_attempts"] = observed_attempts
    if legacy_retry:
        record["retry_mode"] = "legacy"
        return record
    return _bind_same_endpoint_decision(
        record, retry_decisions or [], "source-transfer loss"
    )


def _resource_failure_observation(
    log_text: str,
    assignments: list[dict[str, Any]],
    identities: list[Mapping[str, int] | None],
    compile_identities: Mapping[tuple[int, int, int], Mapping[str, Any]],
    row_job_id: str,
) -> dict[str, Any] | None:
    """Bind resource-loss diagnostics to received results and a fresh remote retry."""
    lines = log_text.splitlines()
    claims = [(n, line) for n, line in enumerate(lines, 1)
              if "P50 worker resource failure normalized" in line]
    if not claims:
        return None
    if len(claims) != 1 or len(assignments) != 2 or len(identities) != 2:
        raise CollectError(f"{row_job_id}: resource failure requires one fresh retry")
    failure_line, line = claims[0]
    match = P50_RESOURCE_FAILURE_RE.search(line)
    first, final = identities
    if match is None or first is None or final is None:
        raise CollectError(f"{row_job_id}: resource failure lacks exact identities")
    fields = ("scheduler_job", "assignment_epoch", "assignment_nonce", "c_guid", "tu_seq")
    first_key = tuple(first[field] for field in fields)
    final_key = tuple(final[field] for field in fields)
    retries = [(n, m) for n, line in enumerate(lines, 1)
               if (m := P50_STRICT_RETRY_REQUEST_RE.search(line)) is not None]
    if (
        tuple(map(int, match.groups())) != first_key
        or len(retries) != 1
        or first_key[:3] == final_key[:3]
        or first_key[3] != final_key[3]
        or first_key[0] != assignments[0]["scheduler_job"]
        or final_key[0] != assignments[1]["scheduler_job"]
        or assignments[0]["worker"] == assignments[1]["worker"]
        or assignments[0]["endpoint"] == assignments[1]["endpoint"]
        or retries[0][1].group(1) != assignments[0]["endpoint"]
        or not (first["line"] < assignments[0]["line"] < failure_line
                < retries[0][0] < final["line"] < assignments[1]["line"])
    ):
        raise CollectError(f"{row_job_id}: resource failure retry identity/order mismatch")
    for key in (first_key, final_key):
        result = compile_identities.get(key[:3], {})
        if tuple(result.get(field) for field in
                 ("job_id", "assignment_epoch", "assignment_nonce", "c_guid", "tu_seq")) != key:
            raise CollectError(f"{row_job_id}: resource failure lacks matching CompileResult")
        occurrences = sum(
            tuple(map(int, marker.groups())) == key
            for marker in P50_ASSIGNMENT_IDENTITY_RE.finditer(log_text)
        )
        if occurrences != 1:
            raise CollectError(f"{row_job_id}: resource failure identity is ambiguous")
    return {
        "row_job_id": row_job_id,
        "first_identity": dict(zip(fields, first_key)),
        "final_identity": dict(zip(fields, final_key)),
        "first_worker": assignments[0]["worker"],
        "final_worker": assignments[1]["worker"],
        "failed_endpoint": assignments[0]["endpoint"],
        "retry_endpoint": assignments[1]["endpoint"],
        "failure_line": failure_line,
        "retry_line": retries[0][0],
        "normalized_error": 106,
        "compile_results_present": True,
    }


def _uncommitted_transport_failure_observation(
    *,
    log_text: str,
    assignment: Mapping[str, Any],
    retry_assignment: Mapping[str, Any],
    assignment_identity: Mapping[str, int] | None,
    retry_identity: Mapping[str, int] | None,
    source_results: Mapping[tuple[int, int, int], Mapping[str, Any]],
    compile_identities: Mapping[tuple[int, int, int], Mapping[str, Any]],
    row_job_id: str,
    attempt_index: int,
    retry_decisions: list[dict[str, Any]] | None = None,
) -> dict[str, Any] | None:
    """Authenticate a strict retry after a pre-commit P50 transport loss.

    This is intentionally separate from a cache source-transfer failure: the
    client may lose an ordinary worker connection before it has emitted a P50
    profile commit marker or any source/compile result.  Only the producer's
    exact transport normalization set is accepted, and every identity and
    endpoint transition remains fail closed.
    """

    assignment_line = assignment.get("line")
    retry_assignment_line = retry_assignment.get("line")
    if type(assignment_line) is not int or type(retry_assignment_line) is not int:
        raise CollectError(f"{row_job_id}: transport-loss assignment line is invalid")
    scoped_lines = [
        (line_number, line)
        for line_number, line in enumerate(log_text.splitlines(), start=1)
        if assignment_line < line_number < retry_assignment_line
    ]
    has_normalized_claim = any(
        "normalizing P50 client error" in line and "for a fresh assignment" in line
        for _, line in scoped_lines
    )
    if not has_normalized_claim:
        return None
    normalized = [
        (line_number, match)
        for line_number, line in scoped_lines
        if (match := P50_NORMALIZED_ERROR106_RE.search(line)) is not None
    ]
    retries = [
        (line_number, match)
        for line_number, line in scoped_lines
        if (match := P50_STRICT_RETRY_REQUEST_RE.search(line)) is not None
    ]
    if len(normalized) != 1 or len(retries) != 1:
        raise CollectError(
            f"{row_job_id}: uncommitted transport-loss markers are malformed or ambiguous"
        )
    if assignment_identity is None or retry_identity is None:
        raise CollectError(
            f"{row_job_id}: uncommitted transport loss lacks exact assignment identities"
        )

    def identity_occurrences(evidence: Mapping[str, int]) -> int:
        expected = (
            evidence["scheduler_job"],
            evidence["assignment_epoch"],
            evidence["assignment_nonce"],
            evidence["c_guid"],
            evidence["tu_seq"],
        )
        return sum(
            tuple(int(match.group(index)) for index in range(1, 6)) == expected
            for match in P50_ASSIGNMENT_IDENTITY_RE.finditer(log_text)
        )

    if (
        identity_occurrences(assignment_identity) != 1
        or identity_occurrences(retry_identity) != 1
    ):
        raise CollectError(
            f"{row_job_id}: uncommitted transport-loss assignment identity is ambiguous"
        )

    normalized_line, normalized_match = normalized[0]
    retry_line, retry_match = retries[0]
    original_error = int(normalized_match.group(1))
    failed_endpoint = retry_match.group(1)
    identity_line = assignment_identity["line"]
    retry_identity_line = retry_identity["line"]
    identity = (
        assignment_identity["scheduler_job"],
        assignment_identity["assignment_epoch"],
        assignment_identity["assignment_nonce"],
    )
    retry_key = (
        retry_identity["scheduler_job"],
        retry_identity["assignment_epoch"],
        retry_identity["assignment_nonce"],
    )
    worker = str(assignment.get("worker", ""))
    source_result_status = _source_result_status(
        source_results,
        identity,
        context="transport loss",
        row_job_id=row_job_id,
    )
    if (
        assignment_identity["scheduler_job"] != assignment.get("scheduler_job")
        or retry_identity["scheduler_job"] != retry_assignment.get("scheduler_job")
        or retry_identity["c_guid"] != assignment_identity["c_guid"]
        or retry_key == identity
        or original_error not in P50_RETRYABLE_TRANSPORT_ERRORS
        or not (
            identity_line
            < assignment_line
            < normalized_line
            < retry_line
            < retry_identity_line
            < retry_assignment_line
        )
        or failed_endpoint != assignment.get("endpoint")
        or not worker
        or source_result_status == 0
        or identity in compile_identities
        or any(PROFILE_RE.search(line) is not None for _, line in scoped_lines)
    ):
        raise CollectError(
            f"{row_job_id}: uncommitted transport loss does not bind one exact "
            "failed assignment to a distinct strict retry"
        )
    record = {
        "assignment_epoch": assignment_identity["assignment_epoch"],
        "assignment_identity_line": identity_line,
        "assignment_line": assignment_line,
        "assignment_nonce": assignment_identity["assignment_nonce"],
        "attempt_index": attempt_index,
        "c_guid": assignment_identity["c_guid"],
        "compile_identity_present": False,
        "failed_endpoint": failed_endpoint,
        "normalized_error": 106,
        "normalized_line": normalized_line,
        "original_error": original_error,
        "profile_commit_present": False,
        "retry_assignment_epoch": retry_identity["assignment_epoch"],
        "retry_assignment_identity_line": retry_identity_line,
        "retry_assignment_line": retry_assignment_line,
        "retry_assignment_nonce": retry_identity["assignment_nonce"],
        "retry_c_guid": retry_identity["c_guid"],
        "retry_endpoint": retry_assignment["endpoint"],
        "retry_line": retry_line,
        "retry_scheduler_job": retry_assignment["scheduler_job"],
        "retry_tu_seq": retry_identity["tu_seq"],
        "row_job_id": row_job_id,
        "scheduler_job": assignment["scheduler_job"],
        "source_result_present": False,
        "source_result_status": source_result_status,
        "tu_seq": assignment_identity["tu_seq"],
        "worker": worker,
    }
    return _bind_same_endpoint_decision(
        record, retry_decisions or [], "uncommitted transport loss"
    )


def _unassigned_p50_failure_observation(
    *, log_text: str, raw: Mapping[str, Any], row_job_id: str
) -> dict[str, Any]:
    """Authenticate a remote-only P50 request rejected before assignment.

    An assignment can be absent because strict cache handoff was unavailable,
    or because the scheduler channel vanished after request dispatch but
    before UseCS reached the wrapper.  Both are compile failures, never
    authenticated success.
    """

    lines = list(enumerate(log_text.splitlines(), start=1))
    requests = [
        line_number
        for line_number, line in lines
        if "asking for host to use" in line
    ]
    failures = [
        line_number
        for line_number, line in lines
        if P50_NO_CACHE_HANDOFF_RE.search(line) is not None
    ]
    refusals = [
        line_number
        for line_number, line in lines
        if REMOTE_ONLY_REFUSAL_RE.search(line) is not None
    ]
    unexpected_usecs = [
        line_number
        for line_number, line in lines
        if UNEXPECTED_USECS_ERROR_RE.search(line) is not None
    ]
    client_error_refusals = [
        line_number
        for line_number, line in lines
        if REMOTE_ONLY_CLIENT_ERROR_REFUSAL_RE.search(line) is not None
    ]
    no_cache_handoff = (
        len(failures) == 1
        and len(refusals) == 1
        and not unexpected_usecs
        and not client_error_refusals
    )
    scheduler_stream_loss = (
        not failures
        and not refusals
        and len(unexpected_usecs) == 1
        and len(client_error_refusals) == 1
    )
    expected_scheduler_job = f"missing-{raw.get('index')}"
    if (
        raw.get("scheduler_job") != expected_scheduler_job
        or raw.get("worker") != "UNKNOWN"
        or type(raw.get("compile_rc")) is not int
        or raw["compile_rc"] == 0
        or raw.get("exact") != 0
        or raw.get("remote") != 0
        or raw.get("retries") != 0
        or raw.get("remote_sha") != "0" * 64
        or len(requests) != 1
        or no_cache_handoff == scheduler_stream_loss
        or CLIENT_ASSIGNMENT_RE.search(log_text) is not None
        or P50_ASSIGNMENT_IDENTITY_RE.search(log_text) is not None
        or PROFILE_RE.search(log_text) is not None
        or P50_SOURCE_TRANSFER_FAILED_RE.search(log_text) is not None
        or P50_NORMALIZED_ERROR106_RE.search(log_text) is not None
        or P50_STRICT_RETRY_REQUEST_RE.search(log_text) is not None
        or any(marker in log_text for marker in LOCAL_BUILD_MARKERS)
    ):
        raise CollectError(
            f"{row_job_id}: missing assignment is not one exact remote-only "
            "P50 cache-handoff refusal"
        )
    if no_cache_handoff and not requests[0] < failures[0] < refusals[0]:
        raise CollectError(
            f"{row_job_id}: missing assignment is not one exact remote-only "
            "P50 cache-handoff refusal"
        )
    if scheduler_stream_loss and not (
        requests[0] < unexpected_usecs[0] < client_error_refusals[0]
    ):
        raise CollectError(
            f"{row_job_id}: missing assignment is not one exact remote-only "
            "P50 cache-handoff refusal"
        )
    common = {
        "client_instance": row_job_id.split(":", 1)[0],
        "compile_rc": raw["compile_rc"],
        "index": raw["index"],
        "request_line": requests[0],
        "request_finished_ms": raw["finished"],
        "request_started_ms": raw["started"],
        "row_job_id": row_job_id,
        "scheduler_job": raw["scheduler_job"],
        "turn": raw["turn"],
        "worker": raw["worker"],
    }
    if no_cache_handoff:
        return {
            **common,
            "failure_line": failures[0],
            "remote_only_refusal_line": refusals[0],
        }
    return {
        **common,
        "failure_line": unexpected_usecs[0],
        "failure_reason": "scheduler-stream-loss-before-usecs",
        "remote_only_refusal_line": client_error_refusals[0],
    }


def _abandoned_p50_retry_request_observation(
    *,
    log_text: str,
    raw: Mapping[str, Any],
    assignment: Mapping[str, Any],
    assignment_identity: Mapping[str, int] | None,
    row_job_id: str,
) -> dict[str, Any] | None:
    """Authenticate a strict retry request rejected before its UseCS arrived.

    The first assignment may have failed before source commit, or after an
    exact source commit when its compile-result stream was lost.  These are
    deliberately disjoint shapes.  This record only reconciles the otherwise
    unclaimed retry dispatch; it never authenticates compile success.
    """

    lines = list(enumerate(log_text.splitlines(), start=1))
    no_cache = [
        line_number
        for line_number, line in lines
        if P50_NO_CACHE_HANDOFF_RE.search(line) is not None
    ]
    if not no_cache:
        return None
    transfers = [
        (line_number, match)
        for line_number, line in lines
        if (match := P50_SOURCE_TRANSFER_FAILED_RE.search(line)) is not None
    ]
    committed_sources = [
        (line_number, match)
        for line_number, line in lines
        if (match := PROFILE_RE.search(line)) is not None
    ]
    normalized_errors = [
        (line_number, match)
        for line_number, line in lines
        if (match := P50_NORMALIZED_ERROR106_RE.search(line)) is not None
    ]
    retries = [
        (line_number, match)
        for line_number, line in lines
        if (match := P50_STRICT_RETRY_REQUEST_RE.search(line)) is not None
    ]
    refusals = [
        line_number
        for line_number, line in lines
        if REMOTE_ONLY_REFUSAL_RE.search(line) is not None
    ]
    assignments = list(CLIENT_ASSIGNMENT_RE.finditer(log_text))
    identities = list(P50_ASSIGNMENT_IDENTITY_RE.finditer(log_text))
    source_transfer_failure = (
        len(transfers) == 1
        and not committed_sources
        and not normalized_errors
    )
    result_stream_loss = (
        not transfers
        and len(committed_sources) == 1
        and len(normalized_errors) == 1
    )
    if (
        assignment_identity is None
        or len(assignments) != 1
        or len(identities) != 1
        or len(retries) != 1
        or len(no_cache) != 1
        or len(refusals) != 1
        or source_transfer_failure == result_stream_loss
    ):
        raise CollectError(
            f"{row_job_id}: abandoned strict-P50 retry markers are absent or ambiguous"
        )
    retry_line, retry = retries[0]
    assignment_line = assignment.get("line")
    identity_line = assignment_identity.get("line")
    if (
        type(assignment_line) is not int
        or type(identity_line) is not int
        or assignment_identity.get("scheduler_job")
        != assignment.get("scheduler_job")
        or retry.group(1) != assignment.get("endpoint")
        or raw.get("compile_rc") == 0
        or raw.get("exact") != 0
        or raw.get("remote") != 1
        or raw.get("retries") != 0
        or raw.get("remote_sha") != "0" * 64
    ):
        raise CollectError(
            f"{row_job_id}: abandoned strict-P50 retry is not an exact failed request"
        )
    common = {
        "client_instance": row_job_id.split(":", 1)[0],
        "compile_rc": raw["compile_rc"],
        "failed_endpoint": retry.group(1),
        "request_finished_ms": raw["finished"],
        "request_started_ms": raw["started"],
        "retry_line": retry_line,
        "row_job_id": row_job_id,
        "scheduler_job": assignment["scheduler_job"],
        "worker": assignment["worker"],
    }
    if source_transfer_failure:
        transfer_line, transfer = transfers[0]
        if (
            int(transfer.group(2)) < 1
            or int(transfer.group(3)) < 1
            or not (
                identity_line
                < assignment_line
                < transfer_line
                < retry_line
                < no_cache[0]
                < refusals[0]
            )
        ):
            raise CollectError(
                f"{row_job_id}: abandoned strict-P50 retry is not an exact failed request"
            )
        return {
            **common,
            "failure_line": transfer_line,
            "failure_reason": "source-transfer-failure",
            "profile": transfer.group(1),
            "status": int(transfer.group(2)),
            "transfer_error": int(transfer.group(3)),
            "transfer_attempts": int(transfer.group(4)),
        }

    commit_line, commit = committed_sources[0]
    normalized_line, normalized = normalized_errors[0]
    normalized_error = int(normalized.group(1))
    if (
        int(commit.group(2)) < 1
        # Source TU sequence is session-local and may differ from the
        # scheduler assignment's client TU sequence under concurrency.  The
        # later source-result join binds it to the full assignment identity.
        or int(commit.group(3)) < 1
        or normalized_error not in P50_RETRYABLE_TRANSPORT_ERRORS
        or not (
            identity_line
            < assignment_line
            < commit_line
            < normalized_line
            < retry_line
            < no_cache[0]
            < refusals[0]
        )
    ):
        raise CollectError(
            f"{row_job_id}: abandoned strict-P50 retry is not an exact failed request"
        )
    return {
        **common,
        "failure_line": normalized_line,
        "failure_reason": "result-stream-loss",
        "normalized_error": normalized_error,
        "profile": commit.group(1),
        "raw_bytes": int(commit.group(2)),
        "tu_seq": int(commit.group(3)),
    }


def _source_transfer_is_exact(
    source: Mapping[str, Any],
    marker: Mapping[str, Any],
    *,
    worker_name: str,
    c_commits: set[tuple[str, int]],
    f_commits: Mapping[str, set[tuple[str, int]]],
) -> bool:
    key = (source["c_store_guid"], source["tu_seq"])
    return (
        source["status"] == 0
        and source["attempts"] >= 1
        and marker["profile"] == source["profile"]
        and marker["raw_bytes"] == source["raw_bytes"]
        and marker["tu_seq"] == source["tu_seq"]
        and key in c_commits
        and key in f_commits[worker_name]
    )


def _source_commit_is_exact(
    source: Mapping[str, Any],
    marker: Mapping[str, Any],
    *,
    worker_name: str,
    scheduler_job: int,
    c_commits: set[tuple[str, int]],
    f_commits: Mapping[str, set[tuple[str, int]]],
    attachments: Mapping[tuple[str, int], str],
) -> bool:
    return _source_transfer_is_exact(
        source,
        marker,
        worker_name=worker_name,
        c_commits=c_commits,
        f_commits=f_commits,
    ) and attachments.get((worker_name, scheduler_job)) == source["profile"]


def _missing_compile_result_identity_reason(
    *,
    raw: Mapping[str, Any],
    final_attempt: bool,
    local_build: bool,
    log_text: str,
    events: list[dict[str, Any]],
    assignment: Mapping[str, Any],
    assignment_identity: tuple[int, int, int] | None,
    source_key: tuple[int, int, int],
    source: Mapping[str, Any],
    marker: Mapping[str, Any],
    c_commits: set[tuple[str, int]],
    f_commits: Mapping[str, set[tuple[str, int]]],
) -> str | None:
    """Classify a committed P50 attempt for which no result frame exists.

    This never authenticates success.  It admits only an explicitly failed
    row (or a failed prior retry attempt) whose assignment, source commit and
    loss witness are all exact; the caller must retain it as failed evidence.
    """

    if (
        assignment_identity is None
        or source_key != assignment_identity
        or local_build
        or raw["remote"] != 1
        or (final_attempt and raw["compile_rc"] == 0)
        or not _source_transfer_is_exact(
            source,
            marker,
            worker_name=str(assignment["worker"]),
            c_commits=c_commits,
            f_commits=f_commits,
        )
    ):
        return None

    restart_losses = {
        (event.get("instance"), scheduler_job)
        for event in events
        if event.get("action") == "restart"
        and isinstance(event.get("receipt"), Mapping)
        and event["receipt"].get("schema") == WORKER_RESTART_SCHEMA
        and isinstance(
            event["receipt"].get("coordination", {}).get("scheduler_rejoin"),
            Mapping,
        )
        for scheduler_job in event["receipt"]["coordination"][
            "scheduler_rejoin"
        ].get("loss_job_ids", [])
        if type(scheduler_job) is int and scheduler_job > 0
    }
    if (assignment["worker"], assignment["scheduler_job"]) in restart_losses:
        return "worker-restart-loss"
    if P50_NORMALIZED_ERROR106_RE.search(log_text) is not None:
        return "result-stream-loss"
    return None


def _legacy_wire_binding_marker(
    text: str, scheduler_job: int
) -> tuple[int, int, int, int, int] | None:
    """Resolve the exact C-local/fenced wire identity from one job log."""

    matches = []
    for match in LEGACY_WIRE_BIND_RE.finditer(text):
        values = tuple(int(match.group(index)) for index in range(1, 6))
        if values[0] != scheduler_job:
            continue
        origin = match.group(6)
        assignment_absent = values[1] == 0 and values[2] == 0
        assignment_complete = values[1] > 0 and values[2] > 0
        if (
            values[0] <= 0
            or values[3] <= 0
            or not (assignment_absent or assignment_complete)
            # A current scheduler in enforcing-compat mode can mint the
            # compile GUID while deliberately withholding epoch/nonce from a
            # relationship that is not assignment-fence eligible.  Only a
            # C-local GUID proves absence of scheduler assignment authority;
            # scheduler origin is valid in either canonical zero/zero or
            # complete/complete form.
            or (origin == "client-local" and not assignment_absent)
        ):
            raise CollectError("client legacy-wire binding marker is invalid")
        matches.append(values)
    unique = set(matches)
    if len(unique) > 1:
        raise CollectError("client legacy-wire binding marker is ambiguous")
    return next(iter(unique)) if unique else None


def _endpoint_workers(
    plan: dict[str, Any], scenario: ScenarioSpec | None = None,
    evidence: Path | None = None,
) -> dict[str, dict[str, Any]]:
    result: dict[str, dict[str, Any]] = {}
    for instance in plan["topology"]["instances"]:
        if instance["role"] != "F":
            continue
        endpoint = (
            f"{instance['address']}:{plan['ports']['instances'][instance['name']]}"
        )
        if endpoint in result:
            raise CollectError(f"duplicate worker endpoint {endpoint}")
        result[endpoint] = instance
    if plan.get("worker_endpoint_contract") == "icefarm-live-bridge-endpoint-v1":
        if scenario is None or evidence is None:
            raise CollectError("bridge endpoints require retained authenticated evidence")
        shaping = _network_shaping_observation(scenario, plan, evidence)
        if shaping is None:
            raise CollectError("bridge endpoint contract requires shaping")
        by_name = {item["name"]: item for item in plan["topology"]["instances"]}
        for record in shaping["bindings"]:
            instance = by_name[record["instance"]]
            request = record["request"]
            directory = evidence / "diagnostics" / instance["host"]
            live = _read_json(directory / f"{instance['name']}.live-inspect")
            stopped = _read_json(directory / f"{instance['name']}.inspect")
            labels = live.get("Config", {}).get("Labels", {})
            networks = live.get("NetworkSettings", {}).get("Networks", {})
            network = networks.get(request["bridge"], {})
            container_id = live.get("Id")
            if (
                instance["role"] != "F"
                or not isinstance(container_id, str)
                or re.fullmatch(r"[0-9a-f]{64}", container_id) is None
                or stopped.get("Id") != container_id
                or labels.get("icefarm.run") != plan["run_id"]
                or labels.get("icefarm.instance") != instance["name"]
                or live.get("State", {}).get("Running") is not True
                or live.get("HostConfig", {}).get("NetworkMode") != request["bridge"]
                or set(networks) != {request["bridge"]}
                or network.get("NetworkID") != record["bridge"]["network_id"]
                or request["container_port"] != plan["ports"]["instances"][instance["name"]]
            ):
                raise CollectError("bridge endpoint container/network identity mismatch")
            try:
                address = ipaddress.IPv4Address(network.get("IPAddress", ""))
            except ipaddress.AddressValueError as exc:
                raise CollectError("bridge endpoint has no valid live IPv4 address") from exc
            if address.is_unspecified or address.is_loopback or address.is_multicast:
                raise CollectError("bridge endpoint address is not unicast")
            endpoint = f"{address}:{request['container_port']}"
            if endpoint in result and result[endpoint] is not instance:
                raise CollectError(f"duplicate worker endpoint {endpoint}")
            result[endpoint] = instance
    return result


def _successful_strict_p50_late_result_binding(
    scenario: ScenarioSpec,
    row: Mapping[str, Any],
    raw: Mapping[str, Any],
    records: list[Mapping[str, Any]],
    events: list[dict[str, Any]],
) -> dict[str, Any] | None:
    """Bind one exact successful result to its declared worker-loss boundary.

    A worker may deliver an exact result to the submitter immediately before
    disappearing, while the scheduler observes only the process loss.  This
    narrow S70 witness is the only successful-wrapper exception for that
    race: the parsed P50 identities and commits must already be exact, and an
    authenticated worker-restart receipt must name the same scheduler job.
    """

    if (
        scenario.data.get("id") != "S70-b4-worker-bounces"
        or scenario.data.get("expect", {}).get("engagement")
        != "s70-b4-worker-bounces"
        or len(records) != 1
        or raw.get("compile_rc") != 0
        or raw.get("exact") != 1
        or raw.get("remote") != 1
        or raw.get("local_build") is not False
        or row.get("exact") is not True
        or row.get("retries") != 0
        or row.get("tail_present") is not True
        or row.get("tail_profile") != "P29V1"
        or row.get("session_outcome") != "committed"
    ):
        return None
    record = records[0]
    if (
        record.get("terminal") != "process-loss-recovery"
        or row.get("cs") != record.get("worker")
        or type(record.get("scheduler_job")) is not int
        or record["scheduler_job"] < 1
        or type(record.get("generation")) is not int
        or record["generation"] < 1
        or type(record.get("dispatch_ms")) is not int
        or type(record.get("terminal_ms")) is not int
        or record["dispatch_ms"] > record["terminal_ms"]
    ):
        return None

    matching: list[dict[str, Any]] = []
    for event_index, event in enumerate(events):
        receipt = event.get("receipt")
        coordination = (
            receipt.get("coordination") if isinstance(receipt, Mapping) else None
        )
        rejoin = (
            coordination.get("scheduler_rejoin")
            if isinstance(coordination, Mapping)
            else None
        )
        fired_ms = event.get("fired_ms")
        prior_fired_ms = (
            events[event_index - 1].get("fired_ms") if event_index else None
        )
        if (
            event.get("action") != "restart"
            or event.get("instance") != record.get("worker")
            or event.get("event_index") != event_index
            or not isinstance(receipt, Mapping)
            or receipt.get("schema") != WORKER_RESTART_SCHEMA
            or not isinstance(rejoin, Mapping)
            or rejoin.get("target") != record.get("worker")
            or not isinstance(rejoin.get("loss_job_ids"), list)
            or record["scheduler_job"] not in rejoin["loss_job_ids"]
            or type(fired_ms) is not int
            or record["terminal_ms"] > fired_ms
            or (
                event_index
                and (
                    type(prior_fired_ms) is not int
                    or record["terminal_ms"] <= prior_fired_ms
                )
            )
        ):
            continue
        matching.append(
            {
                "attempt_index": 0,
                "dispatch_ms": record["dispatch_ms"],
                "generation": record["generation"],
                "job_id": row["job_id"],
                "restart_event_index": event_index,
                "restart_fired_ms": fired_ms,
                "scheduler_job": record["scheduler_job"],
                "terminal_ms": record["terminal_ms"],
                "worker": record["worker"],
            }
        )
    return matching[0] if len(matching) == 1 else None


def _parse_rows(
    scenario: ScenarioSpec,
    plan: dict[str, Any],
    evidence: Path,
    events: list[dict[str, Any]],
) -> tuple[list[dict[str, Any]], dict[str, Any]]:
    topology = plan["topology"]["instances"]
    retry_decisions = _scheduler_retry_decisions(evidence, plan)
    by_name = {item["name"]: item for item in topology}
    workers = _endpoint_workers(plan, scenario, evidence)
    attachments = _worker_attachments(evidence, topology)
    f_commits = {
        item["name"]: _action_commits(
            _instance_results(evidence, item["name"]) / "f-action.jsonl",
            frozenset(("INPUT_COMMITTED",)),
        )
        for item in topology
        if item["role"] == "F"
    }
    cold_witness_contract = scenario.data.get("expect", {}).get("worker_cold_witness")
    if cold_witness_contract is not None and (
        scenario.data.get("id") != "S70-b4-worker-bounces"
        or scenario.data.get("expect", {}).get("engagement") != "s70-b4-worker-bounces"
        or cold_witness_contract != P29_ACTION_LINEAGE_CONTRACT
    ):
        raise CollectError("worker cold-witness contract is invalid for this scenario")
    f_action_lineages = {
        item["name"]: _p29_action_lineages(
            _instance_results(evidence, item["name"]) / "f-action.jsonl"
        )
        for item in topology
        if item["role"] == "F" and cold_witness_contract is not None
    }
    f_legacy_wires = {
        item["name"]: _legacy_wire_results(
            _instance_results(evidence, item["name"]) / "f-legacy-wire.jsonl", "F"
        )
        for item in topology
        if item["role"] == "F"
    }
    rows: list[dict[str, Any]] = []
    raw_jobs: list[dict[str, Any]] = []
    assignment_claims: list[dict[str, Any]] = []
    compile_failures: list[str] = []
    local_fallbacks: list[str] = []
    error106: list[str] = []
    source_mutex_records: list[dict[str, Any]] = []
    source_route_records: list[dict[str, Any]] = []
    p29_action_lineage_records: list[dict[str, Any]] = []
    failed_result_identity_records: list[dict[str, Any]] = []
    failed_source_transfer_records: list[dict[str, Any]] = []
    failed_uncommitted_transport_records: list[dict[str, Any]] = []
    resource_failure_records: list[dict[str, Any]] = []
    failed_unassigned_request_records: list[dict[str, Any]] = []
    abandoned_retry_request_records: list[dict[str, Any]] = []
    legacy_wire_records: list[dict[str, Any]] = []
    wire_revision_mismatches: list[dict[str, Any]] = []
    s30_refusals: list[dict[str, Any]] = []
    s30_canary_refusals: list[dict[str, Any]] = []
    if scenario.data.get("id") == "S30-mutant-f-refusal":
        for item in topology:
            if item["role"] == "F" and item["image"].get("kind") == "daemon-mutant":
                s30_refusals.extend(
                    _s30_mutant_refusals(
                        _instance_results(evidence, item["name"]) / "s30-mutant-f.jsonl"
                    )
                )
                s30_canary_refusals.extend(
                    _s30_mutant_refusals(
                        _instance_results(evidence, item["name"])
                        / "s30-mutant-f-canary.jsonl"
                    )
                )
        expected_canary_refusals = len(scenario.data["workload"]["clients"]) * sum(
            item["role"] == "F" and item["image"].get("kind") == "daemon-mutant"
            for item in topology
        )
        if len(s30_canary_refusals) != expected_canary_refusals:
            raise CollectError(
                "S30 mutant canary refusal count does not match client/worker readiness pairs"
            )

    for client_name in scenario.data["workload"]["clients"]:
        client = by_name[client_name]
        results = _instance_results(evidence, client_name)
        source_results = _source_results(results / "source-result.jsonl")
        compile_identities = _compile_identities(results / "compile-identity.jsonl")
        c_legacy_wires = _legacy_wire_results(results / "c-legacy-wire.jsonl", "C")
        c_commits = _action_commits(
            results / "c-action.jsonl",
            frozenset(("COMMIT_ACCEPTED", "LOST_COMMIT_ACCEPTED")),
        )
        workload_root = results / "workload"
        job_paths = sorted(
            [
                *workload_root.glob("jobs/*/result.tsv"),
                *workload_root.glob("*/jobs/*/result.tsv"),
            ]
        )
        if not job_paths:
            raise CollectError(f"client {client_name} has no workload job rows")
        for path in job_paths:
            raw = _job_result(path)
            job_dir = path.parent
            log_text = (
                _text(job_dir / "client-debug.log")
                + "\n"
                + _text(job_dir / "client-output.log")
            )
            if not raw["scheduler_job"].isdigit():
                job_id = (
                    f"{client_name}:{raw['turn']}:{raw['index']}:{raw['scheduler_job']}"
                )
                observation = _unassigned_p50_failure_observation(
                    log_text=log_text, raw=raw, row_job_id=job_id
                )
                raw_jobs.append(
                    {
                        "assignment_claims": [],
                        "client": client_name,
                        "local_build": False,
                        "row_job_id": job_id,
                        "unassigned_p50_failure": observation,
                        **raw,
                    }
                )
                failed_unassigned_request_records.append(observation)
                compile_failures.append(job_id)
                local_fallbacks.append(job_id)
                rows.append(
                    {
                        "c_to_f_bytes": 0,
                        "client_instance": client_name,
                        "client_version": client["version"],
                        "cs": "UNKNOWN",
                        "cs_version": 0,
                        "event_epoch": _epoch_at(events, raw["started"]),
                        "exact": False,
                        "f_to_c_bytes": 0,
                        "job_id": job_id,
                        "object_sha_local": raw["local_sha"],
                        "object_sha_remote": raw["remote_sha"],
                        "retries": 0,
                        "reuse": None,
                        "schema": ROW_SCHEMA,
                        "session_outcome": "failed",
                        "tail_present": False,
                        "tail_profile": None,
                        "tu": raw["relative"],
                        "wall_ms": raw["finished"] - raw["started"],
                    }
                )
                continue
            scheduler_job = int(raw["scheduler_job"])
            worker = workers.get(raw["worker"])
            if worker is None:
                raise CollectError(
                    f"{path}: assignment names unknown worker {raw['worker']!r}"
                )
            job_id = f"{client_name}:{raw['turn']}:{raw['index']}:{scheduler_job}"
            local_build = any(marker in log_text for marker in LOCAL_BUILD_MARKERS)
            if raw["compile_rc"] == 0 and bool(raw["remote"]) == local_build:
                raise CollectError(
                    f"{job_id}: remote flag disagrees with authenticated local-build evidence"
                )
            assignments = _client_assignments(log_text, str(job_dir))
            if len(assignments) != raw["retries"] + 1:
                raise CollectError(
                    f"{job_id}: wrapper retry count does not match assignment evidence"
                )
            for attempt_index, assignment in enumerate(assignments):
                assigned_worker = workers.get(assignment["endpoint"])
                if assigned_worker is None:
                    raise CollectError(
                        f"{job_id}: assignment names unknown worker "
                        f"{assignment['endpoint']!r}"
                    )
                assignment.update(
                    {
                        "attempt_index": attempt_index,
                        "client": client_name,
                        "kind": "workload",
                        "row_job_id": job_id,
                        "turn": raw["turn"],
                        "worker": assigned_worker["name"],
                    }
                )
                assignment_claims.append(assignment)
            final_assignment = assignments[-1]
            if (
                final_assignment["scheduler_job"] != scheduler_job
                or final_assignment["endpoint"] != raw["worker"]
            ):
                raise CollectError(
                    f"{job_id}: result row does not name its final assignment"
                )
            raw_job = {
                "assignment_claims": assignments,
                "client": client_name,
                "local_build": local_build,
                "row_job_id": job_id,
                **raw,
            }
            raw_jobs.append(raw_job)
            attempt_markers = _profile_markers_by_assignment(
                _profile_markers(job_dir), assignments, str(job_dir)
            )
            attempt_identity_evidence = [
                _p50_assignment_identity_evidence(
                    log_text,
                    assignment["scheduler_job"],
                    after_line=(assignments[index - 1]["line"] if index else 0),
                    before_line=assignment["line"] + 1,
                )
                for index, assignment in enumerate(assignments)
            ]
            attempt_identities = [
                (
                    evidence["scheduler_job"],
                    evidence["assignment_epoch"],
                    evidence["assignment_nonce"],
                )
                if evidence is not None
                else None
                for evidence in attempt_identity_evidence
            ]
            resource_failure = _resource_failure_observation(
                log_text, assignments, attempt_identity_evidence,
                compile_identities, job_id,
            )
            if resource_failure is not None:
                resource_failure_records.append(resource_failure)
                raw_job["resource_failure"] = resource_failure
            log_lines = log_text.splitlines()
            attempt_log_texts = [
                "\n".join(
                    log_lines[
                        assignment["line"] : (
                            assignments[index + 1]["line"] - 1
                            if index + 1 < len(assignments)
                            else len(log_lines)
                        )
                    ]
                )
                for index, assignment in enumerate(assignments)
            ]
            marker = attempt_markers[-1]
            legacy_marker = _legacy_wire_binding_marker(log_text, scheduler_job)
            assignment_identity = attempt_identities[-1]
            abandoned_retry_request = _abandoned_p50_retry_request_observation(
                log_text=log_text,
                raw=raw,
                assignment=final_assignment,
                assignment_identity=attempt_identity_evidence[-1],
                row_job_id=job_id,
            )
            if abandoned_retry_request is not None:
                raw_job["abandoned_p50_retry_request"] = abandoned_retry_request
                abandoned_retry_request_records.append(abandoned_retry_request)
            scheduler = next(item for item in topology if item["role"] == "S")

            missing_result_identities: list[dict[str, Any]] = []
            source_transfer_failures: list[dict[str, Any]] = []
            uncommitted_transport_failures: list[dict[str, Any]] = []
            for attempt_index, (
                assignment,
                attempt_marker,
                attempt_identity,
                attempt_log_text,
            ) in enumerate(
                zip(
                    assignments[:-1],
                    attempt_markers[:-1],
                    attempt_identities[:-1],
                    attempt_log_texts[:-1],
                )
            ):
                if attempt_marker is None:
                    transport_failure = _uncommitted_transport_failure_observation(
                        retry_decisions=retry_decisions,
                        log_text=log_text,
                        assignment=assignment,
                        retry_assignment=assignments[attempt_index + 1],
                        assignment_identity=attempt_identity_evidence[attempt_index],
                        retry_identity=attempt_identity_evidence[attempt_index + 1],
                        source_results=source_results,
                        compile_identities=compile_identities,
                        row_job_id=job_id,
                        attempt_index=attempt_index,
                    )
                    if transport_failure is not None:
                        uncommitted_transport_failures.append(transport_failure)
                        continue
                    failure = _source_transfer_failure_observation(
                        retry_decisions=retry_decisions,
                        log_text=log_text,
                        assignment=assignment,
                        retry_assignment=assignments[attempt_index + 1],
                        assignment_identity=attempt_identity_evidence[attempt_index],
                        retry_identity=attempt_identity_evidence[attempt_index + 1],
                        source_results=source_results,
                        compile_identities=compile_identities,
                        row_job_id=job_id,
                        attempt_index=attempt_index,
                    )
                    if failure is not None:
                        source_transfer_failures.append(failure)
                    continue
                attempt_candidates = _source_candidates_for_assignment(
                    source_results,
                    assignment["scheduler_job"],
                    _instance_version_at(scheduler, events, assignment["observed_ms"]),
                    attempt_identity,
                )
                matching = [
                    (key, candidate)
                    for key, candidate in attempt_candidates
                    if candidate["profile"] == attempt_marker["profile"]
                    and candidate["raw_bytes"] == attempt_marker["raw_bytes"]
                    and candidate["tu_seq"] == attempt_marker["tu_seq"]
                ]
                if len(matching) != 1:
                    raise CollectError(
                        f"{job_id}: prior P50 assignment lacks one exact source result"
                    )
                attempt_key, attempt_source = matching[0]
                if attempt_key in compile_identities:
                    continue
                reason = _missing_compile_result_identity_reason(
                    raw=raw,
                    final_attempt=False,
                    local_build=local_build,
                    log_text=attempt_log_text,
                    events=events,
                    assignment=assignment,
                    assignment_identity=attempt_identity,
                    source_key=attempt_key,
                    source=attempt_source,
                    marker=attempt_marker,
                    c_commits=c_commits,
                    f_commits=f_commits,
                )
                if reason is None:
                    raise CollectError(
                        f"{job_id}: prior P50 commit has no result identity or exact loss witness"
                    )
                missing_result_identities.append(
                    {
                        "assignment_epoch": attempt_identity[1],
                        "assignment_nonce": attempt_identity[2],
                        "attempt_index": attempt_index,
                        "reason": reason,
                        "result_identity_present": False,
                        "row_job_id": job_id,
                        "scheduler_job": assignment["scheduler_job"],
                        "worker": assignment["worker"],
                    }
                )

            assignment_scheduler_version = _instance_version_at(
                scheduler, events, final_assignment["observed_ms"]
            )
            candidates = _source_candidates_for_assignment(
                source_results,
                scheduler_job,
                assignment_scheduler_version,
                assignment_identity,
                legacy_binding_marker=legacy_marker,
            )
            authenticated: list[tuple[tuple[int, int, int], dict[str, Any]]] = []
            if marker is not None:
                authenticated = [
                    (key, source)
                    for key, source in candidates
                    if key in compile_identities
                    and source["profile"] == marker["profile"]
                    and source["raw_bytes"] == marker["raw_bytes"]
                    and source["tu_seq"] == marker["tu_seq"]
                ]
            if len(authenticated) > 1 or (marker is None and len(candidates) > 1):
                raise CollectError(f"{job_id}: source-result assignment is ambiguous")
            source = (
                authenticated[0][1]
                if marker is not None and len(authenticated) == 1
                else candidates[0][1]
                if marker is None and len(candidates) == 1
                else None
            )
            failed_result_identity_reason = None
            if marker is not None and source is None and len(candidates) == 1:
                candidate_key, candidate_source = candidates[0]
                if candidate_key not in compile_identities:
                    failed_result_identity_reason = (
                        _missing_compile_result_identity_reason(
                            raw=raw,
                            final_attempt=True,
                            local_build=local_build,
                            log_text=attempt_log_texts[-1],
                            events=events,
                            assignment=final_assignment,
                            assignment_identity=assignment_identity,
                            source_key=candidate_key,
                            source=candidate_source,
                            marker=marker,
                            c_commits=c_commits,
                            f_commits=f_commits,
                        )
                    )
                    if failed_result_identity_reason is not None:
                        source = candidate_source
                        missing_result_identities.append(
                            {
                                "assignment_epoch": assignment_identity[1],
                                "assignment_nonce": assignment_identity[2],
                                "attempt_index": len(assignments) - 1,
                                "reason": failed_result_identity_reason,
                                "result_identity_present": False,
                                "row_job_id": job_id,
                                "scheduler_job": final_assignment["scheduler_job"],
                                "worker": final_assignment["worker"],
                            }
                        )
            if missing_result_identities:
                raw_job["missing_compile_result_identities"] = missing_result_identities
                failed_result_identity_records.extend(missing_result_identities)
            if source_transfer_failures:
                raw_job["failed_p50_source_transfers"] = source_transfer_failures
                failed_source_transfer_records.extend(source_transfer_failures)
            if uncommitted_transport_failures:
                raw_job["failed_p50_uncommitted_transports"] = (
                    uncommitted_transport_failures
                )
                failed_uncommitted_transport_records.extend(
                    uncommitted_transport_failures
                )
            legacy_candidates = _legacy_wire_candidates_for_assignment(
                c_legacy_wires,
                scheduler_job,
                legacy_marker,
                compile_identities,
            )
            if len(legacy_candidates) > 1:
                raise CollectError(f"{job_id}: legacy-wire assignment is ambiguous")
            legacy_key, legacy_wire = (
                legacy_candidates[0] if legacy_candidates else (None, None)
            )
            if any(
                failure.get("retry_mode") == "legacy"
                for failure in source_transfer_failures
            ) and (
                len(assignments) != 2
                or raw["retries"] != 1
                or marker is not None
                or source is not None
                or legacy_marker is None
                or legacy_wire is None
            ):
                raise CollectError(
                    f"{job_id}: source-transfer legacy retry lacks one exact legacy result"
                )
            if legacy_marker is not None and legacy_wire is None:
                raise CollectError(
                    f"{job_id}: legacy-wire binding marker has no exact result witness"
                )
            if source is not None and legacy_wire is not None:
                raise CollectError(f"{job_id}: source and legacy-wire evidence overlap")
            if marker is not None and source is None:
                if legacy_wire is None:
                    raise CollectError(
                        f"{job_id}: P50 commit marker has no full-identity "
                        "source-result witness"
                    )
                raw_job["orphan_recovery_marker"] = marker
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
                    committed = _source_commit_is_exact(
                        source,
                        marker,
                        worker_name=worker["name"],
                        scheduler_job=scheduler_job,
                        c_commits=c_commits,
                        f_commits=f_commits,
                        attachments=attachments,
                    )
                    outcome = (
                        "failed"
                        if failed_result_identity_reason is not None
                        else "committed"
                        if committed
                        else "refused"
                    )
                reuse = source["system_source_reuse"] if profile == "P29V1" else None
                c_to_f = source["c_to_f_bytes"]
                f_to_c = source["f_to_c_bytes"]
                transfer_retries = max(0, source["attempts"] - 1)
                source_mutex_records.append(
                    {
                        "client_instance": client_name,
                        "job_id": job_id,
                        "outcome": outcome,
                        "profile": profile,
                        "service_ns": source["source_mutex_service_ns"],
                        "turn": raw["turn"],
                        "wait_ns": source["source_mutex_wait_ns"],
                    }
                )
                source_route_records.append(
                    {
                        "c_store_guid": source["c_store_guid"],
                        "c_to_f_bytes": c_to_f,
                        "client_instance": client_name,
                        "f_to_c_bytes": f_to_c,
                        "job_id": job_id,
                        "profile": profile,
                        "raw_bytes": source["raw_bytes"],
                        "raw_digest": source["raw_digest"],
                        "schema": "icefarm-p50-source-route-v1",
                        "tu_seq": source["tu_seq"],
                        "worker_instance": worker["name"],
                    }
                )
                if cold_witness_contract is not None:
                    if profile != "P29V1":
                        raise CollectError(
                            f"{job_id}: cold-witness scenario selected a non-P29 route"
                        )
                    lineage = f_action_lineages[worker["name"]].get(
                        (source["c_store_guid"], source["tu_seq"])
                    )
                    if lineage is None or lineage["raw_digest"] != source["raw_digest"]:
                        raise CollectError(
                            f"{job_id}: P29 source route has no exact F action lineage"
                        )
                    p29_action_lineage_records.append(
                        {
                            **lineage,
                            "job_id": job_id,
                            "schema": P29_ACTION_LINEAGE_SCHEMA,
                            "worker_instance": worker["name"],
                        }
                    )
            elif legacy_wire is not None:
                f_wire = f_legacy_wires[worker["name"]].get(legacy_key)
                if f_wire is None:
                    raise CollectError(f"{job_id}: C legacy-wire result has no F peer")
                if (
                    f_wire["c_guid"] != legacy_wire["c_guid"]
                    or f_wire["tu_seq"] != legacy_wire["tu_seq"]
                    or legacy_wire["c_to_f_sent_bytes"]
                    != f_wire["c_to_f_received_bytes"]
                    or legacy_wire["f_to_c_received_bytes"]
                    != f_wire["f_to_c_sent_bytes"]
                ):
                    raise CollectError(
                        f"{job_id}: legacy-wire bytes/identity do not conserve"
                    )
                c_to_f = legacy_wire["c_to_f_sent_bytes"]
                f_to_c = legacy_wire["f_to_c_received_bytes"]
                legacy_wire_records.append(
                    {
                        "c_to_f_bytes": c_to_f,
                        "client_instance": client_name,
                        "f_to_c_bytes": f_to_c,
                        "job_id": job_id,
                        "turn": raw["turn"],
                        "worker_instance": worker["name"],
                    }
                )
                if scenario.data.get("id") in {
                    "S30-mutant-f-refusal",
                    "S70-b4-scheduler-active-loss",
                }:
                    # A refused P50 assignment is followed by one fresh
                    # legacy assignment.  Once the route owner requests
                    # replacement, later jobs may correctly arrive as
                    # ordinary first-assignment legacy work.
                    outcome = "fallback" if raw["retries"] == 1 else "none"
            if scenario.data.get("id") == "S90-revision-refusal-retry":
                attempt_sources = [
                    (key, source)
                    for assignment in assignments
                    for key, source in source_results.items()
                    if key[0] == assignment["scheduler_job"]
                ]
                if raw["retries"] == 1:
                    if (
                        len(assignments) != 2
                        or len(attempt_sources) != 1
                        or legacy_key is None
                        or legacy_wire is None
                    ):
                        raise CollectError(
                            f"{job_id}: revision mismatch lacks one fresh legacy retry"
                        )
                    first_assignment, retry_assignment = assignments
                    first_key, refused = attempt_sources[0]
                    first_worker = workers[first_assignment["endpoint"]]
                    first_scenario_instance = next(
                        (
                            item
                            for item in scenario.data["instances"]
                            if item.get("name") == first_worker["name"]
                        ),
                        None,
                    )
                    endpoint_revision = (
                        first_scenario_instance.get("env", {}).get(
                            "ICECC_P50_S90_ENDPOINT_WIRE_REVISION"
                        )
                        if isinstance(first_scenario_instance, Mapping)
                        else None
                    )
                    if (
                        first_key[0] != first_assignment["scheduler_job"]
                        or legacy_key[0] != retry_assignment["scheduler_job"]
                        or first_key == legacy_key
                        or refused.get("profile") != "P29V1"
                        or refused.get("status") != 4
                        or refused.get("attempts") != 1
                        or refused.get("terminal_error_code") != 4
                        or refused.get("terminal_error_name")
                        != "WIRE_REVISION_MISMATCH"
                        or first_worker.get("cache_wire_revision") != 1
                        or first_worker.get("image", {}).get("kind") != "daemon-mutant"
                        or endpoint_revision != "2"
                        or by_name[client_name].get("cache_wire_revision") != 1
                    ):
                        raise CollectError(
                            f"{job_id}: revision mismatch identity is not authenticated"
                        )
                    outcome = "fallback"
                    wire_revision_mismatches.append(
                        {
                            "advertised_worker_wire_revision": 1,
                            "client_instance": client_name,
                            "client_wire_revision": 1,
                            "endpoint_wire_revision": 2,
                            "error": "WIRE_REVISION_MISMATCH",
                            "error_code": 4,
                            "first_assignment": {
                                "assignment_epoch": first_key[1],
                                "assignment_nonce": first_key[2],
                                "scheduler_job": first_key[0],
                            },
                            "retry_assignment": {
                                "assignment_epoch": legacy_key[1],
                                "assignment_nonce": legacy_key[2],
                                "scheduler_job": legacy_key[0],
                            },
                            "row_job_id": job_id,
                            "schema": S90_REVISION_MISMATCH_SCHEMA,
                            "worker_instance": first_worker["name"],
                        }
                    )
                elif raw["retries"] != 0 or attempt_sources:
                    raise CollectError(
                        f"{job_id}: direct S90 legacy row has mismatch evidence"
                    )
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
    if scenario.data.get("id") == "S30-mutant-f-refusal":
        fallback_rows = [row for row in rows if row["session_outcome"] == "fallback"]
        if not s30_refusals or len(s30_refusals) > len(fallback_rows):
            raise CollectError(
                "S30 mutant refusals do not bind at least one bounded fallback row"
            )
    return rows, {
        "compile_failure_job_ids": sorted(compile_failures),
        "error106_job_ids": sorted(error106),
        "p50_resource_failures": resource_failure_records,
        "failed_p50_result_identities": {
            "record_count": len(failed_result_identity_records),
            "records": sorted(
                failed_result_identity_records,
                key=lambda item: (item["row_job_id"], item["attempt_index"]),
            ),
        },
        "failed_p50_source_transfers": {
            "record_count": len(failed_source_transfer_records),
            "records": sorted(
                failed_source_transfer_records,
                key=lambda item: (item["row_job_id"], item["attempt_index"]),
            ),
        },
        "failed_p50_uncommitted_transports": {
            "record_count": len(failed_uncommitted_transport_records),
            "records": sorted(
                failed_uncommitted_transport_records,
                key=lambda item: (item["row_job_id"], item["attempt_index"]),
            ),
        },
        "failed_unassigned_p50_requests": {
            "record_count": len(failed_unassigned_request_records),
            "records": sorted(
                failed_unassigned_request_records,
                key=lambda item: item["row_job_id"],
            ),
        },
        "abandoned_p50_retry_requests": {
            "record_count": len(abandoned_retry_request_records),
            "records": sorted(
                abandoned_retry_request_records,
                key=lambda item: item["row_job_id"],
            ),
        },
        "local_fallback_job_ids": sorted(local_fallbacks),
        "legacy_wire": {
            "records": sorted(legacy_wire_records, key=lambda item: item["job_id"]),
            "record_count": len(legacy_wire_records),
        },
        "source_mutex": {
            "records": sorted(source_mutex_records, key=lambda item: item["job_id"]),
            "record_count": len(source_mutex_records),
            "service_total_ns": sum(
                item["service_ns"] for item in source_mutex_records
            ),
            "wait_max_ns": max(
                (item["wait_ns"] for item in source_mutex_records), default=0
            ),
            "wait_total_ns": sum(item["wait_ns"] for item in source_mutex_records),
        },
        "p50_source_routes": {
            "record_count": len(source_route_records),
            "records": sorted(source_route_records, key=lambda item: item["job_id"]),
        },
        "p29_action_lineage": {
            "record_count": len(p29_action_lineage_records),
            "records": sorted(
                p29_action_lineage_records, key=lambda item: item["job_id"]
            ),
            "schema": P29_ACTION_LINEAGE_SCHEMA,
        },
        "assignment_claims": assignment_claims,
        "raw_jobs": raw_jobs,
        "s30_mutant_f": {
            "canary_records": s30_canary_refusals,
            "canary_refusal_count": len(s30_canary_refusals),
            "schema": S30_MUTANT_TRACE_SCHEMA,
            "records": s30_refusals,
            "refusal_count": len(s30_refusals),
        },
        "wire_revision_mismatches": sorted(
            wire_revision_mismatches, key=lambda item: item["row_job_id"]
        ),
    }


def _canary_assignment_claims(
    scenario: ScenarioSpec,
    plan: dict[str, Any],
    evidence: Path,
) -> list[dict[str, Any]]:
    """Authenticate the readiness dispatches which precede every workload."""

    workers = _endpoint_workers(plan, scenario, evidence)
    claims: list[dict[str, Any]] = []
    worker_instances = sorted(
        (item for item in plan["topology"]["instances"] if item["role"] == "F"),
        key=lambda item: item["name"],
    )
    for client_name in sorted(scenario.data["workload"]["clients"]):
        root = _instance_results(evidence, client_name) / "canary"
        for worker in worker_instances:
            pair = f"{client_name}->{worker['name']}"
            debug_path = root / f"{worker['name']}.client.log"
            output_path = root / f"{worker['name']}.stdout.log"
            if not debug_path.is_file() or debug_path.is_symlink():
                raise CollectError(
                    f"readiness canary assignment log is absent for {pair}"
                )
            text = (
                _text(debug_path)
                + "\n"
                + _text(
                    output_path
                    if output_path.is_file() and not output_path.is_symlink()
                    else None
                )
            )
            if any(marker in text for marker in LOCAL_BUILD_MARKERS):
                raise CollectError(f"readiness canary compiled locally for {pair}")
            assignments = _client_assignments(text, str(debug_path))
            if not assignments:
                raise CollectError(
                    f"readiness canary has no assignment evidence for {pair}"
                )
            for attempt_index, assignment in enumerate(assignments):
                assigned_worker = workers.get(assignment["endpoint"])
                if assigned_worker is None:
                    raise CollectError(
                        "readiness canary names unknown worker "
                        f"{assignment['endpoint']!r}"
                    )
                assignment.update(
                    {
                        "attempt_index": attempt_index,
                        "client": client_name,
                        "kind": "canary",
                        "row_job_id": None,
                        "turn": None,
                        "worker": assigned_worker["name"],
                    }
                )
                claims.append(assignment)
            if assignments[-1]["worker"] != worker["name"]:
                raise CollectError(
                    f"readiness canary for {pair} completed on "
                    f"{assignments[-1]['worker']}"
                )
    return claims


def _reconcile_scheduler_dispatches(
    evidence: Path,
    plan: dict[str, Any],
    claims: list[dict[str, Any]],
    *,
    allow_unterminated_job_ids: set[int] | None = None,
    allow_unterminated_generations: set[int] | None = None,
    unclaimed_requests: list[dict[str, Any]] | None = None,
) -> dict[str, Any]:
    """Require an exact scheduler-dispatch/assignment/terminal bijection."""

    dispatches = _scheduler_jobs(
        evidence, plan, allow_unterminated_job_ids=allow_unterminated_job_ids,
        allow_unterminated_generations=allow_unterminated_generations,
    )
    scheduler_groups: dict[tuple[int, str, str], list[dict[str, Any]]] = defaultdict(
        list
    )
    claim_groups: dict[tuple[int, str, str], list[dict[str, Any]]] = defaultdict(list)
    for dispatch in dispatches:
        key = (dispatch["scheduler_job"], dispatch["client"], dispatch["worker"])
        scheduler_groups[key].append(dispatch)
    for claim in claims:
        key = (claim["scheduler_job"], claim["client"], claim["worker"])
        claim_groups[key].append(claim)
    missing = sorted(set(claim_groups) - set(scheduler_groups))
    extra = sorted(set(scheduler_groups) - set(claim_groups))
    if missing:
        raise CollectError(
            f"scheduler dispatch/assignment keys differ missing={missing!r} extra={extra!r}"
        )
    unclaimed_dispatches = [
        dispatch
        for key in extra
        for dispatch in scheduler_groups[key]
    ]
    requests = unclaimed_requests or []
    if extra and not requests:
        raise CollectError(
            f"scheduler dispatch/assignment keys differ missing={missing!r} extra={extra!r}"
        )
    if unclaimed_dispatches:
        request_clients = Counter(item.get("client_instance") for item in requests)
        dispatch_clients = Counter(item.get("client") for item in unclaimed_dispatches)
        request_rows = [item.get("row_job_id") for item in requests]
        if (
            not requests
            # A failed request can be rejected before any scheduler dispatch
            # (for example strict-P50 no-handoff).  Such authenticated rows
            # remain compile failures but do not participate in the
            # dispatch/claim bijection.  Every actual extra dispatch must
            # still have a client/time-compatible failed request witness.
            or any(
                dispatch_clients[client] > request_clients[client]
                for client in dispatch_clients
            )
            or any(not isinstance(item, str) or not item for item in request_rows)
            or len(set(request_rows)) != len(request_rows)
            or any(
                type(item.get("request_started_ms")) is not int
                or type(item.get("request_finished_ms")) is not int
                or item["request_finished_ms"] < item["request_started_ms"]
                for item in requests
            )
            or any(
                dispatch["dispatch_ms"]
                < min(item["request_started_ms"] for item in requests) - 1000
                or dispatch["dispatch_ms"]
                > max(item["request_finished_ms"] for item in requests) + 1000
                for dispatch in unclaimed_dispatches
            )
            or any(len(scheduler_groups[key]) != 1 for key in extra)
        ):
            raise CollectError(
                "unclaimed scheduler dispatches do not bind exactly to "
                "authenticated failed P50 requests"
            )
    preexposure_redispatches: list[dict[str, Any]] = []
    for key in sorted(scheduler_groups):
        if key in extra:
            continue
        observed = scheduler_groups[key]
        asserted = claim_groups[key]
        if len(observed) != len(asserted):
            raise CollectError(
                "scheduler dispatch/assignment multiplicity differs for "
                f"{key}: scheduler={len(observed)} assignments={len(asserted)}"
            )
        if len(asserted) > 1:
            claim_times = [item["observed_ms"] for item in asserted]
            if len(set(claim_times)) != len(claim_times):
                raise CollectError(
                    f"scheduler generation is ambiguous for repeated assignment {key}"
                )
        for claim, dispatch in zip(
            sorted(asserted, key=lambda item: item["observed_ms"]),
            sorted(observed, key=lambda item: item["dispatch_line"]),
            strict=True,
        ):
            claim["scheduler_record"] = dispatch
            histories = dispatch.get("preexposure_redispatches", [])
            if not isinstance(histories, list):
                raise CollectError(
                    "scheduler pre-exposure redispatch history is malformed"
                )
            if histories and (
                claim.get("kind") != "workload"
                or not isinstance(claim.get("row_job_id"), str)
                or not claim["row_job_id"]
                or type(claim.get("attempt_index")) is not int
                or claim["attempt_index"] < 0
            ):
                raise CollectError(
                    "pre-exposure redispatch does not bind a workload assignment"
                )
            for history in histories:
                if not isinstance(history, Mapping):
                    raise CollectError(
                        "scheduler pre-exposure redispatch history is malformed"
                    )
                preexposure_redispatches.append(
                    {
                        **history,
                        "attempt_index": claim["attempt_index"],
                        "row_job_id": claim["row_job_id"],
                    }
                )
    canary_count = sum(claim["kind"] == "canary" for claim in claims)
    result = {
        "canary_dispatches": canary_count,
        "generations": max(item["generation"] for item in dispatches),
        "preexposure_redispatches": sorted(
            preexposure_redispatches,
            key=lambda item: item["marker_line"],
        ),
        "scheduler_dispatches": len(dispatches),
        "workload_dispatches": len(dispatches) - canary_count,
    }
    if unclaimed_dispatches:
        result["unclaimed_dispatches"] = [
            {
                "client": item["client"],
                "dispatch_line": item["dispatch_line"],
                "generation": item["generation"],
                "scheduler_job": item["scheduler_job"],
                "terminal": item["terminal"],
                "worker": item["worker"],
            }
            for item in sorted(
                unclaimed_dispatches, key=lambda item: item["dispatch_line"]
            )
        ]
    return result


def _validate_preexposure_redispatches(
    scenario: ScenarioSpec,
    plan: Mapping[str, Any],
    events: list[dict[str, Any]],
    records: Any,
) -> None:
    """Bind transparent scheduler redispatches to declared worker restarts."""

    if not isinstance(records, list):
        raise CollectError("pre-exposure redispatch observations are malformed")
    if not records:
        return
    if (
        scenario.data.get("expect", {}).get("engagement")
        != "s70-b4-worker-bounces"
    ):
        raise CollectError(
            "pre-exposure redispatch is outside the worker-bounce acceptance gate"
        )
    topology = plan.get("topology")
    instances = topology.get("instances") if isinstance(topology, Mapping) else None
    workers = {
        item.get("name")
        for item in instances
        if isinstance(item, Mapping) and item.get("role") == "F"
    } if isinstance(instances, list) else set()
    clients = set(scenario.data.get("workload", {}).get("clients", []))
    restart_events = [
        event
        for event in events
        if event.get("action") == "restart"
        and event.get("instance") in workers
        and type(event.get("fired_ms")) is int
    ]
    for record in records:
        if (
            not isinstance(record, Mapping)
            or record.get("lost_worker") not in workers
            or record.get("replacement_worker") not in workers
            or record["replacement_worker"] == record["lost_worker"]
            or record.get("client") not in clients
            or type(record.get("lost_dispatch_line")) is not int
            or type(record.get("marker_line")) is not int
            or type(record.get("replacement_dispatch_line")) is not int
            or type(record.get("lost_dispatch_ms")) is not int
            or type(record.get("marker_ms")) is not int
            or type(record.get("replacement_dispatch_ms")) is not int
            or not (
                record["lost_dispatch_line"]
                < record["marker_line"]
                < record["replacement_dispatch_line"]
            )
            or not (
                record["lost_dispatch_ms"]
                <= record["marker_ms"]
                <= record["replacement_dispatch_ms"]
            )
        ):
            raise CollectError("pre-exposure redispatch observation is malformed")
        matching_index = next(
            (
                index
                for index, event in enumerate(restart_events)
                if event["instance"] == record["lost_worker"]
                and record["marker_ms"] <= event["fired_ms"]
                and (
                    index == 0
                    or restart_events[index - 1]["fired_ms"] < record["marker_ms"]
                )
            ),
            None,
        )
        if (
            matching_index is None
            or record["replacement_dispatch_ms"]
            > restart_events[matching_index]["fired_ms"]
            or (
                matching_index > 0
                and record["lost_dispatch_ms"]
                < restart_events[matching_index - 1]["fired_ms"]
            )
        ):
            raise CollectError(
                "pre-exposure redispatch is not bound to one worker restart"
            )


def _assignment_preference(
    scenario: ScenarioSpec,
    plan: dict[str, Any],
    claims: list[dict[str, Any]],
) -> dict[str, Any]:
    """Replay compatible-free preference from reconciled assignment claims.

    Readiness canaries are intentionally excluded.  Every workload claim must
    already carry its authenticated scheduler record from reconciliation; this
    function only derives occupancy and the preference decision from that
    record and the resolved topology.
    """

    topology = plan.get("topology")
    if not isinstance(topology, Mapping):
        raise CollectError("assignment preference has no resolved topology")
    instances = topology.get("instances")
    relationships = topology.get("relationships")
    if not isinstance(instances, list) or not isinstance(relationships, list):
        raise CollectError("assignment preference topology is malformed")
    workers = [
        item
        for item in instances
        if isinstance(item, Mapping) and item.get("role") == "F"
    ]
    worker_names = [item.get("name") for item in workers]
    if any(not isinstance(name, str) or not name for name in worker_names):
        raise CollectError("assignment preference has an unnamed worker")
    if len(set(worker_names)) != len(worker_names):
        raise CollectError("assignment preference has duplicate workers")
    slots: dict[str, int] = {}
    for worker in workers:
        name = worker["name"]
        value = worker.get("slots")
        if type(value) is not int or value <= 0:
            raise CollectError(f"assignment preference has invalid slots for {name!r}")
        slots[name] = value
    clients = {
        item.get("name")
        for item in instances
        if isinstance(item, Mapping) and item.get("role") == "C"
    }
    workload_clients = scenario.data["workload"]["clients"]
    if not isinstance(workload_clients, list) or any(
        not isinstance(name, str) or name not in clients for name in workload_clients
    ):
        raise CollectError("assignment preference has invalid workload clients")
    compatibility: dict[str, list[str]] = {name: [] for name in workload_clients}
    seen_pairs: set[tuple[str, str]] = set()
    for relationship in relationships:
        if not isinstance(relationship, Mapping):
            raise CollectError("assignment preference relationship is malformed")
        client = relationship.get("c")
        worker = relationship.get("f")
        if (
            not isinstance(client, str)
            or client not in clients
            or not isinstance(worker, str)
            or worker not in slots
            or type(relationship.get("cache_expected")) is not bool
        ):
            raise CollectError("assignment preference relationship is unbound")
        pair = (client, worker)
        if pair in seen_pairs:
            raise CollectError(f"assignment preference duplicates relationship {pair!r}")
        seen_pairs.add(pair)
        if relationship["cache_expected"] and client in compatibility:
            compatibility[client].append(worker)
    expected_pairs = {(client, worker) for client in clients for worker in worker_names}
    if seen_pairs != expected_pairs:
        raise CollectError("assignment preference relationship matrix is incomplete")
    for compatible_workers in compatibility.values():
        compatible_workers.sort()
    workload_claims: list[dict[str, Any]] = []
    for claim in claims:
        if not isinstance(claim, Mapping):
            raise CollectError("assignment preference claim is malformed")
        kind = claim.get("kind")
        if kind == "canary":
            continue
        if kind != "workload":
            raise CollectError("assignment preference claim has an unknown kind")
        client = claim.get("client")
        worker = claim.get("worker")
        record = claim.get("scheduler_record")
        if (
            not isinstance(client, str)
            or client not in compatibility
            or not isinstance(worker, str)
            or worker not in slots
            or not isinstance(record, Mapping)
        ):
            raise CollectError("assignment preference workload claim is malformed")
        dispatch_line = record.get("dispatch_line")
        terminal_line = record.get("terminal_line")
        if (
            type(dispatch_line) is not int
            or dispatch_line <= 0
            or type(terminal_line) is not int
            or terminal_line <= dispatch_line
            or type(record.get("scheduler_job")) is not int
            or record["scheduler_job"] <= 0
            or record.get("client") != client
            or record.get("worker") != worker
            or not isinstance(claim.get("row_job_id"), str)
            or not claim["row_job_id"]
        ):
            raise CollectError("assignment preference scheduler record is malformed")
        workload_claims.append(claim)
    workload_claims.sort(key=lambda item: item["scheduler_record"]["dispatch_line"])
    dispatch_lines = [item["scheduler_record"]["dispatch_line"] for item in workload_claims]
    if len(set(dispatch_lines)) != len(dispatch_lines):
        raise CollectError("assignment preference has duplicate dispatch lines")
    preexposure_intervals: list[tuple[int, int, str]] = []
    for claim in workload_claims:
        histories = claim["scheduler_record"].get("preexposure_redispatches", [])
        if not isinstance(histories, list):
            raise CollectError(
                "assignment preference has malformed pre-exposure history"
            )
        for history in histories:
            if (
                not isinstance(history, Mapping)
                or type(history.get("lost_dispatch_line")) is not int
                or type(history.get("marker_line")) is not int
                or history["lost_dispatch_line"] <= 0
                or history["marker_line"] <= history["lost_dispatch_line"]
                or history.get("lost_worker") not in slots
            ):
                raise CollectError(
                    "assignment preference has malformed pre-exposure history"
                )
            preexposure_intervals.append(
                (
                    history["lost_dispatch_line"],
                    history["marker_line"],
                    history["lost_worker"],
                )
            )
    decisions: list[dict[str, Any]] = []
    violations: list[str] = []
    for index, claim in enumerate(workload_claims):
        record = claim["scheduler_record"]
        dispatch_line = record["dispatch_line"]
        occupancy = {worker: 0 for worker in worker_names}
        for prior in workload_claims[:index]:
            prior_record = prior["scheduler_record"]
            if (
                prior_record["dispatch_line"] < dispatch_line
                and dispatch_line < prior_record["terminal_line"]
            ):
                occupancy[prior["worker"]] += 1
        for start_line, stop_line, worker in preexposure_intervals:
            if start_line < dispatch_line < stop_line:
                occupancy[worker] += 1
        compatible = compatibility[claim["client"]]
        compatible_free = [
            worker for worker in compatible if occupancy[worker] < slots[worker]
        ]
        escape = not compatible_free
        preferred = bool(compatible_free and claim["worker"] in compatible_free)
        if compatible_free and not preferred:
            violations.append(claim["row_job_id"])
        decisions.append(
            {
                "client": claim["client"],
                "compatible_free_workers": compatible_free,
                "compatible_workers": compatible,
                "dispatch_line": dispatch_line,
                "escape": escape,
                "occupancy": occupancy,
                "preferred": preferred,
                "row_job_id": claim["row_job_id"],
                "scheduler_job": record.get("scheduler_job"),
                "worker": claim["worker"],
            }
        )
    preferred_count = sum(item["preferred"] for item in decisions)
    escape_count = sum(item["escape"] for item in decisions)
    return {
        "compatibility": {
            client: sorted(compatibility[client]) for client in sorted(compatibility)
        },
        "counts": {
            "checks": len(decisions),
            "escapes": escape_count,
            "preferred": preferred_count,
            "violations": len(violations),
        },
        "decisions": decisions,
        "schema": "icefarm-assignment-preference-v1",
        "workers": {worker: slots[worker] for worker in sorted(slots)},
        "violations": violations,
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
        endpoint, advertisement_format, cache_protocol, profile_text = cache.groups()
        profiles = []
        for raw in profile_text.split():
            profile = PROFILE_LABELS.get(raw)
            if profile is None:
                raise CollectError(f"scheduler login names unknown profile {raw!r}")
            profiles.append(profile)
        latest[name] = {
            "cache_protocol": int(cache_protocol),
            "cache_profiles": profiles,
            "instance": name,
            "protocol": instance["version"],
        }
        # `cache_wire=vN` names the stable three-word advertisement encoding.
        # Pair compatibility is governed by the separately advertised
        # cache_protocol value.
        if int(advertisement_format) != 1:
            raise CollectError(
                f"scheduler login uses unsupported cache advertisement format "
                f"v{advertisement_format}"
            )
        revisions[name] = int(cache_protocol)
        port_text = endpoint.rsplit(":", 1)[-1]
        if not port_text.isdigit() or not (1 <= int(port_text) <= 65535):
            raise CollectError(
                f"scheduler login has invalid cache endpoint {endpoint!r}"
            )
        ports[name] = [int(port_text)]
    return [latest[name] for name in sorted(latest)], revisions, dict(ports)


def _warm_hint_overrides(evidence: Path, plan: dict[str, Any]) -> dict[str, Any]:
    topology = plan["topology"]["instances"]
    scheduler = next(item for item in topology if item["role"] == "S")
    worker_count = sum(item["role"] == "F" for item in topology)
    log = _text(_one_role_log(evidence, scheduler))
    events: list[dict[str, Any]] = []
    for line_number, line in enumerate(log.splitlines(), start=1):
        if "P50_WARM_HINT_OVERRIDE" not in line:
            continue
        match = WARM_HINT_OVERRIDE_RE.search(line)
        if match is None:
            raise CollectError(
                f"scheduler warm-hint diagnostic is malformed at line {line_number}"
            )
        job, warm, compatible_free, idle_excluded = map(int, match.groups())
        if not (
            job >= 1
            and 1 <= warm < compatible_free <= worker_count
            and 1 <= idle_excluded <= compatible_free - warm
        ):
            raise CollectError(
                f"scheduler warm-hint diagnostic is inconsistent at line {line_number}"
            )
        events.append(
            {
                "compatible_free": compatible_free,
                "idle_excluded": idle_excluded,
                "job_id": job,
                "line": line_number,
                "warm": warm,
            }
        )
    return {
        "count": len(events),
        "events": events,
        "job_ids": sorted({item["job_id"] for item in events}),
    }


def _oracle(evidence: Path, scenario: ScenarioSpec) -> dict[str, Any]:
    sample_total = 0
    mismatches: list[str] = []
    for client in scenario.data["workload"]["clients"]:
        root = _instance_results(evidence, client) / "workload"
        turn_roots = (
            [root]
            if (root / "oracle-summary.tsv").is_file()
            else [root / turn for turn in scenario.data["workload"]["turns"]]
        )
        for turn_root in turn_roots:
            summary: dict[str, int] = {}
            try:
                lines = (
                    (turn_root / "oracle-summary.tsv")
                    .read_text(encoding="utf-8")
                    .splitlines()
                )
            except OSError as exc:
                raise CollectError(
                    f"cannot read oracle summary for {client}/{turn_root.name}: {exc}"
                ) from exc
            for line in lines:
                fields = line.split("\t")
                if len(fields) != 2 or not fields[1].isdigit() or fields[0] in summary:
                    raise CollectError(f"oracle summary for {client} is malformed")
                summary[fields[0]] = int(fields[1])
            if set(summary) != {"sample_total", "sample_mismatches"}:
                raise CollectError(f"oracle summary for {client} has the wrong fields")
            sample_total += summary["sample_total"]
            try:
                samples = (
                    (turn_root / "oracle-samples.tsv")
                    .read_text(encoding="utf-8")
                    .splitlines()
                )
            except OSError as exc:
                raise CollectError(
                    f"cannot read oracle samples for {client}/{turn_root.name}: {exc}"
                ) from exc
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
                    raise CollectError(
                        f"oracle sample {client}:{index} exact flag disagrees"
                    )
                if not exact:
                    observed_mismatches += 1
                    mismatches.append(f"oracle:{client}:{fields[0]}")
            if (
                len(samples) != summary["sample_total"]
                or observed_mismatches != summary["sample_mismatches"]
            ):
                raise CollectError(
                    f"oracle summary for {client} disagrees with samples"
                )
    return {"sample_mismatch_job_ids": sorted(mismatches), "sample_total": sample_total}


def _turn_completeness(
    scenario: ScenarioSpec,
    plan: dict[str, Any],
    evidence: Path,
    raw_jobs: list[dict[str, Any]],
) -> list[str]:
    authority = plan["topology"]["corpus_authority"]
    repetitions = authority.get("repeat", 1) * scenario.data["workload"]["repeat"]
    incomplete: list[str] = []
    for client in scenario.data["workload"]["clients"]:
        for turn in scenario.data["workload"]["turns"]:
            workload_root = _instance_results(evidence, client) / "workload"
            turn_root = workload_root / turn
            if not turn_root.is_dir():
                turn_root = workload_root
            manifest = turn_root / "corpus-manifest.sha256"
            group = "files" if "files" in authority["archives"] else turn
            archive = authority["archives"].get(group)
            if archive is None:
                raise CollectError(f"{client}:{turn}: corpus archive group is absent")
            if not manifest.is_file() or manifest.is_symlink():
                raise CollectError(
                    f"{client}:{turn}: authenticated corpus manifest is absent"
                )
            if _sha256(manifest) != archive["manifest_sha256"]:
                raise CollectError(f"{client}:{turn}: corpus manifest digest mismatch")
            try:
                lines = manifest.read_text(encoding="ascii").splitlines()
            except (OSError, UnicodeError) as exc:
                raise CollectError(
                    f"{client}:{turn}: cannot read corpus manifest: {exc}"
                ) from exc
            relatives: list[str] = []
            prefix = group + "/"
            for line_number, line in enumerate(lines, start=1):
                fields = line.split("  ", 1)
                if (
                    len(fields) != 2
                    or SHA256_RE.fullmatch(fields[0]) is None
                    or not fields[1].startswith(prefix)
                ):
                    raise CollectError(
                        f"{client}:{turn}: malformed corpus manifest line {line_number}"
                    )
                relative = fields[1]
                parts = Path(relative).parts
                if (
                    not relative
                    or Path(relative).is_absolute()
                    or any(part in ("", ".", "..") for part in parts)
                ):
                    raise CollectError(
                        f"{client}:{turn}: unsafe corpus manifest line {line_number}"
                    )
                relatives.append(relative)
            if len(relatives) != authority["tus"] or len(set(relatives)) != len(
                relatives
            ):
                raise CollectError(
                    f"{client}:{turn}: corpus manifest TU set is invalid"
                )
            expected = Counter(
                (occurrence, relative)
                for occurrence in range(repetitions)
                for relative in relatives
            )
            selected = [
                item
                for item in raw_jobs
                if item["client"] == client and item["turn"] == turn
            ]
            observed = Counter(
                (item["occurrence"], item["relative"]) for item in selected
            )
            indexes = Counter(item["index"] for item in selected)
            if observed != expected or indexes != Counter(range(1, len(expected) + 1)):
                incomplete.append(f"{client}:{turn}:authenticated-tu-multiset")
    extras = {(item["client"], item["turn"]) for item in raw_jobs} - {
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


def _nearest_rank(values: list[int], percentile: int) -> int:
    ordered = sorted(values)
    index = max(0, (len(ordered) * percentile + 99) // 100 - 1)
    return ordered[index]


def _validate_kill_interval(
    event: Mapping[str, Any], plan: Mapping[str, Any], evidence: Path | None,
) -> None:
    receipt = event.get("receipt")
    target = next((item for item in plan["topology"]["instances"]
                   if item["name"] == event["instance"]), None)
    if (
        not isinstance(receipt, Mapping)
        or set(receipt) != {"schema", "instance", "host", "container_id",
                            "container_name", "started_ms", "completed_ms", "signal"}
        or target is None
        or receipt.get("schema") != "icefarm-kill-interval-v1"
        or receipt.get("instance") != event["instance"]
        or receipt.get("host") != target["host"]
        or receipt.get("container_name") != f"/icefarm-{plan['run_id']}-{event['instance']}"
        or not isinstance(receipt.get("container_id"), str)
        or SHA256_RE.fullmatch(receipt["container_id"]) is None
        or receipt.get("signal") != "KILL"
        or type(receipt.get("started_ms")) is not int
        or type(receipt.get("completed_ms")) is not int
        or not 0 <= receipt["started_ms"] <= receipt["completed_ms"] <= event["fired_ms"]
    ):
        raise CollectError("invalid authenticated kill operation interval")
    if evidence is not None:
        document = _read_json(
            evidence / "diagnostics" / target["host"] / f"{event['instance']}.inspect"
        )
        if (
            document.get("Id") != receipt["container_id"]
            or document.get("Name") != receipt["container_name"]
            or document.get("Config", {}).get("Labels", {}).get("icefarm.run") != plan["run_id"]
            or document.get("Config", {}).get("Labels", {}).get("icefarm.instance") != event["instance"]
        ):
            raise CollectError("kill interval disagrees with retained container identity")


def _kill_overlaps_assignment(
    event: Mapping[str, Any], raw: Mapping[str, Any], record: Mapping[str, Any],
) -> bool:
    if event.get("action") != "kill -9" or event.get("instance") != record["worker"]:
        return False
    receipt = event.get("receipt", {})
    start = receipt.get("started_ms", event["fired_ms"])
    end = receipt.get("completed_ms", event["fired_ms"])
    # The receipt was validated against the plan and retained container before
    # this join. Scheduler timestamps have only whole-second precision.
    return max(start, raw["started"], record["dispatch_ms"]) <= min(
        end, raw["finished"], record["terminal_ms"] + 999
    )


def _result_stream_kill_loss(
    missing: Mapping[str, Any],
    raw: Mapping[str, Any],
    record: Mapping[str, Any],
    events: list[dict[str, Any]],
) -> bool:
    """Join an already-authenticated EOF to this assignment's declared kill.

    Scheduler STOP timestamps have whole-second precision. Only that final
    fractional second is admitted; the exact wrapper interval still bounds
    the kill. This collects a loss, never supplies a successful result identity.
    """
    if (
        missing.get("reason") != "result-stream-loss"
        or missing.get("result_identity_present") is not False
        or missing.get("row_job_id") != raw.get("row_job_id")
        or missing.get("scheduler_job") != record.get("scheduler_job")
        or missing.get("worker") != record.get("worker")
        or record.get("terminal") != "process-loss-recovery"
    ):
        return False
    matches = [event for event in events if _kill_overlaps_assignment(event, raw, record)]
    return len(matches) == 1


def _validate_orphan_recovery_markers(
    raw_jobs: list[dict[str, Any]], events: list[dict[str, Any]]
) -> None:
    """Admit a stale P50 marker only when a killed assignment was retried legacy."""

    kill_events: dict[str, list[dict[str, Any]]] = defaultdict(list)
    restart_losses: set[tuple[str, int]] = set()
    for event in events:
        if event.get("action") == "kill -9":
            kill_events[event["instance"]].append(event)
        receipt = event.get("receipt")
        coordination = (
            receipt.get("coordination") if isinstance(receipt, Mapping) else None
        )
        rejoin = (
            coordination.get("scheduler_rejoin")
            if isinstance(coordination, Mapping)
            else None
        )
        if (
            event.get("action") == "restart"
            and isinstance(receipt, Mapping)
            and receipt.get("schema") == WORKER_RESTART_SCHEMA
            and isinstance(event.get("instance"), str)
            and isinstance(rejoin, Mapping)
            and isinstance(rejoin.get("loss_job_ids"), list)
        ):
            restart_losses.update(
                (event["instance"], item)
                for item in rejoin["loss_job_ids"]
                if type(item) is int and item > 0
            )
    for raw in raw_jobs:
        marker = raw.get("orphan_recovery_marker")
        if marker is None:
            continue
        claims = raw["assignment_claims"]
        marker_line = marker["line"]
        preceding = [claim for claim in claims if claim["line"] < marker_line]
        following = [claim for claim in claims if claim["line"] > marker_line]
        owner = preceding[-1] if preceding else None
        final = claims[-1]
        matching_kills = (
            [
                event
                for event in kill_events.get(owner["worker"], [])
                if (
                    _kill_overlaps_assignment(event, raw, owner["scheduler_record"])
                    if "receipt" in event
                    else raw["started"] <= event["fired_ms"] <= raw["finished"]
                )
            ]
            if owner is not None
            else []
        )
        matching_restart = (
            owner is not None
            and (owner.get("worker"), owner.get("scheduler_job")) in restart_losses
        )
        authenticated = (
            raw["retries"] >= 1
            and len(claims) >= 2
            and owner is not None
            and owner is not final
            and bool(following)
            and owner["scheduler_record"]["terminal"]
            == "process-loss-recovery"
            and final["scheduler_record"]["terminal"] == "completion"
            and final["worker"] != owner["worker"]
            and (bool(matching_kills) or matching_restart)
        )
        if not authenticated:
            raise CollectError(
                f"{raw['row_job_id']}: P50 commit marker has no full-identity "
                "source-result witness or authenticated killed-assignment retry"
            )


def _network_shaping_observation(
    scenario: ScenarioSpec, plan: Mapping[str, Any], evidence: Path
) -> dict[str, Any] | None:
    requests = scenario.data.get("network", {}).get("shaping")
    if not requests:
        return None
    lifecycle = _read_json(evidence / "receipts" / "lifecycle.json")
    try:
        return validate_netem_receipt(
            scenario.data, plan, lifecycle.get("network_shaping")
        )
    except NetemPlanError as exc:
        raise CollectError(f"network shaping evidence is invalid: {exc}") from exc


def _observations(
    farm: FarmSpec,
    scenario: ScenarioSpec,
    plan: dict[str, Any],
    evidence: Path,
    rows: list[dict[str, Any]],
    row_facts: dict[str, Any],
    events: list[dict[str, Any]],
) -> dict[str, Any]:
    active_loss_jobs = {
        event["receipt"]["lost_scheduler_job"]
        for event in events
        if event.get("action") == "scheduler-loss-active"
        and isinstance(event.get("receipt"), Mapping)
        and type(event["receipt"].get("lost_scheduler_job")) is int
    }
    active_loss_generations = {
        event["receipt"]["lost_scheduler_generation"]
        for event in events
        if event.get("action") == "scheduler-loss-active"
        and isinstance(event.get("receipt"), Mapping)
        and type(event["receipt"].get("lost_scheduler_generation")) is int
    }
    logins, revisions, cache_ports = _parse_logins(evidence, plan)
    topology = plan["topology"]["instances"]
    by_name = {item["name"]: item for item in topology}
    schedulers = [item for item in topology if item.get("role") == "S"]
    if len(schedulers) != 1 or not isinstance(schedulers[0].get("name"), str):
        raise CollectError("topology must resolve exactly one scheduler")
    scheduler_name = schedulers[0]["name"]
    epoch_contract = plan.get("scheduler_dispatch_epoch_contract")
    if (
        epoch_contract is not None
        and epoch_contract != SCHEDULER_DISPATCH_EPOCH_CONTRACT
    ):
        raise CollectError("plan has an unknown scheduler dispatch epoch contract")
    worker_epoch_contract = plan.get("worker_rejoin_epoch_contract")
    worker_boundaries = None
    if worker_epoch_contract is not None:
        if worker_epoch_contract != WORKER_REJOIN_EPOCH_CONTRACT:
            raise CollectError("plan has an unknown worker rejoin epoch contract")
        worker_boundaries = _worker_rejoin_boundaries(farm, scenario, plan, evidence, events)
    client_epoch_contract = plan.get("client_route_epoch_contract")
    if client_epoch_contract is not None:
        if (client_epoch_contract != CLIENT_ROUTE_EPOCH_CONTRACT
                or scenario.data.get("expect", {}).get("engagement") != "s70-b4-client-route-restart"
                or len(events) != 1):
            raise CollectError("invalid client route epoch contract or timeline")
        _validate_client_route_restart_receipt(
            events[0].get("receipt"), events[0], scenario, 0,
            farm=farm, plan=plan, evidence=evidence,
        )
    p29_interner_faults = _p29_interner_faults(evidence, topology)
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
    assignment_claims = row_facts.pop("assignment_claims")
    canary_claims = _canary_assignment_claims(scenario, plan, evidence)
    reconciliation = _reconcile_scheduler_dispatches(
        evidence,
        plan,
        [*canary_claims, *assignment_claims],
        allow_unterminated_job_ids=active_loss_jobs or None,
        allow_unterminated_generations=active_loss_generations or None,
        unclaimed_requests=[
            *row_facts["failed_unassigned_p50_requests"]["records"],
            *row_facts["abandoned_p50_retry_requests"]["records"],
        ],
    )
    _validate_preexposure_redispatches(
        scenario,
        plan,
        events,
        reconciliation.get("preexposure_redispatches"),
    )
    if active_loss_jobs:
        if len(active_loss_jobs) != 1:
            raise CollectError("active scheduler loss has multiple lost-job boundaries")
        lost_job = next(iter(active_loss_jobs))
        affected = [
            raw
            for raw in raw_jobs
            if raw["assignment_claims"]
            and raw["assignment_claims"][0]["scheduler_job"] == lost_job
            and raw["assignment_claims"][0]
            .get("scheduler_record", {})
            .get("generation")
            == next(iter(active_loss_generations), None)
        ]
        if len(affected) != 1 or affected[0]["retries"] != 1:
            raise CollectError(
                "active scheduler loss does not bind exactly one fresh retry"
            )
        first = affected[0]["assignment_claims"][0]
        first_record = first.get("scheduler_record", {})
        active_event = next(
            event for event in events if event.get("action") == "scheduler-loss-active"
        )
        active_receipt = active_event["receipt"]
        if (
            first_record.get("terminal") != "scheduler-loss"
            or first_record.get("generation")
            != active_receipt.get("lost_scheduler_generation")
            or first_record.get("scheduler_job")
            != active_receipt.get("lost_scheduler_job")
        ):
            raise CollectError(
                "active scheduler loss lacks its explicit scheduler boundary"
            )
        if any(raw["retries"] != 0 for raw in raw_jobs if raw is not affected[0]):
            raise CollectError("active scheduler loss permits more than one retry")
    assignment_preference = _assignment_preference(
        scenario, plan, [*canary_claims, *assignment_claims]
    )
    _validate_orphan_recovery_markers(raw_jobs, events)
    control_observations = _control_observations(
        scenario, evidence, raw_jobs, events, farm=farm, plan=plan
    )
    lifecycle = []
    assignment_lifecycle = []
    successful_strict_p50_retry_bindings = []
    successful_strict_p50_late_result_bindings = []
    row_by_identity = {row["job_id"]: row for row in rows}
    for raw in raw_jobs:
        row = row_by_identity[raw["row_job_id"]]
        attempts = raw["assignment_claims"]
        if not attempts:
            if raw.get("unassigned_p50_failure") is None:
                raise CollectError(
                    f"{row['job_id']}: assignment lifecycle is unexpectedly empty"
                )
            lifecycle.append(
                {
                    "deadline_ms": raw["started"]
                    + scenario.data["timeouts"]["turn_s"] * 1000,
                    "client_instance": row["client_instance"],
                    "dispatch_ms": raw["started"],
                    "final_dispatch_ms": raw["started"],
                    "first_dispatch_ms": raw["started"],
                    "job_id": row["job_id"],
                    "scheduler_dispatch_line": 0,
                    "scheduler_generation": 0,
                    "terminal": "unassigned-p50-failure",
                    "terminal_ms": raw["finished"],
                    "turn": raw["turn"],
                }
            )
            assignment_lifecycle.append({"attempts": [], "job_id": row["job_id"]})
            continue
        records = [item["scheduler_record"] for item in attempts]
        missing_result_identities = raw.get("missing_compile_result_identities", [])
        source_transfer_failures = raw.get("failed_p50_source_transfers", [])
        uncommitted_transport_failures = raw.get(
            "failed_p50_uncommitted_transports", []
        )
        for missing in missing_result_identities:
            attempt_index = missing["attempt_index"]
            if not 0 <= attempt_index < len(records):
                raise CollectError(
                    f"{row['job_id']}: missing result identity names an invalid attempt"
                )
            record = records[attempt_index]
            terminal = record["terminal"]
            expected_terminals = (
                {"process-loss-recovery"}
                if missing["reason"] == "worker-restart-loss"
                else {"cancellation", "completion"}
            )
            exact_active_scheduler_loss = (
                missing["reason"] == "result-stream-loss"
                and terminal == "scheduler-loss"
                and record.get("scheduler_job") in active_loss_jobs
                and record.get("generation") in active_loss_generations
            )
            exact_killed_worker_loss = _result_stream_kill_loss(
                missing, raw, record, events
            )
            if (
                terminal not in expected_terminals
                and not exact_active_scheduler_loss
                and not exact_killed_worker_loss
            ):
                raise CollectError(
                    f"{row['job_id']}: missing result identity loss witness "
                    "disagrees with scheduler terminal"
                )
        for failure in source_transfer_failures:
            attempt_index = failure["attempt_index"]
            if not 0 <= attempt_index < len(records):
                raise CollectError(
                    f"{row['job_id']}: source-transfer loss names an invalid attempt"
                )
            if records[attempt_index]["terminal"] not in {
                "cancellation",
                "process-loss-recovery",
            }:
                raise CollectError(
                    f"{row['job_id']}: source-transfer loss disagrees with the "
                    "scheduler terminal"
                )
        for failure in uncommitted_transport_failures:
            attempt_index = failure["attempt_index"]
            if not 0 <= attempt_index < len(records):
                raise CollectError(
                    f"{row['job_id']}: uncommitted transport loss names an invalid attempt"
                )
            if records[attempt_index]["terminal"] not in {
                "cancellation",
                "process-loss-recovery",
            }:
                raise CollectError(
                    f"{row['job_id']}: uncommitted transport loss disagrees with the "
                    "scheduler terminal"
                )
        if any(
            current["dispatch_line"] >= following["dispatch_line"]
            for current, following in zip(records, records[1:])
        ):
            raise CollectError(f"{row['job_id']}: retry dispatch order is inconsistent")
        first = records[0]
        final = records[-1]
        resource_failure = raw.get("resource_failure")
        if resource_failure is not None:
            if (
                len(records) != 2
                or first["terminal"] != "cancellation"
                or final["terminal"] != "completion"
                or first["worker"] != resource_failure["first_worker"]
                or final["worker"] != resource_failure["final_worker"]
                or first["scheduler_job"] != resource_failure["first_identity"]["scheduler_job"]
                or final["scheduler_job"] != resource_failure["final_identity"]["scheduler_job"]
                or first["terminal_ms"] > final["dispatch_ms"]
                or raw["compile_rc"] != 0
                or raw["remote"] != 1
                or raw["local_build"]
                or row["exact"] is not True
                or row["retries"] != 1
                or row["session_outcome"] != "committed"
                or row["tail_profile"] != "P29V1"
            ):
                raise CollectError(f"{row['job_id']}: resource retry lifecycle is inconsistent")
            resource_failure.update(
                first_generation=first["generation"],
                final_generation=final["generation"],
                first_dispatch_ms=first["dispatch_ms"],
                first_terminal_ms=first["terminal_ms"],
                final_dispatch_ms=final["dispatch_ms"],
                final_terminal_ms=final["terminal_ms"],
            )
        local_fallback_completion = (
            raw["compile_rc"] == 0
            and raw["remote"] == 0
            and raw["local_build"]
            and final["terminal"] == "cancellation"
        )
        failed_result_stream_completion = any(
            missing["attempt_index"] == len(attempts) - 1
            and missing["reason"] == "result-stream-loss"
            for missing in missing_result_identities
        )
        if (
            not failed_result_stream_completion
            and isinstance(raw.get("abandoned_p50_retry_request"), Mapping)
            and raw["abandoned_p50_retry_request"].get("failure_reason")
            == "result-stream-loss"
            and raw["compile_rc"] != 0
            and raw["remote"] == 1
            and not raw["local_build"]
            and final["terminal"] == "completion"
        ):
            # CompileResult may be authenticated while the following object
            # stream is lost (for example Error 19).  The source/result
            # identities and scheduler completion remain real, but the row
            # is still a compile/exactness failure.  Its exact normalized
            # Error106 + strict retry/no-handoff record was authenticated
            # before lifecycle reconciliation.
            failed_result_stream_completion = True
        strict_p50_late_result_binding = _successful_strict_p50_late_result_binding(
            scenario, row, raw, records, events
        )
        if (
            (raw["compile_rc"] == 0) != (final["terminal"] == "completion")
            and not local_fallback_completion
            and not failed_result_stream_completion
            and strict_p50_late_result_binding is None
        ):
            raise CollectError(
                f"{row['job_id']}: wrapper status disagrees with scheduler terminal"
            )
        if epoch_contract == SCHEDULER_DISPATCH_EPOCH_CONTRACT:
            event_epoch = _scheduler_dispatch_epoch(
                events,
                scheduler_name,
                final["dispatch_ms"],
                final["generation"],
            )
        elif (
            scenario.data.get("expect", {}).get("engagement")
            == "s70-b6-drained-kill-switch-cycle"
        ):
            event_epoch = final["generation"] - 1
        else:
            event_epoch = _epoch_at(events, final["dispatch_ms"])
        if worker_boundaries is not None and row["cs"] == "F1":
            if final["generation"] != 1 or type(final["dispatch_line"]) is not int:
                raise CollectError("worker rejoin dispatch has invalid scheduler identity")
            event_epoch = sum(
                boundary["scheduler_rejoin_line"] < final["dispatch_line"]
                for boundary in worker_boundaries
            )
        if client_epoch_contract is not None:
            try:
                event_epoch = client_route_epoch(
                    events[0], row["client_instance"], raw["started"],
                    raw["finished"], final["dispatch_ms"],
                )
            except ValueError as exc:
                raise CollectError(f"{row['job_id']}: {exc}") from exc
        row["event_epoch"] = event_epoch
        if epoch_contract == SCHEDULER_DISPATCH_EPOCH_CONTRACT:
            row["client_version"] = _planned_instance_version_at_epoch(
                scenario, by_name[row["client_instance"]], event_epoch
            )
            row["cs_version"] = _planned_instance_version_at_epoch(
                scenario, by_name[row["cs"]], event_epoch
            )
        else:
            row["client_version"] = _instance_version_at(
                by_name[row["client_instance"]], events, final["dispatch_ms"]
            )
            row["cs_version"] = _instance_version_at(
                by_name[row["cs"]], events, final["dispatch_ms"]
            )
        lifecycle.append(
            {
                "deadline_ms": first["dispatch_ms"]
                + scenario.data["timeouts"]["turn_s"] * 1000,
                "client_instance": row["client_instance"],
                "dispatch_ms": first["dispatch_ms"],
                "final_dispatch_ms": final["dispatch_ms"],
                "first_dispatch_ms": first["dispatch_ms"],
                **({"wrapper_started_ms": raw["started"],
                    "wrapper_finished_ms": raw["finished"]}
                   if client_epoch_contract is not None else {}),
                "job_id": row["job_id"],
                "scheduler_dispatch_line": final["dispatch_line"],
                "scheduler_generation": final["generation"],
                "terminal": final["terminal"],
                "terminal_ms": final["terminal_ms"],
                "turn": raw["turn"],
            }
        )
        assignment_lifecycle.append(
            {
                "attempts": [
                    {
                        "generation": attempt["scheduler_record"]["generation"],
                        "scheduler_job": attempt["scheduler_record"]["scheduler_job"],
                        "terminal": attempt["scheduler_record"]["terminal"],
                        "worker": attempt["worker"],
                    }
                    for attempt in attempts
                ],
                "job_id": row["job_id"],
            }
        )
        retry_failure_reason = (
            missing_result_identities[0]["reason"]
            if len(missing_result_identities) == 1
            and missing_result_identities[0]["attempt_index"] == 0
            and not source_transfer_failures
            and not uncommitted_transport_failures
            else "source-transfer-loss"
            if not missing_result_identities
            and len(source_transfer_failures) == 1
            and source_transfer_failures[0]["attempt_index"] == 0
            and first["terminal"] == "cancellation"
            and not uncommitted_transport_failures
            else "uncommitted-transport-loss"
            if not missing_result_identities
            and not source_transfer_failures
            and len(uncommitted_transport_failures) == 1
            and uncommitted_transport_failures[0]["attempt_index"] == 0
            and first["terminal"] in {"cancellation", "process-loss-recovery"}
            else "worker-restart-loss"
            if not missing_result_identities
            and first["terminal"] == "process-loss-recovery"
            else None
        )
        b5_zstd_tu_retry = (
            scenario.data.get("expect", {}).get("engagement") == S70_B5_ENGAGEMENT
            and retry_failure_reason == "source-transfer-loss"
            and len(source_transfer_failures) == 1
            and source_transfer_failures[0].get("error") == 0x5001
        )
        if (
            raw["compile_rc"] == 0
            and raw["exact"] == 1
            and raw["remote"] == 1
            and raw["local_build"] is False
            and row["exact"] is True
            and row["retries"] == 1
            and row["tail_present"] is True
            and row["tail_profile"] == ("ZSTD_TU" if b5_zstd_tu_retry else "P29V1")
            and row["session_outcome"] == "committed"
            and len(records) == 2
            and final["terminal"] == "completion"
            and retry_failure_reason is not None
            and not any(
                failure.get("retry_mode") == "legacy"
                for failure in source_transfer_failures
            )
            and (
                retry_failure_reason
                not in {"source-transfer-loss", "uncommitted-transport-loss"}
                or first["worker"] != final["worker"]
                or (
                    len(source_transfer_failures + uncommitted_transport_failures) == 1
                    and same_endpoint_decision_valid(
                        (source_transfer_failures + uncommitted_transport_failures)[0],
                        {
                            "final_generation": final["generation"],
                            "final_scheduler_job": final["scheduler_job"],
                            "final_dispatch_ms": final["dispatch_ms"],
                            "first_worker": first["worker"],
                            "final_worker": final["worker"],
                        },
                    )
                )
            )
        ):
            successful_strict_p50_retry_bindings.append(
                {
                    "failure_reason": retry_failure_reason,
                    "final_dispatch_ms": final["dispatch_ms"],
                    "final_generation": final["generation"],
                    "final_scheduler_job": final["scheduler_job"],
                    "final_terminal_ms": final["terminal_ms"],
                    "final_worker": final["worker"],
                    "first_dispatch_ms": first["dispatch_ms"],
                    "first_generation": first["generation"],
                    "first_scheduler_job": first["scheduler_job"],
                    "first_terminal": first["terminal"],
                    "first_terminal_ms": first["terminal_ms"],
                    "first_worker": first["worker"],
                    "job_id": row["job_id"],
                }
            )
        if strict_p50_late_result_binding is not None:
            successful_strict_p50_late_result_bindings.append(
                strict_p50_late_result_binding
            )
    source_mutex = row_facts["source_mutex"]
    if scenario.data.get("id") == "S30-mutant-f-refusal":
        refusal = row_facts.get("s30_mutant_f")
        fallback_rows = [row for row in rows if row["session_outcome"] == "fallback"]
        direct_legacy_rows = [row for row in rows if row["session_outcome"] == "none"]
        refusal_count = (
            refusal.get("refusal_count") if isinstance(refusal, Mapping) else None
        )
        if (
            not fallback_rows
            or not isinstance(refusal_count, int)
            or refusal_count < 1
            or refusal_count > len(fallback_rows)
            or len(fallback_rows) + len(direct_legacy_rows) != len(rows)
            or any(
                row["retries"] != 1
                or row["tail_present"]
                or row["tail_profile"] is not None
                for row in fallback_rows
            )
            or any(
                row["retries"] != 0
                or row["tail_present"]
                or row["tail_profile"] is not None
                for row in direct_legacy_rows
            )
        ):
            raise CollectError(
                "S30 mutant rows do not prove bounded P50 refusal recovery"
            )
        if row_facts.get("local_fallback_job_ids"):
            raise CollectError("S30 mutant has a local fallback")
        for raw in raw_jobs:
            attempts = raw["assignment_claims"]
            row = row_by_identity[raw["row_job_id"]]
            if row["session_outcome"] == "fallback":
                if len(attempts) != 2:
                    raise CollectError(
                        "S30 affected workload did not make exactly one fresh retry"
                    )
                first, final = attempts
                if first["scheduler_record"].get("terminal") == "completion":
                    raise CollectError(
                        "S30 first P50 assignment has no refusal terminal"
                    )
                if final["scheduler_record"].get("terminal") != "completion":
                    raise CollectError(
                        "S30 fresh legacy assignment lacks a completion terminal"
                    )
            elif (
                len(attempts) != 1
                or attempts[0]["scheduler_record"].get("terminal") != "completion"
            ):
                raise CollectError(
                    "S30 direct legacy workload is not one completed assignment"
                )
        row_facts["s30_mutant_f"]["fallback_job_ids"] = sorted(
            row["job_id"] for row in fallback_rows
        )
        row_facts["s30_mutant_f"]["fresh_legacy_assignment_count"] = len(fallback_rows)
        row_facts["s30_mutant_f"]["local_fallback_job_ids"] = []
    turn_observations: dict[str, dict[str, Any]] = {}
    client_turn_observations: dict[str, dict[str, dict[str, int]]] = {}
    for turn in scenario.data["workload"]["turns"]:
        turn_lifecycle = [item for item in lifecycle if item["turn"] == turn]
        turn_raw = [item for item in raw_jobs if item["turn"] == turn]
        turn_rows = [row_by_identity[item["row_job_id"]] for item in turn_raw]
        if not turn_lifecycle or not turn_raw or len(turn_lifecycle) != len(turn_rows):
            raise CollectError(f"turn {turn!r} has incomplete timing evidence")
        terminal_times = [item["terminal_ms"] for item in turn_lifecycle]
        if any(type(value) is not int for value in terminal_times):
            raise CollectError(f"turn {turn!r} has no terminal timestamp")
        turn_mutex = [item for item in source_mutex["records"] if item["turn"] == turn]
        first_dispatch_ms = min(item["dispatch_ms"] for item in turn_lifecycle)
        last_terminal_ms = max(terminal_times)
        job_walls = [row["wall_ms"] for row in turn_rows]
        turn_observations[turn] = {
            "c_to_f_bytes": sum(row["c_to_f_bytes"] for row in turn_rows),
            "exact_objects": sum(row["exact"] is True for row in turn_rows),
            "f_to_c_bytes": sum(row["f_to_c_bytes"] for row in turn_rows),
            "first_dispatch_ms": first_dispatch_ms,
            "jobs": len(turn_rows),
            "job_wall_p95_ms": _nearest_rank(job_walls, 95),
            "job_wall_p99_ms": _nearest_rank(job_walls, 99),
            "last_terminal_ms": last_terminal_ms,
            "source_mutex_records": len(turn_mutex),
            "source_mutex_service_ns": sum(item["service_ns"] for item in turn_mutex),
            "source_mutex_wait_max_ns": max(
                (item["wait_ns"] for item in turn_mutex), default=0
            ),
            "source_mutex_wait_ns": sum(item["wait_ns"] for item in turn_mutex),
            "wall_ms": last_terminal_ms - first_dispatch_ms,
            "wrapper_wall_ms": max(item["finished"] for item in turn_raw)
            - min(item["started"] for item in turn_raw),
        }
        client_turn_observations[turn] = {}
        for client in scenario.data["workload"]["clients"]:
            client_lifecycle = [
                item for item in turn_lifecycle if item["client_instance"] == client
            ]
            client_raw = [item for item in turn_raw if item["client"] == client]
            if not client_lifecycle or len(client_lifecycle) != len(client_raw):
                raise CollectError(
                    f"turn {turn!r} has incomplete timing for client {client!r}"
                )
            client_turn_observations[turn][client] = {
                "exact_objects": sum(
                    row_by_identity[item["row_job_id"]]["exact"] for item in client_raw
                ),
                "jobs": len(client_raw),
                "wall_ms": max(item["terminal_ms"] for item in client_lifecycle)
                - min(item["dispatch_ms"] for item in client_lifecycle),
            }
    starts = [item["started"] for item in raw_jobs]
    finishes = [item["finished"] for item in raw_jobs]
    preflight = _read_json(evidence / "receipts" / "preflight.json")
    network_shaping = _network_shaping_observation(scenario, plan, evidence)
    f_init = _f_init_observation(plan, evidence)
    return {
        "assignment_preference": assignment_preference,
        "assignment_lifecycle": assignment_lifecycle,
        "cell_wall_ms": max(finishes) - min(starts),
        "client_turns": client_turn_observations,
        **row_facts,
        **control_observations,
        "incomplete_turns": _turn_completeness(scenario, plan, evidence, raw_jobs),
        "f_init": f_init,
        "job_lifecycle": lifecycle,
        **({"worker_rejoin_boundaries": worker_boundaries}
           if worker_boundaries is not None else {}),
        "logins": logins,
        **({"network_shaping": network_shaping} if network_shaping is not None else {}),
        "oracle": _oracle(evidence, scenario),
        "p29_interner_faults": p29_interner_faults,
        "protected_before": {
            host: facts.get("protected", {})
            for host, facts in preflight["hosts"].items()
        },
        "scheduler_reconciliation": reconciliation,
        "sidecars": sidecars,
        "successful_strict_p50_retry_bindings": sorted(
            successful_strict_p50_retry_bindings,
            key=lambda item: item["job_id"],
        ),
        "successful_strict_p50_late_result_bindings": sorted(
            successful_strict_p50_late_result_bindings,
            key=lambda item: item["job_id"],
        ),
        "turns": turn_observations,
        "warm_hint_overrides": _warm_hint_overrides(evidence, plan),
        "wire_revisions": revisions,
    }


def _control_observations(
    scenario: ScenarioSpec,
    evidence: Path,
    raw_jobs: list[dict[str, Any]],
    events: list[dict[str, Any]],
    *,
    farm: FarmSpec | None = None,
    plan: Mapping[str, Any] | None = None,
) -> dict[str, Any]:
    """Derive control facts only from resolved settings and scheduler evidence."""

    controls = set(scenario.data["controls"])
    fault: dict[str, Any] = {}
    if "H2" in controls:
        clients = set(scenario.data["workload"]["clients"])
        fault["client_kill_switch"] = any(
            instance["role"] == "C"
            and instance["name"] in clients
            and instance.get("env", {}).get("ICECC_P50_MODE") == "off"
            for instance in scenario.data["instances"]
        )

    object_corruption: dict[str, Any] | None = None
    if "H4" in controls:
        descriptor = scenario.data.get("fault", {})
        client = descriptor.get("client")
        target = descriptor.get("job")
        path = (
            _instance_results(evidence, client)
            / "workload"
            / scenario.data["workload"]["turns"][0]
            / "fault"
            / "h4.tsv"
        )
        if path.is_file() and not path.is_symlink():
            try:
                lines = path.read_text(encoding="ascii").splitlines()
            except (OSError, UnicodeError) as exc:
                raise CollectError(f"cannot read H4 mutation receipt: {exc}") from exc
            if len(lines) != 1:
                raise CollectError("H4 mutation receipt must contain exactly one row")
            fields = lines[0].split("\t")
            if len(fields) != 6 or fields[0] != "icefarm-h4-object-fault-v1":
                raise CollectError("H4 mutation receipt schema is invalid")
            _schema, observed_client, index_text, before, after, relative = fields
            if (
                observed_client != client
                or not index_text.isdigit()
                or int(index_text) != target
                or SHA256_RE.fullmatch(before) is None
                or SHA256_RE.fullmatch(after) is None
                or before == after
                or relative != f"jobs/{target:06d}/remote.o"
            ):
                raise CollectError("H4 mutation receipt identity is invalid")
            matches = [
                raw
                for raw in raw_jobs
                if raw["client"] == client
                and raw["index"] == target
                and raw["turn"] == scenario.data["workload"]["turns"][0]
            ]
            if len(matches) != 1:
                raise CollectError("H4 mutation does not bind one workload job")
            raw = matches[0]
            if (
                raw["compile_rc"] != 0
                or raw["local_sha"] != before
                or raw["remote_sha"] != after
                or raw["exact"] != 0
            ):
                raise CollectError("H4 mutation digests differ from the workload row")
            object_corruption = {
                "after_sha256": after,
                "before_sha256": before,
                "client": client,
                "job": target,
                "row_job_id": raw["row_job_id"],
            }
        fault["corrupt_object"] = object_corruption is not None

    killed_workers = {
        event["instance"] for event in events if event.get("action") == "kill -9"
    }
    recovered: set[str] = set()
    killed_recovered: set[str] = set()
    recovery_bindings: list[dict[str, Any]] = []
    for raw in raw_jobs:
        for attempt_index, attempt in enumerate(raw["assignment_claims"]):
            record = attempt["scheduler_record"]
            if record["terminal"] != "process-loss-recovery":
                continue
            recovered.add(raw["row_job_id"])
            recovery_bindings.append(
                {
                    "attempt_index": attempt_index,
                    "job_id": raw["row_job_id"],
                    "scheduler_job": record["scheduler_job"],
                    "worker": record["worker"],
                }
            )
            if record["worker"] in killed_workers:
                killed_recovered.add(raw["row_job_id"])
    if "H5" in controls:
        fault["worker_killed_job_ids"] = sorted(killed_recovered)

    return {
        "fault": fault,
        "object_corruption": object_corruption,
        "process_loss_recovery_bindings": recovery_bindings,
        "process_loss_recovery_job_ids": sorted(recovered),
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
    content = "".join(
        f"{digest}  {path}\n" for path, digest in sorted(artifacts.items())
    )
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
    events = _event_log(
        evidence,
        scenario,
        farm=farm,
        plan=plan,
        preflight=receipts["preflight"],
    )
    if scenario.data["controls"] == ["H3"]:
        rows = []
        row_facts = {}
        observations = _h3_control_failure_observations(
            farm, scenario, plan, evidence, receipts, events
        )
    else:
        rows, row_facts = _parse_rows(scenario, plan, evidence, events)
        observations = _observations(
            farm, scenario, plan, evidence, rows, row_facts, events
        )
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
        "launch_contract": plan.get("launch_contract"),
        **({"mode": CONTROL_FAILURE_MODE} if scenario.data["controls"] == ["H3"] else {}),
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


def _h3_control_failure_observations(
    farm: FarmSpec,
    scenario: ScenarioSpec,
    plan: dict[str, Any],
    evidence: Path,
    receipts: Mapping[str, Mapping[str, Any]],
    events: list[dict[str, Any]],
) -> dict[str, Any]:
    """Collect H3's malformed-frame failure without parsing fake product rows."""

    if events:
        raise CollectError("H3 control failure does not permit timeline mutations")
    workload = receipts["workload"]
    summaries = workload.get("clients")
    if (
        workload.get("status") != "COMPLETE_WITH_JOB_FAILURES"
        or not isinstance(summaries, list)
        or len(summaries) != 1
        or type(summaries[0].get("jobs")) is not int
        or summaries[0]["jobs"] <= 0
        or type(summaries[0].get("failures")) is not int
        or summaries[0]["failures"] <= 0
        or summaries[0]["failures"] != summaries[0]["jobs"]
    ):
        raise CollectError("H3 workload receipt does not prove started failed work")

    topology = plan["topology"]["instances"]
    clients = [item for item in topology if item["role"] == "C"]
    workers = [item for item in topology if item["role"] == "F"]
    selected_clients = {
        item["name"]
        for item in clients
        if item["name"] in scenario.data["workload"]["clients"]
        and item["version"] < 50
    }
    selected_workers = {
        item["name"] for item in workers if item["version"] == 50
    }
    if selected_clients != set(scenario.data["workload"]["clients"]):
        raise CollectError("H3 control failure lacks an old selected client")
    if not selected_workers:
        raise CollectError("H3 control failure lacks a current P50 worker")
    scheduler = next(item for item in topology if item["role"] == "S")
    authority_image = farm.data["authority"]["images"].get(
        scheduler["image"]["label"]
    )
    if not isinstance(authority_image, Mapping) or authority_image.get("kind") != "scheduler-mutant":
        raise CollectError("H3 scheduler image is not an authority-bound mutant")
    trace_path = _instance_results(evidence, scheduler["name"]) / Path(
        MUTANT_TRACE_PATH
    ).relative_to("/results")
    trace_records = _read_jsonl(trace_path, required=True)
    scheduler_jobs = _scheduler_jobs(evidence, plan)
    requires_arm = scheduler_mutant_requires_arming(authority_image)
    if requires_arm or "h3_arm_contract" in plan:
        if not requires_arm or plan.get("h3_arm_contract") != MUTANT_ARM_CONTRACT:
            raise CollectError("H3 arming contract is not bound to successor recipe")
        marker_path = _instance_results(evidence, scheduler["name"]) / Path(
            MUTANT_ARM_PATH
        ).name
        log_path = _one_role_log(evidence, scheduler)
        if marker_path.is_symlink() or not marker_path.is_file() or log_path is None:
            raise CollectError("H3 retained arming evidence is missing")
        try:
            scheduler_jobs = h3_workload_dispatches(
                _read_json(marker_path), receipts.get("lifecycle", {}).get("h3_arm"),
                log_path.read_bytes(), scheduler_jobs,
                _canary_assignment_claims(scenario, plan, evidence),
                run_id=plan["run_id"], scheduler=scheduler["name"],
            )
        except (MutantError, KeyError, TypeError, ValueError) as exc:
            raise CollectError(f"invalid H3 arming boundary: {exc}") from exc
    if any(
        job["client"] not in selected_clients or job["worker"] not in selected_workers
        for job in scheduler_jobs
    ):
        raise CollectError("H3 scheduler dispatch is not old-client/current-worker bound")
    try:
        emission = validate_h3_trace(
            trace_records,
            client_instances=selected_clients,
            scheduler_instance=scheduler["name"],
            jobs=scheduler_jobs,
            worker_instances=selected_workers,
        )
        client = next(item for item in clients if item["name"] in selected_clients)
        client_log = _one_role_log(evidence, client)
        rejection_records = parse_h3_client_rejections(
            _text(client_log), client_instance=client["name"]
        )
    except MutantError as exc:
        raise CollectError(f"invalid H3 independent rejection evidence: {exc}") from exc
    if len(rejection_records) != emission["record_count"]:
        raise CollectError("H3 emission/rejection counts do not match")
    if len(scheduler_jobs) != emission["record_count"]:
        raise CollectError("H3 scheduler dispatch/emission counts do not match")
    result_paths = sorted(
        _instance_results(evidence, client["name"])
        .joinpath("workload")
        .glob("*/jobs/*/result.tsv")
    )
    if len(result_paths) != summaries[0]["jobs"]:
        raise CollectError("H3 workload result count is incomplete")
    failed_results = []
    for path in result_paths:
        row = _job_result(path)
        if row["remote"] != 0:
            raise CollectError("H3 control failure contains a successful remote row")
        missing_assignment = (
            row["scheduler_job"].startswith("missing-") and row["worker"] == "UNKNOWN"
        )
        local_assignment = (
            row["scheduler_job"].isdigit()
            and int(row["scheduler_job"]) > 0
            and row["worker"] == "127.0.0.1:0"
        )
        if not (missing_assignment or local_assignment):
            raise CollectError("H3 failure row unexpectedly self-reports an assignment")
        failed_results.append(
            {
                "compile_rc": row["compile_rc"],
                "exact": bool(row["exact"]),
                "index": row["index"],
                "remote": False,
                "scheduler_job": row["scheduler_job"],
                "turn": row["turn"],
                "worker": row["worker"],
            }
        )
    f_init = _f_init_observation(plan, evidence)
    return {
        "f_init": f_init,
        "fault": {"mutant_scheduler": True},
        "h3_control_failure": {
            "authenticated": True,
            "dispatches": scheduler_jobs,
            "emission": emission,
            "failed_jobs": failed_results,
            "rejections": rejection_records,
            "schema": "icefarm-h3-control-failure-v1",
            "scheduler_dispatch_count": len(scheduler_jobs),
            "successful_product_rows": 0,
            "workload_failure_count": summaries[0]["failures"],
            "workload_failed": True,
            "workload_job_count": summaries[0]["jobs"],
            "workload_started": True,
        },
    }


def collect_refusal_bundle(
    farm: FarmSpec,
    scenario: ScenarioSpec,
    plan: dict[str, Any],
) -> dict[str, Any]:
    """Seal an H1 preflight refusal without inventing workload evidence."""

    root = bundle_root(farm, plan["run_id"])
    root.mkdir(parents=True, exist_ok=True)
    if scenario.data.get("controls") != ["H1"]:
        raise CollectError(
            "preflight-refusal bundles are restricted to exact H1 controls"
        )
    if (root / "bundle.json").exists():
        bundle = load_verified_bundle(root)
        if bundle.get("mode") != REFUSAL_MODE:
            raise CollectError("existing immutable bundle is not an H1 refusal")
        return bundle

    refusal = _read_json(root / "preflight-refusal.json")
    lifecycle = _read_json(root / "lifecycle.json")
    expected = {
        "farm_digest": plan["farm_digest"],
        "run_id": plan["run_id"],
        "scenario_digest": plan["scenario_digest"],
        "topology_digest": plan["topology_digest"],
    }
    for name, receipt in (("preflight-refusal", refusal), ("lifecycle", lifecycle)):
        for field, value in expected.items():
            if receipt.get(field) != value:
                raise CollectError(f"{name}.json does not bind {field}")
    if (
        refusal.get("schema") != "icefarm-preflight-refusal-v1"
        or refusal.get("reason_code") != "role-hash-mismatch"
        or refusal.get("jobs_started") != 0
        or refusal.get("persistent_start_attempted") is not False
    ):
        raise CollectError("H1 refusal is not a zero-job role-hash mismatch")
    if lifecycle.get("status") != "FAILED" or lifecycle.get("plan") != plan:
        raise CollectError("H1 lifecycle is not the failed immutable plan")

    workload = {
        **expected,
        "jobs_started": 0,
        "reason": "preflight-refused",
        "schema": "icefarm-workload-v1",
        "status": "NOT_STARTED",
    }
    _atomic_json(root / "workload.json", workload)
    evidence = root / "evidence"
    if evidence.exists():
        raise CollectError("H1 refusal evidence already exists without a bundle")
    temporary = root / f".evidence.tmp-{os.getpid()}"
    if temporary.exists():
        raise CollectError(f"stale collection staging directory exists: {temporary}")
    temporary.mkdir(parents=True)
    try:
        specs = temporary / "specs"
        _atomic_json(specs / "farm.json", farm.data)
        _atomic_json(specs / "scenario.json", scenario.data)
        _atomic_json(specs / "plan.json", plan)
        _atomic_json(specs / "topology.json", plan["topology"])
        receipts = temporary / "receipts"
        _atomic_json(receipts / "preflight-refusal.json", refusal)
        _atomic_json(receipts / "lifecycle.json", lifecycle)
        _atomic_json(receipts / "workload.json", workload)
        observations = {
            "fault": {},
            "job_lifecycle": [],
            "preflight_refusal": refusal,
            "process_loss_recovery_job_ids": [],
        }
        _write_derived(temporary, [], observations, [])
        _validate_regular_tree(temporary)
        os.replace(temporary, evidence)
    except Exception:
        shutil.rmtree(temporary, ignore_errors=True)
        raise

    artifacts = _evidence_artifacts(root)
    sums_sha = _write_checksums(root, artifacts)
    bundle = {
        "artifacts": artifacts,
        "checksum_policy": {
            "root": "evidence/",
            "sha256sums_sha256": sums_sha,
        },
        "event_log": [],
        "farm": farm.data,
        "farm_digest": farm.digest,
        "images": {},
        "instances": plan["topology"]["instances"],
        "launch_contract": plan.get("launch_contract"),
        "mode": REFUSAL_MODE,
        "observations": observations,
        "plan": plan,
        "rows": [],
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


def _verify_h3_raw_replay(bundle, evidence, receipts):
    """Recompute new H3 controls from raw evidence even if derived hashes match."""
    plan = bundle["plan"]
    authority = bundle["farm"].get("authority", {}).get("images", {})
    armed = any(
        item.get("role") == "S" and scheduler_mutant_requires_arming(
            authority.get(item.get("image", {}).get("label"), {})
        ) for item in plan["topology"]["instances"]
    )
    if not armed and "h3_arm_contract" not in plan:
        return
    if (bundle.get("mode") != CONTROL_FAILURE_MODE
            or bundle["scenario"].get("controls") != ["H3"]):
        raise CollectError("armed H3 replay lacks control-failure binding")
    farm = FarmSpec(path=evidence / "specs/farm.json", data=bundle["farm"])
    scenario = ScenarioSpec(path=evidence / "specs/scenario.json", data=bundle["scenario"])
    observed = _h3_control_failure_observations(
        farm, scenario, plan, evidence, receipts, bundle["event_log"],
    )
    if observed != bundle["observations"]:
        raise CollectError("H3 observations differ from retained raw evidence")


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
            item
            for item in set(artifacts) & set(observed)
            if artifacts[item] != observed[item]
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
    if bundle.get("launch_contract") != plan.get("launch_contract"):
        raise CollectError("bundle launch contract differs from its immutable plan")
    if topology.get("topology_digest") != bundle.get("topology_digest"):
        raise CollectError("bundle topology digest differs from its topology")
    if bundle.get("instances") != topology.get("instances"):
        raise CollectError("bundle instances differ from its immutable topology")
    refusal_mode = bundle.get("mode") == REFUSAL_MODE
    control_failure_mode = bundle.get("mode") == CONTROL_FAILURE_MODE
    receipt_bindings = {
        "lifecycle": _read_json(path / "evidence" / "receipts" / "lifecycle.json"),
        "workload": _read_json(path / "evidence" / "receipts" / "workload.json"),
    }
    if refusal_mode:
        receipt_bindings["preflight-refusal"] = _read_json(
            path / "evidence" / "receipts" / "preflight-refusal.json"
        )
    else:
        receipt_bindings["preflight"] = _read_json(
            path / "evidence" / "receipts" / "preflight.json"
        )
    for name, receipt in receipt_bindings.items():
        for field, expected in plan_bindings.items():
            if receipt.get(field) != expected:
                raise CollectError(f"{name} receipt does not bind bundle {field}")
    if receipt_bindings["lifecycle"].get("plan") != plan:
        raise CollectError("lifecycle receipt plan differs from the immutable plan")
    if refusal_mode:
        refusal = receipt_bindings["preflight-refusal"]
        if (
            bundle.get("images") != {}
            or refusal.get("schema") != "icefarm-preflight-refusal-v1"
            or refusal.get("reason_code") != "role-hash-mismatch"
            or refusal.get("jobs_started") != 0
            or refusal.get("persistent_start_attempted") is not False
            or receipt_bindings["lifecycle"].get("status") != "FAILED"
            or receipt_bindings["workload"].get("status") != "NOT_STARTED"
            or receipt_bindings["workload"].get("jobs_started") != 0
            or bundle.get("observations", {}).get("preflight_refusal") != refusal
        ):
            raise CollectError(
                "preflight-refusal bundle is not a bound zero-job H1 refusal"
            )
    elif control_failure_mode:
        if (
            bundle.get("scenario", {}).get("controls") != ["H3"]
            or bundle.get("images") != receipt_bindings["preflight"].get("images")
        ):
            raise CollectError("control-failure bundle is not bound to H3")
    elif bundle.get("mode") is not None:
        raise CollectError("bundle mode is unsupported")
    elif bundle.get("images") != receipt_bindings["preflight"].get("images"):
        raise CollectError("bundle images differ from the immutable preflight receipt")
    if hashlib.sha256(canonical_bytes(bundle["farm"])).hexdigest() != bundle.get(
        "farm_digest"
    ):
        raise CollectError("bundle farm digest is not reproducible")
    if hashlib.sha256(canonical_bytes(bundle["scenario"])).hexdigest() != bundle.get(
        "scenario_digest"
    ):
        raise CollectError("bundle scenario digest is not reproducible")
    rows = _read_jsonl(
        path / "evidence" / "derived" / "rows.jsonl",
        required=not (refusal_mode or control_failure_mode),
    )
    observations = _read_json(path / "evidence" / "derived" / "observations.json")
    events = _read_json(path / "evidence" / "derived" / "events.json").get("events")
    if bundle.get("rows") != rows or bundle.get("observations") != observations:
        raise CollectError("bundle derived values differ from immutable evidence")
    if bundle.get("event_log") != events:
        raise CollectError("bundle event log differs from immutable evidence")
    if plan.get("worker_endpoint_contract") == "icefarm-live-bridge-endpoint-v1":
        scenario = ScenarioSpec(
            path=path / "evidence" / "specs" / "scenario.json", data=bundle["scenario"]
        )
        reparsed, _facts = _parse_rows(scenario, plan, path / "evidence", events)
        if reparsed != rows:
            raise CollectError("bridge endpoint rows differ from retained raw evidence")
    source_failures = observations.get("failed_p50_source_transfers", {})
    failure_records = source_failures.get("records", [])
    raw_disconnect = any(
        "P50 cache control operation ended disconnected" in log.read_text(encoding="utf-8")
        for log in (path / "evidence" / "instances").glob(
            "*/results/workload/**/client-debug.log"
        )
    )
    if (
        raw_disconnect
        and plan.get("source_failure_observation_contract")
        == "icefarm-source-failure-observers-v2"
    ) or any(
        isinstance(item, Mapping) and "control_result_received" in item
        for item in failure_records
    ):
        scenario = ScenarioSpec(
            path=path / "evidence" / "specs" / "scenario.json", data=bundle["scenario"]
        )
        raw_rows, raw_facts = _parse_rows(scenario, plan, path / "evidence", events)
        if raw_rows != rows or raw_facts["failed_p50_source_transfers"] != source_failures:
            raise CollectError("lost control reply observations differ from retained raw evidence")
    _verify_h3_raw_replay(bundle, path / "evidence", receipt_bindings)
    return bundle
