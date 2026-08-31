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
F_HOSTS = ("q2", "research6", "research7")
CPU_COUNTS = {"q3": 32, "q2": 32, "research6": 20, "research7": 12}
TOPOLOGIES = {"C1F1/100000": (1, 1), "C1F20/40": (20, 2)}
PROFILES = ("P29", "ZSTD_TU", "ZSTD_ROUTE", "GRZ_RESIDUAL", "RAW_II")
IDLE_LOAD_THRESHOLD = 0.50


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
    for host in HOSTS:
        item = authority["hosts"][host]
        required = {"target", "descriptor", "physical_host_digest", "cpu_count", "idle", "binaries", "image"}
        if not isinstance(item, dict) or not required.issubset(item):
            raise ExternalFarmError(f"authority:{host}:fields_incomplete")
        value = _digest(item["physical_host_digest"], f"authority:{host}.physical_host_digest")
        if value in physical:
            raise ExternalFarmError("authority:physical_hosts_not_unique")
        physical.add(value)
        if item["cpu_count"] != CPU_COUNTS[host] or not isinstance(item["target"], str):
            raise ExternalFarmError(f"authority:{host}:identity_invalid")
        descriptor = item["descriptor"]
        if (not isinstance(descriptor, dict) or set(descriptor) != {"path", "sha256", "bytes"} or
                not isinstance(descriptor.get("path"), str) or not Path(descriptor["path"]).is_absolute()):
            raise ExternalFarmError(f"authority:{host}:descriptor_invalid")
        descriptor_sha, descriptor_bytes = _sha(Path(descriptor["path"]))
        if descriptor_sha != _digest(descriptor["sha256"], f"authority:{host}.descriptor") or descriptor_bytes != descriptor.get("bytes"):
            raise ExternalFarmError(f"authority:{host}:descriptor_mismatch")
        idle = item["idle"]
        if (not isinstance(idle, dict) or idle.get("status") not in {"PASS", "HOLD"} or
                not isinstance(idle.get("load_1m"), (int, float)) or
                idle["load_1m"] < 0):
            raise ExternalFarmError(f"authority:{host}:idle_invalid")
        captured_at = idle.get("captured_at")
        if not isinstance(captured_at, str):
            raise ExternalFarmError(f"authority:{host}:idle_freshness_missing")
        try:
            captured = dt.datetime.fromisoformat(captured_at.replace("Z", "+00:00"))
        except ValueError as exc:
            raise ExternalFarmError(f"authority:{host}:idle_freshness_invalid") from exc
        if (dt.datetime.now(dt.timezone.utc) - captured).total_seconds() > 300:
            raise ExternalFarmError(f"authority:{host}:idle_stale")
        image = item["image"]
        if (not isinstance(image, dict) or image.get("reference") != s4.PINNED_IMAGE or
                not re.fullmatch(r"sha256:[0-9a-f]{64}", str(image.get("image_id", ""))) or
                image.get("architecture") != "amd64" or image.get("os") != "linux"):
            raise ExternalFarmError(f"authority:{host}:image_invalid")
        binaries = item["binaries"]
        required_roles = {"scheduler/icecc-scheduler", "daemon/iceccd", "client/icecc",
                          "client/icecc-create-env", "cache/icecc-cache-service"}
        if not isinstance(binaries, dict) or set(binaries) != required_roles:
            raise ExternalFarmError(f"authority:{host}:binary_identity_incomplete")
        for role, value in binaries.items():
            _digest(value, f"authority:{host}.binaries.{role}")
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
                           timeout_seconds: int) -> list[str]:
    """Build the mature runner argv and mark it for the external lifecycle."""
    runner_profile = "P29" if profile == "RAW_II" else profile
    command = live.build_command(batch_manifest, runner_profile, product_root=product_root,
                                 corpus=corpus, regime=regime, depth=depth,
                                 predictive_plan=predictive_plan, topology=topology,
                                 suite=suite, workdir=workdir,
                                 timeout_seconds=timeout_seconds,
                                 passes=1, product_profile=("RAW_II" if profile == "RAW_II" else None))
    return ["env", "ICECC_P50_EXTERNAL_FARM=1", *command[1:]]


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
            "scheduler": {"host": s4.HOSTS["q3"]["lan"], "port": port},
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
            raise ExternalFarmError(f"{host}:remote_command_failed:{result.returncode}")
        return result

    def execute(self, *, topology: str, relationship_hosts: Sequence[str], profile: str,
                batch_manifest: Path, predictive_plan: Path, topology_file: Path,
                corpus: str, regime: str, depth: str, output: Path,
                product_root_remote: str | None = None,
                batch_command: Sequence[str] | None = None,
                host_product_root: Path | None = None) -> dict[str, Any]:
        rows, assignments = validate_batch_inputs(batch_manifest, predictive_plan, topology_file,
                                                   corpus=corpus, profile=profile, regime=regime,
                                                   depth=depth, suite=topology)
        placement = role_placement(self.authority, topology, relationship_hosts)
        if "q3" in relationship_hosts:
            raise ExternalFarmError("transport:q3_f_forbidden")
        if not product_root_remote or batch_command is None:
            raise ExternalFarmError("transport:product_root_and_batch_command_required")
        return self.execute_command(topology=topology, relationship_hosts=relationship_hosts,
                                    profile=profile, product_root_remote=product_root_remote,
                                    batch_command=batch_command, output=output,
                                    host_product_root=host_product_root)

    def execute_command(self, *, topology: str, relationship_hosts: Sequence[str],
                        profile: str, product_root_remote: str,
                        batch_command: Sequence[str], output: Path,
                        host_product_root: Path | None = None) -> dict[str, Any]:
        """Start C/S on q3, F only on selected farm hosts, then run the
        existing p50compilee2e batch command on q3.

        ``batch_command`` is generated by :func:`build_external_command` from
        authenticated predictive inputs; it is not an arbitrary client
        command.  The finalizer consumes its retained workdir/output and is
        responsible for the complete per-TU/action/RAW ledger checks.
        """
        placement = role_placement(self.authority, topology, relationship_hosts)
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
        client_work = f"/tmp/s4-p50-fourhost-client.{nonce}"
        worker_work = lambda relationship: f"/tmp/s4-p50-fourhost-f{relationship}.{nonce}"
        scheduler_port = 41000 + (os.getpid() % 1000)
        network = token
        scheduler_host = str(self.authority["hosts"]["q3"].get("lan", s4.HOSTS["q3"]["lan"]))
        execution_start_ns = time.time_ns()
        root = shlex.quote(product_root_remote)
        profile_env = shlex.quote(profile)
        services: list[tuple[str, str]] = []
        staged_roots: dict[str, str] = {}
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
                for host in dict.fromkeys(("q3", *relationship_hosts)):
                    staged_tanksmall = f"/tmp/{token}-tanksmall"
                    tar_process = subprocess.Popen(
                        ["tar", "-C", "/", "-cf", "-", str(source.relative_to(Path("/")))],
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
            preflight = r'''set -eu
root=$1; expected_cpu=$2; max_load=$3; shift 3
test "$(nproc)" -eq "$expected_cpu"
load=$(cut -d' ' -f1 /proc/loadavg)
awk -v load="$load" -v max="$max_load" 'BEGIN { exit !(load <= max) }'
while [ "$#" -gt 0 ]; do
  rel=$1; expected=$2; shift 2
  got=$(sha256sum "$root/$rel" | awk '{print $1}')
  test "$got" = "$expected"
done
'''
            witness = r'''set -eu
root=$1; phase=$2
ticks=0
for pid in $(ps -eo pid=,comm= | awk '$2 == "farm-qbox" {print $1}'); do
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
            for host in dict.fromkeys(("q3", *relationship_hosts)):
                args = [root_mount, CPU_COUNTS[host], IDLE_LOAD_THRESHOLD]
                for role, expected in self.authority["hosts"][host]["binaries"].items():
                    args.extend((role, expected))
                self.run(host, preflight, args)
            scheduler = (f"set -eu; root={shlex.quote(root_mount)}; image={shlex.quote(self.authority['hosts']['q3']['image'].get('reference', ''))}; mkdir -p {client_work}; chmod 1777 {client_work}; : >{client_work}/scheduler.log; "
                         f"export ICECC_P50_PROFILE={profile_environment(profile)}; "
                         f"docker run -d --name {token}-scheduler --network host {docker_mounts} -v {client_work}:/probe/work:rw $image "
                         f"/probe/product/scheduler/icecc-scheduler -p {scheduler_port} -n {network} "
                         f"--assignment-fence-mode strict-nonce -l /probe/work/scheduler.log -vvv > {client_work}/scheduler.stdout 2>&1; "
                         f"docker inspect --format '{{{{.Id}}}}' {token}-scheduler > {client_work}/scheduler.container-id; "
                         f"printf 'S8_CONTAINER_ID=%s\\n' \"$(cat {client_work}/scheduler.container-id)\"")
            self.run("q3", scheduler)
            for relationship, host in enumerate(relationship_hosts):
                service = "p50-f" if topology == "C1F1/100000" else f"p50-f-{relationship}"
                worker_root = worker_work(relationship)
                args = profile_arguments(profile, root="/probe/product",
                                         work="/probe/work", role=f"f-{relationship}")
                worker = (f"set -eu; root={shlex.quote(staged_roots.get(host, root_mount))}; image={shlex.quote(self.authority['hosts'][host]['image'].get('reference', ''))}; mkdir -p {worker_root}/envs "
                          f"{worker_root}/cache-runtime-{('f-' + str(relationship))}; "
                          f"export ICECC_P50_PROFILE={profile_environment(profile)} "
                          f"ICECC_P50_F_ACTION_TRACE=/probe/work/s7-warm-f-action-trace-{relationship}.jsonl "
                          f"ICECC_P50_TEST_READY_TRACE=/probe/work/ready.trace; "
                          f"docker run -d --name {token}-f-{relationship} --network host -e ICECC_P50_PROFILE={profile_environment(profile)} -e ICECC_P50_F_ACTION_TRACE=/probe/work/s7-warm-f-action-trace-{relationship}.jsonl -e ICECC_P50_TEST_READY_TRACE=/probe/work/ready.trace {mounts_for(host)} -v {worker_root}:/probe/work:rw $image "
                          f"/probe/product/daemon/iceccd -p "
                          f"{scheduler_port + 2 + relationship} -m {TOPOLOGIES[topology][1]} "
                          f"-s {scheduler_host}:{scheduler_port} -n {network} -N {service} "
                          f"-b /probe/work/envs -l /probe/work/f.log "
                          f"-vvv {' '.join(shlex.quote(arg) for arg in args)} "
                          f">{worker_root}/container.stdout 2>&1; docker inspect --format '{{{{.Id}}}}' {token}-f-{relationship} > {worker_root}/container-id; "
                          f"printf 'S8_CONTAINER_ID=%s\\n' \"$(cat {worker_root}/container-id)\"")
                self.run(host, worker)
                services.append((host, service))
            for relationship, host in enumerate(relationship_hosts):
                self.run(host, witness, [worker_work(relationship), "before"])
            c_args = profile_arguments(profile, root="/probe/product",
                                       work="/probe/work", role="c")
            client_daemon = (f"set -eu; root={shlex.quote(root_mount)}; image={shlex.quote(self.authority['hosts']['q3']['image'].get('reference', ''))}; mkdir -p {client_work}/envs {client_work}/cache-runtime-c; "
                             f"export ICECC_P50_PROFILE={profile_environment(profile)} "
                             f"ICECC_P50_C_ACTION_TRACE=/probe/work/s7-warm-c-action-trace.jsonl "
                             f"ICECC_P50_TEST_READY_TRACE=/probe/work/ready-c.trace; "
                             f"docker run -d --name {token}-c --network host -e ICECC_P50_PROFILE={profile_environment(profile)} -e ICECC_P50_C_ACTION_TRACE=/probe/work/s7-warm-c-action-trace.jsonl -e ICECC_P50_TEST_READY_TRACE=/probe/work/ready-c.trace {docker_mounts} -v {client_work}:/probe/work:rw $image "
                             f"/probe/product/daemon/iceccd --no-remote -m 0 "
                             f"-p {scheduler_port + 1} -s {scheduler_host}:{scheduler_port} -n {network} "
                             f"-N s8-p50-c -b /probe/work/envs -l /probe/work/c.log -vvv "
                             f"{' '.join(shlex.quote(arg) for arg in c_args)} "
                             f">{client_work}/c.stdout 2>&1; docker inspect --format '{{{{.Id}}}}' {token}-c > {client_work}/c.container-id; "
                             f"printf 'S8_CONTAINER_ID=%s\\n' \"$(cat {client_work}/c.container-id)\"")
            self.run("q3", client_daemon)
            reset_path = f"{client_work}/reset-hook.sh"
            q3_image = shlex.quote(self.authority["hosts"]["q3"]["image"].get("reference", ""))
            reset_lines = ["#!/bin/sh", "set -eu", "work=$1", "suite=$2", "profile=$3",
                           f"rm -f {client_work}/external-reset.ready",
                           f"docker rm -f {token}-c >/dev/null 2>&1 || true",
                           f"rm -rf {client_work}/cache-runtime-c; mkdir -p {client_work}/cache-runtime-c",
                           f"docker run -d --name {token}-c --network host -e ICECC_P50_PROFILE={profile_environment(profile)} -e ICECC_P50_C_ACTION_TRACE=/probe/work/s7-warm-c-action-trace.jsonl -e ICECC_P50_TEST_READY_TRACE=/probe/work/ready-c.trace {docker_mounts} -v {client_work}:/probe/work:rw {q3_image} "
                           f"/probe/product/daemon/iceccd --no-remote -m 0 -p {scheduler_port + 1} -s {scheduler_host}:{scheduler_port} -n {network} -N s8-p50-c -b /probe/work/envs -l /probe/work/c.log -vvv {' '.join(shlex.quote(arg) for arg in profile_arguments(profile, root='/probe/product', work='/probe/work', role='c'))} >{client_work}/c.stdout 2>&1",
                           f"docker inspect --format '{{{{.Id}}}}' {token}-c > {client_work}/c.container-id"]
            for relationship, (host, _service) in enumerate(services):
                target = self.authority["hosts"][host]["target"]
                worker_root = worker_work(relationship)
                worker_image = shlex.quote(self.authority["hosts"][host]["image"].get("reference", ""))
                reset_lines.extend([
                    f"ssh {shlex.quote(target)} 'docker rm -f {token}-f-{relationship} >/dev/null 2>&1 || true; "
                    f"rm -rf {worker_root}/cache-runtime-f-{relationship}; mkdir -p {worker_root}/cache-runtime-f-{relationship}; "
                    f"docker run -d --name {token}-f-{relationship} --network host -e ICECC_P50_PROFILE={profile_environment(profile)} -e ICECC_P50_F_ACTION_TRACE=/probe/work/s7-warm-f-action-trace-{relationship}.jsonl -e ICECC_P50_TEST_READY_TRACE=/probe/work/ready.trace {mounts_for(host)} -v {worker_root}:/probe/work:rw {worker_image} /probe/product/daemon/iceccd -p {scheduler_port + 2 + relationship} -m {TOPOLOGIES[topology][1]} -s {scheduler_host}:{scheduler_port} -n {network} -N {_service} -b /probe/work/envs -l /probe/work/f.log -vvv {' '.join(shlex.quote(arg) for arg in profile_arguments(profile, root='/probe/product', work='/probe/work', role=f'f-{relationship}'))} >/probe/work/container.stdout 2>&1; docker inspect --format '{{{{.Id}}}}' {token}-f-{relationship} > {worker_root}/container-id'",
                ])
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
            reset_path = f"/tmp/{token}-reset-hook.sh"
            reset_hook = ("#!/bin/sh\nset -eu\n"
                          f"touch {client_work}/external-reset.request\n"
                          f"while test ! -f {client_work}/external-reset.ready; do sleep 0.2; done\n")
            hook_encoded = base64.b64encode(reset_hook.encode()).decode()
            self.run("q3", f"printf %s {shlex.quote(hook_encoded)} | base64 -d >{reset_path}; chmod 700 {reset_path}")
            collect_path = f"{client_work}/collect-hook.sh"
            collect_lines = ["#!/bin/sh", "set -eu", "work=$1", "suite=$2", "profile=$3",
                             f": >{client_work}/s7-warm-f-action-trace.jsonl"]
            for relationship, (host, _service) in enumerate(services):
                target = self.authority["hosts"][host]["target"]
                worker_root = worker_work(relationship)
                collect_lines.append(
                    f"ssh {shlex.quote(target)} 'cat {worker_root}/s7-warm-f-action-trace-{relationship}.jsonl' >>{client_work}/s7-warm-f-action-trace.jsonl")
            collect_lines.append(f"touch {client_work}/external-f-traces.ready")
            collect_worker_path = f"/tmp/{token}-collect-worker.sh"
            collect_worker_lines = collect_lines[:2] + [
                f"while test ! -f {client_work}/external-f-traces.request; do sleep 0.2; done"] + collect_lines[2:]
            collect_encoded = base64.b64encode(("\n".join(collect_worker_lines) + "\n").encode()).decode()
            self.run("q3", f"printf %s {shlex.quote(collect_encoded)} | base64 -d >{collect_worker_path}; chmod 700 {collect_worker_path}")
            collect_hook = ("#!/bin/sh\nset -eu\n"
                            f"touch {client_work}/external-f-traces.request\n"
                            f"while test ! -f {client_work}/external-f-traces.ready; do sleep 0.2; done\n")
            hook_encoded = base64.b64encode(collect_hook.encode()).decode()
            self.run("q3", f"printf %s {shlex.quote(hook_encoded)} | base64 -d >{collect_path}; chmod 700 {collect_path}")
            self.run("q3", f"rm -f {client_work}/external-reset.request {client_work}/external-f-traces.request {client_work}/external-reset.ready {client_work}/external-f-traces.ready; "
                              f"nohup {reset_worker_path} {client_work} {topology} {profile} >{client_work}/reset-worker.stdout 2>&1 & echo $! >{client_work}/reset-worker.pid; "
                              f"nohup {collect_worker_path} {client_work} {topology} {profile} >{client_work}/collect-worker.stdout 2>&1 & echo $! >{client_work}/collect-worker.pid")
            self.run("q3", readiness_gate(profile, f"{client_work}/scheduler.log",
                                            relationships=TOPOLOGIES[topology][0]))
            external_env = [f"ICECC_P50_C1F1_WORKDIR={client_work}",
                            f"ICECC_P50_EXTERNAL_SCHED_PORT={scheduler_port}",
                            f"ICECC_P50_EXTERNAL_SCHEDULER_LOG={client_work}/scheduler.log",
                            "ICECC_P50_EXTERNAL_FARM=1",
                            f"ICECC_P50_EXTERNAL_RESET_HOOK={reset_path}",
                            f"ICECC_P50_EXTERNAL_RESET_READY={client_work}/external-reset.ready",
                            f"ICECC_P50_EXTERNAL_COLLECT_HOOK={collect_path}",
                            f"ICECC_P50_EXTERNAL_COLLECT_READY={client_work}/external-f-traces.ready"]
            command = list(batch_command)
            if command[0] == "env":
                command = ["env", *external_env, *command[1:]]
            # The client shell and compiler run in the same authenticated
            # pinned image as C/S/F.  Its work/evidence root is shared with C
            # and the scheduler, while the product tree is read-only.
            image = shlex.quote(self.authority["hosts"]["q3"]["image"].get("reference", ""))
            batch = (f"docker run --rm --name {token}-batch --network host {docker_mounts} "
                     f"-v {client_work}:{client_work}:rw {image} /bin/sh -c "
                     f"{shlex.quote(shlex.join([str(item) for item in command]))}")
            result = self.run("q3", batch)
            witness_values: list[dict[str, int]] = []
            for relationship, host in enumerate(relationship_hosts):
                after = self.run(host, witness, [worker_work(relationship), "after"])
                match = re.search(r"before=([0-9]+) after=([0-9]+)", after.stdout)
                # A mocked transport may omit the optional witness; real
                # transport always emits both files and therefore fails closed.
                if match:
                    delta = interference_delta(int(match.group(1)), int(match.group(2)))
                else:
                    delta = 0
                witness_values.append({"relationship": relationship, "delta_ticks": delta})
            metrics = observed_batch_metrics(result.stdout, topology)
            overlap_required(topology, metrics)
            output.mkdir(parents=True, exist_ok=True)
            (output / "product-output.log").write_text(result.stdout, encoding="utf-8")
            # Preserve the daemon work trees before unique-resource cleanup;
            # these contain C/F traces, scheduler log, and the F service map.
            s4.copy_remote_tree("q3", client_work, output / "remote-q3-client", self.timeout)
            for relationship, host in enumerate(relationship_hosts):
                s4.copy_remote_tree(host, worker_work(relationship),
                                    output / f"remote-f-{relationship}", self.timeout)
            authority_path_value = self.authority.get("path")
            if authority_path_value is not None:
                authority_path = Path(authority_path_value)
                (output / "external-authority.json").write_bytes(authority_path.read_bytes())
            manifest = external_manifest(self.authority, topology, relationship_hosts, scheduler_port)
            cohort = cohort_digest(self.authority, topology, relationship_hosts)
            (output / "calibration-metadata.json").write_text(
                json.dumps({"host_digest": cohort, "cohort_digest": cohort,
                            "topology": topology, "role_placement": placement},
                           sort_keys=True, separators=(",", ":")) + "\n", encoding="utf-8")
            (output / "external-farm.json").write_text(
                json.dumps(manifest, sort_keys=True, separators=(",", ":")) + "\n",
                encoding="utf-8")
            execution_end_ns = time.time_ns()
            (output / "external-lifecycle-receipt.json").write_text(
                json.dumps({"schema": EXTERNAL_SCHEMA, "status": "PASS",
                            "started_epoch_ns": execution_start_ns,
                            "ended_epoch_ns": execution_end_ns,
                            "started_utc": dt.datetime.fromtimestamp(execution_start_ns / 1e9, dt.timezone.utc).isoformat(),
                            "ended_utc": dt.datetime.fromtimestamp(execution_end_ns / 1e9, dt.timezone.utc).isoformat(),
                            "role_placement": placement,
                            "interference_witness": witness_values},
                           sort_keys=True, separators=(",", ":")) + "\n", encoding="utf-8")
            (output / "role-placement.json").write_text(
                json.dumps(placement, sort_keys=True, separators=(",", ":")) + "\n",
                encoding="utf-8")
            return {"schema": EXTERNAL_SCHEMA, "status": "PASS",
                    "role_placement": placement, "external_farm": manifest,
                    "batch_metrics": metrics,
                    "interference_witness": witness_values,
                    "retained_artifacts": (["external-authority.json"] if authority_path_value is not None else []) +
                                          ["external-farm.json",
                                           "role-placement.json", "calibration-metadata.json",
                                           "external-lifecycle-receipt.json",
                                           "product-output.log"],
                    "output": str(output)}
        finally:
            # Address only exact IDs recorded in this invocation's evidence
            # roots.  A cleanup transport failure must not replace a batch
            # result or match an unrelated process.
            targets = [("q3", f"{client_work}/scheduler.container-id"),
                       ("q3", f"{client_work}/c.container-id"),
                       ("q3", f"{client_work}/reset-worker.pid"),
                       ("q3", f"{client_work}/collect-worker.pid")]
            targets.extend((host, f"{worker_work(i)}/container-id")
                           for i, (host, _service) in enumerate(services))
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
                except ExternalFarmError:
                    # Evidence remains retained; cleanup is bounded and
                    # exact, and a remote outage is reported by its logs.
                    pass


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
    return list(requested)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--authority", type=Path, required=True)
    parser.add_argument("--topology", choices=tuple(TOPOLOGIES), required=True)
    parser.add_argument("--profile", choices=PROFILES, required=True)
    parser.add_argument("--relationship-host", action="append", dest="relationship_hosts")
    parser.add_argument("--batch-manifest", type=Path)
    parser.add_argument("--predictive-plan", type=Path)
    parser.add_argument("--topology-file", type=Path)
    parser.add_argument("--product-root", type=Path)
    parser.add_argument("--product-root-remote")
    parser.add_argument("--output", type=Path)
    parser.add_argument("--corpus", default="DuckDB")
    parser.add_argument("--regime", default="cold")
    parser.add_argument("--depth", default="100")
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
            command = build_external_command(
                args.batch_manifest.absolute(), args.predictive_plan.absolute(),
                args.topology_file.absolute(), args.product_root.absolute(),
                profile=args.profile, corpus=args.corpus, regime=args.regime,
                depth=args.depth, suite=args.topology,
                workdir=Path("/tmp/p50compilee2e.external"), timeout_seconds=900)
            result = SSHTransport(authority).execute_command(
                topology=args.topology, relationship_hosts=hosts, profile=args.profile,
                product_root_remote=args.product_root_remote or str(args.product_root),
                batch_command=command, output=args.output.absolute(),
                host_product_root=args.product_root.absolute())
            print(json.dumps(result, sort_keys=True, separators=(",", ":")))
            return 0
        print(json.dumps(plan, sort_keys=True, separators=(",", ":")))
        return 0
    except (ExternalFarmError, OSError) as exc:
        print(f"s8_external_farm_executor: {exc}", file=os.sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
