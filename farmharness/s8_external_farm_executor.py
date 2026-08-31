#!/usr/bin/env python3
"""External S8 farm adapter.

This module is intentionally an adapter, not another compile harness.  Batch
and predictive input validation, compile-database command selection, action
joining, and finalization remain in :mod:`s8_real_c1f1_live_runner`.  The
adapter owns only authenticated role placement and the bounded SSH lifecycle.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import shlex
import re
import subprocess
import time
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
        if (not isinstance(idle, dict) or idle.get("status") != "PASS" or
                not isinstance(idle.get("load_1m"), (int, float)) or
                idle["load_1m"] > IDLE_LOAD_THRESHOLD):
            raise ExternalFarmError(f"authority:{host}:not_idle")
        image = item["image"]
        if (not isinstance(image, dict) or not re.fullmatch(r"sha256:[0-9a-f]{64}", str(image.get("image_id", ""))) or
                image.get("architecture") != "amd64" or image.get("os") != "linux"):
            raise ExternalFarmError(f"authority:{host}:image_invalid")
        binaries = item["binaries"]
        required_roles = {"scheduler/icecc-scheduler", "daemon/iceccd", "client/icecc",
                          "client/icecc-create-env", "cache/icecc-cache-service"}
        if not isinstance(binaries, dict) or set(binaries) != required_roles:
            raise ExternalFarmError(f"authority:{host}:binary_identity_incomplete")
        for role, value in binaries.items():
            _digest(value, f"authority:{host}.binaries.{role}")


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
                batch_command: Sequence[str] | None = None) -> dict[str, Any]:
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
                                    batch_command=batch_command, output=output)

    def execute_command(self, *, topology: str, relationship_hosts: Sequence[str],
                        profile: str, product_root_remote: str,
                        batch_command: Sequence[str], output: Path) -> dict[str, Any]:
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
        if not batch_command or batch_command[0] != "env" or "ICECC_P50_EXTERNAL_FARM=1" not in batch_command:
            raise ExternalFarmError("transport:authenticated_batch_command_required")
        token = f"s8ext-{os.getpid()}-{time.monotonic_ns()}"
        scheduler_port = 41000 + (os.getpid() % 1000)
        network = token
        root = shlex.quote(product_root_remote)
        profile_env = shlex.quote(profile)
        services: list[tuple[str, str]] = []
        try:
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
            for host in dict.fromkeys(("q3", *relationship_hosts)):
                args = [product_root_remote, CPU_COUNTS[host], IDLE_LOAD_THRESHOLD]
                for role, expected in self.authority["hosts"][host]["binaries"].items():
                    args.extend((role, expected))
                self.run(host, preflight, args)
            scheduler = (f"set -eu; root={root}; mkdir -p /tmp/{token}-client; "
                         f"export ICECC_P50_PROFILE={profile_environment(profile)}; "
                         f"$root/scheduler/icecc-scheduler -p {scheduler_port} -n {network} "
                         f"--assignment-fence-mode strict-nonce -l /tmp/{token}-client/scheduler.log -vvv "
                         f">/tmp/{token}-scheduler.stdout 2>&1 &")
            self.run("q3", scheduler)
            for relationship, host in enumerate(relationship_hosts):
                service = "p50-f" if topology == "C1F1/100000" else f"p50-f-{relationship}"
                args = profile_arguments(profile, root=product_root_remote,
                                         work=f"/tmp/{token}-{relationship}", role=f"f-{relationship}")
                worker = (f"set -eu; root={root}; mkdir -p /tmp/{token}-{relationship}/envs "
                          f"/tmp/{token}-{relationship}/cache-runtime-{('f-' + str(relationship))}; "
                          f"export ICECC_P50_PROFILE={profile_environment(profile)} "
                          f"ICECC_P50_F_ACTION_TRACE=/tmp/{token}-{relationship}/s7-measured-f-action-trace.jsonl "
                          f"ICECC_P50_TEST_READY_TRACE=/tmp/{token}-{relationship}/ready.trace; "
                          f"exec $root/daemon/iceccd -p "
                          f"{scheduler_port + 2 + relationship} -m {TOPOLOGIES[topology][1]} "
                          f"-s 10.0.27.101:{scheduler_port} -n {network} -N {service} "
                          f"-b /tmp/{token}-{relationship}/envs -l /tmp/{token}-f-{relationship}.log "
                          f"-vvv {' '.join(shlex.quote(arg) for arg in args)} "
                          f">/tmp/{token}-f-{relationship}.stdout 2>&1 &")
                self.run(host, worker)
                services.append((host, service))
            c_args = profile_arguments(profile, root=product_root_remote,
                                       work=f"/tmp/{token}-c", role="c")
            client_daemon = (f"set -eu; root={root}; mkdir -p /tmp/{token}-client/envs /tmp/{token}-client/cache-runtime-c; "
                             f"export ICECC_P50_PROFILE={profile_environment(profile)} "
                             f"ICECC_P50_C_ACTION_TRACE=/tmp/{token}-client/s7-measured-c-action-trace.jsonl "
                             f"ICECC_P50_TEST_READY_TRACE=/tmp/{token}-client/ready-c.trace; "
                             f"exec $root/daemon/iceccd --no-remote -m 0 "
                             f"-p {scheduler_port + 1} -s 10.0.27.101:{scheduler_port} -n {network} "
                             f"-N s8-p50-c -b /tmp/{token}-client/envs -l /tmp/{token}-client/c.log -vvv "
                             f"{' '.join(shlex.quote(arg) for arg in c_args)} "
                             f">/tmp/{token}-c.stdout 2>&1 &")
            self.run("q3", client_daemon)
            self.run("q3", readiness_gate(profile, f"/tmp/{token}-client/scheduler.log",
                                            relationships=len(set(relationship_hosts))))
            result = self.run("q3", "exec " + shlex.join([str(item) for item in batch_command]))
            metrics = observed_batch_metrics(result.stdout, topology)
            overlap_required(topology, metrics)
            output.mkdir(parents=True, exist_ok=True)
            (output / "product-output.log").write_text(result.stdout, encoding="utf-8")
            # Preserve the daemon work trees before unique-resource cleanup;
            # these contain C/F traces, scheduler log, and the F service map.
            s4.copy_remote_tree("q3", f"/tmp/{token}-client", output / "remote-q3-client", self.timeout)
            for relationship, host in enumerate(relationship_hosts):
                s4.copy_remote_tree(host, f"/tmp/{token}-{relationship}",
                                    output / f"remote-f-{relationship}", self.timeout)
            authority_path_value = self.authority.get("path")
            if authority_path_value is not None:
                authority_path = Path(authority_path_value)
                (output / "external-authority.json").write_bytes(authority_path.read_bytes())
            manifest = external_manifest(self.authority, topology, relationship_hosts, scheduler_port)
            (output / "external-farm.json").write_text(
                json.dumps(manifest, sort_keys=True, separators=(",", ":")) + "\n",
                encoding="utf-8")
            (output / "role-placement.json").write_text(
                json.dumps(placement, sort_keys=True, separators=(",", ":")) + "\n",
                encoding="utf-8")
            return {"schema": EXTERNAL_SCHEMA, "status": "PASS",
                    "role_placement": placement, "external_farm": manifest,
                    "batch_metrics": metrics,
                    "retained_artifacts": (["external-authority.json"] if authority_path_value is not None else []) +
                                          ["external-farm.json",
                                           "role-placement.json", "product-output.log"],
                    "output": str(output)}
        finally:
            # Names are unique to this invocation; no unrelated service is
            # ever addressed.  Logs remain in their remote work paths.
            self.run("q3", f"pkill -TERM -f {shlex.quote(token)} 2>/dev/null || true",
                     )
            for host, _service in services:
                self.run(host, f"pkill -TERM -f {shlex.quote(token)} 2>/dev/null || true")


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
        hosts = args.relationship_hosts or (["q2"] if count == 1 else ["q2"] * 15 + ["research7"] * 5)
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
                batch_command=command, output=args.output.absolute())
            print(json.dumps(result, sort_keys=True, separators=(",", ":")))
            return 0
        print(json.dumps(plan, sort_keys=True, separators=(",", ":")))
        return 0
    except (ExternalFarmError, OSError) as exc:
        print(f"s8_external_farm_executor: {exc}", file=os.sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
