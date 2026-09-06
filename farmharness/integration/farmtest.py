#!/usr/bin/env python3
"""Spec-driven controlled-farm lifecycle, workload, and evidence runner."""

from __future__ import annotations

import argparse
import base64
import hashlib
import json
import os
import re
import secrets
import sys
from datetime import UTC, datetime
from pathlib import Path, PurePosixPath
from typing import Any

try:
    from farmharness import newgen_farm_env
    from .authority import AuthorityCaptureError, capture_authority, render_capture_plan
    from .collect import (
        COLLECT_SCHEMA,
        CollectError,
        collect_bundle,
        collect_refusal_bundle,
        load_verified_bundle,
    )
    from .daemon_mutant_promotion import (
        DaemonMutantPromotionError,
        promote_daemon_mutant,
    )
    from .farm_spec import FarmSpec, FarmSpecError, load_farm_spec
    from .images import (
        ImageError,
        build_and_distribute,
        build_and_distribute_foundations,
        export_source_archive_inventory,
        foundation_targets_for_scenario,
    )
    from .layout import instance_root, oracle_root, runtime_root, toolchain_root
    from .live_lock import LiveRunLockError, live_run_lock
    from .mutant import MUTANT_TRACE_PATH
    from .lifecycle import (
        LifecycleError,
        PreflightRefusal,
        bring_up,
        bundle_root,
        down_from_state,
    )
    from .performance import (
        S80_ORDER,
        S80EvidenceError,
        render_s80_report,
        s80_cell_from_bundle,
        score_s80_cells,
        validate_s80_arm_scenario,
        validate_s80_matrix_scenarios,
    )
    from .remote import (
        FakeRecorder,
        PlannedCommand,
        RemoteError,
        docker_argv,
        execute,
        ssh_argv,
    )
    from .scenario_spec import ScenarioSpec, ScenarioSpecError, load_scenario_spec
    from .schema_validation import ValidationError, canonical_bytes, load_json, validate
    from .system_source_snapshot import derived_mounts, derived_root
    from .system_source_authority import (
        SystemSourceAuthorityError,
        capture_system_source_from_runtime,
    )
    from .report import ReportError, report_bundle, verify_bundle, verify_control_bundle
    from .s50_fairness import (
        S50FairnessError,
        render_fairness_report,
        score_s50_fairness,
    )
    from .suite_spec import SCHEMA_PATH as SUITE_SCHEMA_PATH
    from .suite_spec import SuiteSpec, SuiteSpecError, load_suite_spec
    from .verdict import evaluate_bundle, evaluate_control
    from .workload import WorkloadError, run_workload
except ImportError:  # Executed as ./farmtest.py.
    sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
    import newgen_farm_env

    from authority import AuthorityCaptureError, capture_authority, render_capture_plan
    from collect import (
        COLLECT_SCHEMA,
        CollectError,
        collect_bundle,
        collect_refusal_bundle,
        load_verified_bundle,
    )
    from daemon_mutant_promotion import DaemonMutantPromotionError, promote_daemon_mutant
    from farm_spec import FarmSpec, FarmSpecError, load_farm_spec
    from images import (
        ImageError,
        build_and_distribute,
        build_and_distribute_foundations,
        export_source_archive_inventory,
        foundation_targets_for_scenario,
    )
    from layout import instance_root, oracle_root, runtime_root, toolchain_root
    from live_lock import LiveRunLockError, live_run_lock
    from mutant import MUTANT_TRACE_PATH
    from lifecycle import (
        LifecycleError,
        PreflightRefusal,
        bring_up,
        bundle_root,
        down_from_state,
    )
    from performance import (
        S80_ORDER,
        S80EvidenceError,
        render_s80_report,
        s80_cell_from_bundle,
        score_s80_cells,
        validate_s80_arm_scenario,
        validate_s80_matrix_scenarios,
    )
    from remote import (
        FakeRecorder,
        PlannedCommand,
        RemoteError,
        docker_argv,
        execute,
        ssh_argv,
    )
    from scenario_spec import ScenarioSpec, ScenarioSpecError, load_scenario_spec
    from schema_validation import ValidationError, canonical_bytes, load_json, validate
    from system_source_snapshot import derived_mounts, derived_root
    from system_source_authority import (
        SystemSourceAuthorityError,
        capture_system_source_from_runtime,
    )
    from report import ReportError, report_bundle, verify_bundle, verify_control_bundle
    from s50_fairness import S50FairnessError, render_fairness_report, score_s50_fairness
    from suite_spec import SCHEMA_PATH as SUITE_SCHEMA_PATH
    from suite_spec import SuiteSpec, SuiteSpecError, load_suite_spec
    from verdict import evaluate_bundle, evaluate_control
    from workload import WorkloadError, run_workload


PLAN_SCHEMA = "icefarm-plan-v1"
RUN_ID_RE = re.compile(r"^[A-Za-z0-9][A-Za-z0-9._-]{0,79}$")
CONTAINER_TEMP_ROOT = "/tmp/icefarm"
CACHE_DISK_FAULT_PATH = "/var/cache/icecream"
CACHE_DISK_FAULT_BYTES = 128 * 1024 * 1024


PREPARE_INSTANCE_SCRIPT = r"""
import base64, hashlib, pathlib, shutil, sys

scratch = pathlib.Path(sys.argv[1])
run_id = sys.argv[2]
instance = sys.argv[3]
role = sys.argv[4]
client_entry_payload = sys.argv[5]
client_entry_sha256 = sys.argv[6]
root = scratch / "icefarm" / run_id / instance
if role not in ("S", "F", "C") or run_id in ("", ".", "..") or instance in ("", ".", ".."):
    raise SystemExit("unsafe instance layout")
for value in (run_id, instance):
    if "/" in value or "\\" in value or any(ord(character) < 32 for character in value):
        raise SystemExit("unsafe instance layout")
for leaf in ("cache", "tmp", "log"):
    path = root / leaf
    if path.is_symlink():
        raise SystemExit("managed directory is a symlink: " + str(path))
    path.mkdir(parents=True, exist_ok=True, mode=0o777)
    path.chmod(0o777)
output = root / "output"
if output.is_symlink():
    raise SystemExit("managed output is a symlink")
if output.exists():
    shutil.rmtree(output)
output.mkdir(parents=True, mode=0o777)
output.chmod(0o777)
if role == "C":
    input_root = root / "input"
    if input_root.is_symlink():
        raise SystemExit("managed input is a symlink")
    input_root.mkdir(parents=True, exist_ok=True, mode=0o755)
    try:
        client_entry = base64.b64decode(
            client_entry_payload.encode("ascii"), altchars=b"-_", validate=True
        )
    except (ValueError, UnicodeError) as exc:
        raise SystemExit("invalid client entry payload") from exc
    if hashlib.sha256(client_entry).hexdigest() != client_entry_sha256:
        raise SystemExit("client entry payload hash mismatch")
    entry_path = root / "entry-client.sh"
    temporary = root / ".entry-client.sh.tmp"
    if entry_path.is_symlink() or temporary.is_symlink():
        raise SystemExit("managed client entry is a symlink")
    temporary.write_bytes(client_entry)
    temporary.chmod(0o555)
    temporary.replace(entry_path)
elif client_entry_payload or client_entry_sha256:
    raise SystemExit("client entry payload supplied for non-client instance")
""".strip()


class PlanError(ValueError):
    """The validated specs cannot resolve to one safe command plan."""


class SuiteRunError(RuntimeError):
    """A suite cell had a harness failure after the suite record was created."""


def _configure_host_temp_environment() -> Path | None:
    """Route host-side temporary files from one explicit farm variable."""

    configured = os.environ.get("ICEFARM_TMPDIR")
    if configured is None:
        return None
    root = Path(configured)
    if not root.is_absolute():
        raise PlanError("ICEFARM_TMPDIR must be an absolute path")
    try:
        root.mkdir(parents=True, exist_ok=True)
    except OSError as exc:
        raise PlanError(f"cannot create ICEFARM_TMPDIR {root}: {exc}") from exc
    if not root.is_dir():
        raise PlanError(f"ICEFARM_TMPDIR is not a directory: {root}")
    for variable in ("TMPDIR", "TMP", "TEMP", "TEMPDIR"):
        os.environ[variable] = str(root)
    return root


def _image_reference(farm: FarmSpec, label: str) -> str:
    if "/" in label:
        return label
    registry = farm.data["registry"]
    return f"{registry['host']}:{registry['port']}/icefarm/icecream:{label}"


def _resolver_environment(farm: FarmSpec, scenario: ScenarioSpec) -> dict[str, str]:
    data = scenario.data
    by_role = {
        role: [item for item in data["instances"] if item["role"] == role]
        for role in ("S", "C", "F")
    }
    profiles = sorted(
        {
            item.get("env", {}).get("ICECC_P50_PROFILE")
            for item in data["instances"]
            if item.get("env", {}).get("ICECC_P50_PROFILE") is not None
        }
    )
    if len(profiles) > 1:
        raise PlanError(f"scenario declares conflicting P50 profiles: {profiles!r}")
    any_p50 = any(
        data["images"][item["image"]].rsplit(":", 1)[-1].lower().startswith("p50")
        for item in data["instances"]
    )
    instances = [
        {
            "name": item["name"],
            "role": item["role"],
            "host": item["host"],
            "image": data["images"][item["image"]],
            **(
                {"client_environment": item["client_environment"]}
                if item["role"] == "C"
                else {}
            ),
            **(
                {"system_source_snapshot": item["system_source_snapshot"]}
                if item.get("system_source_snapshot") is not None
                else {}
            ),
            **(
                {"env": dict(sorted(item.get("env", {}).items()))}
                if item.get("env")
                else {}
            ),
            **({"slots": item["slots"]} if item["role"] == "F" else {}),
        }
        for item in data["instances"]
    ]
    icefarm_env: dict[str, str] = {
        "ICEFARM_AUTHORITY_SHA256": hashlib.sha256(
            canonical_bytes(farm.data["authority"])
        ).hexdigest(),
        "ICEFARM_CORPUS": data["workload"]["corpus"],
        "ICEFARM_DRY_RUN": "1",
        "ICEFARM_INSTANCES": json.dumps(
            instances, sort_keys=True, separators=(",", ":"), ensure_ascii=False
        ),
        "ICEFARM_LINK_RATES": "1000000000,100000000",
        "ICEFARM_REGIME": "cold",
        "ICEFARM_TOPOLOGY": f"C{len(by_role['C'])}F{len(by_role['F'])}",
    }
    if any_p50:
        icefarm_env["ICEFARM_PROFILE"] = profiles[0] if profiles else "P29V1"
    corpus = farm.data["corpora"][data["workload"]["corpus"]]
    if "manifest" in corpus:
        icefarm_env["ICEFARM_MANIFEST"] = corpus["manifest"]
    elif "turn_a_manifest" in corpus:
        icefarm_env["ICEFARM_MANIFEST"] = corpus["turn_a_manifest"]
    elif "turn_a" in corpus:
        icefarm_env["ICEFARM_MANIFEST"] = str(
            PurePosixPath(corpus["hub_path"]) / corpus["turn_a"]
        )
    return icefarm_env


def resolve_topology(farm: FarmSpec, scenario: ScenarioSpec) -> dict[str, Any]:
    """Resolve through the normative v2 boundary; never fork digest logic."""

    try:
        topology = newgen_farm_env.resolve(
            _resolver_environment(farm, scenario),
            farm.data["authority"],
            host_policies=farm.hosts,
            corpus_authorities=farm.data["corpora"],
            runtime_image=farm.data["runtime_image"],
            client_environments=farm.data["client_environments"],
        )
        return _bind_system_source_snapshots(farm, scenario.data, topology)
    except newgen_farm_env.ResolutionError as exc:
        raise PlanError(str(exc)) from exc


def _bind_system_source_snapshots(
    farm: FarmSpec, scenario_data: dict[str, Any], topology: dict[str, Any]
) -> dict[str, Any]:
    snapshots = farm.data.get("system_source_snapshots", {})
    requested = {item["name"]: item for item in scenario_data["instances"]}
    for resolved in topology["instances"]:
        requested_instance = requested[resolved["name"]]
        snapshot_name = requested_instance.get("system_source_snapshot")
        if snapshot_name is None:
            continue
        snapshot = snapshots.get(snapshot_name)
        if not isinstance(snapshot, dict):
            raise PlanError(f"system source snapshot {snapshot_name!r} is absent")
        host_scratch = farm.hosts[resolved["host"]]["scratch_root"]
        resolved["system_source_snapshot"] = {
            "file_count": snapshot["file_count"],
            "manifest_sha256": snapshot["manifest_sha256"],
            "mounts": derived_mounts(host_scratch, snapshot["manifest_sha256"]),
            "name": snapshot_name,
            "root": str(derived_root(host_scratch, snapshot["manifest_sha256"])),
            "source_runtime_closure_sha256": snapshot["source_runtime_closure_sha256"],
            "source_runtime_id": snapshot["source_runtime_id"],
        }
    topology_body = dict(topology)
    topology_body.pop("topology_digest", None)
    topology["topology_digest"] = hashlib.sha256(
        canonical_bytes(topology_body)
    ).hexdigest()
    return topology


def _container_name(run_id: str, instance: str) -> str:
    return f"icefarm-{run_id}-{instance}"


def _allocated_ports(farm: FarmSpec, topology: dict[str, Any]) -> dict[str, Any]:
    start, end = farm.data["port_range"]
    peers = sorted(
        item["name"] for item in topology["instances"] if item["role"] != "S"
    )
    last = start + 1 + len(peers)
    if last > end:
        raise PlanError(
            f"port_range needs {2 + len(peers)} ports for scheduler/control and instances"
        )
    return {
        "instances": {name: start + 2 + index for index, name in enumerate(peers)},
        "scheduler": start,
        "scheduler_control": start + 1,
    }


def _env_args(environment: dict[str, str]) -> list[str]:
    result: list[str] = []
    for key, value in sorted(environment.items()):
        result.extend(("--env", f"{key}={value}"))
    return result


def _assignment_fence_mode(topology: dict[str, Any]) -> str | None:
    """Select the scheduler fence from the resolved relationship law."""

    cache_expected = [
        relationship["cache_expected"] for relationship in topology["relationships"]
    ]
    if not any(cache_expected):
        return None
    if all(cache_expected):
        return "strict-nonce"
    return "enforcing-compat"


def _planned_commands(
    farm: FarmSpec,
    scenario: ScenarioSpec,
    topology: dict[str, Any],
    ports: dict[str, Any],
    run_id: str,
) -> list[PlannedCommand]:
    commands: list[PlannedCommand] = []
    timeout = scenario.data["timeouts"]["up_s"]
    sequence = 0

    scheduler = next(item for item in topology["instances"] if item["role"] == "S")
    scheduler_port = ports["scheduler"]
    scheduler_addr = f"{scheduler['address']}:{scheduler_port}"
    netname = f"{farm.data['netname_prefix']}-{run_id}"
    client_entry = (Path(__file__).parent / "docker" / "entry-client.sh").read_bytes()
    client_entry_payload = base64.urlsafe_b64encode(client_entry).decode("ascii")
    client_entry_sha256 = hashlib.sha256(client_entry).hexdigest()
    disk_fill_targets = {
        event["instance"]
        for event in scenario.data.get("timeline", [])
        if event.get("action") == "disk_fill"
    }

    for instance in topology["instances"]:
        image_label = instance["image"].get("label")
        authority_image = farm.data["authority"]["images"].get(image_label)
        pending_mutant = (
            isinstance(authority_image, dict)
            and (
                ("H3" in scenario.data["controls"] and authority_image.get("kind") == "scheduler-mutant")
                or (scenario.data.get("id") == "S30-mutant-f-refusal" and authority_image.get("kind") == "daemon-mutant")
                or (
                    scenario.data.get("shape") == "S'[F'F''][C']"
                    and authority_image.get("kind") == "daemon-mutant"
                )
            )
        )
        if not isinstance(instance["image"].get("closure_sha256"), str) and not pending_mutant:
            raise PlanError(
                f"product image {instance['image']['label']} has no captured runtime closure"
            )
        host = farm.hosts[instance["host"]]
        root = instance_root(farm, instance["host"], run_id, instance["name"])
        directories = []
        if instance["role"] == "C":
            directories.append(
                str(oracle_root(farm, instance, scenario.data["workload"]["corpus"]))
            )
        prepare_argv = ssh_argv(
            farm,
            instance["host"],
            (
                "python3",
                "-c",
                PREPARE_INSTANCE_SCRIPT,
                host["scratch_root"],
                run_id,
                instance["name"],
                instance["role"],
                client_entry_payload if instance["role"] == "C" else "",
                client_entry_sha256 if instance["role"] == "C" else "",
            ),
        )
        commands.append(
            PlannedCommand(
                sequence=sequence,
                phase="up.prepare",
                host=instance["host"],
                instance=instance["name"],
                transport="ssh",
                timeout_s=timeout,
                argv=prepare_argv,
            )
        )
        sequence += 1
        if directories:
            commands.append(
                PlannedCommand(
                    sequence=sequence,
                    phase="up.prepare-persistent",
                    host=instance["host"],
                    instance=instance["name"],
                    transport="ssh",
                    timeout_s=timeout,
                    argv=ssh_argv(
                        farm,
                        instance["host"],
                        ("install", "-d", "-m", "0777", "--", *directories),
                    ),
                )
            )
            sequence += 1

    role_order = {"S": 0, "F": 1, "C": 2}
    for instance in sorted(
        topology["instances"], key=lambda item: (role_order[item["role"]], item["name"])
    ):
        host = farm.hosts[instance["host"]]
        root = instance_root(farm, instance["host"], run_id, instance["name"])
        args = [
            "run",
            "--detach",
            "--pull=never",
            "--name",
            _container_name(run_id, instance["name"]),
            "--label",
            f"icefarm.run={run_id}",
            "--label",
            f"icefarm.instance={instance['name']}",
            "--network",
            "host",
        ]
        environment = {
            **instance["env"],
            "ICECC_NETNAME": netname,
            "ICECC_TEST_SOCKET": (
                f"{CONTAINER_TEMP_ROOT}/{netname}-{instance['name']}.sock"
            ),
            "TEMP": CONTAINER_TEMP_ROOT,
            "TEMPDIR": CONTAINER_TEMP_ROOT,
            "TMP": CONTAINER_TEMP_ROOT,
            "TMPDIR": CONTAINER_TEMP_ROOT,
        }
        if "H3" in scenario.data["controls"] and instance["role"] == "S":
            environment.update(
                {
                    "ICECC_P50_H3_MUTANT": "1",
                    "ICECC_P50_H3_SCHEDULER_INSTANCE": instance["name"],
                    "ICECC_P50_H3_TRACE": MUTANT_TRACE_PATH,
                }
            )
        if instance["role"] == "C":
            environment.update(
                {
                    "ICECC_P50_COMPILE_IDENTITY_TRACE": "/results/compile-identity.jsonl",
                    "ICECC_P50_C_ACTION_TRACE": "/results/c-action.jsonl",
                    "ICECC_P50_C_LEGACY_WIRE_TRACE": "/results/c-legacy-wire.jsonl",
                    "ICECC_P50_SOURCE_RESULT_TRACE": "/results/source-result.jsonl",
                    "ICECC_P50_TEST_LIFECYCLE_TRACE": "/results/c-lifecycle.trace",
                    "ICECC_P50_TEST_READY_TRACE": "/results/c-ready.trace",
                    "ICECC_SCHEDULER": scheduler_addr,
                }
            )
        elif instance["role"] == "F":
            environment.update(
                {
                    "ICECC_P50_F_ACTION_TRACE": "/results/f-action.jsonl",
                    "ICECC_P50_F_LEGACY_WIRE_TRACE": "/results/f-legacy-wire.jsonl",
                    "ICECC_P50_TEST_LIFECYCLE_TRACE": "/results/f-lifecycle.trace",
                    "ICECC_P50_TEST_READY_TRACE": "/results/f-ready.trace",
                }
            )
            if scenario.data.get("id") == "S30-mutant-f-refusal":
                environment["ICECC_P50_S30_MUTANT_TRACE"] = "/results/s30-mutant-f.jsonl"
        if len(environment["ICECC_TEST_SOCKET"].encode("ascii")) > 107:
            raise PlanError(
                f"instance {instance['name']!r} ICECC_TEST_SOCKET exceeds the 107-byte Unix limit"
            )
        if instance["role"] == "F":
            args.extend(("--user", "0", "--cap-add", "SYS_CHROOT"))
        elif instance["role"] == "C":
            args.extend(("--user", "0"))
        for source, target in (
            (root / "cache", "/var/cache/icecream"),
            (root / "tmp", CONTAINER_TEMP_ROOT),
            (root / "log", "/var/log/icecream"),
            (root / "output", "/results"),
        ):
            if target == CACHE_DISK_FAULT_PATH and instance["name"] in disk_fill_targets:
                args.extend(
                    (
                        "--mount",
                        "type=tmpfs,"
                        f"dst={CACHE_DISK_FAULT_PATH},"
                        f"tmpfs-size={CACHE_DISK_FAULT_BYTES},tmpfs-mode=0700",
                    )
                )
            else:
                args.extend(("--mount", f"type=bind,src={source},dst={target}"))
        args.extend(
            (
                "--mount",
                f"type=bind,src={runtime_root(farm, instance)},dst=/opt/icecream,readonly",
            )
        )
        if instance["role"] == "C":
            corpus_root = root / "input"
            client_oracle_root = oracle_root(
                farm, instance, scenario.data["workload"]["corpus"]
            )
            args.extend(
                (
                    "--mount",
                    f"type=bind,src={corpus_root},dst=/corpus,readonly",
                    "--mount",
                    f"type=bind,src={client_oracle_root},dst=/oracle",
                    "--mount",
                    f"type=bind,src={root / 'entry-client.sh'},"
                    "dst=/icefarm-entry-client.sh,readonly",
                )
            )
            client_toolchain_root = toolchain_root(farm, instance)
            if client_toolchain_root is not None:
                args.extend(
                    (
                        "--mount",
                        f"type=bind,src={client_toolchain_root},"
                        f"dst={instance['compiler_recipe']['toolchain']['mount']},readonly",
                    )
                )
            snapshot = instance.get("system_source_snapshot")
            if snapshot is not None:
                mounts = snapshot.get("mounts")
                if not isinstance(mounts, dict) or set(mounts) != {
                    "/usr/include", "/usr/lib/gcc", "/usr/local/include"
                }:
                    raise PlanError(
                        f"client {instance['name']!r} has incomplete system source snapshot mounts"
                    )
                for destination, source in sorted(mounts.items()):
                    args.extend(
                        (
                            "--mount",
                            f"type=bind,src={source},dst={destination},readonly",
                        )
                    )
        args.extend(_env_args(environment))
        entrypoint = {
            "S": "/opt/icecream/entry-scheduler.sh",
            "F": "/opt/icecream/entry-daemon.sh",
            "C": "/icefarm-entry-client.sh",
        }[instance["role"]]
        args.extend(
            (
                "--entrypoint",
                entrypoint,
                instance["container_image"]["reference"],
            )
        )
        if instance["role"] == "S":
            args.extend(("--port", str(scheduler_port), "--netname", netname))
            fence_mode = _assignment_fence_mode(topology)
            if fence_mode is not None:
                args.extend(("--assignment-fence-mode", fence_mode))
        elif instance["role"] == "F":
            args.extend(
                (
                    "--scheduler",
                    scheduler_addr,
                    "--netname",
                    netname,
                    "--port",
                    str(ports["instances"][instance["name"]]),
                    "--slots",
                    str(instance["slots"]),
                    "--name",
                    instance["name"],
                )
            )
        else:
            args.extend(
                (
                    "--scheduler",
                    scheduler_addr,
                    "--netname",
                    netname,
                    "--port",
                    str(ports["instances"][instance["name"]]),
                    "--name",
                    instance["name"],
                    "--idle",
                )
            )
        transport = "docker-context" if host.get("docker_context") else "ssh-docker"
        commands.append(
            PlannedCommand(
                sequence=sequence,
                phase=f"up.start-{instance['role'].lower()}",
                host=instance["host"],
                instance=instance["name"],
                transport=transport,
                timeout_s=timeout,
                argv=docker_argv(farm, instance["host"], args),
            )
        )
        sequence += 1
    return commands


def build_plan(
    farm: FarmSpec, scenario: ScenarioSpec, *, run_id: str | None = None
) -> dict[str, Any]:
    topology = resolve_topology(farm, scenario)
    ports = _allocated_ports(farm, topology)
    selected_run_id = run_id or f"plan-{topology['topology_digest'][:12]}"
    if RUN_ID_RE.fullmatch(selected_run_id) is None or selected_run_id in (".", ".."):
        raise PlanError("run id must be 1-80 safe, non-dot filename/label characters")
    commands = _planned_commands(farm, scenario, topology, ports, selected_run_id)
    return {
        "commands": [command.as_dict() for command in commands],
        "farm": str(farm.path),
        "farm_digest": farm.digest,
        "icefarm_env": _resolver_environment(farm, scenario),
        "ports": ports,
        "run_id": selected_run_id,
        "scenario": str(scenario.path),
        "schema": PLAN_SCHEMA,
        "scenario_digest": scenario.digest,
        "timeouts": dict(sorted(scenario.data["timeouts"].items())),
        "topology": topology,
        "topology_digest": topology["topology_digest"],
    }


def render_plan(plan: dict[str, Any]) -> str:
    return json.dumps(plan, indent=2, sort_keys=True, ensure_ascii=False) + "\n"


def _commands_from_plan(plan: dict[str, Any]) -> list[PlannedCommand]:
    return [
        PlannedCommand(
            sequence=item["sequence"],
            phase=item["phase"],
            host=item["host"],
            instance=item["instance"],
            transport=item["transport"],
            timeout_s=item["timeout_s"],
            argv=tuple(item["argv"]),
        )
        for item in plan["commands"]
    ]


def fake_up(plan: dict[str, Any]) -> list[dict[str, Any]]:
    recorder = FakeRecorder()
    execute(_commands_from_plan(plan), recorder)
    return [command.as_dict() for command in recorder.commands]


def new_run_id() -> str:
    """Return the normative collision-resistant UTC run identifier."""

    timestamp = datetime.now(UTC).strftime("%Y%m%dT%H%M%SZ")
    return f"{timestamp}-{secrets.token_hex(3)}"


def run_scenario(
    farm: FarmSpec,
    scenario: ScenarioSpec,
    plan: dict[str, Any],
    *,
    reap_stale_hours: float | None = None,
) -> tuple[dict[str, Any], str]:
    """Run one cell and always tear down its authenticated live objects."""

    up = False
    primary: BaseException | None = None
    try:
        bring_up(
            farm,
            scenario,
            plan,
            reap_stale_hours=reap_stale_hours,
        )
        up = True
        run_workload(farm, scenario, plan)
        collect_bundle(farm, scenario, plan)
        verify_bundle(bundle_root(farm, plan["run_id"]))
    except BaseException as exc:
        if isinstance(exc, PreflightRefusal) and scenario.data["controls"] == ["H1"]:
            try:
                collect_refusal_bundle(farm, scenario, plan)
                verify_bundle(bundle_root(farm, plan["run_id"]))
            except BaseException as refusal_exc:
                primary = refusal_exc
        else:
            primary = exc
    finally:
        if up:
            try:
                down_from_state(farm, plan)
            except BaseException as down_exc:
                if primary is None:
                    primary = down_exc
                else:
                    primary = LifecycleError(
                        f"{primary}; teardown also failed: {down_exc}"
                    )
    if primary is not None:
        raise primary
    return report_bundle(bundle_root(farm, plan["run_id"]))


def _atomic_output(path: Path, value: bytes) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(path.name + f".tmp-{os.getpid()}")
    try:
        temporary.write_bytes(value)
        os.replace(temporary, path)
    finally:
        temporary.unlink(missing_ok=True)


def promote_daemon_mutant_file(
    farm: FarmSpec,
    receipt_path: Path,
    label: str,
    output: Path,
) -> dict[str, Any]:
    """Promote once, validate the new uncaptured farm, and never overwrite."""

    output = Path(output)
    if os.path.lexists(output):
        raise DaemonMutantPromotionError(f"promotion output already exists: {output}")
    try:
        receipt = load_json(receipt_path.resolve())
    except (OSError, UnicodeError, ValueError) as exc:
        raise DaemonMutantPromotionError(
            f"cannot read image receipt {receipt_path}: {exc}"
        ) from exc
    promoted = promote_daemon_mutant(farm, label, receipt)
    temporary = output.with_name(output.name + f".tmp-{os.getpid()}")
    try:
        temporary.parent.mkdir(parents=True, exist_ok=True)
        try:
            descriptor = os.open(
                temporary,
                os.O_WRONLY | os.O_CREAT | os.O_EXCL,
                0o644,
            )
        except FileExistsError as exc:
            raise DaemonMutantPromotionError(
                f"promotion temporary output already exists: {temporary}"
            ) from exc
        with os.fdopen(descriptor, "wb") as stream:
            stream.write(canonical_bytes(promoted))
            stream.flush()
            os.fsync(stream.fileno())
        loaded = load_farm_spec(temporary)
        if "authority_capture" in loaded.data:
            raise DaemonMutantPromotionError(
                "promoted farm unexpectedly retains authority_capture"
            )
        try:
            os.link(temporary, output)
        except FileExistsError as exc:
            raise DaemonMutantPromotionError(
                f"promotion output already exists: {output}"
            ) from exc
        directory_descriptor = os.open(
            output.parent, os.O_RDONLY | getattr(os, "O_DIRECTORY", 0)
        )
        try:
            os.fsync(directory_descriptor)
        finally:
            os.close(directory_descriptor)
    except (OSError, FarmSpecError) as exc:
        raise DaemonMutantPromotionError(
            f"promoted farm failed uncaptured validation: {exc}"
        ) from exc
    finally:
        temporary.unlink(missing_ok=True)
    return {"label": label, "output": str(output), "status": "PROMOTED"}


def _render_suite(result: dict[str, Any]) -> str:
    manifest = result["execution_manifest"]
    suite_document = manifest["suite"]
    if "groups" in result:
        selected = [
            (
                child["name"]
                if child["selection"] == "all"
                else f"{child['name']}[{','.join(child['selection'])}]"
            )
            for child in manifest["children"]
        ]
        full_coverage = (
            [child["name"] for child in manifest["children"]]
            == suite_document["suites"]
            and all(child["selection"] == "all" for child in manifest["children"])
        )
    else:
        selected = list(
            dict.fromkeys(cell["scenario_id"] for cell in manifest["cells"])
        )
        full_coverage = selected == suite_document["scenarios"]
    lines = [
        f"# Integration suite — {result['suite_id']}",
        "",
        f"Verdict: **{result['status']}**",
        "",
        f"Suite digest: `{result['suite_digest']}`",
        f"Execution digest: `{result['execution_digest']}`",
        f"Coverage: **{'Full' if full_coverage else 'Selected'}**",
        f"Selection: `{'all' if full_coverage else ', '.join(selected)}`",
        "",
    ]
    if "groups" in result:
        lines.extend(
            (
                "| # | Included suite | Declared id | Selection | Status | Bundle |",
                "|---:|---|---|---|---:|---|",
            )
        )
        for group in result["groups"]:
            bundle = group.get("bundle", "—")
            if bundle != "—":
                bundle = f"`{bundle}`"
            lines.append(
                f"| {group['index']} | `{group['suite']}` | "
                f"`{group.get('suite_id', '—')}` | "
                f"`{group.get('selection', '—')}` | {group['status']} | {bundle} |"
            )
        return "\n".join(lines) + "\n"
    lines.extend(
        (
            "| # | Scenario | Rep | Product | Controls | Suite status | Bundle |",
            "|---:|---|---:|---:|---:|---:|---|",
        )
    )
    for cell in result["cells"]:
        bundle = cell.get("bundle", "—")
        if bundle != "—":
            bundle = f"`{bundle}`"
        lines.append(
            f"| {cell['index']} | `{cell['scenario']}` | "
            f"{cell.get('repetition', 1)} | "
            f"{cell.get('product_status', '—')} | "
            f"{cell.get('control_status', '—')} | {cell['status']} | {bundle} |"
        )
    return "\n".join(lines) + "\n"


def _suite_selector_paths(
    suite: SuiteSpec,
    prefix: tuple[str, ...] = (),
) -> dict[str, list[tuple[str, ...]]]:
    paths: dict[str, list[tuple[str, ...]]] = {}
    if not suite.is_composite:
        return paths
    for suite_name, child in suite.included_suites:
        path = (*prefix, suite_name)
        paths.setdefault(suite_name.casefold(), []).append(path)
        for selector, descendants in _suite_selector_paths(child, path).items():
            paths.setdefault(selector, []).extend(descendants)
    return paths


def _selected_composite_children(
    suite: SuiteSpec,
    only: set[str] | None,
) -> list[tuple[str, SuiteSpec, set[str] | None]]:
    """Route named suite selectors to immediate children without flattening."""

    if only is None:
        return [(name, child, None) for name, child in suite.included_suites]
    selector_paths = _suite_selector_paths(suite)
    selected_paths: list[tuple[str, ...]] = []
    unknown: list[str] = []
    ambiguous: list[str] = []
    for selector in sorted(only, key=str.casefold):
        matches = selector_paths.get(selector.casefold(), [])
        if not matches:
            unknown.append(selector)
        elif len(matches) != 1:
            ambiguous.append(selector)
        else:
            selected_paths.append(matches[0])
    if unknown:
        raise SuiteSpecError(
            f"--only names included suites absent from suite tree: {unknown!r}"
        )
    if ambiguous:
        raise SuiteSpecError(
            f"--only names ambiguous included suites: {ambiguous!r}"
        )

    # Selecting an ancestor means the whole subtree. Redundant descendant
    # selectors cannot narrow it and are removed deterministically.
    effective_paths = [
        path
        for path in selected_paths
        if not any(
            len(ancestor) < len(path) and path[: len(ancestor)] == ancestor
            for ancestor in selected_paths
        )
    ]
    routed: list[tuple[str, SuiteSpec, set[str] | None]] = []
    for suite_name, child in suite.included_suites:
        matches = [path for path in effective_paths if path[0] == suite_name]
        if not matches:
            continue
        child_only = None if any(len(path) == 1 for path in matches) else {
            path[-1] for path in matches
        }
        routed.append((suite_name, child, child_only))
    if not routed:
        raise SuiteSpecError("suite selection contains no included suites")
    return routed


def _prepared_run_ids(prepared: dict[str, Any]) -> list[str]:
    run_ids = [prepared["suite_run_id"]]
    for _name, child, _child_only in prepared.get("children", ()):
        run_ids.extend(_prepared_run_ids(child))
    run_ids.extend(
        plan["run_id"] for *_metadata, plan in prepared.get("planned_cells", ())
    )
    return run_ids


def _prepare_suite_run(
    farm: FarmSpec,
    suite: SuiteSpec,
    *,
    only: set[str] | None,
    stop_on_fail: bool,
    suite_run_id: str,
) -> dict[str, Any]:
    """Snapshot and plan one selected suite tree before any farm mutation."""

    kind = suite.data.get("kind", "generic")
    if suite.is_composite:
        selected = _selected_composite_children(suite, only)
        children = tuple(
            (
                name,
                _prepare_suite_run(
                    farm,
                    child,
                    only=child_only,
                    stop_on_fail=stop_on_fail,
                    suite_run_id=new_run_id(),
                ),
                child_only,
            )
            for name, child, child_only in selected
        )
        identity = {
            "children": [
                {
                    "execution_digest": child["execution_digest"],
                    "execution_manifest": child["identity"],
                    "name": name,
                    "selection": "all" if child_only is None else sorted(child_only),
                }
                for name, child, child_only in children
            ],
            "included_suite_digests": [
                {"digest": child.digest, "name": name}
                for name, child in suite.included_suites
            ],
            "schema": "icefarm-suite-execution-manifest-v1",
            "selection": (
                "all"
                if only is None
                else sorted(
                    selector
                    for name, _child, child_only in selected
                    for selector in (
                        (name,) if child_only is None else tuple(child_only)
                    )
                )
            ),
            "stop_on_fail": stop_on_fail,
            "suite": suite.data,
            "suite_digest": suite.digest,
            "suite_run_id": suite_run_id,
        }
        return {
            "children": children,
            "execution_digest": hashlib.sha256(canonical_bytes(identity)).hexdigest(),
            "identity": identity,
            "kind": "composite",
            "planned_cells": (),
            "scenario_by_id": {},
            "suite": suite,
            "suite_run_id": suite_run_id,
        }

    if kind in {"controls", "smoke", "s50-fairness"} and only is not None:
        raise SuiteSpecError(f"--only cannot select a partial {kind} suite")
    if suite.data.get("performance", {}).get("kind") == "s80" and only is not None:
        raise SuiteSpecError("--only cannot select an incomplete S80 matrix")

    scenario_by_id = {
        scenario_id: load_scenario_spec(suite.scenario_path(scenario_id), farm)
        for scenario_id in suite.data["scenarios"]
    }
    for expected_id, scenario in scenario_by_id.items():
        if scenario.data["id"] != expected_id:
            raise SuiteSpecError(
                f"suite id {expected_id!r} differs from scenario id "
                f"{scenario.data['id']!r}"
            )

    performance = suite.data.get("performance", {})
    if performance.get("kind") == "s80":
        arm_ids = performance["arms"]
        for arm, scenario_id in arm_ids.items():
            validate_s80_arm_scenario(arm, scenario_by_id[scenario_id].data)
        validate_s80_matrix_scenarios(
            {
                arm: scenario_by_id[scenario_id].data
                for arm, scenario_id in arm_ids.items()
            }
        )
        planned_cells = tuple(
            (
                sequence,
                arm,
                repetition,
                scenario_by_id[arm_ids[arm]],
                build_plan(
                    farm,
                    scenario_by_id[arm_ids[arm]],
                    run_id=new_run_id(),
                ),
            )
            for sequence, (arm, repetition) in enumerate(S80_ORDER, start=1)
        )
    else:
        known = set(scenario_by_id)
        selected_ids = known if only is None else only
        unknown = sorted(selected_ids - known)
        if unknown:
            raise SuiteSpecError(
                f"--only names scenarios absent from suite: {unknown!r}"
            )
        cells = [
            (scenario_by_id[scenario_id], repetition)
            for scenario_id, repetition in suite.expanded_scenario_ids()
            if scenario_id in selected_ids
        ]
        if not cells:
            raise SuiteSpecError("suite selection contains no scenarios")
        planned_cells = tuple(
            (
                scenario,
                repetition,
                build_plan(farm, scenario, run_id=new_run_id()),
            )
            for scenario, repetition in cells
        )

    identity_cells = []
    for cell in planned_cells:
        if performance.get("kind") == "s80":
            scenario = cell[-2]
            plan = cell[-1]
            metadata = cell[:-2]
        else:
            scenario, repetition, plan = cell
            metadata = (repetition,)
        identity_cells.append(
            {
                "metadata": list(metadata),
                "plan_digest": hashlib.sha256(canonical_bytes(plan)).hexdigest(),
                "run_id": plan["run_id"],
                "scenario_digest": scenario.digest,
                "scenario_id": scenario.data["id"],
            }
        )
    identity = {
        "cells": identity_cells,
        "schema": "icefarm-suite-execution-manifest-v1",
        "selection": "all" if only is None else sorted(selected_ids),
        "stop_on_fail": stop_on_fail
        or performance.get("kind") == "s80",
        "suite": suite.data,
        "suite_digest": suite.digest,
        "suite_run_id": suite_run_id,
    }
    return {
        "children": (),
        "execution_digest": hashlib.sha256(canonical_bytes(identity)).hexdigest(),
        "identity": identity,
        "kind": "atomic",
        "planned_cells": planned_cells,
        "scenario_by_id": scenario_by_id,
        "suite": suite,
        "suite_run_id": suite_run_id,
    }


def _prepare_execution(
    farm: FarmSpec,
    suite: SuiteSpec,
    *,
    only: set[str] | None,
    suite_run_id: str | None,
    stop_on_fail: bool = False,
) -> dict[str, Any]:
    prepared = _prepare_suite_run(
        farm,
        suite,
        only=only,
        stop_on_fail=stop_on_fail,
        suite_run_id=suite_run_id or new_run_id(),
    )
    run_ids = _prepared_run_ids(prepared)
    if len(set(run_ids)) != len(run_ids):
        raise SuiteSpecError("prepared suite run ids are not globally unique")
    return prepared


def _preflight_suite(
    farm: FarmSpec,
    suite: SuiteSpec,
    *,
    only: set[str] | None = None,
) -> None:
    """Resolve every descendant cell without touching the farm or result tree."""

    _prepare_execution(
        farm,
        suite,
        only=only,
        suite_run_id=f"preflight-{suite.digest[:12]}",
    )


def run_composite_suite(
    farm: FarmSpec,
    suite: SuiteSpec,
    *,
    only: set[str] | None = None,
    stop_on_fail: bool = False,
    reap_stale_hours: float | None = None,
    suite_run_id: str | None = None,
    _prepared: dict[str, Any] | None = None,
) -> tuple[dict[str, Any], str]:
    """Run included suites in order while preserving each child's own gate."""

    prepared = _prepared or _prepare_execution(
        farm,
        suite,
        only=only,
        suite_run_id=suite_run_id,
        stop_on_fail=stop_on_fail,
    )
    included = prepared["children"]
    selected_suite_run_id = prepared["suite_run_id"]
    output_root = (
        Path(farm.data["hub"]["results_root"]) / "suites" / selected_suite_run_id
    )
    output_root.mkdir(parents=True, exist_ok=False)
    result: dict[str, Any] = {
        "groups": [],
        "execution_digest": prepared["execution_digest"],
        "execution_manifest": prepared["identity"],
        "run_id": selected_suite_run_id,
        "schema": "icefarm-suite-result-v1",
        "status": "RUNNING",
        "suite_digest": suite.digest,
        "suite_id": suite.data["id"],
    }

    def persist() -> str:
        rendered = _render_suite(result)
        _atomic_output(output_root / "suite.json", canonical_bytes(result))
        _atomic_output(output_root / "SUITE.md", rendered.encode("utf-8"))
        return rendered

    persist()
    for index, (suite_name, child_prepared, child_only) in enumerate(
        included, start=1
    ):
        child = child_prepared["suite"]
        child_run_id = child_prepared["suite_run_id"]
        child_root = Path(farm.data["hub"]["results_root"]) / "suites" / child_run_id
        group: dict[str, Any] = {
            "bundle": str(child_root),
            "index": index,
            "run_id": child_run_id,
            "status": "RUNNING",
            "suite": suite_name,
            "suite_digest": child.digest,
            "suite_id": child.data["id"],
            "selection": "all" if child_only is None else sorted(child_only),
        }
        result["groups"].append(group)
        persist()
        try:
            child_result, _child_rendered = run_suite(
                farm,
                child,
                only=child_only,
                stop_on_fail=stop_on_fail,
                reap_stale_hours=reap_stale_hours,
                suite_run_id=child_run_id,
                _prepared=child_prepared,
            )
            group["status"] = child_result["status"]
            for gate in ("fairness", "performance"):
                if gate in child_result:
                    group[gate] = child_result[gate]
        except Exception as exc:
            group["error"] = f"{type(exc).__name__}: {exc}"
            group["status"] = "ERROR"
            result["status"] = "ERROR"
            rendered = persist()
            raise SuiteRunError(
                f"included suite {suite_name} failed; report retained at "
                f"{output_root / 'SUITE.md'}"
            ) from exc
        if group["status"] != "PASS":
            result["status"] = (
                "ERROR" if group["status"] == "ERROR" else "FAIL"
            )
            if stop_on_fail:
                break
        persist()
    if result["status"] == "RUNNING":
        result["status"] = "PASS"
    rendered = persist()
    return result, rendered


def run_s80_suite(
    farm: FarmSpec,
    suite: SuiteSpec,
    *,
    only: set[str] | None = None,
    reap_stale_hours: float | None = None,
    suite_run_id: str | None = None,
    _prepared: dict[str, Any] | None = None,
) -> tuple[dict[str, Any], str]:
    """Run twelve fresh, interleaved A/B cells and retain the strict score."""

    prepared = _prepared or _prepare_execution(
        farm,
        suite,
        only=only,
        suite_run_id=suite_run_id,
    )
    planned_cells = prepared["planned_cells"]
    cell_run_ids = [plan["run_id"] for *_metadata, plan in planned_cells]
    if len(set(cell_run_ids)) != len(S80_ORDER):
        raise SuiteSpecError("S80 cell run ids are not unique")

    selected_suite_run_id = prepared["suite_run_id"]
    output_root = (
        Path(farm.data["hub"]["results_root"]) / "suites" / selected_suite_run_id
    )
    output_root.mkdir(parents=True, exist_ok=False)
    result: dict[str, Any] = {
        "cells": [],
        "execution_digest": prepared["execution_digest"],
        "execution_manifest": prepared["identity"],
        "run_id": selected_suite_run_id,
        "schema": "icefarm-suite-result-v1",
        "status": "RUNNING",
        "suite_digest": suite.digest,
        "suite_id": suite.data["id"],
    }

    def persist() -> str:
        rendered = _render_suite(result)
        _atomic_output(output_root / "suite.json", canonical_bytes(result))
        _atomic_output(output_root / "SUITE.md", rendered.encode("utf-8"))
        performance = result.get("performance")
        if performance is not None:
            _atomic_output(
                output_root / "performance.json", canonical_bytes(performance)
            )
            _atomic_output(
                output_root / "PERFORMANCE.md",
                render_s80_report(performance).encode("utf-8"),
            )
        return rendered

    s80_cells: list[dict[str, Any]] = []
    persist()
    for sequence, arm, repetition, scenario, plan in planned_cells:
        cell_root = bundle_root(farm, plan["run_id"])
        cell: dict[str, Any] = {
            "arm": arm,
            "bundle": str(cell_root),
            "index": sequence,
            "repetition": repetition,
            "run_id": plan["run_id"],
            "scenario": scenario.data["id"],
            "status": "RUNNING",
        }
        result["cells"].append(cell)
        persist()
        try:
            product_verdict, _report = run_scenario(
                farm,
                scenario,
                plan,
                reap_stale_hours=reap_stale_hours,
            )
            cell["product_status"] = product_verdict["status"]
            cell["control_status"] = None
            cell["status"] = product_verdict["status"]
            if cell["status"] != "PASS":
                result["status"] = "FAIL"
                break
            reduced = s80_cell_from_bundle(
                arm,
                repetition,
                sequence,
                load_verified_bundle(cell_root),
                product_verdict,
            )
            s80_cells.append(reduced)
            cell["s80_cell"] = reduced
        except Exception as exc:
            cell["error"] = f"{type(exc).__name__}: {exc}"
            cell["status"] = "ERROR"
            result["status"] = "ERROR"
            rendered = persist()
            raise SuiteRunError(
                f"S80 cell {arm}/{repetition} failed; report retained at "
                f"{output_root / 'SUITE.md'}"
            ) from exc
        persist()

    if len(s80_cells) == len(S80_ORDER):
        result["performance"] = score_s80_cells(s80_cells)
        result["status"] = result["performance"]["status"]
    rendered = persist()
    return result, rendered


def run_suite(
    farm: FarmSpec,
    suite: SuiteSpec,
    *,
    only: set[str] | None = None,
    stop_on_fail: bool = False,
    reap_stale_hours: float | None = None,
    suite_run_id: str | None = None,
    _prepared: dict[str, Any] | None = None,
) -> tuple[dict[str, Any], str]:
    """Run a validated suite in declared order and persist its exact outcome."""

    prepared = _prepared or _prepare_execution(
        farm,
        suite,
        only=only,
        suite_run_id=suite_run_id,
        stop_on_fail=stop_on_fail,
    )
    if prepared.get("suite") is not suite:
        raise SuiteSpecError("prepared execution tree does not match suite snapshot")
    if suite_run_id is not None and prepared.get("suite_run_id") != suite_run_id:
        raise SuiteSpecError("prepared execution tree has a different suite run id")
    kind = suite.data.get("kind", "generic")
    expected_stop = stop_on_fail or suite.data.get("performance", {}).get("kind") == "s80"
    if prepared["identity"].get("stop_on_fail") is not expected_stop:
        raise SuiteSpecError("prepared execution tree has a different failure policy")
    if suite.is_composite:
        return run_composite_suite(
            farm,
            suite,
            only=only,
            stop_on_fail=stop_on_fail,
            reap_stale_hours=reap_stale_hours,
            suite_run_id=suite_run_id,
            _prepared=prepared,
        )
    if kind in {"controls", "smoke", "s50-fairness"} and only is not None:
        raise SuiteSpecError(f"--only cannot select a partial {kind} suite")

    if suite.data.get("performance", {}).get("kind") == "s80":
        return run_s80_suite(
            farm,
            suite,
            only=only,
            reap_stale_hours=reap_stale_hours,
            suite_run_id=suite_run_id,
            _prepared=prepared,
        )

    scenario_by_id = prepared["scenario_by_id"]
    planned_cells = prepared["planned_cells"]
    cell_run_ids = [plan["run_id"] for _scenario, _repetition, plan in planned_cells]
    if len(set(cell_run_ids)) != len(cell_run_ids):
        raise SuiteSpecError("suite cell run ids are not unique")

    selected_suite_run_id = prepared["suite_run_id"]
    output_root = (
        Path(farm.data["hub"]["results_root"]) / "suites" / selected_suite_run_id
    )
    output_root.mkdir(parents=True, exist_ok=False)
    result: dict[str, Any] = {
        "cells": [],
        "execution_digest": prepared["execution_digest"],
        "execution_manifest": prepared["identity"],
        "run_id": selected_suite_run_id,
        "schema": "icefarm-suite-result-v1",
        "status": "RUNNING",
        "suite_digest": suite.digest,
        "suite_id": suite.data["id"],
    }

    def persist() -> str:
        rendered = _render_suite(result)
        _atomic_output(output_root / "suite.json", canonical_bytes(result))
        _atomic_output(output_root / "SUITE.md", rendered.encode("utf-8"))
        fairness = result.get("fairness")
        if fairness is not None:
            _atomic_output(output_root / "fairness.json", canonical_bytes(fairness))
            _atomic_output(
                output_root / "FAIRNESS.md",
                render_fairness_report(fairness).encode("utf-8"),
            )
        return rendered

    for index, (scenario, repetition, plan) in enumerate(planned_cells, start=1):
        cell_run_id = plan["run_id"]
        cell_root = bundle_root(farm, cell_run_id)
        cell: dict[str, Any] = {
            "bundle": str(cell_root),
            "index": index,
            "repetition": repetition,
            "run_id": cell_run_id,
            "scenario": scenario.data["id"],
            "status": "RUNNING",
        }
        result["cells"].append(cell)
        persist()
        try:
            product_verdict, _report = run_scenario(
                farm,
                scenario,
                plan,
                reap_stale_hours=reap_stale_hours,
            )
            cell["product_status"] = product_verdict["status"]
            if scenario.data["controls"]:
                control_verdict = verify_control_bundle(cell_root)
                cell["control_status"] = control_verdict["status"]
                cell["status"] = control_verdict["status"]
            else:
                cell["control_status"] = None
                cell["status"] = product_verdict["status"]
        except Exception as exc:
            cell["error"] = f"{type(exc).__name__}: {exc}"
            cell["status"] = "ERROR"
            result["status"] = "ERROR"
            rendered = persist()
            raise SuiteRunError(
                f"suite cell {scenario.data['id']} failed; report retained at "
                f"{output_root / 'SUITE.md'}"
            ) from exc
        if cell["status"] != "PASS":
            result["status"] = "FAIL"
            if stop_on_fail:
                break
        persist()
    if result["status"] == "RUNNING":
        result["status"] = "PASS"
    if kind == "s50-fairness":
        metadata = suite.data["fairness"]
        control_id = metadata["control"]
        mixed_ids = metadata["mixed"]
        cells_by_id = {cell["scenario"]: cell for cell in result["cells"]}
        try:
            if any(cell["status"] != "PASS" for cell in result["cells"]):
                raise S50FairnessError("every S50 catalogue scenario must be PASS")
            fairness = score_s50_fairness(
                {
                    **cells_by_id[control_id],
                    "bundle_data": load_verified_bundle(
                        Path(cells_by_id[control_id]["bundle"])
                    ),
                    "scenario_data": scenario_by_id[control_id].data,
                },
                [
                    {
                        **cells_by_id[scenario_id],
                        "bundle_data": load_verified_bundle(
                            Path(cells_by_id[scenario_id]["bundle"])
                        ),
                        "scenario_data": scenario_by_id[scenario_id].data,
                    }
                    for scenario_id in mixed_ids
                ],
                ratio_limit=metadata["ratio_limit"],
            )
        except (KeyError, S50FairnessError, CollectError) as exc:
            fairness = {
                "error": f"{type(exc).__name__}: {exc}",
                "mixed": [],
                "ratio_limit": metadata["ratio_limit"],
                "schema": "icefarm-s50-fairness-v1",
                "status": "FAIL",
            }
        result["fairness"] = fairness
        result["status"] = fairness["status"]
    rendered = persist()
    return result, rendered


def replay_bundle(
    root: Path | str,
    *,
    _retained: dict[str, Any] | None = None,
) -> dict[str, Any]:
    """Recompute a retained topology digest and verdict without farm access."""

    bundle_path = Path(root).resolve()
    retained = (
        _retained if _retained is not None else load_verified_bundle(bundle_path)
    )
    farm_data = retained["farm"]
    scenario_data = retained["scenario"]
    if farm_data.get("schema") != "icefarm-farm-v1":
        raise ReportError("retained farm schema is unsupported")
    if scenario_data.get("schema") != "icefarm-scenario-v1":
        raise ReportError("retained scenario schema is unsupported")
    topology = retained.get("topology")
    if (
        not isinstance(topology, dict)
        or topology.get("schema") != newgen_farm_env.SCHEMA_V2
    ):
        raise ReportError("retained topology schema is unsupported")
    topology_body = dict(topology)
    topology_digest = topology_body.pop("topology_digest", None)
    reproduced_digest = hashlib.sha256(canonical_bytes(topology_body)).hexdigest()
    if topology_digest != reproduced_digest or topology_digest != retained.get(
        "topology_digest"
    ):
        raise ReportError("replayed topology digest differs from the retained bundle")

    # Immutable early-I4 bundles predate the container-foundation extension
    # to resolver v2.  They have neither farm-level runtime/client images nor
    # a client_environment in ICEFARM_INSTANCES.  Their exact topology bytes
    # remain self-authenticating under the unchanged v2 digest law, but the
    # current resolver intentionally refuses those old inputs.  Keep this
    # compatibility case explicit and narrow; all current bundles must still
    # reproduce by invoking the current pure resolver.
    try:
        requested = json.loads(retained["plan"]["icefarm_env"]["ICEFARM_INSTANCES"])
    except (KeyError, TypeError, json.JSONDecodeError) as exc:
        raise ReportError(f"retained resolver input cannot be replayed: {exc}") from exc
    historical_v2 = (
        "runtime_image" not in farm_data
        and "client_environments" not in farm_data
        and isinstance(requested, list)
        and any(
            isinstance(item, dict)
            and item.get("role") == "C"
            and "client_environment" not in item
            for item in requested
        )
    )
    if historical_v2:
        resolver_mode = "historical-v2-self-digest"
    else:
        # A v1 bundle is immutable input. Revalidating it against a later,
        # stricter revision of the live v1 schema loader would make retained
        # evidence unreplayable. The verified bundle already authenticates
        # these snapshot bytes; invoke only the pure resolver here.
        farm = FarmSpec(
            path=bundle_path / "evidence" / "specs" / "farm.json",
            data=farm_data,
        )
        try:
            reproduced_topology = newgen_farm_env.resolve(
                retained["plan"]["icefarm_env"],
                farm_data["authority"],
                host_policies=farm.hosts,
                corpus_authorities=farm_data.get("corpora"),
                runtime_image=farm_data.get("runtime_image"),
                client_environments=farm_data.get("client_environments"),
            )
            reproduced_topology = _bind_system_source_snapshots(
                farm, scenario_data, reproduced_topology
            )
        except (KeyError, TypeError, newgen_farm_env.ResolutionError) as exc:
            raise ReportError(
                f"retained resolver input cannot be replayed: {exc}"
            ) from exc
        if reproduced_topology != topology:
            raise ReportError("replayed topology differs from the retained bundle")
        resolver_mode = "current-v2-resolver"
    reproduced_verdict = evaluate_bundle(retained)
    verdict_path = bundle_path / "verdict.json"
    if verdict_path.is_file():
        try:
            persisted_verdict = json.loads(verdict_path.read_text(encoding="utf-8"))
        except (OSError, UnicodeError, json.JSONDecodeError) as exc:
            raise ReportError(f"cannot load retained verdict: {exc}") from exc
        if persisted_verdict != reproduced_verdict:
            raise ReportError("retained verdict differs from replayed verdict")
    reproduced_controls = None
    requested_controls = scenario_data.get("controls", [])
    if requested_controls:
        control_results = [
            evaluate_control(control_id, retained) for control_id in requested_controls
        ]
        reproduced_controls = {
            "controls": control_results,
            "schema": "icefarm-controls-verdict-v1",
            "status": "PASS"
            if all(item["status"] == "PASS" for item in control_results)
            else "FAIL",
        }
        control_path = bundle_path / "control-verdict.json"
        if control_path.is_file():
            try:
                persisted_controls = json.loads(
                    control_path.read_text(encoding="utf-8")
                )
            except (OSError, UnicodeError, json.JSONDecodeError) as exc:
                raise ReportError(
                    f"cannot load retained control verdict: {exc}"
                ) from exc
            if persisted_controls != reproduced_controls:
                raise ReportError(
                    "retained control verdict differs from replayed controls"
                )
    return {
        "controls": reproduced_controls,
        "run_id": retained["run_id"],
        "schema": "icefarm-replay-v1",
        "status": "REPRODUCED",
        "topology_digest": retained["topology_digest"],
        "resolver_mode": resolver_mode,
        "verdict": reproduced_verdict,
    }


def _suite_result(path: Path) -> dict[str, Any]:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError) as exc:
        raise ReportError(f"cannot load retained suite result {path}: {exc}") from exc
    if not isinstance(value, dict):
        raise ReportError(f"retained suite result {path} is not an object")
    return value


def _require_derived_suite_file(path: Path, expected: bytes) -> None:
    try:
        observed = path.read_bytes()
    except OSError as exc:
        raise ReportError(f"cannot load derived suite report {path}: {exc}") from exc
    if observed != expected:
        raise ReportError(f"derived suite report does not reproduce: {path.name}")


def _require_suite_directory_shape(root: Path, result: dict[str, Any]) -> None:
    expected = {"suite.json", "SUITE.md"}
    if "fairness" in result:
        expected.update(("fairness.json", "FAIRNESS.md"))
    if "performance" in result:
        expected.update(("performance.json", "PERFORMANCE.md"))
    try:
        entries = list(root.iterdir())
    except OSError as exc:
        raise ReportError(f"cannot inspect retained suite directory {root}: {exc}") from exc
    observed = {entry.name for entry in entries}
    if observed != expected:
        raise ReportError(
            "retained suite directory file set differs: "
            f"expected {sorted(expected)!r}, observed {sorted(observed)!r}"
        )
    unsafe = sorted(
        entry.name for entry in entries if entry.is_symlink() or not entry.is_file()
    )
    if unsafe:
        raise ReportError(f"retained suite directory contains unsafe files: {unsafe!r}")


def _suite_manifest_run_ids(manifest: object) -> list[str]:
    """Validate a self-contained execution manifest and return every run id."""

    if not isinstance(manifest, dict):
        raise ReportError("suite execution manifest is not an object")
    if manifest.get("schema") != "icefarm-suite-execution-manifest-v1":
        raise ReportError("suite execution manifest schema is unsupported")
    selection = manifest.get("selection")
    if selection != "all" and (
        not isinstance(selection, list)
        or not selection
        or any(not isinstance(item, str) or not item for item in selection)
        or selection != sorted(set(selection))
    ):
        raise ReportError("suite execution manifest has an invalid selection")
    if type(manifest.get("stop_on_fail")) is not bool:
        raise ReportError("suite execution manifest lacks its failure policy")
    suite = manifest.get("suite")
    if not isinstance(suite, dict) or suite.get("schema") != "icefarm-suite-v1":
        raise ReportError("suite execution manifest lacks its suite document")
    try:
        validate(suite, SUITE_SCHEMA_PATH)
    except ValidationError as exc:
        raise ReportError(f"retained suite document is invalid: {exc}") from exc
    suite_run_id = manifest.get("suite_run_id")
    if not isinstance(suite_run_id, str) or not RUN_ID_RE.fullmatch(suite_run_id):
        raise ReportError("suite execution manifest has an invalid suite run id")
    has_cells = "cells" in manifest
    has_children = "children" in manifest
    if has_cells == has_children:
        raise ReportError("suite execution manifest must have cells xor children")

    run_ids = [suite_run_id]
    if has_children:
        children = manifest.get("children")
        declared = suite.get("suites")
        if not isinstance(children, list) or not isinstance(declared, list):
            raise ReportError("composite execution manifest is malformed")
        if not children:
            raise ReportError("composite execution manifest selects no child")
        selected_names = [
            item.get("name") for item in children if isinstance(item, dict)
        ]
        if (
            len(selected_names) != len(children)
            or len(set(selected_names)) != len(selected_names)
            or selected_names
            != [name for name in declared if name in set(selected_names)]
        ):
            raise ReportError("composite execution order differs from suite document")
        child_digests = manifest.get("included_suite_digests")
        if (
            not isinstance(child_digests, list)
            or any(not isinstance(item, dict) for item in child_digests)
            or [item.get("name") for item in child_digests if isinstance(item, dict)]
            != declared
            or any(
                not isinstance(item.get("digest"), str)
                or not re.fullmatch(r"[0-9a-f]{64}", item["digest"])
                for item in child_digests
            )
        ):
            raise ReportError("composite suite digest catalogue is malformed")
        for child in children:
            if not isinstance(child, dict):
                raise ReportError("composite execution child is not an object")
            child_manifest = child.get("execution_manifest")
            child_digest = hashlib.sha256(canonical_bytes(child_manifest)).hexdigest()
            if child.get("execution_digest") != child_digest:
                raise ReportError("nested suite execution digest does not reproduce")
            child_ids = _suite_manifest_run_ids(child_manifest)
            run_ids.extend(child_ids)
            if child.get("selection") != child_manifest.get("selection"):
                raise ReportError("parent child selection differs from child manifest")
            if child_manifest["suite_digest"] != next(
                (
                    item.get("digest")
                    for item in child_digests
                    if isinstance(item, dict) and item.get("name") == child["name"]
                ),
                None,
            ):
                raise ReportError("selected child digest differs from suite snapshot")
        if selection == "all":
            if selected_names != declared or any(
                child.get("selection") != "all" for child in children
            ):
                raise ReportError("full composite selection omits suite coverage")
        else:
            actual_selection = sorted(
                selector
                for child in children
                for selector in (
                    (child["name"],)
                    if child["selection"] == "all"
                    else tuple(child["selection"])
                )
            )
            if actual_selection != selection:
                raise ReportError("composite selection differs from child coverage")
        suite_digest_input: dict[str, Any] = {
            "document": suite,
            "included_suites": child_digests,
            "schema": "icefarm-composite-suite-digest-v1",
        }
    else:
        cells = manifest.get("cells")
        scenarios = suite.get("scenarios")
        if not isinstance(cells, list) or not isinstance(scenarios, list):
            raise ReportError("atomic execution manifest is malformed")
        performance = suite.get("performance", {})
        if isinstance(performance, dict) and performance.get("kind") == "s80":
            arms = performance.get("arms")
            if not isinstance(arms, dict):
                raise ReportError("S80 execution manifest lacks arm metadata")
            expected = [
                (arms.get(arm), [sequence, arm, repetition])
                for sequence, (arm, repetition) in enumerate(S80_ORDER, start=1)
            ]
        else:
            repetitions = suite.get("repetitions", {})
            if (
                not isinstance(repetitions, dict)
                or any(type(value) is not int or value < 1 for value in repetitions.values())
            ):
                raise ReportError("suite repetitions are malformed")
            expected = [
                (scenario_id, [repetition])
                for scenario_id in scenarios
                for repetition in range(1, repetitions.get(scenario_id, 1) + 1)
            ]
        observed = []
        for cell in cells:
            if not isinstance(cell, dict):
                raise ReportError("suite execution cell is not an object")
            run_id = cell.get("run_id")
            if not isinstance(run_id, str) or not RUN_ID_RE.fullmatch(run_id):
                raise ReportError("suite execution cell has an invalid run id")
            for field in ("plan_digest", "scenario_digest"):
                value = cell.get(field)
                if not isinstance(value, str) or not re.fullmatch(r"[0-9a-f]{64}", value):
                    raise ReportError(f"suite execution cell has invalid {field}")
            observed.append((cell.get("scenario_id"), cell.get("metadata")))
            run_ids.append(run_id)
        is_s80 = isinstance(performance, dict) and performance.get("kind") == "s80"
        specialized = suite.get("kind") in {"controls", "smoke", "s50-fairness"}
        if is_s80 or specialized:
            if selection != "all" or observed != expected:
                raise ReportError("specialized suite execution is not complete")
        else:
            selected_scenarios = {scenario_id for scenario_id, _metadata in observed}
            expected = [
                item for item in expected if item[0] in selected_scenarios
            ]
            if (
                not observed
                or observed != expected
                or (
                    selection != "all"
                    and selection != sorted(selected_scenarios)
                )
                or (selection == "all" and selected_scenarios != set(scenarios))
            ):
                raise ReportError("suite execution cells differ from suite document")
        suite_digest_input = suite

    reproduced_suite_digest = hashlib.sha256(
        canonical_bytes(suite_digest_input)
    ).hexdigest()
    if manifest.get("suite_digest") != reproduced_suite_digest:
        raise ReportError("suite document digest does not reproduce")
    if len(run_ids) != len(set(run_ids)):
        raise ReportError("suite execution manifest contains duplicate run ids")
    return run_ids


def _replayed_prefix_status(
    statuses: list[str],
    *,
    complete: bool,
    stop_on_fail: bool,
    label: str,
) -> str:
    failures = [index for index, status in enumerate(statuses) if status != "PASS"]
    if failures and stop_on_fail and failures != [len(statuses) - 1]:
        raise ReportError(f"{label} continued after a stop-on-fail result")
    if not complete and (
        not stop_on_fail or failures != [len(statuses) - 1]
    ):
        raise ReportError(f"{label} has an illegitimate stopped prefix")
    return "FAIL" if failures else "PASS"


def _replay_suite_node(
    root: Path,
    *,
    expected_manifest: dict[str, Any] | None = None,
    loaded_result: dict[str, Any] | None = None,
) -> dict[str, Any]:
    result = (
        loaded_result
        if loaded_result is not None
        else _suite_result(root / "suite.json")
    )
    manifest = result.get("execution_manifest")
    if expected_manifest is not None and manifest != expected_manifest:
        raise ReportError("child suite manifest differs from its parent snapshot")
    digest = hashlib.sha256(canonical_bytes(manifest)).hexdigest()
    if result.get("execution_digest") != digest:
        raise ReportError("suite execution digest does not reproduce")
    _suite_manifest_run_ids(manifest)
    if (
        result.get("schema") != "icefarm-suite-result-v1"
        or result.get("run_id") != manifest["suite_run_id"]
        or result.get("suite_digest") != manifest["suite_digest"]
        or result.get("suite_id") != manifest["suite"].get("id")
    ):
        raise ReportError("suite result identity differs from its execution manifest")
    if root.name != result["run_id"]:
        raise ReportError("suite result directory differs from its run id")
    if result.get("status") not in {"PASS", "FAIL"}:
        raise ReportError("suite result is not a completed PASS or FAIL")

    suite = manifest["suite"]
    if "children" in manifest:
        groups = result.get("groups")
        if not isinstance(groups, list):
            raise ReportError("composite suite result lacks groups")
        planned = manifest["children"]
        if len(groups) > len(planned):
            raise ReportError("composite suite has more results than planned children")
        statuses: list[str] = []
        for index, group in enumerate(groups):
            child_plan = planned[index]
            if not isinstance(group, dict):
                raise ReportError("composite suite group is not an object")
            child_manifest = child_plan["execution_manifest"]
            child_run_id = child_manifest["suite_run_id"]
            child_root = root.parent / child_run_id
            if (
                group.get("index") != index + 1
                or group.get("run_id") != child_run_id
                or group.get("suite") != child_plan["name"]
                or group.get("suite_id") != child_manifest["suite"].get("id")
                or group.get("suite_digest") != child_manifest["suite_digest"]
                or group.get("selection") != child_plan.get("selection")
                or Path(str(group.get("bundle", ""))).name != child_run_id
            ):
                raise ReportError("composite suite group differs from its plan")
            replayed = _replay_suite_node(
                child_root,
                expected_manifest=child_manifest,
            )
            if group.get("status") != replayed["suite_status"]:
                raise ReportError("composite suite group status does not replay")
            for gate in ("fairness", "performance"):
                if group.get(gate) != replayed.get(gate):
                    raise ReportError(f"composite suite copied {gate} does not replay")
            statuses.append(replayed["suite_status"])
        complete = len(groups) == len(planned)
        reproduced_status = _replayed_prefix_status(
            statuses,
            complete=complete,
            stop_on_fail=manifest["stop_on_fail"],
            label="composite suite",
        )
        replayed_result: dict[str, Any] = {
            "groups": len(groups),
            "suite_status": reproduced_status,
        }
    else:
        cells = result.get("cells")
        if not isinstance(cells, list):
            raise ReportError("atomic suite result lacks cells")
        planned_cells = manifest["cells"]
        if len(cells) > len(planned_cells):
            raise ReportError("suite has more results than planned cells")
        statuses = []
        retained_by_scenario: dict[str, tuple[dict[str, Any], dict[str, Any]]] = {}
        reduced_s80: list[dict[str, Any]] = []
        s80_scenarios: dict[str, dict[str, Any]] = {}
        is_s80 = suite.get("performance", {}).get("kind") == "s80"
        for index, cell in enumerate(cells):
            identity = planned_cells[index]
            if not isinstance(cell, dict):
                raise ReportError("suite cell result is not an object")
            run_id = identity["run_id"]
            bundle_path = root.parent.parent / run_id
            if (
                cell.get("index") != index + 1
                or cell.get("run_id") != run_id
                or cell.get("scenario") != identity["scenario_id"]
                or Path(str(cell.get("bundle", ""))).name != run_id
            ):
                raise ReportError("suite cell result differs from its execution plan")
            metadata = identity["metadata"]
            if is_s80:
                sequence, arm, repetition = metadata
                if (
                    cell.get("arm") != arm
                    or cell.get("repetition") != repetition
                    or cell.get("index") != sequence
                ):
                    raise ReportError("S80 cell metadata differs from its execution plan")
            elif cell.get("repetition") != metadata[0]:
                raise ReportError("suite repetition differs from its execution plan")

            retained = load_verified_bundle(bundle_path)
            retained_scenario = retained.get("scenario")
            retained_plan = retained.get("plan")
            if (
                retained.get("run_id") != run_id
                or not isinstance(retained_plan, dict)
                or retained_plan.get("run_id") != run_id
            ):
                raise ReportError("suite run id differs from its authenticated bundle")
            if (
                not isinstance(retained_scenario, dict)
                or retained_scenario.get("id") != identity["scenario_id"]
            ):
                raise ReportError(
                    "suite scenario id differs from its authenticated bundle"
                )
            if retained.get("scenario_digest") != identity["scenario_digest"]:
                raise ReportError("suite scenario digest differs from its bundle")
            if hashlib.sha256(canonical_bytes(retained_plan)).hexdigest() != identity[
                "plan_digest"
            ]:
                raise ReportError("suite plan digest differs from its bundle")
            replayed = replay_bundle(bundle_path, _retained=retained)
            product_status = replayed["verdict"].get("status")
            controls = replayed.get("controls")
            control_status = controls.get("status") if isinstance(controls, dict) else None
            cell_status = control_status if control_status is not None else product_status
            if (
                cell.get("product_status") != product_status
                or cell.get("control_status") != control_status
                or cell.get("status") != cell_status
            ):
                raise ReportError("suite cell verdict status does not replay")
            statuses.append(cell_status)
            retained_by_scenario[identity["scenario_id"]] = (cell, retained)
            if is_s80 and cell_status == "PASS":
                validate_s80_arm_scenario(arm, retained_scenario)
                previous_scenario = s80_scenarios.setdefault(arm, retained_scenario)
                if previous_scenario != retained_scenario:
                    raise ReportError(
                        f"S80 {arm} repetitions use different scenario documents"
                    )
                reduced = s80_cell_from_bundle(
                    arm,
                    repetition,
                    sequence,
                    retained,
                    replayed["verdict"],
                )
                if cell.get("s80_cell") != reduced:
                    raise ReportError("retained S80 cell reduction does not replay")
                reduced_s80.append(reduced)

        complete = len(cells) == len(planned_cells)
        reproduced_status = _replayed_prefix_status(
            statuses,
            complete=complete,
            stop_on_fail=manifest["stop_on_fail"],
            label="atomic suite",
        )
        replayed_result = {"cells": len(cells), "suite_status": reproduced_status}

        if is_s80 and complete and all(status == "PASS" for status in statuses):
            validate_s80_matrix_scenarios(s80_scenarios)
            performance = score_s80_cells(reduced_s80)
            if result.get("performance") != performance:
                raise ReportError("suite S80 performance result does not replay")
            replayed_result["performance"] = performance
            reproduced_status = performance["status"]
            replayed_result["suite_status"] = reproduced_status
        elif "performance" in result:
            raise ReportError("suite has performance output without a complete S80 matrix")

        if suite.get("kind") == "s50-fairness":
            fairness_meta = suite.get("fairness", {})
            try:
                if any(status != "PASS" for status in statuses) or not complete:
                    raise S50FairnessError("every S50 catalogue scenario must be PASS")
                control_id = fairness_meta["control"]
                mixed_ids = fairness_meta["mixed"]
                control_cell, control_bundle = retained_by_scenario[control_id]
                fairness = score_s50_fairness(
                    {
                        **control_cell,
                        "bundle_data": control_bundle,
                        "scenario_data": control_bundle["scenario"],
                    },
                    [
                        {
                            **retained_by_scenario[scenario_id][0],
                            "bundle_data": retained_by_scenario[scenario_id][1],
                            "scenario_data": retained_by_scenario[scenario_id][1][
                                "scenario"
                            ],
                        }
                        for scenario_id in mixed_ids
                    ],
                    ratio_limit=fairness_meta["ratio_limit"],
                )
            except (KeyError, S50FairnessError) as exc:
                fairness = {
                    "error": f"{type(exc).__name__}: {exc}",
                    "mixed": [],
                    "ratio_limit": fairness_meta.get("ratio_limit"),
                    "schema": "icefarm-s50-fairness-v1",
                    "status": "FAIL",
                }
            if result.get("fairness") != fairness:
                raise ReportError("suite S50 fairness result does not replay")
            replayed_result["fairness"] = fairness
            replayed_result["suite_status"] = fairness["status"]

    if result["status"] != replayed_result["suite_status"]:
        raise ReportError("suite aggregate status does not replay")
    _require_suite_directory_shape(root, result)
    _require_derived_suite_file(
        root / "SUITE.md",
        _render_suite(result).encode("utf-8"),
    )
    if "fairness" in result:
        if suite.get("kind") != "s50-fairness":
            raise ReportError("non-S50 suite contains a fairness result")
        _require_derived_suite_file(
            root / "fairness.json",
            canonical_bytes(result["fairness"]),
        )
        _require_derived_suite_file(
            root / "FAIRNESS.md",
            render_fairness_report(result["fairness"]).encode("utf-8"),
        )
    if "performance" in result:
        if suite.get("performance", {}).get("kind") != "s80":
            raise ReportError("non-S80 suite contains a performance result")
        _require_derived_suite_file(
            root / "performance.json",
            canonical_bytes(result["performance"]),
        )
        _require_derived_suite_file(
            root / "PERFORMANCE.md",
            render_s80_report(result["performance"]).encode("utf-8"),
        )
    return replayed_result


def replay_suite(
    root: Path | str,
    *,
    expected_execution_digest: str | None = None,
) -> dict[str, Any]:
    """Recursively replay a retained suite and every authenticated cell."""

    suite_root = Path(root).resolve()
    try:
        result = _suite_result(suite_root / "suite.json")
        if expected_execution_digest is not None:
            if not re.fullmatch(r"[0-9a-f]{64}", expected_execution_digest):
                raise ReportError("expected suite execution digest is invalid")
            if result.get("execution_digest") != expected_execution_digest:
                raise ReportError("suite differs from expected execution digest")
        replayed = _replay_suite_node(suite_root, loaded_result=result)
    except ReportError:
        raise
    except (KeyError, TypeError, ValueError) as exc:
        raise ReportError(f"retained suite cannot be replayed: {exc}") from exc
    return {
        **replayed,
        "execution_digest": result["execution_digest"],
        "run_id": result["run_id"],
        "schema": "icefarm-suite-replay-v1",
        "status": "REPRODUCED",
    }


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    subparsers = parser.add_subparsers(dest="command", required=True)
    for command in ("plan", "up", "run", "collect", "verify", "down", "report"):
        child = subparsers.add_parser(command)
        child.add_argument("--farm", required=True)
        child.add_argument("--scenario", required=True)
        child.add_argument(
            "--run-id",
            required=command in ("run", "collect", "verify", "down", "report"),
        )
        if command == "up":
            child.add_argument("--fake-recorder", action="store_true")
            child.add_argument("--reap-stale", type=float, metavar="HOURS")
    scenario = subparsers.add_parser("scenario")
    scenario.add_argument("--farm", required=True)
    scenario.add_argument("--scenario", required=True)
    scenario.add_argument("--run-id")
    scenario.add_argument("--reap-stale", type=float, metavar="HOURS")
    replay = subparsers.add_parser("replay")
    replay.add_argument("--bundle", required=True)
    replay.add_argument("--expected-execution-digest")
    control = subparsers.add_parser("control")
    control.add_argument("--bundle", required=True)
    suite = subparsers.add_parser("suite")
    suite.add_argument("--farm", required=True)
    suite.add_argument("--suite", required=True)
    suite.add_argument("--only")
    suite.add_argument("--stop-on-fail", action="store_true")
    suite.add_argument("--reap-stale", type=float, metavar="HOURS")
    images = subparsers.add_parser("images")
    images.add_argument("--farm", required=True)
    images.add_argument("--labels")
    images.add_argument("--foundations", action="store_true")
    images.add_argument("--scenario")
    images.add_argument("--repo", default=str(Path(__file__).resolve().parents[2]))
    images.add_argument("--output")
    images.add_argument("--source-archive-dir")
    source_archives = subparsers.add_parser("source-archives")
    source_archives.add_argument("--farm", required=True)
    source_archives.add_argument("--labels")
    source_archives.add_argument(
        "--repo", default=str(Path(__file__).resolve().parents[2])
    )
    source_archives.add_argument("--output-dir", required=True)
    authority = subparsers.add_parser("authority")
    authority_commands = authority.add_subparsers(
        dest="authority_command", required=True
    )
    authority_capture = authority_commands.add_parser("capture")
    authority_capture.add_argument("--farm", required=True)
    authority_capture.add_argument("--output", required=True)
    authority_capture.add_argument("--descriptor-dir", required=True)
    authority_capture.add_argument("--timeout", type=int, default=120)
    authority_capture.add_argument("--dry-run", action="store_true")
    authority_promote = authority_commands.add_parser("promote-daemon-mutant")
    authority_promote.add_argument("--farm", required=True)
    authority_promote.add_argument("--receipt", required=True)
    authority_promote.add_argument("--label", required=True)
    authority_promote.add_argument("--output", required=True)
    authority_snapshot = authority_commands.add_parser("capture-system-source")
    authority_snapshot.add_argument("--farm", required=True)
    authority_snapshot.add_argument("--output", required=True)
    authority_snapshot.add_argument("--timeout", type=int, default=120)
    authority_snapshot.add_argument("--archive-dir")
    return parser


def main(argv: list[str] | None = None) -> int:
    args = _parser().parse_args(argv)
    try:
        _configure_host_temp_environment()
        if args.command == "replay":
            replay_root = Path(args.bundle)
            if (replay_root / "SHA256SUMS").is_file():
                if args.expected_execution_digest is not None:
                    raise ReportError(
                        "--expected-execution-digest applies only to suite replay"
                    )
                receipt = replay_bundle(replay_root)
            elif (replay_root / "suite.json").is_file():
                receipt = replay_suite(
                    replay_root,
                    expected_execution_digest=args.expected_execution_digest,
                )
            else:
                if args.expected_execution_digest is not None:
                    raise ReportError(
                        "--expected-execution-digest applies only to suite replay"
                    )
                receipt = replay_bundle(replay_root)
            print(json.dumps(receipt, indent=2, sort_keys=True))
            return 0
        if args.command == "control":
            receipt = verify_control_bundle(args.bundle)
            print(json.dumps(receipt, indent=2, sort_keys=True))
            return 0 if receipt["status"] == "PASS" else 1
        farm = load_farm_spec(args.farm)
        if args.command == "source-archives":
            labels = (
                [item for item in args.labels.split(",") if item]
                if args.labels
                else sorted(farm.data["authority"]["images"])
            )
            receipt = export_source_archive_inventory(
                farm,
                labels,
                repo=Path(args.repo),
                output_dir=Path(args.output_dir),
            )
            print(json.dumps(receipt, indent=2, sort_keys=True))
            return 0
        if args.command == "suite":
            suite = load_suite_spec(args.suite)
            selected = (
                {item for item in args.only.split(",") if item}
                if args.only is not None
                else None
            )
            with live_run_lock():
                receipt, rendered = run_suite(
                    farm,
                    suite,
                    only=selected,
                    stop_on_fail=args.stop_on_fail,
                    reap_stale_hours=args.reap_stale,
                )
            print(rendered, end="")
            return 0 if receipt["status"] == "PASS" else 1
        if args.command == "authority":
            if args.authority_command == "capture-system-source":
                receipt = capture_system_source_from_runtime(
                    farm,
                    output=Path(args.output),
                    archive_dir=Path(args.archive_dir) if args.archive_dir else None,
                    timeout_s=args.timeout,
                )
                print(json.dumps(receipt, indent=2, sort_keys=True))
                return 0
            if args.authority_command == "promote-daemon-mutant":
                receipt = promote_daemon_mutant_file(
                    farm,
                    Path(args.receipt),
                    args.label,
                    Path(args.output),
                )
                print(json.dumps(receipt, indent=2, sort_keys=True))
                return 0
            if args.dry_run:
                print(render_capture_plan(farm, timeout_s=args.timeout), end="")
                return 0
            receipt = capture_authority(
                farm,
                output=Path(args.output),
                descriptor_dir=Path(args.descriptor_dir),
                timeout_s=args.timeout,
            )
            print(json.dumps(receipt, indent=2, sort_keys=True))
            return 0
        if args.command == "images":
            if args.foundations:
                if args.labels:
                    raise PlanError(
                        "--labels and --foundations are separate image modes"
                    )
                if args.source_archive_dir:
                    raise PlanError(
                        "--source-archive-dir applies only to product images"
                    )
                targets = None
                if args.scenario:
                    selected_scenario = load_scenario_spec(args.scenario, farm)
                    targets = foundation_targets_for_scenario(selected_scenario.data)
                output = (
                    Path(args.output)
                    if args.output
                    else (
                        Path(farm.data["hub"]["results_root"])
                        / "images"
                        / farm.digest[:12]
                        / "foundations.json"
                    )
                )
                receipt = build_and_distribute_foundations(
                    farm,
                    repo=Path(args.repo),
                    output=output,
                    target_hosts=targets,
                )
                print(json.dumps(receipt, indent=2, sort_keys=True))
                return 0
            if args.scenario:
                raise PlanError("--scenario is valid only with --foundations")
            labels = (
                [item for item in args.labels.split(",") if item]
                if args.labels
                else sorted(farm.data["authority"]["images"])
            )
            output = (
                Path(args.output)
                if args.output
                else (
                    Path(farm.data["hub"]["results_root"])
                    / "images"
                    / farm.digest[:12]
                    / "images.json"
                )
            )
            receipt = build_and_distribute(
                farm,
                labels,
                repo=Path(args.repo),
                output=output,
                source_archive_dir=(
                    Path(args.source_archive_dir)
                    if args.source_archive_dir
                    else None
                ),
            )
            print(json.dumps(receipt, indent=2, sort_keys=True))
            return 0
        scenario = load_scenario_spec(args.scenario, farm)
        selected_run_id = args.run_id
        if args.command == "scenario" and selected_run_id is None:
            selected_run_id = new_run_id()
        plan = build_plan(farm, scenario, run_id=selected_run_id)
        if args.command == "scenario":
            with live_run_lock():
                verdict, report = run_scenario(
                    farm,
                    scenario,
                    plan,
                    reap_stale_hours=args.reap_stale,
                )
            print(report, end="")
            return 0 if verdict["status"] == "PASS" else 1
        if args.command == "up":
            if args.fake_recorder:
                recorded = fake_up(plan)
                if recorded != plan["commands"]:
                    print(
                        "farmtest internal error: fake recorder diverged from plan",
                        file=sys.stderr,
                    )
                    return 2
                print(render_plan(plan), end="")
                return 0
            receipt = bring_up(
                farm,
                scenario,
                plan,
                reap_stale_hours=args.reap_stale,
            )
            print(json.dumps(receipt, indent=2, sort_keys=True))
            return 0
        if args.command == "down":
            receipt = down_from_state(farm, plan)
            print(json.dumps(receipt, indent=2, sort_keys=True))
            return 0
        if args.command == "run":
            receipt = run_workload(farm, scenario, plan)
            print(json.dumps(receipt, indent=2, sort_keys=True))
            return 0
        if args.command == "collect":
            bundle = collect_bundle(farm, scenario, plan)
            receipt = {
                "artifacts": len(bundle["artifacts"]),
                "jobs": len(bundle["rows"]),
                "run_id": plan["run_id"],
                "schema": COLLECT_SCHEMA,
                "sha256sums_sha256": bundle["checksum_policy"]["sha256sums_sha256"],
                "status": "COLLECTED",
            }
            print(json.dumps(receipt, indent=2, sort_keys=True))
            return 0
        if args.command in ("verify", "report"):
            root = bundle_root(farm, plan["run_id"])
            retained = load_verified_bundle(root)
            if (
                retained.get("farm_digest") != farm.digest
                or retained.get("scenario_digest") != scenario.digest
                or retained.get("topology_digest") != plan["topology_digest"]
            ):
                raise CollectError(
                    "bundle does not belong to the requested farm/scenario plan"
                )
            if args.command == "verify":
                verdict = verify_bundle(root)
                print(json.dumps(verdict, indent=2, sort_keys=True))
                return 0 if verdict["status"] == "PASS" else 1
            verdict, report = report_bundle(root)
            print(report, end="")
            return 0 if verdict["status"] == "PASS" else 1
        print(render_plan(plan), end="")
        return 0
    except PreflightRefusal as exc:
        print(f"farmtest refused: {exc}", file=sys.stderr)
        return 3
    except LiveRunLockError as exc:
        print(f"farmtest refused: live farm lock unavailable: {exc}", file=sys.stderr)
        return 3
    except (
        AuthorityCaptureError,
        SystemSourceAuthorityError,
        DaemonMutantPromotionError,
        FarmSpecError,
        ScenarioSpecError,
        S80EvidenceError,
        SuiteSpecError,
        PlanError,
    ) as exc:
        print(f"farmtest refused: {exc}", file=sys.stderr)
        return 3
    except (
        CollectError,
        ImageError,
        LifecycleError,
        RemoteError,
        ReportError,
        SuiteRunError,
        WorkloadError,
    ) as exc:
        print(f"farmtest harness failure: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
