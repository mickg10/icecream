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
REMOTE_JOB_RE = re.compile(r"Have to use host [0-9.]+:[0-9]+ - Job ID: ([0-9]+)")
REMOTE_COMPILE_RE = re.compile(r"Remote compilation completed with exit code 0")
IMAGE_LABELS_RE = re.compile(r"^(\S+)\s+(\{.*\})$")
OLD50_SCHEDULER_COMMIT = "94e9b44025887412c70c1c46c35fc588d6dec776"


class MixedError(RuntimeError):
    pass


def _resource_missing(result: subprocess.CompletedProcess[str]) -> bool:
    message = (result.stdout + result.stderr).lower()
    return "no such container" in message or "no such network" in message or (
        "network" in message and "not found" in message
    )


def _d18_cc1plus_identity(
    run: DockerRun, container: str, standard: str, pid: str | None = None
) -> dict[str, str] | None:
    """Read a cc1plus identity (PID, /proc starttime, and argv) in a worker."""
    command = [
        "exec", container, "/bin/bash", "-c",
        "\n".join((
            "set -eu",
            f"wanted={shlex.quote(pid or '')}",
            "for entry in /proc/[0-9]*; do",
            "  candidate=${entry##*/}",
            "  [ -z \"$wanted\" ] || [ \"$candidate\" = \"$wanted\" ] || continue",
            "  [ -r \"$entry/comm\" ] && [ -r \"$entry/cmdline\" ] && [ -r \"$entry/stat\" ] || continue",
            "  [ \"$(cat \"$entry/comm\" 2>/dev/null || true)\" = cc1plus ] || continue",
            "  args=$(tr '\\000' ' ' < \"$entry/cmdline\" 2>/dev/null || true)",
            f"  case \"$args\" in *-std=gnu++{standard}*|*-std=c++{standard}*) ;; *) continue ;; esac",
            "  start=$(awk '{print $22}' \"$entry/stat\" 2>/dev/null || true)",
            "  [ -n \"$start\" ] || continue",
            "  printf '%s|%s|%s\\n' \"$candidate\" \"$start\" \"$args\"",
            "done",
        )),
    ]
    result = run.call("d18-process-identity", command, check=False, timeout=5)
    for line in result.stdout.splitlines():
        fields = line.split("|", 2)
        if len(fields) == 3 and fields[0].isdigit() and fields[1].isdigit():
            return {"pid": fields[0], "starttime": fields[1], "argv": fields[2]}
    return None


def _d18_exact_job_line(text: str, prefix: str, job_id: str) -> bool:
    """Match one logged job number, not a prefix of a larger job number."""
    line = re.compile(r"(?m)^.*" + re.escape(prefix) + re.escape(job_id) + r"(?=\s|$)")
    return line.search(text) is not None


def _d18_remote_compile_pid(worker_text: str, source_name: str) -> str | None:
    source_line = re.compile(
        r"(?m)^\[(\d+)\]\s+[^\n]*remote compile for file "
        + re.escape("/source/" + source_name)
        + r"\s*$"
    )
    matches = source_line.findall(worker_text)
    if len(matches) != 1:
        return None
    pid = matches[0]
    completion = re.compile(
        r"(?m)^\[" + re.escape(pid)
        + r"\]\s+[^\n]*Remote compilation completed with exit code 0(?:\s|$)"
    )
    return pid if completion.search(worker_text) else None


def _d18_image_provenance(run: "DockerRun", image: str, name: str) -> dict[str, object]:
    result = run.call(
        f"d18-{name}-image-inspect",
        ["image", "inspect", "--format={{.Id}} {{json .Config.Labels}}", image],
    )
    match = IMAGE_LABELS_RE.match(result.stdout.strip())
    if match is None:
        raise MixedError(f"could not parse image provenance for {image}")
    try:
        labels = json.loads(match.group(2))
    except json.JSONDecodeError as exc:
        raise MixedError(f"could not parse image labels for {image}") from exc
    if labels is None:
        labels = {}
    if not isinstance(labels, dict):
        raise MixedError(f"image labels for {image} are not an object")
    return {
        "reference": image,
        "image_id": match.group(1),
        "source_identity": labels.get("icecream.source.identity"),
        "recipe": labels.get("org.icecream.dev.recipe"),
        "profile": labels.get("org.icecream.dev.profile"),
    }


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


def _run_concurrent_mixed(
    run: DockerRun, current: str, legacy: str, jobs: int, memory_gb: int,
    run_id: str, profile: str = "P29V1",
) -> dict[str, object]:
    """Exercise P43, R1 and R2 jobs concurrently on one local scheduler/pool."""
    if profile not in ("P29V1", "ZSTD_TU", "ZSTD_ROUTE"):
        raise MixedError("unsupported concurrent cache profile")
    if jobs < 3:
        raise MixedError("--concurrent-mixed requires --jobs >= 3")
    if memory_gb < 4:
        raise MixedError("--concurrent-mixed requires --memory-gb >= 4")
    scratch = os.environ.get("ICEFARM_TMPDIR")
    if not scratch:
        raise MixedError("--concurrent-mixed requires ICEFARM_TMPDIR on scratch")
    scratch_root = Path(scratch).resolve()
    if not run.output.resolve().is_relative_to(scratch_root):
        raise MixedError("--concurrent-mixed output must be under ICEFARM_TMPDIR")

    root = run.output / "concurrent-mixed"
    source = root / "source"
    control = root / "control"
    scheduler_logs = root / "scheduler-logs"
    worker_logs = root / "worker-logs"
    source.mkdir(parents=True)
    control.mkdir(parents=True)
    scheduler_logs.mkdir()
    worker_logs.mkdir()
    # Keep the compile input modest while long enough for the three role-tagged
    # cc1plus processes to overlap. The observer below, not a timing threshold,
    # is the acceptance witness.
    expected = {
        "p43": "ICECREAM_D18_P43_OK",
        "r1": "ICECREAM_D18_R1_OK",
        "r2": "ICECREAM_D18_R2_OK",
    }
    for role, output in expected.items():
        functions = "\n".join(
            f"static int d18_{role}_{index}() {{ return {index % 97}; }}"
            for index in range(30000)
        )
        (source / f"{role}.cpp").write_text(
            "#include <iostream>\n" + functions + "\nint main() { "
            f"std::cout << \"{output}\\n\"; return 0; }}\n",
            encoding="utf-8",
        )
    (source / "warm.cpp").write_text("int d18_warmup() { return 0; }\n",
                                     encoding="utf-8")

    network = f"icecream-d18-{run_id[:12]}"
    scheduler = network + "-scheduler"
    workers = {"r1": network + "-r1-worker", "r2": network + "-r2-worker"}
    clients = {role: network + "-" + role for role in expected}
    owned_containers = [scheduler, *workers.values(), *clients.values()]
    label = f"icecream.qa.run={run_id}"
    case: dict[str, object] = {
        "name": "concurrent-p43-r1-r2",
        "status": "FAIL",
        "scope": f"local shared scheduler; P43 client with separate {profile} protocol-1 and protocol-2 workers",
        "selected_profile": profile,
        "assignment_policy": "enforcing-compat",
        "worker_modes": {"r1": f"{profile}/R1", "r2": f"{profile}/R2"},
    }
    total_mb = memory_gb * 1024
    scheduler_mb = max(256, total_mb // 10)
    client_mb = max(256, total_mb // 12)
    worker_mb = (total_mb - scheduler_mb - 3 * client_mb) // 2
    if worker_mb < 512:
        raise MixedError("--memory-gb leaves less than 512 MiB per protocol worker")
    scheduler_logs.chmod(0o1777)
    worker_log_dirs: dict[str, Path] = {}
    for role in workers:
        directory = worker_logs / role
        directory.mkdir()
        directory.chmod(0o1777)
        worker_log_dirs[role] = directory
    client_logs: dict[str, Path] = {}
    for role in expected:
        directory = root / f"{role}-logs"
        directory.mkdir()
        directory.chmod(0o1777)
        client_logs[role] = directory
    scheduler_ip = ""
    worker_ips: dict[str, str] = {}
    image_provenance: dict[str, dict[str, object]] = {}
    try:
        image_provenance = {
            "current": _d18_image_provenance(run, current, "current"),
            "p43": _d18_image_provenance(run, legacy, "p43"),
        }
        run.call("d18-network-create", ["network", "create", "--label", label, network])
        scheduler_command = (
            "runtime_uid=$(id -u nobody); runtime_gid=$(id -g nobody); "
            "install -d -m 1777 /var/log/icecream && "
            "install -m 0644 /dev/null /qa-logs/scheduler.log && "
            'chown "$runtime_uid:$runtime_gid" /qa-logs/scheduler.log && exec '
            "/opt/icecream/sbin/icecc-scheduler -n \"$1\" -p 8765 "
            "--assignment-fence-mode enforcing-compat -u nobody "
            "-l /qa-logs/scheduler.log -vvv"
        )
        run.call(
            "d18-scheduler-start",
            ["run", "--detach", "--pull=never", "--name", scheduler,
             "--cpus", "0.5", "--memory", f"{scheduler_mb}m",
             "--env", f"ICECC_P50_PROFILE={profile}", "--env", "ICECC_P51_MODE=on",
             "--label", label, "--network", network, "--network-alias", "scheduler",
             "--mount", f"type=bind,src={scheduler_logs},dst=/qa-logs",
             current, "/bin/bash", "-c", scheduler_command, "_", network],
        )
        ready_deadline = time.monotonic() + 30
        while True:
            result = run.call(
                "d18-scheduler-ready",
                ["exec", scheduler, "sh", "-c", "nc -z 127.0.0.1 8766"],
                check=False, timeout=4,
            )
            if result.returncode == 0:
                break
            if time.monotonic() >= ready_deadline:
                raise MixedError("concurrent mixed scheduler did not become ready")
            time.sleep(0.25)

        worker_cpu = max(0.5, (jobs - 0.5 - 3 * 0.25) / 2)
        for role, name in workers.items():
            p51_mode = "off" if role == "r1" else "on"
            worker_script = (
                "runtime_uid=$(id -u nobody); runtime_gid=$(id -g nobody); "
                "install -d -m 1777 /var/cache/icecream/envs /var/log/icecream && "
                "install -d -m 700 /var/cache/icecream/runtime-worker && "
                'chown -R "$runtime_uid:$runtime_gid" /var/cache/icecream && exec '
                "/opt/icecream/sbin/iceccd -n \"$1\" -s scheduler "
                f"-N qa-d18-{role} -m 2 -u nobody "
                "-b /var/cache/icecream/envs "
                "--cache-service /opt/icecream/sbin/icecc-cache-service "
                "--cache-runtime-dir /var/cache/icecream/runtime-worker -vvv"
            )
            run.call(
                f"d18-{role}-worker-start",
                ["run", "--detach", "--pull=never", "--name", name,
                 "--cpus", str(worker_cpu), "--memory", f"{worker_mb}m",
                 "--env", f"ICECC_P50_PROFILE={profile}", "--env", f"ICECC_P51_MODE={p51_mode}",
                 "--label", label, "--network", network,
                 "--network-alias", f"worker-{role}",
                 "--cap-add", "SYS_CHROOT", "--mount",
                 f"type=bind,src={worker_log_dirs[role]},dst=/qa-logs", current,
                 "/bin/bash", "-c", worker_script, "_", network],
            )
        ready_deadline = time.monotonic() + 30
        while True:
            listing = run.call(
                "d18-worker-ready",
                ["exec", scheduler, "sh", "-c",
                 "printf 'listcs\\nquit\\n' | nc -w 1 127.0.0.1 8766"],
                check=False, timeout=4,
            )
            if (listing.returncode == 0 and "qa-d18-r1" in listing.stdout
                    and "qa-d18-r2" in listing.stdout):
                break
            if time.monotonic() >= ready_deadline:
                raise MixedError("concurrent mixed worker did not register")
            time.sleep(0.25)
        scheduler_ip = run.call(
            "d18-scheduler-address",
            ["inspect", "--format",
             "{{range .NetworkSettings.Networks}}{{.IPAddress}}{{end}}", scheduler],
        ).stdout.strip()
        for role, name in workers.items():
            worker_ips[role] = run.call(
                f"d18-{role}-worker-address",
                ["inspect", "--format",
                 "{{range .NetworkSettings.Networks}}{{.IPAddress}}{{end}}", name],
            ).stdout.strip()
        for address in (scheduler_ip, *worker_ips.values()):
            if not re.fullmatch(r"[0-9]+(?:\.[0-9]+){3}", address):
                raise MixedError("Docker returned an invalid D18 role address")

        standards = {"p43": "c++11", "r1": "c++14", "r2": "c++17"}
        preferred_worker = {
            "p43": worker_ips["r1"],
            "r1": worker_ips["r1"],
            "r2": worker_ips["r2"],
        }
        for role, output in expected.items():
            current_client = role != "p43"
            client_script = "\n".join((
                "set -eu",
                "runtime_uid=$(id -u nobody); runtime_gid=$(id -g nobody)",
                "install -d -m 1777 /var/cache/icecream/envs /var/log/icecream",
                "install -d -m 700 /var/cache/icecream/runtime-client",
                "chown -R \"$runtime_uid:$runtime_gid\" /var/cache/icecream",
                "install -m 0644 /dev/null /qa-logs/client-daemon.log",
                "chown \"$runtime_uid:$runtime_gid\" /qa-logs/client-daemon.log",
                "cache_args=()",
                *( ["cache_args=(--cache-service /opt/icecream/sbin/icecc-cache-service --cache-runtime-dir /var/cache/icecream/runtime-client)"] if current_client else [] ),
                f"/opt/icecream/sbin/iceccd -n \"$ICECC_NETNAME\" -s scheduler -N qa-d18-client-{role} -m 0 --no-remote -u nobody -l /qa-logs/client-daemon.log -vvv \"${{cache_args[@]}}\" &",
                "daemon=$!",
                "trap 'kill \"$daemon\" 2>/dev/null || true; wait \"$daemon\" 2>/dev/null || true' EXIT",
                "ready=0",
                "for attempt in $(seq 1 80); do",
                f"  if printf 'listcs\\nquit\\n' | nc -w 1 scheduler 8766 | grep -q 'qa-d18-client-{role} ('; then ready=1; break; fi",
                "  sleep 0.25",
                "done",
                "test \"$ready\" = 1 || { echo 'client daemon did not connect' >&2; exit 1; }",
                f"export ICECC_PREFERRED_HOST={preferred_worker[role]}",
                "export ICECC_DEBUG=debug ICECC_LOGFILE=/qa-logs/warm-icecc.log",
                *( ["export ICECC_REMOTE_REQUIRED=1"] if current_client else [] ),
                *( [f"export ICECC_P50_PROFILE={profile} ICECC_P50_C1F1_REQUIRED=1 ICECC_P50_C1F1_WORKER_SCHEDULER_HOST={scheduler_ip}"] if current_client else [] ),
                *( ["export ICECC_P51_MODE=off"] if role == "r1" else [] ),
                *( ["export ICECC_P51_MODE=on"] if role == "r2" else [] ),
                f"icecc /usr/bin/g++ -std={standards[role]} -O0 -c /source/warm.cpp -o /qa-logs/warm-{role}.o",
                f"echo D18_ENV_WARMED role={role}",
                f"touch /qa-control/ready-{role}",
                f"while [ ! -f /qa-control/go ]; do sleep 0.02; done",
                f"echo D18_CLIENT_BEGIN role={role}",
                "export ICECC_LOGFILE=/qa-logs/icecc.log",
                f"icecc /usr/bin/g++ -std={standards[role]} -O0 -DICECC_D18_ROLE_{role.upper()}=1 -c /source/{role}.cpp -o /qa-logs/{role}.o",
                f"/usr/bin/g++ /qa-logs/{role}.o -o /qa-logs/{role}",
                f"actual=$( /qa-logs/{role} )",
                f"test \"$actual\" = {output}",
                f"printf '%s\\n' \"D18_RESULT role={role} output=$actual\"",
            ))
            image = legacy if role == "p43" else current
            env = ["--env", f"ICECC_NETNAME={network}"]
            env += ["--env", f"ICECC_PREFERRED_HOST={preferred_worker[role]}"]
            if current_client:
                env += ["--env", f"ICECC_P50_PROFILE={profile}"]
            if role == "r1":
                env += ["--env", "ICECC_P51_MODE=off"]
            if role == "r2":
                env += ["--env", "ICECC_P51_MODE=on"]
            run.call(
                f"d18-{role}-start",
                ["run", "--detach", "--pull=never", "--name", clients[role],
                 "--label", label, "--cpus", "0.25", "--memory", f"{client_mb}m",
                 "--network", network, "--mount",
                 f"type=bind,src={source},dst=/source,readonly", "--mount",
                 f"type=bind,src={client_logs[role]},dst=/qa-logs", "--mount",
                 f"type=bind,src={control},dst=/qa-control", *env,
                 image, "/bin/bash", "-c", client_script],
                timeout=20,
            )

        ready_paths = [control / f"ready-{role}" for role in expected]
        ready_deadline = time.monotonic() + 45
        while not all(path.is_file() for path in ready_paths):
            if time.monotonic() >= ready_deadline:
                missing = [path.name for path in ready_paths if not path.is_file()]
                raise MixedError(f"concurrent mixed clients did not reach barrier: {missing}")
            time.sleep(0.05)
        (control / "go").touch()

        role_workers = {"p43": "r1", "r1": "r1", "r2": "r2"}
        standards_short = {role: standards[role].removeprefix("c++")
                           for role in expected}
        witnesses: dict[str, dict[str, str]] = {}
        witness_deadline = time.monotonic() + 45
        while len(witnesses) != len(expected):
            for role in expected:
                if role in witnesses:
                    continue
                icecc_log = client_logs[role] / "icecc.log"
                text = (icecc_log.read_text(encoding="utf-8", errors="replace")
                        if icecc_log.is_file() else "")
                hosts = {match.group(1) for match in REMOTE_HOST_RE.finditer(text)}
                if not hosts:
                    continue
                if hosts != {preferred_worker[role]}:
                    raise MixedError(f"{role}: assigned remote host differs from its intended worker")
                found = _d18_cc1plus_identity(
                    run, workers[role_workers[role]], standards_short[role])
                if found is not None:
                    found.update(role=role, worker=role_workers[role],
                                 container=workers[role_workers[role]])
                    witnesses[role] = found
            if time.monotonic() >= witness_deadline:
                raise MixedError("concurrent cc1plus role processes were not simultaneously observable")
            if len(witnesses) != len(expected):
                time.sleep(0.02)
        if len({(item["container"], item["pid"]) for item in witnesses.values()}) != 3:
            raise MixedError("role witness did not identify three distinct compiler processes")
        for role, witness in witnesses.items():
            checked = _d18_cc1plus_identity(
                run, witness["container"], standards_short[role], witness["pid"])
            if checked is None or checked["starttime"] != witness["starttime"]:
                raise MixedError(f"{role}: compiler PID/starttime did not survive overlap bracket")
            witness["revalidated"] = "true"
        (worker_logs / "concurrent-cc1plus.json").write_text(
            json.dumps(witnesses, indent=2) + "\n", encoding="utf-8")
        for role in expected:
            waited = run.call(f"d18-{role}-wait", ["wait", clients[role]], timeout=150)
            if waited.stdout.strip() != "0":
                raise MixedError(f"{role} client exited {waited.stdout.strip()}")
        overlap_path = worker_logs / "concurrent-cc1plus.json"
        if not overlap_path.is_file() or len(json.loads(overlap_path.read_text())) != 3:
            raise MixedError("three-role compiler overlap witness was not retained")

        remote_hosts: dict[str, list[str]] = {}
        measured_job_ids: dict[str, str] = {}
        for role, output in expected.items():
            logs = client_logs[role]
            icecc_log = logs / "icecc.log"
            text = icecc_log.read_text(encoding="utf-8", errors="replace")
            hosts = sorted({match.group(1) for match in REMOTE_HOST_RE.finditer(text)})
            job_ids = REMOTE_JOB_RE.findall(text)
            if hosts != [preferred_worker[role]] or len(job_ids) != 1:
                raise MixedError(f"{role}: actual remote host did not match its intended worker")
            remote_hosts[role] = hosts
            measured_job_ids[role] = job_ids[0]
            container_log = run.call(f"d18-{role}-logs", ["logs", clients[role]])
            if f"D18_RESULT role={role} output={output}" not in (
                container_log.stdout + container_log.stderr
            ):
                raise MixedError(f"{role}: exact compiled-program output missing")

        worker_texts: dict[str, str] = {}
        for role, name in workers.items():
            result = run.call(f"d18-{role}-worker-logs", ["logs", name])
            worker_texts[role] = result.stdout + result.stderr
        measured_worker_compile_pids: dict[str, str] = {}
        for role in expected:
            worker_role = role_workers[role]
            worker_text = worker_texts[worker_role]
            job_id = measured_job_ids[role]
            compile_pid = _d18_remote_compile_pid(worker_text, f"{role}.cpp")
            if compile_pid is None:
                raise MixedError(
                    f"{role}: worker log lacks a successful completion for the measured source/PID"
                )
            measured_worker_compile_pids[role] = compile_pid
            if role in ("r1", "r2"):
                if not _d18_exact_job_line(
                    worker_text,
                    f"P50 CompileFile attached exact {profile} input for job ",
                    job_id,
                ):
                    raise MixedError(f"{role}: measured job {job_id} lacks exact F input attachment")
                measured_icecc = (client_logs[role] / "icecc.log").read_text(
                    encoding="utf-8", errors="replace")
                if not _d18_exact_job_line(
                    measured_icecc, "P50 assignment identity bound for job ", job_id
                ):
                    raise MixedError(f"{role}: measured job {job_id} lacks P50 assignment identity")
                # The source-commit line currently lacks a job field; it is
                # corroborative only within this post-barrier per-role log.
                if f"{profile} source committed for P50 CompileFile" not in measured_icecc:
                    raise MixedError(f"{role}: measured job {job_id} lacks exact source commit")
        r2_client = (client_logs["r2"] / "client-daemon.log").read_text(
            encoding="utf-8", errors="replace")
        r2_job_id = measured_job_ids["r2"]
        if not _d18_exact_job_line(
            r2_client,
            "P51 C-cache source-control lease delivered for assignment ",
            r2_job_id,
        ):
            raise MixedError(f"R2 measured job {r2_job_id} lacks its source-control lease")
        if "P51 cache-link descriptor adopted by sidecar" not in worker_texts["r2"]:
            raise MixedError("protocol-2 worker did not prove R2 link adoption")
        case.update(
            status="PASS",
            scheduler_ip=scheduler_ip,
            image_provenance=image_provenance,
            shared_workers=worker_ips,
            environment_warmups=True,
            concurrent_role_cc1plus=witnesses,
            remote_hosts=remote_hosts,
            measured_job_ids=measured_job_ids,
            measured_worker_compile_pids=measured_worker_compile_pids,
            exact_outputs=expected,
            p43_remote=True,
            r1_remote_p29v1=True,
            r2_remote_p29v1=True,
            r2_source_lease=True,
            r2_link_adopted=True,
            source_commit_correlation="per-role measured log only; commit event has no job field",
        )
        return case
    finally:
        for role, name in clients.items():
            logs = run.call(f"d18-{role}-container-logs", ["logs", name],
                            check=False)
            (client_logs[role] / "container.log").write_text(
                logs.stdout + logs.stderr, encoding="utf-8")
        for role, name in workers.items():
            logs = run.call(f"d18-{role}-worker-container-logs", ["logs", name],
                            check=False)
            (worker_log_dirs[role] / "container.log").write_text(
                logs.stdout + logs.stderr, encoding="utf-8")
        for name, directory, step in (
            (scheduler, scheduler_logs, "d18-scheduler-container-logs"),
        ):
            logs = run.call(step, ["logs", name], check=False)
            (directory / "container.log").write_text(
                logs.stdout + logs.stderr, encoding="utf-8")
        cleanup_errors: list[str] = []
        for container in reversed(owned_containers):
            result = run.call("d18-cleanup-container",
                              ["rm", "--force", container], check=False)
            if result.returncode and not _resource_missing(result):
                cleanup_errors.append(container)
        result = run.call("d18-cleanup-network", ["network", "rm", network],
                          check=False)
        if result.returncode and not _resource_missing(result):
            cleanup_errors.append(network)
        if cleanup_errors:
            case["cleanup_errors"] = cleanup_errors
            raise MixedError(f"concurrent mixed cleanup failed: {cleanup_errors}")


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
    parser.add_argument("--concurrent-mixed", action="store_true",
                        help="run concurrent P43, R1 and R2 jobs on one local scheduler/pool")
    parser.add_argument("--concurrent-profile", choices=("P29V1", "ZSTD_TU", "ZSTD_ROUTE"),
                        help="cache profile for --concurrent-mixed (default: P29V1)")
    parser.add_argument("--ordinary50-scheduler-binary", type=Path,
                        help="pinned protocol-50 scheduler executable for the R2 fallback row")
    parser.add_argument("--ordinary50-scheduler-sha256",
                        help="required SHA256 of --ordinary50-scheduler-binary")
    parser.add_argument("--ordinary50-scheduler-source-commit",
                        help="required source commit for the pinned protocol-50 scheduler")
    args = parser.parse_args(argv)
    if args.concurrent_profile and not args.concurrent_mixed:
        parser.error("--concurrent-profile requires --concurrent-mixed")
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
    if args.concurrent_mixed and (args.p51_r2 or args.only_p51_r2 or
                                  args.only_old50_scheduler_fallback):
        parser.error("--concurrent-mixed is a standalone gate; do not combine it with row selectors")
    if args.concurrent_mixed and args.jobs < 3:
        parser.error("--concurrent-mixed requires --jobs >= 3")
    if args.concurrent_mixed and args.memory_gb < 4:
        parser.error("--concurrent-mixed requires --memory-gb >= 4")
    if args.concurrent_mixed and not os.environ.get("ICEFARM_TMPDIR"):
        parser.error("--concurrent-mixed requires ICEFARM_TMPDIR on scratch")
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
    if args.concurrent_mixed:
        try:
            results.append(_run_concurrent_mixed(
                run, args.current_image, args.legacy_image, args.jobs,
                args.memory_gb, run_id, args.concurrent_profile or "P29V1"))
        except (MixedError, OSError) as exc:
            results.append({"name": "concurrent-p43-r1-r2", "status": "FAIL",
                            "error": str(exc)})
            status = 1
        expected_cases = 1
    else:
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
               "concurrent_mixed_requested": args.concurrent_mixed,
               "concurrent_profile": (args.concurrent_profile or "P29V1")
                   if args.concurrent_mixed else None,
               "only_p51_r2": args.only_p51_r2,
               "only_old50_scheduler_fallback": args.only_old50_scheduler_fallback,
               "ordinary50_scheduler": old50_scheduler,
               "cases": results, "steps": run.steps}
    (output / "summary.json").write_text(json.dumps(summary, indent=2) + "\n", encoding="utf-8")
    print(f"Mixed QA {summary['status']}; results: {output / 'summary.json'}", flush=True)
    return status


if __name__ == "__main__":
    raise SystemExit(main())
