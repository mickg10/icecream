#!/usr/bin/env python3
"""External S8 farm adapter.

This module is intentionally an adapter, not another compile harness.  Batch
and predictive input validation, compile-database command selection, action
joining, and finalization remain in :mod:`s8_real_c1f1_live_runner`.  The
adapter owns only authenticated role placement and the bounded SSH lifecycle.
"""
from __future__ import annotations

import argparse
import base64
import hashlib
import json
import os
import shlex
import re
import subprocess
import time
import datetime as dt
import threading
from pathlib import Path
from typing import Any, Mapping, Sequence

try:
    from . import s4_multihost_c1f4 as s4
    from . import s8_real_c1f1_live_runner as live
except ImportError:  # pragma: no cover
    import s4_multihost_c1f4 as s4
    import s8_real_c1f1_live_runner as live

ROLE_PLACEMENT_SCHEMA = "icecream-s8-role-placement-v1"
EXTERNAL_SCHEMA = "icecream-s8-external-farm-v1"
AUTHORITY_SCHEMA = "icecream-s8-external-farm-authority-v1"
HOSTS = ("q3", "q2", "research6", "research7")
# research6 remains a captured authority host, but it is deliberately not an
# executable F target until the production placement policy is changed and a
# fresh authority is generated for that change.
F_HOSTS = ("q2", "research7")
CPU_COUNTS = {"q3": 32, "q2": 32, "research6": 20, "research7": 12}
TOPOLOGIES = {"C1F1/100000": (1, 1), "C1F20/40": (20, 2)}
PROFILES = ("P29", "ZSTD_TU", "ZSTD_ROUTE", "GRZ_RESIDUAL", "RAW_II")
IDLE_LOAD_THRESHOLD = 0.50
MIN_IDLE_PERCENT = 95.0


class ExternalFarmError(ValueError):
    pass


def _sha(path: Path) -> tuple[str, int]:
    info = path.lstat()
    if path.is_symlink() or not path.is_file() or info.st_nlink != 1:
        raise ExternalFarmError(f"private_file_required:{path}")
    h = hashlib.sha256(); size = 0
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1 << 20), b""):
            h.update(block); size += len(block)
    return h.hexdigest(), size


def _digest(value: object, label: str) -> str:
    if not isinstance(value, str) or len(value) != 64:
        raise ExternalFarmError(f"{label}:digest_invalid")
    try:
        int(value, 16)
    except ValueError as exc:
        raise ExternalFarmError(f"{label}:digest_invalid") from exc
    return value.lower()


def _load_private_json(path: Path, label: str) -> dict[str, Any]:
    _sha(path)
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError) as exc:
        raise ExternalFarmError(f"{label}:invalid_json") from exc
    if not isinstance(value, dict):
        raise ExternalFarmError(f"{label}:object_required")
    return value


def _validate_authority(authority: Mapping[str, Any]) -> None:
    if authority.get("schema") != AUTHORITY_SCHEMA or set(authority.get("hosts", {})) != set(HOSTS):
        raise ExternalFarmError("authority:host_set_invalid")
    placements = authority.get("placements")
    if not isinstance(placements, dict) or set(placements) != set(TOPOLOGIES):
        raise ExternalFarmError("authority:placements_invalid")
    physical: set[str] = set()
    boots: set[str] = set()
    reference_binaries: dict[str, str] | None = None
    now = dt.datetime.now(dt.timezone.utc)
    for host in HOSTS:
        item = authority["hosts"][host]
        required = {"target", "lan", "hostname", "descriptor", "physical_host_digest", "boot_id_digest",
                    "cpu_count", "idle", "binaries", "image", "cpu_sample",
                    "cpu_sample_digest"}
        if not isinstance(item, dict) or set(item) != required:
            raise ExternalFarmError(f"authority:{host}:fields_incomplete")
        value = _digest(item["physical_host_digest"], f"authority:{host}.physical_host_digest")
        if value in physical:
            raise ExternalFarmError("authority:physical_hosts_not_unique")
        physical.add(value)
        boot = _digest(item["boot_id_digest"], f"authority:{host}.boot_id_digest")
        if boot in boots:
            raise ExternalFarmError("authority:boot_ids_not_unique")
        boots.add(boot)
        if (item["cpu_count"] != CPU_COUNTS[host] or
                item["target"] != s4.HOSTS[host]["target"] or
                item["lan"] != s4.HOSTS[host]["lan"] or
                not isinstance(item["hostname"], str) or not item["hostname"]):
            raise ExternalFarmError(f"authority:{host}:identity_invalid")
        descriptor = item["descriptor"]
        if (not isinstance(descriptor, dict) or set(descriptor) != {"path", "sha256", "bytes"} or
                not isinstance(descriptor.get("path"), str) or not Path(descriptor["path"]).is_absolute()):
            raise ExternalFarmError(f"authority:{host}:descriptor_invalid")
        descriptor_sha, descriptor_bytes = _sha(Path(descriptor["path"]))
        if descriptor_sha != _digest(descriptor["sha256"], f"authority:{host}.descriptor") or descriptor_bytes != descriptor.get("bytes"):
            raise ExternalFarmError(f"authority:{host}:descriptor_mismatch")
        descriptor_value = _load_private_json(Path(descriptor["path"]),
                                              f"authority:{host}.descriptor")
        facts = descriptor_value.get("facts")
        fact_fields = {"machine_id_sha256", "boot_id_sha256", "nic_identity_sha256",
                       "cpu_vendor_sha256", "cpu_model_sha256", "cpu_count",
                       "physical_host_digest"}
        if (set(descriptor_value) != {"schema", "facts"} or
                descriptor_value.get("schema") != "icecream-s8-external-host-descriptor-v1" or
                not isinstance(facts, dict) or set(facts) != fact_fields):
            raise ExternalFarmError(f"authority:{host}:descriptor_fields_invalid")
        for field in fact_fields - {"cpu_count"}:
            _digest(facts[field], f"authority:{host}.descriptor.{field}")
        physical_input = {"machine_id_sha256": facts["machine_id_sha256"],
                          "nic_identity_sha256": facts["nic_identity_sha256"]}
        expected_physical = hashlib.sha256(json.dumps(
            physical_input, sort_keys=True, separators=(",", ":")).encode("ascii")).hexdigest()
        if (facts["physical_host_digest"] != expected_physical or
                facts["physical_host_digest"] != value or
                facts["boot_id_sha256"] != boot or
                facts["cpu_count"] != CPU_COUNTS[host]):
            raise ExternalFarmError(f"authority:{host}:descriptor_identity_mismatch")
        idle = item["idle"]
        sample = item["cpu_sample"]
        if (not isinstance(idle, dict) or
                set(idle) != {"status", "load_1m", "captured_at", "baseline_digest"} or
                idle.get("status") not in {"PASS", "HOLD"} or
                type(idle.get("load_1m")) not in (int, float) or idle["load_1m"] < 0 or
                not isinstance(sample, dict) or
                set(sample) != {"before", "after", "duration_seconds", "idle_percent"} or
                any(not isinstance(sample.get(field), str) or not sample[field]
                    for field in ("before", "after")) or
                type(sample.get("duration_seconds")) not in (int, float) or
                not .5 <= float(sample["duration_seconds"]) <= 5 or
                type(sample.get("idle_percent")) not in (int, float) or
                not 0 <= float(sample["idle_percent"]) <= 100):
            raise ExternalFarmError(f"authority:{host}:idle_invalid")
        _digest(idle["baseline_digest"], f"authority:{host}.baseline_digest")
        sample_sha = hashlib.sha256((json.dumps(
            sample, sort_keys=True, separators=(",", ":")) + "\n").encode("ascii")).hexdigest()
        if (_digest(item["cpu_sample_digest"], f"authority:{host}.cpu_sample_digest") !=
                sample_sha or idle["status"] !=
                ("PASS" if float(idle["load_1m"]) <= IDLE_LOAD_THRESHOLD and
                 float(sample["idle_percent"]) >= 95.0 else "HOLD")):
            raise ExternalFarmError(f"authority:{host}:idle_invalid")
        captured_at = idle.get("captured_at")
        if not isinstance(captured_at, str):
            raise ExternalFarmError(f"authority:{host}:idle_freshness_missing")
        try:
            captured = dt.datetime.fromisoformat(captured_at.replace("Z", "+00:00"))
        except (TypeError, ValueError) as exc:
            raise ExternalFarmError(f"authority:{host}:idle_freshness_invalid") from exc
        if captured.tzinfo is None or not -30 <= (now - captured).total_seconds() <= 300:
            raise ExternalFarmError(f"authority:{host}:idle_stale")
        image = item["image"]
        expected_image = (s4.EXPECTED_IMAGE_CONFIG_ID if host == "research7"
                          else s4.EXPECTED_IMAGE_ID)
        if (not isinstance(image, dict) or
                set(image) != {"reference", "image_id", "architecture", "os", "created"} or
                image.get("reference") != s4.PINNED_IMAGE or
                image.get("image_id") != expected_image or
                image.get("architecture") != "amd64" or image.get("os") != "linux" or
                not isinstance(image.get("created"), str) or not image["created"]):
            raise ExternalFarmError(f"authority:{host}:image_invalid")
        binaries = item["binaries"]
        required_roles = {"scheduler/icecc-scheduler", "daemon/iceccd", "client/icecc",
                          "client/icecc-create-env", "cache/icecc-cache-service"}
        if not isinstance(binaries, dict) or set(binaries) != required_roles:
            raise ExternalFarmError(f"authority:{host}:binary_identity_incomplete")
        for role, value in binaries.items():
            _digest(value, f"authority:{host}.binaries.{role}")
        if reference_binaries is None:
            reference_binaries = binaries
        elif binaries != reference_binaries:
            raise ExternalFarmError("authority:per_host_binary_mismatch")
    for suite, (count, _slots) in TOPOLOGIES.items():
        mapping = placements[suite]
        if (not isinstance(mapping, dict) or set(mapping) != {"relationship_hosts"} or
                not isinstance(mapping["relationship_hosts"], list) or
                len(mapping["relationship_hosts"]) != count or
                any(host not in F_HOSTS for host in mapping["relationship_hosts"])):
            raise ExternalFarmError(f"authority:placements:{suite}:invalid")


def load_authority(path: Path) -> dict[str, Any]:
    value = _load_private_json(path, "authority")
    _validate_authority(value)
    value = dict(value)
    value["path"] = str(path.resolve())
    value["sha256"], value["bytes"] = _sha(path)
    return value


def role_placement(authority: Mapping[str, Any], topology: str,
                   relationship_hosts: Sequence[str]) -> dict[str, Any]:
    if topology not in TOPOLOGIES or len(relationship_hosts) != TOPOLOGIES[topology][0]:
        raise ExternalFarmError("placement:topology_mapping_invalid")
    if any(host not in F_HOSTS for host in relationship_hosts):
        raise ExternalFarmError("placement:q3_f_forbidden")
    if authority["hosts"]["q3"]["idle"].get("status") != "PASS":
        raise ExternalFarmError("placement:q3_not_idle")
    if any(authority["hosts"][host]["idle"].get("status") != "PASS"
           for host in relationship_hosts):
        raise ExternalFarmError("placement:f_host_not_idle")
    c = authority["hosts"]["q3"]["physical_host_digest"]
    f = list(dict.fromkeys(authority["hosts"][host]["physical_host_digest"]
                           for host in relationship_hosts))
    if c in f or len(f) != len(set(f)):
        raise ExternalFarmError("placement:physical_overlap")
    return {"schema": ROLE_PLACEMENT_SCHEMA, "mode": "external_farm",
            "c_host_digest": c, "scheduler_host_digest": c,
            "f_host_digests": f, "roles_disjoint": True, "timing_eligible": True}


def validate_batch_inputs(batch_manifest: Path, predictive_plan: Path,
                          topology: Path, *, corpus: str, profile: str,
                          regime: str, depth: str, suite: str) -> tuple[list[dict[str, Any]], list[dict[str, Any]]]:
    """Delegate all input semantics to the current live runner.

    External execution requires predictive `.ii` descriptors and a unique
    compile-database binding for every TU.  Raw source files are retained as
    authenticated provenance but are never renamed/staged as ``N.cpp``.
    """
    plan, inputs, _ = live.load_predictive_plan(predictive_plan, corpus=corpus,
                                                 profile=profile, regime=regime,
                                                 depth=depth)
    count = live.selected_count(depth, len(inputs) if depth == "full" else None)
    rows = live.load_batch_manifest(batch_manifest, count)
    live.bind_batch_to_plan(rows, inputs)
    if any(not {"compile_db", "compile_db_sha256", "compile_source", "compile_output"}.issubset(row)
           for row in rows):
        raise ExternalFarmError("batch:compile_database_binding_required")
    live.load_topology(topology, rows, suite, plan.get("scheduling"))
    return rows, plan.get("scheduling", {}).get("assignments", [])


def _stage_input_paths(batch_manifest: Path, predictive_plan: Path, topology: Path,
                       product_root: Path, rows: Sequence[Mapping[str, Any]]) -> list[Path]:
    """Return every authenticated path that the q3 image must be able to read.

    The remote image has no shared checkout.  Staging only the product tree
    would silently make a valid local plan resolve to a different input (or a
    generated smoke TU), so every manifest/plan/source/database/payload path
    is explicitly included and must live below the exported absolute root.
    """
    values: list[Path] = [batch_manifest, predictive_plan, topology, product_root]
    for row in rows:
        # compile_output authenticates the database's original ``-o`` operand,
        # but the mature runner replaces it with its private remote/local
        # object paths.  It is an output contract, never a staged input.
        for key in ("source", "compile_db", "compile_source"):
            value = row.get(key)
            if value is not None:
                values.append(Path(str(value)))
        payload = row.get("predictive_input")
        if isinstance(payload, Mapping) and payload.get("path") is not None:
            values.append(Path(str(payload["path"])))
    result: list[Path] = []
    seen: set[Path] = set()
    for path in values:
        resolved = path.resolve()
        try:
            resolved.relative_to(Path("/tanksmall"))
        except ValueError as exc:
            raise ExternalFarmError(f"transport:input_outside_staged_product:{path}") from exc
        if not resolved.exists() or resolved.is_symlink() or not resolved.is_file() and not resolved.is_dir():
            raise ExternalFarmError(f"transport:staged_input_unavailable:{path}")
        if resolved not in seen:
            result.append(resolved)
            seen.add(resolved)
    return result


def profile_arguments(profile: str, *, root: str, work: str, role: str) -> list[str]:
    """Return parameterized daemon profile arguments; RAW has no sidecar."""
    if profile not in PROFILES:
        raise ExternalFarmError("profile:unsupported")
    if profile == "RAW_II":
        return []
    return ["--cache-service", f"{root}/cache/icecc-cache-service",
            "--cache-runtime-dir", f"{work}/cache-runtime-{role}"]


def profile_environment(profile: str) -> str:
    if profile not in PROFILES:
        raise ExternalFarmError("profile:unsupported")
    return {"P29": "p29", "ZSTD_TU": "zstd_tu", "ZSTD_ROUTE": "z3_long",
            "GRZ_RESIDUAL": "grz", "RAW_II": "none"}[profile]


def readiness_gate(profile: str, scheduler_log: str, *, relationships: int) -> str:
    """Shell gate for registration/cache READY, before measured work starts."""
    if profile not in PROFILES:
        raise ExternalFarmError("profile:unsupported")
    pattern = {"P29": "p29", "ZSTD_TU": "zstd_tu", "ZSTD_ROUTE": "z3_long",
               "GRZ_RESIDUAL": "grz"}.get(profile)
    if profile == "RAW_II":
        return ("ready=0; for _ in $(seq 1 60); do "
                f"n=$(grep -c login {shlex.quote(scheduler_log)} 2>/dev/null || true); "
                f"test \"$n\" -ge {relationships + 1} && ready=1 && break; sleep 1; done; "
                "test \"$ready\" -eq 1")
    return ("ready=0; for _ in $(seq 1 60); do "
            f"n=$(grep -E 'RELOGIN p50-f(-[0-9]+)?.*cache=.*cache_profiles=.*{pattern}' "
            f"{shlex.quote(scheduler_log)} 2>/dev/null | grep -oE 'p50-f(-[0-9]+)?' | sort -u | wc -l); "
            f"test \"$n\" -ge {relationships} && ready=1 && break; sleep 1; done; "
            "test \"$ready\" -eq 1")


def build_external_command(batch_manifest: Path, predictive_plan: Path, topology: Path,
                           product_root: Path, *, profile: str, corpus: str,
                           regime: str, depth: str, suite: str, workdir: Path,
                           timeout_seconds: int, passes: int = 1,
                           repeat_predictive_plan: Path | None = None) -> list[str]:
    """Build the mature runner argv and mark it for the external lifecycle."""
    runner_profile = "P29" if profile == "RAW_II" else profile
    command = live.build_command(batch_manifest, runner_profile, product_root=product_root,
                                 corpus=corpus, regime=regime, depth=depth,
                                 predictive_plan=predictive_plan, topology=topology,
                                 suite=suite, workdir=workdir,
                                 timeout_seconds=timeout_seconds,
                                 passes=passes, product_profile=("RAW_II" if profile == "RAW_II" else None))
    if repeat_predictive_plan is not None:
        command.append(f"ICECC_P50_REPEAT_PREDICTIVE_PLAN={repeat_predictive_plan}")
    return ["env", "ICECC_P50_EXTERNAL_FARM=1", *command[1:]]


def external_timeout_seconds(tu_count: int, passes: int, warm: bool) -> int:
    """Bound remote work, post-measurement local references, and collection."""
    if type(tu_count) is not int or tu_count <= 0 or passes not in (1, 2):
        raise ExternalFarmError("timeout:arguments_invalid")
    # Every remote batch has an equally sized, deliberately non-overlapping
    # local correctness pass.  A warm regime adds one more such pair.
    work_passes = 2 * (passes + (1 if warm else 0))
    return min(live.MAX_TIMEOUT_SECONDS,
               max(live.MIN_TIMEOUT_SECONDS, 30 + tu_count * 12 * work_passes))


def marker_wait_hook(request: str, ready: str, failed: str) -> str:
    """Create a hook which cannot outlive a failed marker supervisor."""
    paths = (request, ready, failed)
    if any(not isinstance(path, str) or not Path(path).is_absolute() or
           "\n" in path or "\0" in path
           for path in paths):
        raise ExternalFarmError("marker:path_invalid")
    request_q, ready_q, failed_q = map(shlex.quote, paths)
    return ("#!/bin/sh\nset -eu\n"
            f"touch {request_q}\n"
            f"while test ! -f {ready_q}; do\n"
            f"  if test -f {failed_q}; then cat {failed_q} >&2; exit 1; fi\n"
            "  sleep 0.2\n"
            "done\n")


def f_service_map_script(client_work: str, relationship_count: int) -> str:
    """Return a composable heredoc command with a real terminator newline."""
    if (not isinstance(client_work, str) or not Path(client_work).is_absolute() or
            "\n" in client_work or "\0" in client_work or
            type(relationship_count) is not int or
            relationship_count <= 0):
        raise ExternalFarmError("service_map:arguments_invalid")
    return f'''python3 - {shlex.quote(client_work)} <<'PY'
import json, pathlib, sys
root = pathlib.Path(sys.argv[1])
rows = []
for relationship in range({relationship_count}):
    path = root / f"f-trace-{{relationship}}.jsonl"
    identities = {{json.loads(line).get("f_store_guid") for line in path.read_text().splitlines()
                  if line.strip() and json.loads(line).get("action") == "TX_BEGIN"
                  and json.loads(line).get("actor") == "F"}}
    if len(identities) != 1:
        raise SystemExit("missing unique F store identity")
    rows.append(f"{{relationship}}\\tp50-f{{('-' + str(relationship)) if {relationship_count} > 1 else ''}}\\t{{identities.pop()}}\\n")
(root / "s8-f-service-map.tsv").write_text("".join(rows))
PY
'''


def overlap_required(topology: str, observed: Mapping[str, int]) -> None:
    if topology == "C1F20/40":
        if observed.get("planned_lanes") != 40 or observed.get("max_concurrent", 0) <= 1:
            raise ExternalFarmError("batch:parallel_overlap_missing")


def observed_batch_metrics(stdout: str, topology: str) -> dict[str, int]:
    """Extract the mature runner's authenticated concurrency witness."""
    if "PASS: all-P50 C1F1" not in stdout:
        raise ExternalFarmError("batch:product_runner_did_not_pass")
    if topology == "C1F20/40" and "execution_slots=40" not in stdout:
        raise ExternalFarmError("batch:planned_lanes_missing")
    match = re.search(r"max_concurrent_admitted_or_compiling_jobs=([0-9]+)", stdout)
    if match is None:
        raise ExternalFarmError("batch:concurrency_witness_missing")
    return {"planned_lanes": 40 if topology == "C1F20/40" else 1,
            "max_concurrent": int(match.group(1))}


def f_to_c_bytes(profile: str, *, remote_object_bytes: int,
                 f_action_stage_bytes: int, legacy_wire_bytes: int | None = None) -> int:
    """Return the product-defined F->C metric, never the F source-stage size."""
    if remote_object_bytes <= 0:
        raise ExternalFarmError("evidence:returned_object_missing")
    if profile == "RAW_II":
        if legacy_wire_bytes is None or legacy_wire_bytes <= 0:
            raise ExternalFarmError("evidence:raw_wire_ledger_missing")
        return legacy_wire_bytes
    if f_action_stage_bytes < 0:
        raise ExternalFarmError("evidence:f_action_invalid")
    return remote_object_bytes


def scheduler_assignment(client_log: str, scheduler_log: str, *, service: str) -> tuple[int, str]:
    """Join the client Job ID to scheduler ``BEGIN: N client=... server=...``."""
    jobs = re.findall(r"Have to use host .* - Job ID: ([0-9]+) - env:", client_log)
    if len(jobs) != 1:
        raise ExternalFarmError("evidence:client_job_identity_missing")
    job = jobs[0]
    matches = re.findall(rf"^BEGIN: {re.escape(job)} client=([^ ]+) server=([^ (\r\n]+)",
                         scheduler_log, re.MULTILINE)
    if len(matches) != 1 or matches[0][1] != service:
        raise ExternalFarmError("evidence:scheduler_assignment_mismatch")
    return int(job), matches[0][0]


def interference_delta(before: int, after: int, *, limit: int = 100) -> int:
    """Validate the farm-qbox witness without treating daemon keepalive as work."""
    if type(before) is not int or type(after) is not int or before < 0 or after < before:
        raise ExternalFarmError("evidence:interference_witness_invalid")
    delta = after - before
    if delta > limit:
        raise ExternalFarmError("evidence:background_farm_activity")
    return delta


def compile_database_argv(entry: Mapping[str, Any], *, staged_input: Path,
                          output: Path) -> list[str]:
    """Bind the authenticated compile-database argv to staged `.ii` input."""
    command = entry.get("command")
    if not isinstance(command, str):
        raise ExternalFarmError("batch:compile_command_missing")
    try:
        tokens = shlex.split(command)
    except ValueError as exc:
        raise ExternalFarmError("batch:compile_command_invalid") from exc
    if len(tokens) < 2:
        raise ExternalFarmError("batch:compile_command_invalid")
    source_positions = [i for i, token in enumerate(tokens) if token.endswith((".c", ".cc", ".cpp", ".ii"))]
    if len(source_positions) != 1:
        raise ExternalFarmError("batch:compile_source_ambiguous")
    output_positions = [i + 1 for i, token in enumerate(tokens[:-1]) if token == "-o"]
    if len(output_positions) != 1:
        raise ExternalFarmError("batch:compile_output_ambiguous")
    tokens[source_positions[0]] = str(staged_input)
    tokens[output_positions[0]] = str(output)
    return tokens


def external_manifest(authority: Mapping[str, Any], topology: str,
                      relationship_hosts: Sequence[str], port: int) -> dict[str, Any]:
    placement = role_placement(authority, topology, relationship_hosts)
    workers = [{"relationship": i, "service": "p50-f" if len(relationship_hosts) == 1 else f"p50-f-{i}",
                "host_digest": authority["hosts"][host]["physical_host_digest"]}
               for i, host in enumerate(relationship_hosts)]
    return {"schema": EXTERNAL_SCHEMA, "suite": topology,
            "scheduler": {"host": authority["hosts"]["q3"].get("lan", s4.HOSTS["q3"]["lan"]), "port": port},
            "client": {"host_digest": placement["c_host_digest"]},
            "workers": workers, "role_placement": placement}


def cohort_digest(authority: Mapping[str, Any], topology: str,
                  relationship_hosts: Sequence[str]) -> str:
    """Digest the authenticated C/S + ordered physical F cohort and mapping."""
    payload = {"topology": topology, "submission": authority["hosts"]["q3"]["descriptor"],
               "scheduler": authority["hosts"]["q3"]["descriptor"],
               "workers": [{"relationship": i, "host": authority["hosts"][host]["descriptor"],
                            "physical_host_digest": authority["hosts"][host]["physical_host_digest"]}
                           for i, host in enumerate(relationship_hosts)]}
    return hashlib.sha256((json.dumps(payload, sort_keys=True, separators=(",", ":")) + "\n").encode()).hexdigest()


def _retained_identity(root: Path, container_name: str) -> tuple[str, int]:
    """Read the daemon identity copied from a remote role work tree."""
    container = root / container_name
    pid_name = {"scheduler.container-id": "scheduler.pid",
                "c.container-id": "c.pid"}.get(
                    container_name, container_name.replace("container-id", "container-pid"))
    pid_path = root / pid_name
    try:
        container_id = container.read_text(encoding="utf-8").strip()
        pid = int(pid_path.read_text(encoding="utf-8").strip())
    except (OSError, UnicodeError, ValueError) as exc:
        raise ExternalFarmError(f"evidence:role_identity_missing:{root}") from exc
    if not re.fullmatch(r"[0-9a-fA-F]{12,128}", container_id) or not 1 <= pid <= 4_194_304:
        raise ExternalFarmError(f"evidence:role_identity_invalid:{root}")
    return container_id.lower(), pid


def _copy_remote_tree(host: str, remote: str, destination: Path,
                      image: str, timeout: float) -> None:
    """Copy a role tree through a root reader for daemon/compiler-owned files."""
    pattern = (r"/tmp/(?:s4-p50-fourhost-[a-z0-9-]+|"
               r"p50compilee2e\.external)\.[A-Za-z0-9]+")
    if not re.fullmatch(pattern, remote):
        raise ExternalFarmError("evidence:unexpected_remote_workdir")
    if destination.exists() or destination.is_symlink():
        raise ExternalFarmError("evidence:destination_already_exists")
    destination.mkdir(parents=True, exist_ok=False)
    remote_command = shlex.join(
        ["docker", "run", "--rm", "--network", "none", "--user", "0",
         "-v", f"{remote}:/probe/evidence:ro", "--entrypoint", "/bin/tar",
         image, "--exclude=*.sock", "-C", "/probe/evidence", "-cf", "-", "."])
    source = subprocess.Popen(
        [*s4.ssh_argv(host), remote_command],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    assert source.stdout is not None
    sink = subprocess.Popen(["tar", "-C", str(destination), "-xf", "-"],
                            stdin=source.stdout, stdout=subprocess.PIPE,
                            stderr=subprocess.PIPE)
    source.stdout.close()
    _, sink_error = sink.communicate(timeout=timeout)
    source_error = source.stderr.read() if source.stderr else b""
    source_rc = source.wait(timeout=30)
    if source_rc != 0 or sink.returncode != 0:
        raise ExternalFarmError("evidence:remote_tree_copy_failed:" +
                                (source_error + sink_error)[-300:].decode(errors="replace"))


def _finish_cleanup(cleanup_errors: Sequence[str], primary_error: BaseException | None) -> None:
    if not cleanup_errors:
        return
    cleanup_error = ExternalFarmError(
        "cleanup:incomplete:" + "|".join(cleanup_errors[:3]))
    if primary_error is not None:
        primary_error.add_note(str(cleanup_error))
        return
    raise cleanup_error


class SSHTransport:
    """Bounded command transport; lifecycle policy stays in this adapter."""
    def __init__(self, authority: Mapping[str, Any], *, timeout: float = 900):
        if not 0 < timeout <= 21600:
            raise ExternalFarmError("timeout:outside_bound")
        self.authority = authority; self.timeout = timeout

    def run(self, host: str, script: str, args: Sequence[object] = ()) -> subprocess.CompletedProcess[str]:
        if host not in HOSTS:
            raise ExternalFarmError("transport:unknown_host")
        result = s4.run_script(host, script, [str(arg) for arg in args], timeout=self.timeout)
        if result.returncode:
            script_sha = hashlib.sha256(script.encode("utf-8")).hexdigest()[:12]
            preview = " ".join(script.split())[:160]
            combined = "\n".join(value for value in (result.stdout, result.stderr) if value)
            detail = combined[-2000:].strip().replace("\n", " | ")
            raise ExternalFarmError(
                f"{host}:remote_command_failed:{result.returncode}:"
                f"script={script_sha}:{preview}:detail={detail or 'none'}")
        return result

    def execute(self, *, topology: str, relationship_hosts: Sequence[str], profile: str,
                batch_manifest: Path, predictive_plan: Path, topology_file: Path,
                corpus: str, regime: str, depth: str, output: Path,
                product_root_remote: str | None = None,
                batch_command: Sequence[str] | None = None,
                host_product_root: Path | None = None,
                extra_stage_paths: Sequence[Path] = ()) -> dict[str, Any]:
        rows, assignments = validate_batch_inputs(batch_manifest, predictive_plan, topology_file,
                                                   corpus=corpus, profile=profile, regime=regime,
                                                   depth=depth, suite=topology)
        placement = role_placement(self.authority, topology, relationship_hosts)
        selected_hosts(self.authority, topology, relationship_hosts)
        if "q3" in relationship_hosts:
            raise ExternalFarmError("transport:q3_f_forbidden")
        if not product_root_remote or batch_command is None:
            raise ExternalFarmError("transport:product_root_and_batch_command_required")
        if host_product_root is None:
            raise ExternalFarmError("transport:host_product_root_required")
        if Path(product_root_remote).resolve() != host_product_root.resolve():
            raise ExternalFarmError("transport:remote_product_path_must_match_staged_absolute_path")
        stage_paths = _stage_input_paths(batch_manifest, predictive_plan, topology_file,
                                         host_product_root, rows)
        for extra in extra_stage_paths:
            resolved = Path(extra).resolve()
            try:
                resolved.relative_to(Path("/tanksmall"))
            except ValueError as exc:
                raise ExternalFarmError("transport:input_outside_staged_product") from exc
            if not resolved.is_file() or resolved.is_symlink():
                raise ExternalFarmError(f"transport:staged_input_unavailable:{extra}")
            if resolved not in stage_paths:
                stage_paths.append(resolved)
        return self.execute_command(topology=topology, relationship_hosts=relationship_hosts,
                                    profile=profile, product_root_remote=product_root_remote,
                                    batch_command=batch_command, output=output,
                                    host_product_root=host_product_root,
                                    stage_paths=stage_paths)

    def execute_command(self, *, topology: str, relationship_hosts: Sequence[str],
                        profile: str, product_root_remote: str,
                        batch_command: Sequence[str], output: Path,
                        host_product_root: Path | None = None,
                        stage_paths: Sequence[Path] | None = None) -> dict[str, Any]:
        """Start C/S on q3, F only on selected farm hosts, then run the
        existing p50compilee2e batch command on q3.

        ``batch_command`` is generated by :func:`build_external_command` from
        authenticated predictive inputs; it is not an arbitrary client
        command.  The finalizer consumes its retained workdir/output and is
        responsible for the complete per-TU/action/RAW ledger checks.
        """
        placement = role_placement(self.authority, topology, relationship_hosts)
        selected_hosts(self.authority, topology, relationship_hosts)
        if any(host not in F_HOSTS for host in relationship_hosts):
            raise ExternalFarmError("transport:q3_f_forbidden")
        if (not batch_command or batch_command[0] != "env" or
                "ICECC_P50_EXTERNAL_FARM=1" not in batch_command or
                not any(str(item).endswith("p50compilee2e-run.sh") for item in batch_command) or
                any(str(item) == "/bin/true" for item in batch_command)):
            raise ExternalFarmError("transport:authenticated_batch_command_required")
        # Preserve s4's private evidence-root validation; external roots must
        # use its accepted unique naming convention.
        nonce = f"{os.getpid()}{time.monotonic_ns()}"
        token = f"s8ext-{nonce}"
        client_work = f"/tmp/p50compilee2e.external.{nonce}"
        worker_work = lambda relationship: f"/tmp/s4-p50-fourhost-f{relationship}.{nonce}"
        scheduler_port = 41000 + (os.getpid() % 1000)
        network = token
        scheduler_host = str(self.authority["hosts"]["q3"].get("lan", s4.HOSTS["q3"]["lan"]))
        execution_start_ns = time.time_ns()
        root = shlex.quote(product_root_remote)
        profile_env = shlex.quote(profile)
        services: list[tuple[str, str]] = []
        f_reset_scripts: list[tuple[str, str]] = []
        f_prewarm_scripts: list[tuple[str, str]] = []
        staged_roots: dict[str, str] = {}
        supervisor_stop = threading.Event()
        supervisor: threading.Thread | None = None
        supervisor_errors: list[str] = []
        cleanup_errors: list[str] = []
        if output.exists() or output.is_symlink():
            raise ExternalFarmError("output:private_create_once_required")
        output.mkdir(parents=True, exist_ok=False)
        if Path(client_work).exists() or Path(client_work).is_symlink():
            raise ExternalFarmError("transport:client_work_collision")
        authority_path_value = self.authority.get("path")
        if authority_path_value is not None:
            authority_source = Path(str(authority_path_value))
            authority_bytes = authority_source.read_bytes()
            authority_sha, authority_size = _sha(authority_source)
        else:
            authority_bytes = (json.dumps({key: value for key, value in self.authority.items()
                                           if key not in {"path", "sha256", "bytes"}},
                                          sort_keys=True, separators=(",", ":")) + "\n").encode()
            authority_sha = hashlib.sha256(authority_bytes).hexdigest()
            authority_size = len(authority_bytes)
        manifest = external_manifest(self.authority, topology, relationship_hosts, scheduler_port)
        manifest_bytes = (json.dumps(manifest, sort_keys=True, separators=(",", ":")) + "\n").encode()
        manifest_sha = hashlib.sha256(manifest_bytes).hexdigest()
        manifest_size = len(manifest_bytes)
        (output / "external-farm.json").write_bytes(manifest_bytes)
        (output / "external-authority.json").write_bytes(authority_bytes)
        receipt_context: dict[str, Any] | None = None
        primary_error: BaseException | None = None
        try:
            if host_product_root is not None:
                # q3 intentionally has no shared /tanksmall.  Stage the
                # authenticated absolute-path tree once, then bind its
                # tanksmall subtree into every pinned role/client container.
                source = host_product_root.resolve()
                try:
                    source.relative_to(Path("/tanksmall"))
                except ValueError as exc:
                    raise ExternalFarmError("transport:product_must_be_under_tanksmall") from exc
                paths = [Path(path).resolve() for path in (stage_paths or [source])]
                if source not in paths:
                    paths.append(source)
                for path in paths:
                    try:
                        path.relative_to(Path("/tanksmall"))
                    except ValueError as exc:
                        raise ExternalFarmError("transport:input_outside_staged_product") from exc
                roots: list[Path] = []
                for path in sorted(paths, key=lambda value: (len(value.parts), str(value))):
                    if any(path == root or root in path.parents for root in roots):
                        continue
                    roots.append(path)
                tar_names = [str(path.relative_to(Path("/"))) for path in roots]
                for host in dict.fromkeys(("q3", *relationship_hosts)):
                    staged_tanksmall = f"/tmp/{token}-tanksmall"
                    tar_process = subprocess.Popen(
                        ["tar", "-C", "/", "-cf", "-", *tar_names],
                        stdout=subprocess.PIPE, stderr=subprocess.PIPE)
                    assert tar_process.stdout is not None
                    remote_stage = f"set -eu; rm -rf {staged_tanksmall}; mkdir -p {staged_tanksmall}; tar -xf - -C {staged_tanksmall}"
                    receiver = subprocess.Popen(
                        [*s4.ssh_argv(host), "bash", "-c", remote_stage],
                        stdin=tar_process.stdout, stdout=subprocess.PIPE,
                        stderr=subprocess.PIPE)
                    tar_process.stdout.close()
                    _, receiver_error = receiver.communicate(timeout=self.timeout)
                    tar_error = tar_process.stderr.read() if tar_process.stderr else b""
                    tar_rc = tar_process.wait(timeout=30)
                    if tar_rc != 0 or receiver.returncode != 0:
                        raise ExternalFarmError(f"transport:{host}_sparse_stage_failed:" +
                                                 (tar_error + receiver_error)[-300:].decode(errors="replace"))
                    staged_roots[host] = f"{staged_tanksmall}{product_root_remote}"
                root_mount = staged_roots["q3"]
                docker_mounts = f"-v {root_mount}:/probe/product:ro -v /tmp/{token}-tanksmall/tanksmall:/tanksmall:ro"
            else:
                root_mount = product_root_remote
                docker_mounts = f"-v {shlex.quote(product_root_remote)}:/probe/product:ro"
            def mounts_for(host: str) -> str:
                if host in staged_roots:
                    stage = staged_roots[host].split(product_root_remote, 1)[0]
                    return f"-v {staged_roots[host]}:/probe/product:ro -v {stage}:/tanksmall:ro"
                return docker_mounts
            profile_flag = f"-e ICECC_TEST_SOCKET=/probe/work/client.sock" + ("" if profile == "RAW_II" else f" -e ICECC_P50_PROFILE={profile} -e ICECC_P50_C1F1_REQUIRED=1")
            scheduler_profile_flag = "" if profile == "RAW_II" else f"-e ICECC_P50_PROFILE={profile}"
            profile_export = "" if profile == "RAW_II" else f"export ICECC_P50_PROFILE={profile}; "
            daemon_account = ("if ! getent group icecc >/dev/null 2>&1; then groupadd --system icecc; fi; "
                              "if ! getent passwd icecc >/dev/null 2>&1; then useradd --system --gid icecc "
                              "--no-create-home --home-dir /nonexistent --shell /usr/sbin/nologin icecc; fi; ")
            preflight = r'''set -eu
root=$1; expected_cpu=$2; min_idle=$3; expected_physical=$4; expected_boot=$5; shift 5
fail() { printf 'S8_PREFLIGHT_FAIL field=%s observed=%s expected=%s\n' "$1" "$2" "$3" >&2; exit 77; }
actual_cpu=$(nproc)
test "$actual_cpu" -eq "$expected_cpu" || fail cpu_count "$actual_cpu" "$expected_cpu"
idle_ready=0
for _ in $(seq 1 10); do
  before=$(awk '/^cpu / {print; exit}' /proc/stat)
  sleep 1
  after=$(awk '/^cpu / {print; exit}' /proc/stat)
  idle=$(awk -v b="$before" -v a="$after" 'BEGIN {split(b,x," "); n=split(a,y," "); total=0; for(i=2;i<=n;i++) total+=y[i]-x[i]; if(total<=0) exit 1; print (100*(y[5]-x[5])/total)}')
  if awk -v idle="$idle" -v min="$min_idle" 'BEGIN { exit !(idle >= min) }'; then
    idle_ready=1
    break
  fi
done
test "$idle_ready" -eq 1 || fail cpu_idle_percent "$idle" "$min_idle"
machine=$(sha256sum /etc/machine-id | awk '{print $1}')
boot=$(sha256sum /proc/sys/kernel/random/boot_id | awk '{print $1}')
test "$boot" = "$expected_boot" || fail boot_id "$boot" "$expected_boot"
nic_rows=$(for p in /sys/class/net/*; do n=${p##*/}; test "$n" = lo && continue; real=$(readlink -f "$p" 2>/dev/null || true); mac=$(cat "$p/address" 2>/dev/null || true); case "$real" in */virtual/*) continue;; esac; test -n "$mac" && test "$mac" != 00:00:00:00:00:00 && printf '%s:%s:%s\n' "$n" "$mac" "$real"; done | sort)
test -n "$nic_rows" || fail nic_inventory empty nonempty
nic=$(printf '%s\n' "$nic_rows" | sha256sum | awk '{print $1}')
physical=$(printf '{"machine_id_sha256":"%s","nic_identity_sha256":"%s"}' "$machine" "$nic" | sha256sum | awk '{print $1}')
test "$physical" = "$expected_physical" || fail physical_host "$physical" "$expected_physical"
while [ "$#" -gt 0 ]; do
  rel=$1; expected=$2; shift 2
  test -f "$root/$rel" && test ! -L "$root/$rel" || fail "role:$rel" missing regular_file
  got=$(sha256sum "$root/$rel" | awk '{print $1}')
  test "$got" = "$expected" || fail "role:$rel" "$got" "$expected"
done
'''
            witness = r'''set -eu
root=$1; phase=$2
ticks=0
for pid in $(ps -eo pid=,args= | awk '$0 ~ /iceccd/ && $0 ~ /-N farm-qbox/ {print $1}'); do
  stat=$(cat "/proc/$pid/stat" 2>/dev/null || true)
  set -- $stat
  test "$#" -ge 15 || continue
  ticks=$((ticks + $14 + $15))
done
printf '%s %s\n' "$phase" "$ticks" >"$root/interference-$phase"
if test "$phase" = after; then
  before=$(awk '{print $2}' "$root/interference-before")
  printf 'S8_F_INTERFERENCE before=%s after=%s\n' "$before" "$ticks"
else
  printf 'S8_F_INTERFERENCE phase=before ticks=%s\n' "$ticks"
fi
'''
            port_gate = r'''set -eu
python3 - "$@" <<'PY'
import socket, sys
ports = [int(value) for value in sys.argv[1:]]
sockets = []
try:
    for port in ports:
        sock = socket.socket(); sock.bind(("0.0.0.0", port)); sockets.append(sock)
finally:
    for sock in sockets: sock.close()
PY
'''
            for host in dict.fromkeys(("q3", *relationship_hosts)):
                args = [root_mount, CPU_COUNTS[host], MIN_IDLE_PERCENT,
                        self.authority["hosts"][host]["physical_host_digest"],
                        self.authority["hosts"][host]["boot_id_digest"]]
                for role, expected in self.authority["hosts"][host]["binaries"].items():
                    args.extend((role, expected))
                self.run(host, preflight, args)
            self.run("q3", port_gate, [scheduler_port, scheduler_port + 1, scheduler_port + 2])
            for relationship, host in enumerate(relationship_hosts):
                self.run(host, port_gate, [scheduler_port + 3 + relationship])
            scheduler_inner = (daemon_account +
                               f"/probe/product/scheduler/icecc-scheduler -p {scheduler_port} -n {network} "
                               f"--assignment-fence-mode strict-nonce -l /probe/work/scheduler.log -vvv")
            scheduler = (f"set -eu; root={shlex.quote(root_mount)}; image={shlex.quote(self.authority['hosts']['q3']['image'].get('reference', ''))}; mkdir -p {client_work}; chmod 1777 {client_work}; : >{client_work}/scheduler.log; chmod 0666 {client_work}/scheduler.log; "
                         f"test \"$(docker image inspect --format '{{{{.Id}}}}' \"$image\")\" = {self.authority['hosts']['q3']['image']['image_id']}; "
                         f"{profile_export}"
                         f"docker run -d --name {token}-scheduler --network host --user 0 --entrypoint /bin/sh {scheduler_profile_flag} {docker_mounts} -v {client_work}:/probe/work:rw $image -c "
                         f"{shlex.quote(scheduler_inner)} > {client_work}/scheduler.stdout 2>&1; "
                         f"test \"$(docker inspect --format '{{{{.State.Running}}}}' {token}-scheduler)\" = true; docker inspect --format '{{{{.Id}}}}' {token}-scheduler > {client_work}/scheduler.container-id; docker inspect --format '{{{{.State.Pid}}}}' {token}-scheduler > {client_work}/scheduler.pid; "
                         f"printf 'S8_CONTAINER_ID=%s\\n' \"$(cat {client_work}/scheduler.container-id)\"")
            self.run("q3", scheduler)
            sidecar_kill_inner = r'''set -eu
pid=$1
role=$2
runtime=$3
printf '%s' "$pid" | grep -Eq '^[0-9]+$'
test -r "/proc/$pid/status" -a -r "/proc/$pid/cmdline"
command=$(tr '\0' ' ' <"/proc/$pid/cmdline")
printf '%s\n' "$command" | grep -F '/probe/product/cache/icecc-cache-service'
printf '%s\n' "$command" | grep -F -- "--socket $runtime/"
parent=$(awk '/^PPid:/ {print $2}' "/proc/$pid/status")
test -n "$parent" -a -r "/proc/$parent/cmdline"
parent_command=$(tr '\0' ' ' <"/proc/$parent/cmdline")
printf '%s\n' "$parent_command" | grep -F '/probe/product/daemon/iceccd'
case $role in
  C) printf '%s\n' "$parent_command" | grep -F -- '--no-remote' | grep -F -- '-N s8-p50-c' ;;
  F) printf '%s\n' "$parent_command" | grep -F -- '-N p50-f' ;;
  *) exit 77 ;;
esac
kill -9 "$pid"
'''
            for relationship, host in enumerate(relationship_hosts):
                service = "p50-f" if topology == "C1F1/100000" else f"p50-f-{relationship}"
                worker_root = worker_work(relationship)
                worker_shared_files = " ".join((
                    f"{worker_root}/f.log",
                    f"{worker_root}/f-service.stderr",
                    f"{worker_root}/ready.trace",
                    f"{worker_root}/s7-prewarm-f-action-trace-{relationship}.jsonl",
                    f"{worker_root}/s7-warm-f-action-trace-{relationship}.jsonl",
                    f"{worker_root}/s7-measured-f-legacy-wire-trace-{relationship}.jsonl",
                ))
                args = profile_arguments(profile, root="/probe/product",
                                         work="/probe/work", role=f"f-{relationship}")
                worker_inner = (daemon_account +
                                f"chown -R icecc:icecc /probe/work/envs /probe/work/cache-runtime-f-{relationship}; "
                                f"/probe/product/daemon/iceccd -p {scheduler_port + 3 + relationship} "
                                f"-m {TOPOLOGIES[topology][1]} -s {scheduler_host}:{scheduler_port} "
                                f"-n {network} -N {service} -b /probe/work/envs -l /probe/work/f.log "
                                f"-vvv {' '.join(shlex.quote(arg) for arg in args)} "
                                f"2>>/probe/work/f-service.stderr")
                worker = (f"set -eu; root={shlex.quote(staged_roots.get(host, root_mount))}; image={shlex.quote(self.authority['hosts'][host]['image'].get('reference', ''))}; test \"$(docker image inspect --format '{{{{.Id}}}}' \"$image\")\" = {self.authority['hosts'][host]['image']['image_id']}; mkdir -p {worker_root}/envs "
                          f"{worker_root}/cache-runtime-{('f-' + str(relationship))}; chmod 1777 {worker_root} {worker_root}/envs; chmod 700 {worker_root}/cache-runtime-{('f-' + str(relationship))}; "
                          f"for path in {worker_shared_files}; do : >\"$path\"; done; chmod 0666 {worker_shared_files}; "
                          f"docker run -d --name {token}-f-{relationship} --network host --user 0 --cap-add SYS_CHROOT {profile_flag} -e ICECC_P50_RELATIONSHIP={relationship} -e ICECC_P50_F_ACTION_TRACE=/probe/work/s7-warm-f-action-trace-{relationship}.jsonl -e ICECC_P50_F_LEGACY_WIRE_TRACE=/probe/work/s7-measured-f-legacy-wire-trace-{relationship}.jsonl -e ICECC_P50_TEST_READY_TRACE=/probe/work/ready.trace {mounts_for(host)} -v {worker_root}:/probe/work:rw --entrypoint /bin/sh $image -c {shlex.quote(worker_inner)} "
                          f">{worker_root}/container.stdout 2>&1; test \"$(docker inspect --format '{{{{.State.Running}}}}' {token}-f-{relationship})\" = true; docker inspect --format '{{{{.Id}}}}' {token}-f-{relationship} > {worker_root}/container-id; docker inspect --format '{{{{.State.Pid}}}}' {token}-f-{relationship} > {worker_root}/container-pid; "
                          f"printf 'S8_CONTAINER_ID=%s\\n' \"$(cat {worker_root}/container-id)\"")
                self.run(host, worker)
                services.append((host, service))
                f_reset_scripts.append((host, f'''set -eu
test -s {worker_root}/container-id
cp {worker_root}/container-id {worker_root}/rotation-before-id
test -s {worker_root}/ready.trace
cp {worker_root}/ready.trace {worker_root}/ready-before.trace
container_id=$(tr -d '[:space:]' <{worker_root}/container-id)
printf '%s' "$container_id" | grep -Eq '^[0-9a-fA-F]{{12,64}}$'
docker inspect --format '{{{{.State.Running}}}}' "$container_id" | grep -Fx true
field() {{ printf '%s\n' "$1" | awk -v key="$2" '{{for(i=1;i<=NF;i++){{split($i,a,"="); if(a[1]==key){{print a[2]; exit}}}}}}'; }}
before_count=$(grep -c '^READY v2 ' {worker_root}/ready.trace)
before_ready=$(grep '^READY v2 ' {worker_root}/ready.trace | tail -1)
before_pid=$(field "$before_ready" pid)
before_c_guid=$(field "$before_ready" C_STORE_GUID)
before_f_guid=$(field "$before_ready" F_STORE_GUID)
printf '%s' "$before_pid" | grep -Eq '^[0-9]+$'
test -n "$before_c_guid" -a -n "$before_f_guid"
if test -f {worker_root}/s7-warm-f-action-trace-{relationship}.jsonl; then
  mv {worker_root}/s7-warm-f-action-trace-{relationship}.jsonl {worker_root}/setup-f-action-trace-{relationship}.jsonl
fi
: >{worker_root}/s7-warm-f-action-trace-{relationship}.jsonl
: >{worker_root}/s7-measured-f-legacy-wire-trace-{relationship}.jsonl
chmod 0666 {worker_root}/s7-warm-f-action-trace-{relationship}.jsonl {worker_root}/s7-measured-f-legacy-wire-trace-{relationship}.jsonl
docker exec --user 0 "$container_id" /bin/sh -c {shlex.quote(sidecar_kill_inner)} p50-sidecar-kill "$before_pid" F /probe/work/cache-runtime-f-{relationship}
replacement_ready=0
for _ in $(seq 1 300); do
  ready_now=$(grep -c '^READY v2 ' {worker_root}/ready.trace 2>/dev/null || true)
  if test "${{ready_now:-0}}" -gt "$before_count"; then replacement_ready=1; break; fi
  sleep 0.1
done
test "$replacement_ready" -eq 1
after_ready=$(grep '^READY v2 ' {worker_root}/ready.trace | tail -1)
after_pid=$(field "$after_ready" pid)
after_c_guid=$(field "$after_ready" C_STORE_GUID)
after_f_guid=$(field "$after_ready" F_STORE_GUID)
printf '%s' "$after_pid" | grep -Eq '^[0-9]+$'
test "$after_pid" != "$before_pid"
test "$after_c_guid" != "$before_c_guid"
test "$after_f_guid" != "$before_f_guid"
stat -c %s {worker_root}/f.log >{worker_root}/f-measured-log-offset
cp {worker_root}/container-id {worker_root}/rotation-after-id
'''))
                f_prewarm_scripts.append((host,
                    f"set -eu; test -s {worker_root}/s7-warm-f-action-trace-{relationship}.jsonl; "
                    f"cp {worker_root}/s7-warm-f-action-trace-{relationship}.jsonl {worker_root}/s7-prewarm-f-action-trace-{relationship}.jsonl; "
                    f": >{worker_root}/s7-warm-f-action-trace-{relationship}.jsonl; : >{worker_root}/s7-measured-f-legacy-wire-trace-{relationship}.jsonl"))
            first_relationship_for_host = {host: relationship_hosts.index(host)
                                           for host in dict.fromkeys(relationship_hosts)}
            for host, relationship in first_relationship_for_host.items():
                self.run(host, witness, [worker_work(relationship), "before"])
            c_args = profile_arguments(profile, root="/probe/product",
                                       work="/probe/work", role="c")
            client_inner = (daemon_account +
                            "chown -R icecc:icecc /probe/work/envs /probe/work/cache-runtime-c; "
                            f"/probe/product/daemon/iceccd --no-remote -m 0 -p {scheduler_port + 2} "
                            f"-s {scheduler_host}:{scheduler_port} -n {network} -N s8-p50-c "
                            f"-b /probe/work/envs -l /probe/work/c.log -vvv "
                            f"{' '.join(shlex.quote(arg) for arg in c_args)} "
                            f"2>>/probe/work/c-service.stderr")
            client_daemon = (f"set -eu; root={shlex.quote(root_mount)}; image={shlex.quote(self.authority['hosts']['q3']['image'].get('reference', ''))}; test \"$(docker image inspect --format '{{{{.Id}}}}' \"$image\")\" = {self.authority['hosts']['q3']['image']['image_id']}; mkdir -p {client_work}/envs {client_work}/cache-runtime-c; "
                             f"chmod 1777 {client_work}/envs; chmod 700 {client_work}/cache-runtime-c; "
                             f"docker run -d --name {token}-c --pid=host --network host --user 0 {profile_flag} -e ICECC_TEST_SOCKET=/probe/work/client.sock -e ICECC_P50_C_ACTION_TRACE=/probe/work/s7-warm-c-action-trace.jsonl -e ICECC_P50_C_LEGACY_WIRE_TRACE=/probe/work/s7-measured-c-legacy-wire-trace.jsonl -e ICECC_P50_TEST_READY_TRACE=/probe/work/ready-c.trace {docker_mounts} -v {client_work}:/probe/work:rw -v {client_work}:{client_work}:rw --entrypoint /bin/sh $image -c {shlex.quote(client_inner)} "
                             f">{client_work}/c.stdout 2>&1; test \"$(docker inspect --format '{{{{.State.Running}}}}' {token}-c)\" = true; docker inspect --format '{{{{.Id}}}}' {token}-c > {client_work}/c.container-id; docker inspect --format '{{{{.State.Pid}}}}' {token}-c > {client_work}/c.pid; "
                             f"printf 'S8_CONTAINER_ID=%s\\n' \"$(cat {client_work}/c.container-id)\"")
            client_shared_files = " ".join((
                f"{client_work}/c.log",
                f"{client_work}/c-service.stderr",
                f"{client_work}/ready-c.trace",
                f"{client_work}/s7-prewarm-c-action-trace.jsonl",
                f"{client_work}/s7-prewarm-f-action-trace.jsonl",
                f"{client_work}/s7-warm-c-action-trace.jsonl",
                f"{client_work}/s7-measured-c-legacy-wire-trace.jsonl",
            ))
            self.run("q3", f"set -eu; mkdir -p {client_work}; "
                            f"for path in {client_shared_files}; do : >\"$path\"; done; "
                            f"chmod 0666 {client_shared_files}")
            self.run("q3", client_daemon)
            reset_path = f"{client_work}/reset-hook.sh"
            reset_lines = ["#!/bin/sh", "set -eu", "work=$1", "suite=$2", "profile=$3",
                           f'''rm -f {client_work}/external-reset.ready
test -s {client_work}/c.container-id
cp {client_work}/c.container-id {client_work}/c-rotation-before-id
test -s {client_work}/c.pid
cp {client_work}/c.pid {client_work}/c-rotation-before-pid
test -s {client_work}/ready-c.trace
cp {client_work}/ready-c.trace {client_work}/ready-c-before.trace
container_id=$(tr -d '[:space:]' <{client_work}/c.container-id)
printf '%s' "$container_id" | grep -Eq '^[0-9a-fA-F]{{12,64}}$'
docker inspect --format '{{{{.State.Running}}}}' "$container_id" | grep -Fx true
field() {{ printf '%s\n' "$1" | awk -v key="$2" '{{for(i=1;i<=NF;i++){{split($i,a,"="); if(a[1]==key){{print a[2]; exit}}}}}}'; }}
before_count=$(grep -c '^READY v2 ' {client_work}/ready-c.trace)
before_ready=$(grep '^READY v2 ' {client_work}/ready-c.trace | tail -1)
before_pid=$(field "$before_ready" pid)
before_c_guid=$(field "$before_ready" C_STORE_GUID)
before_f_guid=$(field "$before_ready" F_STORE_GUID)
printf '%s' "$before_pid" | grep -Eq '^[0-9]+$'
test -n "$before_c_guid" -a -n "$before_f_guid"
if test -f {client_work}/s7-warm-c-action-trace.jsonl; then
  mv {client_work}/s7-warm-c-action-trace.jsonl {client_work}/setup-c-action-trace.jsonl
fi
: >{client_work}/s7-warm-c-action-trace.jsonl
: >{client_work}/s7-measured-c-legacy-wire-trace.jsonl
chmod 0666 {client_work}/s7-warm-c-action-trace.jsonl {client_work}/s7-measured-c-legacy-wire-trace.jsonl
docker exec --user 0 "$container_id" /bin/sh -c {shlex.quote(sidecar_kill_inner)} p50-sidecar-kill "$before_pid" C /probe/work/cache-runtime-c
replacement_ready=0
for _ in $(seq 1 300); do
  ready_now=$(grep -c '^READY v2 ' {client_work}/ready-c.trace 2>/dev/null || true)
  if test "${{ready_now:-0}}" -gt "$before_count"; then replacement_ready=1; break; fi
  sleep 0.1
done
test "$replacement_ready" -eq 1
after_ready=$(grep '^READY v2 ' {client_work}/ready-c.trace | tail -1)
after_pid=$(field "$after_ready" pid)
after_c_guid=$(field "$after_ready" C_STORE_GUID)
after_f_guid=$(field "$after_ready" F_STORE_GUID)
printf '%s' "$after_pid" | grep -Eq '^[0-9]+$'
test "$after_pid" != "$before_pid"
test "$after_c_guid" != "$before_c_guid"
test "$after_f_guid" != "$before_f_guid"
cp {client_work}/c.container-id {client_work}/c-rotation-after-id
printf 'S8_SIDECAR_ROTATION role=C relationship=0 before_pid=%s after_pid=%s before_c_store_guid=%s after_c_store_guid=%s before_f_store_guid=%s after_f_store_guid=%s\n' "$before_pid" "$after_pid" "$before_c_guid" "$after_c_guid" "$before_f_guid" "$after_f_guid" >{client_work}/external-rotation-evidence
''']
            reset_lines.extend([f"for _ in $(seq 1 600); do test -f {client_work}/external-f-reset.ready && break; sleep 0.2; done",
                                f"test -f {client_work}/external-f-reset.ready"])
            if profile == "RAW_II":
                reset_lines.extend([f"for _ in $(seq 1 60); do test $(grep -c login {client_work}/scheduler.log 2>/dev/null || true) -ge {TOPOLOGIES[topology][0] + 1} && break; sleep 1; done"])
            else:
                advertisement = {"P29": "p29", "ZSTD_TU": "zstd_tu", "ZSTD_ROUTE": "z3_long", "GRZ_RESIDUAL": "grz"}[profile]
                reset_lines.extend([f"for _ in $(seq 1 60); do test $(grep -E 'RELOGIN p50-f(-[0-9]+)?.*cache=.*cache_profiles=.*{advertisement}' {client_work}/scheduler.log 2>/dev/null | grep -oE 'p50-f(-[0-9]+)?' | sort -u | wc -l) -ge {TOPOLOGIES[topology][0]} && break; sleep 1; done"])
            reset_lines.extend([f"touch {client_work}/external-reset.ready"])
            # The shell itself runs in the pinned image and therefore does
            # not receive the host Docker socket or SSH agent.  A bounded q3
            # host watcher performs reset/collection after marker requests;
            # the image-local hooks only signal and wait.
            reset_worker_path = f"/tmp/{token}-reset-worker.sh"
            reset_worker_lines = reset_lines[:2] + [
                f"while test ! -f {client_work}/external-reset.request; do sleep 0.2; done"] + reset_lines[2:]
            encoded = base64.b64encode(("\n".join(reset_worker_lines) + "\n").encode()).decode()
            self.run("q3", f"printf %s {shlex.quote(encoded)} | base64 -d >{reset_worker_path}; chmod 700 {reset_worker_path}")
            supervisor_failure_path = f"{client_work}/external-supervisor.failed"
            reset_hook = marker_wait_hook(
                f"{client_work}/external-reset.request",
                f"{client_work}/external-reset.ready", supervisor_failure_path)
            hook_encoded = base64.b64encode(reset_hook.encode()).decode()
            self.run("q3", f"printf %s {shlex.quote(hook_encoded)} | base64 -d >{reset_path}; chmod 700 {reset_path}")
            collect_path = f"{client_work}/collect-hook.sh"
            collect_lines = ["#!/bin/sh", "set -eu", "work=$1", "suite=$2", "profile=$3",
                             f"for _ in $(seq 1 600); do test -f {client_work}/external-f-traces.ready && break; sleep 0.2; done",
                             f"test -f {client_work}/external-f-traces.ready"]
            collect_worker_path = f"/tmp/{token}-collect-worker.sh"
            collect_worker_lines = collect_lines[:2] + [
                f"while test ! -f {client_work}/external-f-traces.request; do sleep 0.2; done"] + collect_lines[2:]
            collect_encoded = base64.b64encode(("\n".join(collect_worker_lines) + "\n").encode()).decode()
            self.run("q3", f"printf %s {shlex.quote(collect_encoded)} | base64 -d >{collect_worker_path}; chmod 700 {collect_worker_path}")
            collect_hook = marker_wait_hook(
                f"{client_work}/external-f-traces.request",
                f"{client_work}/external-f-traces.ready", supervisor_failure_path)
            hook_encoded = base64.b64encode(collect_hook.encode()).decode()
            self.run("q3", f"printf %s {shlex.quote(hook_encoded)} | base64 -d >{collect_path}; chmod 700 {collect_path}")
            prewarm_path = f"{client_work}/prewarm-hook.sh"
            prewarm_hook = marker_wait_hook(
                f"{client_work}/external-prewarm.request",
                f"{client_work}/external-prewarm.ready", supervisor_failure_path)
            prewarm_encoded = base64.b64encode(prewarm_hook.encode()).decode()
            self.run("q3", f"printf %s {shlex.quote(prewarm_encoded)} | base64 -d >{prewarm_path}; chmod 700 {prewarm_path}")
            self.run("q3", f"rm -f {client_work}/external-reset.request {client_work}/external-f-traces.request {client_work}/external-reset.ready {client_work}/external-f-traces.ready {supervisor_failure_path}; "
                              f"nohup {reset_worker_path} {client_work} {topology} {profile} >{client_work}/reset-worker.stdout 2>&1 & echo $! >{client_work}/reset-worker.pid; "
                              f"nohup {collect_worker_path} {client_work} {topology} {profile} >{client_work}/collect-worker.stdout 2>&1 & echo $! >{client_work}/collect-worker.pid")
            def supervise_external_markers() -> None:
                reset_done = prewarm_done = collection_done = False
                deadline = time.monotonic() + self.timeout
                def record_failure(message: str) -> None:
                    supervisor_errors.append(message)
                    encoded_error = base64.b64encode((message + "\n").encode()).decode()
                    try:
                        s4.run_script(
                            "q3",
                            f"printf %s {shlex.quote(encoded_error)} | base64 -d >"
                            f"{shlex.quote(supervisor_failure_path)}",
                            timeout=30)
                    except (OSError, subprocess.SubprocessError):
                        pass
                while time.monotonic() < deadline and not supervisor_stop.is_set():
                    def exists(path: str) -> bool:
                        result = s4.run_script("q3", "test -f \"$1\"", [path], timeout=30)
                        return result.returncode == 0
                    try:
                        if not reset_done and exists(f"{client_work}/external-reset.request"):
                            for host, script in f_reset_scripts:
                                self.run(host, script)
                            rotation_lines: list[str] = []
                            for relationship, (host, _service) in enumerate(services):
                                worker_root = worker_work(relationship)
                                rotation = self.run(host, f"set -eu; before_container=$(cat {worker_root}/rotation-before-id); after_container=$(cat {worker_root}/rotation-after-id); test \"$before_container\" = \"$after_container\"; before_ready=$(grep '^READY v2 ' {worker_root}/ready-before.trace | tail -1); after_ready=$(grep '^READY v2 ' {worker_root}/ready.trace | tail -1); field() {{ printf '%s\\n' \"$1\" | awk -v key=\"$2\" '{{for(i=1;i<=NF;i++){{split($i,a,\"=\"); if(a[1]==key){{print a[2]; exit}}}}}}'; }}; before_pid=$(field \"$before_ready\" pid); after_pid=$(field \"$after_ready\" pid); test -n \"$before_pid\" -a -n \"$after_pid\" -a \"$before_pid\" != \"$after_pid\"; printf 'S8_F_READY before_pid=%s after_pid=%s before=%s after=%s\\n' \"$before_pid\" \"$after_pid\" \"$before_ready\" \"$after_ready\"")
                                match = re.search(r"S8_F_READY before_pid=([0-9]+) after_pid=([0-9]+) before=(READY v2 .*?) after=(READY v2 .*)", rotation.stdout)
                                if match is None:
                                    raise ExternalFarmError(f"evidence:f_rotation_ready_missing:{relationship}")
                                def ready_field(line: str, key: str) -> str:
                                    found = re.search(rf"(?:^| )" + re.escape(key) + r"=([^ ]+)", line)
                                    if found is None:
                                        raise ExternalFarmError(f"evidence:f_rotation_field_missing:{relationship}:{key}")
                                    return found.group(1)
                                before, after = match.group(3), match.group(4)
                                c_before = ready_field(before, "C_STORE_GUID")
                                c_after = ready_field(after, "C_STORE_GUID")
                                f_before = ready_field(before, "F_STORE_GUID")
                                f_after = ready_field(after, "F_STORE_GUID")
                                rotation_lines.append(
                                    f"S8_SIDECAR_ROTATION role=F relationship={relationship} before_pid={match.group(1)} after_pid={match.group(2)} "
                                    f"before_c_store_guid={c_before} after_c_store_guid={c_after} "
                                    f"before_f_store_guid={f_before} after_f_store_guid={f_after}")
                            encoded_rotation = base64.b64encode(("\n".join(rotation_lines) + "\n").encode()).decode()
                            self.run("q3", f"printf %s {shlex.quote(encoded_rotation)} | base64 -d >>{client_work}/external-rotation-evidence")
                            self.run("q3", f"touch {client_work}/external-f-reset.ready")
                            reset_done = True
                        if not prewarm_done and exists(f"{client_work}/external-prewarm.request"):
                            for host, script in f_prewarm_scripts:
                                self.run(host, script)
                            self.run("q3", f"test -s {client_work}/s7-warm-c-action-trace.jsonl; cp {client_work}/s7-warm-c-action-trace.jsonl {client_work}/s7-prewarm-c-action-trace.jsonl; : >{client_work}/s7-warm-c-action-trace.jsonl; touch {client_work}/external-prewarm.ready")
                            prewarm_done = True
                        if not collection_done and exists(f"{client_work}/external-f-traces.request"):
                            destinations: list[str] = []
                            for relationship, (host, _service) in enumerate(services):
                                worker_root = worker_work(relationship)
                                destination = f"{client_work}/f-trace-{relationship}.jsonl"
                                source = subprocess.Popen(
                                    [*s4.ssh_argv(host), "cat", f"{worker_root}/s7-warm-f-action-trace-{relationship}.jsonl"],
                                    stdout=subprocess.PIPE, stderr=subprocess.PIPE)
                                assert source.stdout is not None
                                sink = subprocess.Popen(
                                    [*s4.ssh_argv("q3"), "bash", "-c", f"cat >{shlex.quote(destination)}"],
                                    stdin=source.stdout, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
                                source.stdout.close()
                                _, sink_error = sink.communicate(timeout=self.timeout)
                                source_error = source.stderr.read() if source.stderr else b""
                                if source.wait(timeout=30) != 0 or sink.returncode != 0:
                                    raise ExternalFarmError("external:f_trace_copy_failed:" +
                                                             (source_error + sink_error)[-300:].decode(errors="replace"))
                                destinations.append(destination)
                                log_destination = f"{client_work}/f.log" if len(services) == 1 else f"{client_work}/f-{relationship}.log"
                                log_source = subprocess.Popen(
                                    [*s4.ssh_argv(host), "cat", f"{worker_root}/f.log"],
                                    stdout=subprocess.PIPE, stderr=subprocess.PIPE)
                                assert log_source.stdout is not None
                                log_sink = subprocess.Popen(
                                    [*s4.ssh_argv("q3"), "bash", "-c", f"cat >{shlex.quote(log_destination)}"],
                                    stdin=log_source.stdout, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
                                log_source.stdout.close()
                                _, log_sink_error = log_sink.communicate(timeout=self.timeout)
                                log_source_error = log_source.stderr.read() if log_source.stderr else b""
                                if log_source.wait(timeout=30) != 0 or log_sink.returncode != 0:
                                    raise ExternalFarmError("external:f_log_copy_failed:" +
                                                             (log_source_error + log_sink_error)[-300:].decode(errors="replace"))
                                ready_destination = (f"{client_work}/ready-f.trace" if len(services) == 1
                                                     else f"{client_work}/ready-f-{relationship}.trace")
                                ready_source = subprocess.Popen(
                                    [*s4.ssh_argv(host), "cat", f"{worker_root}/ready.trace"],
                                    stdout=subprocess.PIPE, stderr=subprocess.PIPE)
                                assert ready_source.stdout is not None
                                ready_sink = subprocess.Popen(
                                    [*s4.ssh_argv("q3"), "bash", "-c", f"cat >{shlex.quote(ready_destination)}"],
                                    stdin=ready_source.stdout, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
                                ready_source.stdout.close()
                                _, ready_sink_error = ready_sink.communicate(timeout=self.timeout)
                                ready_source_error = ready_source.stderr.read() if ready_source.stderr else b""
                                if ready_source.wait(timeout=30) != 0 or ready_sink.returncode != 0:
                                    raise ExternalFarmError("external:ready_trace_copy_failed:" +
                                                             (ready_source_error + ready_sink_error)[-300:].decode(errors="replace"))
                                prewarm_source = f"{worker_root}/s7-prewarm-f-action-trace-{relationship}.jsonl"
                                prewarm_destination = f"{client_work}/prewarm-f-trace-{relationship}.jsonl"
                                prewarm = subprocess.Popen(
                                    [*s4.ssh_argv(host), "cat", prewarm_source],
                                    stdout=subprocess.PIPE, stderr=subprocess.PIPE)
                                assert prewarm.stdout is not None
                                prewarm_sink = subprocess.Popen(
                                    [*s4.ssh_argv("q3"), "bash", "-c", f"cat >{shlex.quote(prewarm_destination)}"],
                                    stdin=prewarm.stdout, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
                                prewarm.stdout.close()
                                _, prewarm_error = prewarm_sink.communicate(timeout=self.timeout)
                                prewarm_source_error = prewarm.stderr.read() if prewarm.stderr else b""
                                if prewarm.wait(timeout=30) != 0 or prewarm_sink.returncode != 0:
                                    raise ExternalFarmError("external:prewarm_f_trace_copy_failed:" +
                                                             (prewarm_source_error + prewarm_error)[-300:].decode(errors="replace"))
                                if profile == "RAW_II":
                                    legacy_destination = f"{client_work}/s7-measured-f-legacy-wire-trace-{relationship}.jsonl"
                                    legacy_source = subprocess.Popen(
                                        [*s4.ssh_argv(host), "cat", f"{worker_root}/s7-measured-f-legacy-wire-trace-{relationship}.jsonl"],
                                        stdout=subprocess.PIPE, stderr=subprocess.PIPE)
                                    assert legacy_source.stdout is not None
                                    legacy_sink = subprocess.Popen(
                                        [*s4.ssh_argv("q3"), "bash", "-c", f"cat >{shlex.quote(legacy_destination)}"],
                                        stdin=legacy_source.stdout, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
                                    legacy_source.stdout.close()
                                    legacy_sink.communicate(timeout=self.timeout)
                                    if legacy_source.wait(timeout=30) != 0 or legacy_sink.returncode != 0:
                                        raise ExternalFarmError("external:raw_wire_copy_failed")
                            aggregate = f": >{client_work}/s7-warm-f-action-trace.jsonl; " + "; ".join(
                                f"cat {shlex.quote(path)} >>{client_work}/s7-warm-f-action-trace.jsonl" for path in destinations)
                            map_script = ""
                            if profile != "RAW_II":
                                aggregate += "; : >" + client_work + "/s7-prewarm-f-action-trace.jsonl; " + "; ".join(
                                    f"cat {shlex.quote(client_work + '/prewarm-f-trace-' + str(i) + '.jsonl')} >>{client_work}/s7-prewarm-f-action-trace.jsonl"
                                    for i in range(len(destinations)))
                                map_script = f_service_map_script(
                                    client_work, len(destinations))
                            completion = aggregate + (("\n" + map_script) if map_script else "")
                            self.run("q3", completion +
                                     f"\ntouch {client_work}/external-f-traces.ready")
                            collection_done = True
                    except (ExternalFarmError, OSError, subprocess.SubprocessError) as exc:
                        record_failure(str(exc))
                        return
                    time.sleep(0.2)
                if not supervisor_stop.is_set() and (not reset_done or not collection_done):
                    record_failure("external:marker_supervisor_timeout")
            supervisor = threading.Thread(target=supervise_external_markers, daemon=True)
            supervisor.start()
            self.run("q3", readiness_gate(profile, f"{client_work}/scheduler.log",
                                            relationships=TOPOLOGIES[topology][0]))
            external_env = [f"ICECC_P50_C1F1_WORKDIR={client_work}",
                            f"ICECC_P50_EXTERNAL_SCHED_PORT={scheduler_port}",
                            f"ICECC_P50_EXTERNAL_SCHEDULER_LOG={client_work}/scheduler.log",
                            "ICECC_P50_EXTERNAL_FARM=1",
                            f"ICECC_P50_EXTERNAL_EXECUTION_ID={token}",
                            f"ICECC_P50_EXTERNAL_MANIFEST_SHA256={manifest_sha}",
                            f"ICECC_P50_EXTERNAL_AUTHORITY_SHA256={authority_sha}",
                            f"ICECC_P50_EXTERNAL_START_UTC={dt.datetime.fromtimestamp(execution_start_ns / 1e9, dt.timezone.utc).strftime('%Y-%m-%dT%H:%M:%SZ')}",
                            f"ICECC_P50_EXTERNAL_RESET_HOOK={reset_path}",
                            f"ICECC_P50_EXTERNAL_RESET_READY={client_work}/external-reset.ready",
                            f"ICECC_P50_EXTERNAL_COLLECT_HOOK={collect_path}",
                            f"ICECC_P50_EXTERNAL_COLLECT_READY={client_work}/external-f-traces.ready",
                            f"ICECC_P50_EXTERNAL_PREWARM_HOOK={prewarm_path}",
                            f"ICECC_P50_EXTERNAL_PREWARM_READY={client_work}/external-prewarm.ready"]
            command = list(batch_command)
            if command[0] == "env":
                # build_command places its ordinary workdir assignment near
                # the tail; remove it so the single precreated external root
                # below is authoritative rather than shadowed by a later
                # duplicate environment entry.
                command = ["env", *external_env,
                           *[item for item in command[1:]
                             if not str(item).startswith("ICECC_P50_C1F1_WORKDIR=")]]
            # C and the compiler wrapper exchange an absolute CLOCK_MONOTONIC
            # deadline whose wire identity includes /proc/self/ns/time.
            # ``--pid=host`` does not make two sibling Docker containers share
            # a time namespace, so execute the batch inside C's already pinned
            # container.  The second C bind above exposes the unique evidence
            # root at the same absolute path expected by the mature runner.
            batch = (f"docker exec --user 0 {token}-c /bin/sh -c "
                     f"{shlex.quote(shlex.join([str(item) for item in command]))}")
            result = self.run("q3", batch)
            supervisor_stop.set()
            supervisor.join(timeout=30)
            if supervisor_errors:
                raise ExternalFarmError(supervisor_errors[0])
            witness_values: list[dict[str, int]] = []
            for host, relationship in first_relationship_for_host.items():
                after = self.run(host, witness, [worker_work(relationship), "after"])
                match = re.search(r"before=([0-9]+) after=([0-9]+)", after.stdout)
                if not match:
                    raise ExternalFarmError("evidence:interference_witness_missing")
                delta = interference_delta(int(match.group(1)), int(match.group(2)))
                witness_values.append({"host": host, "relationship": relationship,
                                       "delta_ticks": delta})
            metrics = observed_batch_metrics(result.stdout, topology)
            overlap_required(topology, metrics)
            # Preserve the daemon work trees before unique-resource cleanup;
            # these contain C/F traces, scheduler log, and the F service map.
            # The finalizer binds S7_WORKDIR to this exact absolute path, so
            # retain q3's tree at the same private path used by the client.
            _copy_remote_tree(
                "q3", client_work, Path(client_work),
                str(self.authority["hosts"]["q3"]["image"]["reference"]), self.timeout)
            for relationship, host in enumerate(relationship_hosts):
                _copy_remote_tree(host, worker_work(relationship),
                                  output / f"remote-f-{relationship}",
                                  str(self.authority["hosts"][host]["image"]["reference"]),
                                  self.timeout)
                f_evidence = output / f"remote-f-{relationship}"
                f_log = f_evidence / "f.log"
                f_offset = f_evidence / "f-measured-log-offset"
                if not f_log.is_file() or not f_offset.is_file():
                    raise ExternalFarmError(f"evidence:f_log_missing:{relationship}")
                try:
                    offset = int(f_offset.read_text(encoding="utf-8").strip())
                    measured_log = f_log.read_bytes()[offset:]
                except (OSError, UnicodeError, ValueError) as exc:
                    raise ExternalFarmError(f"evidence:f_log_invalid:{relationship}") from exc
                if re.search(rb"start_install_environment|handle_transfer_env", measured_log):
                    raise ExternalFarmError(f"evidence:f_environment_install_measured:{relationship}")
            cohort = cohort_digest(self.authority, topology, relationship_hosts)
            (output / "calibration-metadata.json").write_text(
                json.dumps({"host_digest": cohort, "cohort_digest": cohort,
                            "topology": topology, "role_placement": placement},
                           sort_keys=True, separators=(",", ":")) + "\n", encoding="utf-8")
            started_utc = dt.datetime.fromtimestamp(execution_start_ns / 1e9, dt.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")
            execution_id = token
            start_marker = (f"S8_EXTERNAL_FARM_EXECUTION execution_id={execution_id} "
                            f"manifest_sha256={manifest_sha} authority_sha256={authority_sha} "
                            f"phase=start timestamp={started_utc}")
            stdout_text = result.stdout
            start_rows = [line for line in stdout_text.splitlines()
                          if line.startswith("S8_EXTERNAL_FARM_EXECUTION ") and "phase=start " in line]
            if start_rows and start_rows != [start_marker]:
                raise ExternalFarmError("evidence:start_marker_mismatch")
            if not start_rows:
                stdout_text = start_marker + "\n" + stdout_text
            stdout_path = output / "product-output.log"
            stdout_path.write_text(stdout_text, encoding="utf-8", newline="")
            scheduler_id, scheduler_pid = _retained_identity(Path(client_work), "scheduler.container-id")
            client_id, client_pid = _retained_identity(Path(client_work), "c.container-id")
            worker_identities = []
            for relationship, (host, service) in enumerate(services):
                worker_root = output / f"remote-f-{relationship}"
                worker_id, worker_pid = _retained_identity(worker_root, "container-id")
                worker_identities.append({"relationship": relationship, "host": host,
                                          "service": service,
                                          "physical_host_digest": self.authority["hosts"][host]["physical_host_digest"],
                                          "boot_id_digest": self.authority["hosts"][host]["boot_id_digest"],
                                          "pid": worker_pid, "container_id": worker_id})
            receipt_context = {
                "execution_id": execution_id, "started_at": started_utc,
                "finished_at": None, "start_marker": start_marker,
                "end_marker": None, "manifest_sha256": manifest_sha,
                "authority_sha256": authority_sha, "stdout_path": str(stdout_path),
                "stdout_sha256": _sha(stdout_path)[0], "stdout_bytes": _sha(stdout_path)[1],
                "workdir": client_work,
                "scheduler": {"host": "q3", "service": "p50-scheduler",
                               "physical_host_digest": self.authority["hosts"]["q3"]["physical_host_digest"],
                               "boot_id_digest": self.authority["hosts"]["q3"]["boot_id_digest"],
                               "pid": scheduler_pid, "container_id": scheduler_id},
                "client": {"host": "q3", "service": "p50-c",
                            "physical_host_digest": self.authority["hosts"]["q3"]["physical_host_digest"],
                            "boot_id_digest": self.authority["hosts"]["q3"]["boot_id_digest"],
                            "pid": client_pid, "container_id": client_id},
                "workers": worker_identities,
            }
            (output / "role-placement.json").write_text(
                json.dumps(placement, sort_keys=True, separators=(",", ":")) + "\n",
                encoding="utf-8")
            result_payload = {"schema": EXTERNAL_SCHEMA, "status": "PASS",
                              "role_placement": placement, "external_farm": manifest,
                              "batch_metrics": metrics,
                              "interference_witness": witness_values,
                              "retained_artifacts": ["external-authority.json", "external-farm.json",
                                                     "role-placement.json", "calibration-metadata.json",
                                                     "external-farm-receipt.json", "product-output.log"],
                              "output": str(output)}
            receipt_context["result_payload"] = result_payload
            return result_payload
        except BaseException as exc:
            primary_error = exc
            raise
        finally:
            supervisor_stop.set()
            if supervisor is not None:
                supervisor.join(timeout=30)
            # Address only exact IDs recorded in this invocation's evidence
            # roots.  A cleanup transport failure must not replace a batch
            # result or match an unrelated process.
            # Stop helper loops and data-plane roles before S.  Killing S
            # first makes every still-live daemon enter its reconnect path
            # and can burn a core while cleanup works through the list.
            targets = [("q3", f"{client_work}/reset-worker.pid"),
                       ("q3", f"{client_work}/collect-worker.pid"),
                       ("q3", f"{client_work}/c.container-id")]
            targets.extend((host, f"{worker_work(i)}/container-id")
                           for i, (host, _service) in enumerate(services))
            targets.append(("q3", f"{client_work}/scheduler.container-id"))
            cleanup = '''set -eu
path=$1
test -f "$path" || exit 0
id=$(tr -d '[:space:]' <"$path")
printf '%s' "$id" | grep -Eq '^[0-9a-fA-F]{12,64}$' || exit 77
docker rm -f "$id" >/dev/null 2>&1 || true
'''
            for host, path in targets:
                try:
                    if path.endswith(".pid"):
                        self.run(host, '''set -eu
path=$1; test -f "$path" || exit 0
pid=$(tr -d '[:space:]' <"$path")
printf '%s' "$pid" | grep -Eq '^[0-9]+$' || exit 77
kill "$pid" 2>/dev/null || true
''', [path])
                    else:
                        self.run(host, cleanup, [path])
                except ExternalFarmError as exc:
                    cleanup_errors.append(f"{host}:{path}:{exc}")
            if primary_error is not None:
                retained_failure: list[str] = []
                failure_copy_errors: list[str] = []
                failure_trees = [("q3", client_work, "q3-workdir")]
                failure_trees.extend(
                    (host, worker_work(i), f"f-{i}-workdir")
                    for i, (host, _service) in enumerate(services))
                for host, remote, name in failure_trees:
                    destination = output / "failure-diagnostics" / name
                    try:
                        _copy_remote_tree(
                            host, remote, destination,
                            str(self.authority["hosts"][host]["image"]["reference"]),
                            self.timeout)
                        retained_failure.append(str(destination.relative_to(output)))
                    except ExternalFarmError as exc:
                        failure_copy_errors.append(f"{host}:{remote}:{exc}")
                failure_record = {
                    "schema": "icecream-s8-external-farm-failure-v1",
                    "status": "FAIL",
                    "error_type": type(primary_error).__name__,
                    "error": str(primary_error),
                    "retained_diagnostics": retained_failure,
                    "diagnostic_copy_errors": failure_copy_errors,
                }
                (output / "failure.json").write_text(
                    json.dumps(failure_record, sort_keys=True, separators=(",", ":")) + "\n",
                    encoding="utf-8")
            cleanup_paths = [("q3", client_work),
                             *[(host, worker_work(i)) for i, (host, _service) in enumerate(services)],
                             *[(host, f"/tmp/{token}-tanksmall")
                               for host in dict.fromkeys(("q3", *relationship_hosts))],
                             ("q3", f"/tmp/{token}-reset-worker.sh"),
                             ("q3", f"/tmp/{token}-collect-worker.sh")]
            remove_path = r'''set -eu
path=$1; image=$2
case $path in
  /tmp/s8ext-*|/tmp/p50compilee2e.external.*|/tmp/s4-p50-fourhost-f*.*) ;;
  *) exit 77;;
esac
test ! -L "$path" || exit 77
test -e "$path" || exit 0
if test -f "$path"; then
  rm -f -- "$path"
  exit 0
fi
test -d "$path" || exit 77
docker run --rm --network none --user 0 -v "$path:/probe/cleanup:rw" \
  --entrypoint /bin/sh "$image" -c 'find /probe/cleanup -mindepth 1 -delete'
rmdir -- "$path"
'''
            for host, path in cleanup_paths:
                try:
                    self.run(host, remove_path,
                             [path, self.authority["hosts"][host]["image"]["reference"]])
                except ExternalFarmError as exc:
                    cleanup_errors.append(f"{host}:{path}:{exc}")
            if receipt_context is not None and not cleanup_errors:
                finished_ns = time.time_ns()
                finished_at = dt.datetime.fromtimestamp(finished_ns / 1e9, dt.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")
                end_marker = (f"S8_EXTERNAL_FARM_EXECUTION execution_id={receipt_context['execution_id']} "
                              f"manifest_sha256={receipt_context['manifest_sha256']} authority_sha256={receipt_context['authority_sha256']} "
                              f"phase=end timestamp={finished_at}")
                stdout_path = Path(receipt_context["stdout_path"])
                with stdout_path.open("a", encoding="utf-8", newline="") as stream:
                    stream.write(end_marker + "\n")
                stdout_sha, stdout_bytes = _sha(stdout_path)
                receipt_context["finished_at"] = finished_at
                receipt_context["end_marker"] = end_marker
                receipt_context["stdout_sha256"] = stdout_sha
                receipt_context["stdout_bytes"] = stdout_bytes
                receipt = {"schema": "icecream-s8-external-farm-receipt-v1",
                           "mode": "external_farm", "suite": topology,
                           "manifest_sha256": receipt_context["manifest_sha256"],
                           "authority_sha256": receipt_context["authority_sha256"],
                           "stdout": {"path": receipt_context["stdout_path"],
                                      "sha256": receipt_context["stdout_sha256"],
                                      "bytes": receipt_context["stdout_bytes"]},
                           "workdir": receipt_context["workdir"],
                           "execution": {"execution_id": receipt_context["execution_id"],
                                         "scheduler": receipt_context["scheduler"],
                                         "client": receipt_context["client"],
                                         "workers": receipt_context["workers"],
                                         "started_at": receipt_context["started_at"],
                                         "finished_at": receipt_context["finished_at"],
                                         "start_marker": receipt_context["start_marker"],
                                         "end_marker": receipt_context["end_marker"],
                                         "artifacts": {"collection": "complete",
                                                        "cleanup": "complete" if not cleanup_errors else "failed"}}}
                (output / "external-farm-receipt.json").write_text(
                    json.dumps(receipt, sort_keys=True, separators=(",", ":")) + "\n",
                    encoding="utf-8")
                if "result_payload" in receipt_context:
                    receipt_path = output / "external-farm-receipt.json"
                    receipt_sha, receipt_bytes = _sha(receipt_path)
                    receipt_context["result_payload"]["finalizer_input"] = {
                        "manifest_path": str(output / "external-farm.json"),
                        "manifest_sha256": manifest_sha,
                        "authority_path": str(output / "external-authority.json"),
                        "authority_sha256": authority_sha,
                        "workdir": client_work,
                        "stdout_path": receipt_context["stdout_path"],
                        "stdout_sha256": receipt_context["stdout_sha256"],
                        "stdout_bytes": receipt_context["stdout_bytes"],
                        "receipt_path": str(receipt_path),
                        "receipt_sha256": receipt_sha,
                        "receipt_bytes": receipt_bytes,
                    }
            _finish_cleanup(cleanup_errors, primary_error)


def build_plan(authority: Mapping[str, Any], topology: str,
               relationship_hosts: Sequence[str], profile: str) -> dict[str, Any]:
    placement = role_placement(authority, topology, relationship_hosts)
    return {"schema": EXTERNAL_SCHEMA, "suite": topology, "profile": profile,
            "role_placement": placement, "relationship_hosts": list(relationship_hosts),
            "control": {"submission_host": "q3", "scheduler_host": "q3"},
            "workers": [{"relationship": i, "host": host, "service":
                          "p50-f" if len(relationship_hosts) == 1 else f"p50-f-{i}",
                          "slots": TOPOLOGIES[topology][1]}
                         for i, host in enumerate(relationship_hosts)]}


def selected_hosts(authority: Mapping[str, Any], topology: str,
                   requested: Sequence[str] | None) -> list[str]:
    """Use the authenticated authority mapping unless explicitly overridden."""
    if requested is None:
        try:
            values = authority["placements"][topology]["relationship_hosts"]
        except (KeyError, TypeError) as exc:
            raise ExternalFarmError("authority:placement_mapping_missing") from exc
        if not isinstance(values, list):
            raise ExternalFarmError("authority:placement_mapping_invalid")
        return list(values)
    requested_values = list(requested)
    if requested_values != list(authority["placements"][topology]["relationship_hosts"]):
        raise ExternalFarmError("placement:explicit_mapping_differs_from_authority")
    return requested_values


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--authority", type=Path, required=True)
    parser.add_argument("--topology", choices=tuple(TOPOLOGIES), required=True)
    parser.add_argument("--profile", choices=PROFILES, required=True)
    parser.add_argument("--relationship-host", action="append", dest="relationship_hosts")
    parser.add_argument("--batch-manifest", type=Path)
    parser.add_argument("--predictive-plan", type=Path)
    parser.add_argument("--topology-file", type=Path)
    parser.add_argument("--repeat-predictive-plan", type=Path)
    parser.add_argument("--product-root", type=Path)
    parser.add_argument("--product-root-remote")
    parser.add_argument("--output", type=Path)
    parser.add_argument("--corpus", default="DuckDB")
    parser.add_argument("--regime", default="cold")
    parser.add_argument("--depth", default="100")
    parser.add_argument("--passes", type=int, choices=(1, 2), default=1)
    parser.add_argument("--execute", action="store_true")
    args = parser.parse_args(argv)
    try:
        authority = load_authority(args.authority.absolute())
        count = TOPOLOGIES[args.topology][0]
        hosts = selected_hosts(authority, args.topology, args.relationship_hosts)
        if len(hosts) != count:
            raise ExternalFarmError("placement:relationship_count_invalid")
        plan = build_plan(authority, args.topology, hosts, args.profile)
        if args.execute:
            required = (args.batch_manifest, args.predictive_plan, args.topology_file,
                        args.product_root, args.output)
            if any(item is None for item in required):
                raise ExternalFarmError("execute:batch,predictive,topology,product-root,output required")
            if args.repeat_predictive_plan is not None:
                raise ExternalFarmError("execute:repeat_predictive_plan_external_hold")
            rows, _assignments = validate_batch_inputs(
                args.batch_manifest.absolute(), args.predictive_plan.absolute(),
                args.topology_file.absolute(), corpus=args.corpus,
                profile=args.profile, regime=args.regime, depth=args.depth,
                suite=args.topology)
            timeout_seconds = external_timeout_seconds(
                len(rows), args.passes, args.regime == "warm")
            command = build_external_command(
                args.batch_manifest.absolute(), args.predictive_plan.absolute(),
                args.topology_file.absolute(), args.product_root.absolute(),
                profile=args.profile, corpus=args.corpus, regime=args.regime,
                depth=args.depth, suite=args.topology,
                workdir=Path("/tmp/p50compilee2e.external"),
                timeout_seconds=timeout_seconds,
                passes=args.passes, repeat_predictive_plan=(args.repeat_predictive_plan.absolute()
                                                            if args.repeat_predictive_plan else None))
            extra_plans = ()
            result = SSHTransport(authority, timeout=timeout_seconds).execute(
                topology=args.topology, relationship_hosts=hosts, profile=args.profile,
                batch_manifest=args.batch_manifest.absolute(),
                predictive_plan=args.predictive_plan.absolute(),
                topology_file=args.topology_file.absolute(), corpus=args.corpus,
                regime=args.regime, depth=args.depth, output=args.output.absolute(),
                product_root_remote=args.product_root_remote or str(args.product_root.absolute()),
                batch_command=command, host_product_root=args.product_root.absolute(),
                extra_stage_paths=extra_plans)
            print(json.dumps(result, sort_keys=True, separators=(",", ":")))
            return 0
        print(json.dumps(plan, sort_keys=True, separators=(",", ":")))
        return 0
    except (ExternalFarmError, OSError, subprocess.TimeoutExpired) as exc:
        print(f"s8_external_farm_executor: {exc}", file=os.sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
