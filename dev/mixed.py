#!/usr/bin/env python3
"""Run small, local P43/P50/P51 Docker role-interoperability checks."""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import subprocess
import time
import uuid


EXPECTED_OUTPUT = "ICECREAM_MIXED_OK"
REMOTE_HOST_RE = re.compile(r"Have to use host ([0-9.]+):([0-9]+)")
REMOTE_COMPILE_RE = re.compile(r"Remote compilation completed with exit code 0")
OLD50_SCHEDULER_COMMIT = "94e9b44025887412c70c1c46c35fc588d6dec776"


class MixedError(RuntimeError):
    pass


def _resource_missing(result: subprocess.CompletedProcess[str]) -> bool:
    message = (result.stdout + result.stderr).lower()
    return "no such container" in message or "no such network" in message or (
        "network" in message and "not found" in message
    )


class DockerRun:
    def __init__(self, output: Path, docker: str, timeout: int):
        self.output = output
        self.docker = docker
        self.timeout = timeout
        self.steps: list[dict[str, object]] = []
        self.sequence = 0

    def call(
        self,
        name: str,
        argv: list[str],
        *,
        check: bool = True,
        timeout: int | None = None,
    ) -> subprocess.CompletedProcess[str]:
        log = self.output / "logs" / f"{self.sequence:03d}-{name}.log"
        self.sequence += 1
        command = [self.docker, *argv]
        started = time.monotonic()
        try:
            result = subprocess.run(
                command,
                capture_output=True,
                text=True,
                check=False,
                timeout=timeout or self.timeout,
            )
            with log.open("w", encoding="utf-8") as stream:
                stream.write("$ " + shlex.join(command) + "\n")
                stream.write(result.stdout)
                stream.write(result.stderr)
            self.steps.append(
                {"step": name, "exit_code": result.returncode, "log": str(log),
                 "elapsed_s": round(time.monotonic() - started, 3)}
            )
            if check and result.returncode:
                raise MixedError(f"{name} failed (exit {result.returncode}); see {log}")
            return result
        except (OSError, subprocess.TimeoutExpired) as exc:
            with log.open("w", encoding="utf-8") as stream:
                stream.write("$ " + shlex.join(command) + "\n")
                stream.write(f"command failed: {exc}\n")
            self.steps.append(
                {"step": name, "exit_code": -1, "log": str(log),
                 "elapsed_s": round(time.monotonic() - started, 3)}
            )
            if check:
                raise MixedError(f"{name} failed; see {log}: {exc}") from exc
            return subprocess.CompletedProcess(command, 124, "", str(exc))


def _case_specs(
    current: str,
    legacy: str,
    *,
    p51_r2: bool = False,
    old50_scheduler: dict[str, str] | None = None,
) -> tuple[dict[str, str], ...]:
    cases: tuple[dict[str, str], ...] = (
        {"name": "p50-p29v1", "scheduler": current, "worker": current,
         "client": current, "profile": "P29V1"},
        {"name": "p50-zstd-tu", "scheduler": current, "worker": current,
         "client": current, "profile": "ZSTD_TU"},
        {"name": "p50-zstd-route", "scheduler": current, "worker": current,
         "client": current, "profile": "ZSTD_ROUTE"},
        {"name": "p43-worker", "scheduler": current, "worker": legacy,
         "client": current},
        {"name": "p43-client", "scheduler": current, "worker": current,
         "client": legacy},
    )
    if p51_r2:
        cases += tuple(
            {"name": f"p51-r2-{tag}", "scheduler": current, "worker": current,
             "client": current, "profile": profile, "r2": "true"}
            for tag, profile in (
                ("p29v1", "P29V1"), ("zstd-tu", "ZSTD_TU"),
                ("zstd-route", "ZSTD_ROUTE"),
            )
        )
        if old50_scheduler is not None:
            cases += ({
                "name": "p51-old50-scheduler-fallback",
                "scheduler": current,
                "worker": current,
                "client": current,
                "profile": "P29V1",
                "r2": "true",
                "cache_fallback": "true",
                "scheduler_binary": old50_scheduler["binary"],
                "scheduler_binary_sha256": old50_scheduler["sha256"],
                "scheduler_source_commit": old50_scheduler["source_commit"],
            },)
    return cases


def _run_case(
    run: DockerRun, spec: dict[str, str], jobs: int, memory_gb: int, run_id: str
) -> dict[str, object]:
    name = spec["name"]
    root = run.output / name
    source = root / "source"
    source.mkdir(parents=True)
    (source / "probe.cpp").write_text(
        '#include <iostream>\nint main() { std::cout << "ICECREAM_MIXED_OK\\n"; }\n',
        encoding="utf-8",
    )
    expected = f"icecream.qa.run={run_id}"
    network = f"icecream-qa-{run_id[:10]}-{name}"
    scheduler = f"{network}-scheduler"
    worker = f"{network}-worker"
    client = f"{network}-client"
    owned_containers = [scheduler, worker, client]
    case: dict[str, object] = {"name": name, "status": "FAIL", "images": spec.copy()}
    profile = spec.get("profile", "")
    cache_enabled = bool(profile)
    strict_p50 = cache_enabled and spec.get("cache_fallback") != "true"
    p51_r2 = spec.get("r2") == "true"
    scheduler_log_dir = root / "scheduler-logs"
    scheduler_log_dir.mkdir()
    worker_log_dir = root / "worker-logs"
    worker_log_dir.mkdir()
    client_log_dir = root / "client-logs"
    client_log_dir.mkdir()
    if strict_p50:
        scheduler_log_dir.chmod(0o1777)
    total_memory_mb = memory_gb * 1024
    scheduler_memory_mb = max(128, total_memory_mb // 8)
    worker_memory_mb = max(256, total_memory_mb // 2)
    client_memory_mb = total_memory_mb - scheduler_memory_mb - worker_memory_mb
    if client_memory_mb < 256:
        raise MixedError("--memory-gb must allow at least 256 MiB for each role")
    try:
        run.call("network-create", ["network", "create", "--label", expected, network])
        scheduler_executable = (
            "/qa-bin/icecc-scheduler" if spec.get("scheduler_binary") else
            "/opt/icecream/sbin/icecc-scheduler"
        )
        scheduler_command = (
            "runtime_uid=$(id -u nobody); runtime_gid=$(id -g nobody); "
            "install -d -m 1777 /var/log/icecream && "
            "install -m 0644 /dev/null /qa-logs/scheduler.log && "
            "chown \"$runtime_uid:$runtime_gid\" /qa-logs/scheduler.log && exec "
            + scheduler_executable + " -n \"$1\" -p 8765 "
            + ("--assignment-fence-mode strict-nonce " if strict_p50 else "")
            + "-u nobody -l /qa-logs/scheduler.log -vvv"
        )
        scheduler_mounts = (
            ["--mount", f"type=bind,src={spec['scheduler_binary']},dst=/qa-bin/icecc-scheduler,readonly"]
            if spec.get("scheduler_binary") else []
        )
        scheduler_env = (
            ["--env", f"ICECC_P50_PROFILE={profile}"] if strict_p50 else []
        )
        if p51_r2 and spec.get("cache_fallback") != "true":
            scheduler_env += ["--env", "ICECC_P51_MODE=on"]
        run.call(
            "scheduler-start",
            ["run", "--detach", "--pull=never", "--name", scheduler,
             "--cpus", "1", "--memory", f"{scheduler_memory_mb}m",
             *scheduler_env,
             *scheduler_mounts,
             "--label", expected, "--network", network, "--network-alias", "scheduler",
             "--mount", f"type=bind,src={scheduler_log_dir},dst=/qa-logs",
             spec["scheduler"], "/bin/bash", "-c",
             scheduler_command, "_", network],
        )
        deadline = time.monotonic() + 30
        while True:
            result = run.call(
                "scheduler-ready",
                ["exec", scheduler, "sh", "-c", "nc -z 127.0.0.1 8766"],
                check=False,
                timeout=4,
            )
            if result.returncode == 0:
                break
            if time.monotonic() >= deadline:
                raise MixedError(f"{name}: scheduler did not become ready")
            time.sleep(0.25)

        worker_cache_args = (
            "--cache-service /opt/icecream/sbin/icecc-cache-service "
            "--cache-runtime-dir /var/cache/icecream/runtime-worker "
            if cache_enabled else ""
        )
        worker_script = (
            "runtime_uid=$(id -u nobody); runtime_gid=$(id -g nobody); "
            "install -d -m 1777 /var/cache/icecream/envs /var/log/icecream && "
            + ("install -d -m 700 /var/cache/icecream/runtime-worker && " if cache_enabled else "")
            + "chown -R \"$runtime_uid:$runtime_gid\" /var/cache/icecream && exec "
            "/opt/icecream/sbin/iceccd -n \"$1\" -s scheduler "
            "-N qa-worker -m \"$2\" -u nobody -b /var/cache/icecream/envs "
            + worker_cache_args + "-vvv"
        )
        worker_env: list[str] = []
        if cache_enabled:
            worker_env += ["--env", f"ICECC_P50_PROFILE={profile}"]
        if p51_r2:
            worker_env += ["--env", "ICECC_P51_MODE=on"]
        run.call(
            "worker-start",
            ["run", "--detach", "--pull=never", "--name", worker,
             "--cpus", str(jobs), "--memory", f"{worker_memory_mb}m",
             *worker_env,
             "--label", expected, "--network", network, "--network-alias", "worker",
             "--cap-add", "SYS_CHROOT", "--mount",
             f"type=bind,src={worker_log_dir},dst=/qa-logs", spec["worker"],
             "/bin/bash", "-c",
             worker_script, "_", network, str(jobs)],
        )

        deadline = time.monotonic() + 30
        while True:
            listing = run.call(
                "worker-ready",
                ["exec", scheduler, "sh", "-c",
                 "printf 'listcs\\nquit\\n' | nc -w 1 127.0.0.1 8766"],
                check=False,
                timeout=4,
            )
            if listing.returncode == 0 and re.search(r"(?m)^\s*qa-worker\s+\(", listing.stdout):
                break
            if time.monotonic() >= deadline:
                raise MixedError(f"{name}: worker did not register with scheduler")
            time.sleep(0.25)

        inspect = run.call(
            "worker-address",
            ["inspect", "--format",
             "{{range .NetworkSettings.Networks}}{{.IPAddress}}{{end}}", worker],
        )
        worker_ip = inspect.stdout.strip()
        if not re.fullmatch(r"[0-9]+(?:\.[0-9]+){3}", worker_ip):
            raise MixedError(f"{name}: Docker returned an invalid worker address")

        scheduler_ip = ""
        if cache_enabled and spec.get("cache_fallback") != "true":
            scheduler_address = run.call(
                "scheduler-address",
                ["inspect", "--format",
                 "{{range .NetworkSettings.Networks}}{{.IPAddress}}{{end}}", scheduler],
            )
            scheduler_ip = scheduler_address.stdout.strip()
            if not re.fullmatch(r"[0-9]+(?:\.[0-9]+){3}", scheduler_ip):
                raise MixedError(f"{name}: Docker returned an invalid scheduler address")

        script = "\n".join(
            (
                "set -eu",
                "runtime_uid=$(id -u nobody); runtime_gid=$(id -g nobody)",
                "install -d -m 1777 /var/cache/icecream/envs /var/log/icecream",
                *(["install -d -m 700 /var/cache/icecream/runtime-client"] if cache_enabled else []),
                "chown -R \"$runtime_uid:$runtime_gid\" /var/cache/icecream",
                "install -m 0644 /dev/null /qa-logs/client-daemon.log",
                "chown \"$runtime_uid:$runtime_gid\" /qa-logs/client-daemon.log",
                "cache_args=()",
                *(["cache_args=(--cache-service /opt/icecream/sbin/icecc-cache-service --cache-runtime-dir /var/cache/icecream/runtime-client)"] if cache_enabled else []),
                "/opt/icecream/sbin/iceccd -n \"$ICECC_NETNAME\" -s scheduler -N qa-client -m 0 "
                "--no-remote -u nobody -l /qa-logs/client-daemon.log -vvv \"${cache_args[@]}\" &",
                "daemon=$!",
                "trap 'kill \"$daemon\" 2>/dev/null || true; wait \"$daemon\" 2>/dev/null || true' EXIT",
                "ready=0",
                "for attempt in $(seq 1 60); do",
                "  if printf 'listcs\\nquit\\n' | nc -w 1 scheduler 8766 | grep -q 'qa-client ('; then ready=1; break; fi",
                "  sleep 0.25",
                "done",
                "test \"$ready\" = 1 || { echo 'client daemon did not connect' >&2; exit 1; }",
                "export ICECC_DEBUG=debug ICECC_LOGFILE=/qa-logs/icecc.log",
                "if [ \"$ICECC_CLIENT_VERSION\" = current ]; then export ICECC_REMOTE_REQUIRED=1; fi",
                *([f"export ICECC_P50_C1F1_REQUIRED=1 ICECC_P50_C1F1_WORKER_SCHEDULER_HOST={scheduler_ip}"] if strict_p50 else []),
                "/opt/icecream/bin/icecc /usr/bin/g++ -std=c++17 -O0 -c /source/probe.cpp -o /qa-logs/probe.o",
                "/usr/bin/g++ /qa-logs/probe.o -o /qa-logs/probe",
                "actual=$( /qa-logs/probe )",
                f"test \"$actual\" = {EXPECTED_OUTPUT}",
                f"echo {EXPECTED_OUTPUT}",
            )
        )
        client_image = spec["client"]
        version = "current" if client_image == spec["scheduler"] else "legacy"
        env = ["--env", f"ICECC_NETNAME={network}", "--env", f"ICECC_CLIENT_VERSION={version}"]
        if cache_enabled:
            env += ["--env", f"ICECC_P50_PROFILE={profile}"]
        if p51_r2:
            env += ["--env", "ICECC_P51_MODE=on"]
        run.call(
            "client-compile",
            ["run", "--pull=never", "--name", client, "--label", expected,
             "--cpus", "1", "--memory", f"{client_memory_mb}m", "--network", network,
             "--mount", f"type=bind,src={source},dst=/source,readonly",
             "--mount", f"type=bind,src={client_log_dir},dst=/qa-logs", *env,
             client_image, "/bin/bash", "-c", script],
            timeout=120,
        )
        client_container_logs = run.call(
            "client-container-log", ["logs", client], check=False
        )
        (client_log_dir / "client-container.log").write_text(
            client_container_logs.stdout + client_container_logs.stderr,
            encoding="utf-8",
        )
        client_log = (client_log_dir / "icecc.log").read_text(encoding="utf-8", errors="replace")
        hosts = {match.group(1) for match in REMOTE_HOST_RE.finditer(client_log)}
        if worker_ip not in hosts:
            raise MixedError(
                f"{name}: client log does not prove compilation on worker {worker_ip}"
            )
        worker_log = run.call("worker-log", ["logs", worker])
        worker_text = worker_log.stdout + worker_log.stderr
        (worker_log_dir / "iceccd.log").write_text(worker_text, encoding="utf-8")
        if REMOTE_COMPILE_RE.search(worker_text) is None:
            raise MixedError(f"{name}: worker log does not prove successful remote compilation")
        evidence: dict[str, object] = {}
        if strict_p50:
            scheduler_evidence = run.call("scheduler-evidence", ["logs", scheduler])
            scheduler_text = scheduler_evidence.stdout + scheduler_evidence.stderr
            scheduler_file = scheduler_log_dir / "scheduler.log"
            if scheduler_file.is_file():
                scheduler_text += scheduler_file.read_text(encoding="utf-8", errors="replace")
            client_text = client_log
            if "P50 assignment identity bound for job" not in client_text:
                raise MixedError(f"{name}: client log does not prove a strict P50 assignment")
            if f"{profile} source committed for P50 CompileFile" not in client_text:
                raise MixedError(f"{name}: client log does not prove {profile} source transfer")
            expected_profile = profile
            if f"P50 CompileFile attached exact {expected_profile} input" not in worker_text:
                raise MixedError(
                    f"{name}: worker log does not prove {expected_profile} input attachment"
                )
            (scheduler_log_dir / "scheduler-evidence.log").write_text(
                scheduler_text, encoding="utf-8"
            )
            if "assignment fence: strict-nonce" not in scheduler_text:
                raise MixedError(f"{name}: scheduler log does not prove strict-nonce mode")
            profile_token = profile.lower()
            if not re.search(
                rf"RELOGIN qa-worker.*cache=.*cache_profiles=.*\b{profile_token}\b",
                scheduler_text,
            ):
                raise MixedError(f"{name}: scheduler log does not prove worker cache-profile advertisement")
            evidence = {"strict_assignment_fence": True, "cache_sidecar_advertisement": True,
                        "scheduler_ip": scheduler_ip, "selected_profile": profile,
                        "p50_source_transfer": True}
        if p51_r2:
            scheduler_evidence = run.call("scheduler-evidence", ["logs", scheduler])
            scheduler_text = scheduler_evidence.stdout + scheduler_evidence.stderr
            scheduler_file = scheduler_log_dir / "scheduler.log"
            if scheduler_file.is_file():
                scheduler_text += scheduler_file.read_text(encoding="utf-8", errors="replace")
            client_daemon_log = client_log_dir / "client-daemon.log"
            client_daemon_text = (
                client_daemon_log.read_text(encoding="utf-8", errors="replace")
                if client_daemon_log.is_file() else ""
            )
            client_container_log = client_log_dir / "client-container.log"
            if client_container_log.is_file():
                client_daemon_text += client_container_log.read_text(
                    encoding="utf-8", errors="replace"
                )
            if spec.get("cache_fallback") == "true":
                listing = run.call(
                    "old50-cache-selection",
                    ["exec", scheduler, "sh", "-c",
                     "printf 'listcs\\nquit\\n' | nc -w 1 127.0.0.1 8766"],
                ).stdout
                worker_rows = [
                    line for line in listing.splitlines() if "qa-worker" in line
                ]
                if not worker_rows:
                    raise MixedError(f"{name}: old protocol-50 scheduler did not list the worker")
                worker_row = "\n".join(worker_rows)
                if not (re.search(r"(?:^|\s)cache=off(?:\s|$)", worker_row)
                        or "cache_protocol=1" in worker_row):
                    raise MixedError(
                        f"{name}: old scheduler did not prove R1/disabled cache selection"
                    )
                if "cache_protocol=2" in worker_row or (
                    "P51 C-cache source-control lease delivered" in client_daemon_text
                ):
                    raise MixedError(f"{name}: old protocol-50 scheduler selected or attempted R2")
                if "P51 cache-link descriptor adopted by sidecar" in worker_text:
                    raise MixedError(f"{name}: old protocol-50 scheduler reached an R2 data link")
                evidence.update(
                    ordinary_scheduler_protocol=50,
                    ordinary_scheduler_source_commit=spec["scheduler_source_commit"],
                    ordinary_scheduler_binary_sha256=spec["scheduler_binary_sha256"],
                    p51_opt_in=True,
                    r2_selected=False,
                    cache_fallback="R1-or-disabled",
                )
            else:
                if "P51 C-cache source-control lease delivered" not in client_daemon_text:
                    raise MixedError(f"{name}: C daemon log does not prove a P51 source lease")
                if "P51 cache-link descriptor adopted by sidecar" not in worker_text:
                    raise MixedError(f"{name}: F daemon log does not prove persistent R2 link adoption")
                if f"{profile} source committed for P50 CompileFile" not in client_log:
                    raise MixedError(f"{name}: R2 profile {profile} source was not committed")
                evidence.update(
                    p51_opt_in=True,
                    r2_selected=True,
                    r2_source_lease=True,
                    r2_link_adopted=True,
                    selected_profile=profile,
                )
            (scheduler_log_dir / "scheduler-r2-evidence.log").write_text(
                scheduler_text, encoding="utf-8"
            )
        case.update(status="PASS", worker_ip=worker_ip, remote_hosts=sorted(hosts),
                    output=EXPECTED_OUTPUT, **evidence)
        return case
    finally:
        cleanup_errors: list[str] = []
        client_logs = run.call("client-container-log", ["logs", client], check=False)
        (client_log_dir / "client-container.log").write_text(
            client_logs.stdout + client_logs.stderr, encoding="utf-8"
        )
        if client_logs.returncode and not _resource_missing(client_logs):
            cleanup_errors.append("client-log-capture")
        scheduler_logs = run.call("scheduler-log", ["logs", scheduler], check=False)
        (scheduler_log_dir / "scheduler-container.log").write_text(
            scheduler_logs.stdout + scheduler_logs.stderr, encoding="utf-8"
        )
        if scheduler_logs.returncode and not _resource_missing(scheduler_logs):
            cleanup_errors.append("scheduler-log-capture")
        worker_logs = run.call("worker-container-log", ["logs", worker], check=False)
        (worker_log_dir / "worker-container.log").write_text(
            worker_logs.stdout + worker_logs.stderr, encoding="utf-8"
        )
        if worker_logs.returncode and not _resource_missing(worker_logs):
            cleanup_errors.append("worker-log-capture")
        for container in reversed(owned_containers):
            result = run.call("cleanup-container", ["rm", "--force", container], check=False)
            if result.returncode and not _resource_missing(result):
                cleanup_errors.append(container)
        result = run.call("cleanup-network", ["network", "rm", network], check=False)
        if result.returncode and not _resource_missing(result):
            cleanup_errors.append(network)
        if cleanup_errors:
            case["cleanup_errors"] = cleanup_errors
            raise MixedError(f"{name}: cleanup failed for owned resources: {cleanup_errors}")


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--current-image", required=True)
    parser.add_argument("--legacy-image", required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--jobs", type=int, default=2)
    parser.add_argument("--memory-gb", type=int, default=8)
    parser.add_argument("--timeout", type=int, default=180)
    parser.add_argument("--p51-r2", action="store_true",
                        help="add opt-in protocol-51 R2 cases for all cache profiles")
    parser.add_argument("--only-p51-r2", action="store_true",
                        help="run only the opt-in R2 rows (requires --p51-r2)")
    parser.add_argument("--only-old50-scheduler-fallback", action="store_true",
                        help="run only the pinned protocol-50 fallback row")
    parser.add_argument("--ordinary50-scheduler-binary", type=Path,
                        help="pinned protocol-50 scheduler executable for the R2 fallback row")
    parser.add_argument("--ordinary50-scheduler-sha256",
                        help="required SHA256 of --ordinary50-scheduler-binary")
    parser.add_argument("--ordinary50-scheduler-source-commit",
                        help="required source commit for the pinned protocol-50 scheduler")
    args = parser.parse_args(argv)
    if not 1 <= args.jobs <= 8:
        parser.error("--jobs must be between 1 and 8")
    if not 1 <= args.memory_gb <= 256:
        parser.error("--memory-gb must be between 1 and 256")
    if not 10 <= args.timeout <= 3600:
        parser.error("--timeout must be between 10 and 3600 seconds")
    if args.only_p51_r2 and not args.p51_r2:
        parser.error("--only-p51-r2 requires --p51-r2")
    if args.only_old50_scheduler_fallback and not args.p51_r2:
        parser.error("--only-old50-scheduler-fallback requires --p51-r2")
    for image in (args.current_image, args.legacy_image):
        if not image or any(char.isspace() for char in image):
            parser.error("image references must be non-empty and contain no whitespace")
    old50_scheduler: dict[str, str] | None = None
    old50_args = (args.ordinary50_scheduler_binary, args.ordinary50_scheduler_sha256,
                  args.ordinary50_scheduler_source_commit)
    if any(value is not None for value in old50_args):
        if not args.p51_r2 or any(value is None for value in old50_args):
            parser.error("old protocol-50 scheduler options require --p51-r2 and all three values")
        binary = args.ordinary50_scheduler_binary.absolute()
        if not binary.is_file() or not os.access(binary, os.X_OK):
            parser.error("--ordinary50-scheduler-binary must be an executable file")
        digest = hashlib.sha256(binary.read_bytes()).hexdigest()
        if digest != args.ordinary50_scheduler_sha256.lower():
            parser.error("--ordinary50-scheduler-sha256 does not match the scheduler binary")
        if "," in str(binary):
            parser.error("--ordinary50-scheduler-binary path must not contain commas (Docker mount syntax)")
        if args.ordinary50_scheduler_source_commit != OLD50_SCHEDULER_COMMIT:
            parser.error(
                "the R2 fallback gate is pinned to protocol-50 scheduler source "
                + OLD50_SCHEDULER_COMMIT
            )
        old50_scheduler = {"binary": str(binary), "sha256": digest,
                           "source_commit": OLD50_SCHEDULER_COMMIT}
    if args.only_old50_scheduler_fallback and old50_scheduler is None:
        parser.error("--only-old50-scheduler-fallback requires pinned old-scheduler options")
    output = args.output.absolute()
    if "," in str(output):
        parser.error("--output path must not contain commas (Docker mount syntax)")
    try:
        output.mkdir(parents=True, exist_ok=False)
    except OSError as exc:
        parser.error(f"cannot create a new output directory: {exc}")
    (output / "logs").mkdir()
    run_id = uuid.uuid4().hex
    docker = os.environ.get("ICEFARM_DOCKER", "docker")
    run = DockerRun(output, docker, args.timeout)
    results: list[dict[str, object]] = []
    status = 0
    case_specs = _case_specs(args.current_image, args.legacy_image,
                             p51_r2=args.p51_r2, old50_scheduler=old50_scheduler)
    if args.only_p51_r2:
        case_specs = tuple(spec for spec in case_specs if spec.get("r2") == "true")
    if args.only_old50_scheduler_fallback:
        case_specs = tuple(spec for spec in case_specs if spec.get("cache_fallback") == "true")
    for spec in case_specs:
        try:
            results.append(_run_case(run, spec, args.jobs, args.memory_gb, run_id))
        except (MixedError, OSError) as exc:
            results.append({"name": spec["name"], "status": "FAIL", "error": str(exc)})
            status = 1
            break
    expected_cases = len(case_specs)
    summary = {"schema": "icecream-local-mixed-qa-v1", "run_id": run_id,
               "status": "PASS" if status == 0 and len(results) == expected_cases else "FAIL",
               "jobs": args.jobs, "p51_r2_requested": args.p51_r2,
               "only_p51_r2": args.only_p51_r2,
               "only_old50_scheduler_fallback": args.only_old50_scheduler_fallback,
               "ordinary50_scheduler": old50_scheduler,
               "cases": results, "steps": run.steps}
    (output / "summary.json").write_text(json.dumps(summary, indent=2) + "\n", encoding="utf-8")
    print(f"Mixed QA {summary['status']}; results: {output / 'summary.json'}", flush=True)
    return status


if __name__ == "__main__":
    raise SystemExit(main())
