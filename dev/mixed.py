#!/usr/bin/env python3
"""Run small, local P43/P50 Docker role-interoperability checks."""
from __future__ import annotations

import argparse
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


def _case_specs(current: str, legacy: str) -> tuple[dict[str, str], ...]:
    return (
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
    strict_p50 = bool(profile)
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
        scheduler_command = (
            "install -d -m 1777 /var/log/icecream && exec "
            "/opt/icecream/sbin/icecc-scheduler -n \"$1\" -p 8765 "
            + ("--assignment-fence-mode strict-nonce " if strict_p50 else "")
            + "-u nobody -l /qa-logs/scheduler.log -vvv"
        )
        run.call(
            "scheduler-start",
            ["run", "--detach", "--pull=never", "--name", scheduler,
             "--cpus", "1", "--memory", f"{scheduler_memory_mb}m",
             *( ["--env", f"ICECC_P50_PROFILE={profile}"] if strict_p50 else [] ),
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
            if strict_p50 else ""
        )
        worker_script = (
            "runtime_uid=$(id -u nobody); runtime_gid=$(id -g nobody); "
            "install -d -m 1777 /var/cache/icecream/envs /var/log/icecream && "
            + ("install -d -m 700 /var/cache/icecream/runtime-worker && " if strict_p50 else "")
            + "chown -R \"$runtime_uid:$runtime_gid\" /var/cache/icecream && exec "
            "/opt/icecream/sbin/iceccd -n \"$1\" -s scheduler "
            "-N qa-worker -m \"$2\" -u nobody -b /var/cache/icecream/envs "
            + worker_cache_args + "-vvv"
        )
        run.call(
            "worker-start",
            ["run", "--detach", "--pull=never", "--name", worker,
             "--cpus", str(jobs), "--memory", f"{worker_memory_mb}m",
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
        if strict_p50:
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
                *(["install -d -m 700 /var/cache/icecream/runtime-client"] if strict_p50 else []),
                "chown -R \"$runtime_uid:$runtime_gid\" /var/cache/icecream",
                "cache_args=()",
                *(["cache_args=(--cache-service /opt/icecream/sbin/icecc-cache-service --cache-runtime-dir /var/cache/icecream/runtime-client)"] if strict_p50 else []),
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
        if strict_p50:
            env += ["--env", f"ICECC_P50_PROFILE={profile}"]
        run.call(
            "client-compile",
            ["run", "--pull=never", "--name", client, "--label", expected,
             "--cpus", "1", "--memory", f"{client_memory_mb}m", "--network", network,
             "--mount", f"type=bind,src={source},dst=/source,readonly",
             "--mount", f"type=bind,src={client_log_dir},dst=/qa-logs", *env,
             client_image, "/bin/bash", "-c", script],
            timeout=120,
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
            (scheduler_log_dir / "scheduler-container.log").write_text(
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
        case.update(status="PASS", worker_ip=worker_ip, remote_hosts=sorted(hosts),
                    output=EXPECTED_OUTPUT, **evidence)
        return case
    finally:
        cleanup_errors: list[str] = []
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
    args = parser.parse_args(argv)
    if not 1 <= args.jobs <= 8:
        parser.error("--jobs must be between 1 and 8")
    if not 1 <= args.memory_gb <= 256:
        parser.error("--memory-gb must be between 1 and 256")
    if not 10 <= args.timeout <= 3600:
        parser.error("--timeout must be between 10 and 3600 seconds")
    for image in (args.current_image, args.legacy_image):
        if not image or any(char.isspace() for char in image):
            parser.error("image references must be non-empty and contain no whitespace")
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
    for spec in _case_specs(args.current_image, args.legacy_image):
        try:
            results.append(_run_case(run, spec, args.jobs, args.memory_gb, run_id))
        except (MixedError, OSError) as exc:
            results.append({"name": spec["name"], "status": "FAIL", "error": str(exc)})
            status = 1
            break
    expected_cases = len(_case_specs(args.current_image, args.legacy_image))
    summary = {"schema": "icecream-local-mixed-qa-v1", "run_id": run_id,
               "status": "PASS" if status == 0 and len(results) == expected_cases else "FAIL",
               "jobs": args.jobs, "cases": results, "steps": run.steps}
    (output / "summary.json").write_text(json.dumps(summary, indent=2) + "\n", encoding="utf-8")
    print(f"Mixed QA {summary['status']}; results: {output / 'summary.json'}", flush=True)
    return status


if __name__ == "__main__":
    raise SystemExit(main())
