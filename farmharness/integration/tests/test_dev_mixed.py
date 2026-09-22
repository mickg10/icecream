from __future__ import annotations

import json
import os
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[3]


def _fake_docker(tmp_path: Path, *, failure: str = "", remote: bool = True,
                 wrong_profile: bool = False) -> tuple[Path, Path]:
    path = tmp_path / "docker"
    calls = tmp_path / "docker-calls.jsonl"
    code = f"""#!{sys.executable}
import json, os, pathlib, sys
args = sys.argv[1:]
with open(os.environ['FAKE_DOCKER_CALLS'], 'a', encoding='utf-8') as out:
    out.write(json.dumps(args) + '\\n')
fail = os.environ.get('FAKE_DOCKER_FAILURE')
if fail and ((fail == 'scheduler-start' and args[:1] == ['run'] and '--network-alias' in args and 'scheduler' in args) or
             (fail == 'client-compile' and args[:1] == ['run'] and '--network-alias' not in args)):
    print('injected docker failure', file=sys.stderr)
    raise SystemExit(17)
if args[:1] == ['exec'] and args[1].endswith('-scheduler'):
    if 'nc -z' in args[-1]:
        raise SystemExit(0)
    print(' qa-worker (172.18.0.2:10245) 1 1')
    raise SystemExit(0)
if args[:2] == ['inspect', '--format']:
    print('172.18.0.2')
    raise SystemExit(0)
if args[:1] == ['logs'] and any(arg.endswith('-worker') for arg in args):
    print('Remote compilation completed with exit code 0')
    joined = ' '.join(args)
    profile = ('ZSTD_TU' if {str(wrong_profile)} else
               'ZSTD_ROUTE' if 'p50-zstd-route' in joined else
               'ZSTD_TU' if 'p50-zstd-tu' in joined else 'P29V1')
    print('P50 CompileFile attached exact ' + profile + ' input for job 1')
    raise SystemExit(0)
if args[:1] == ['logs'] and any(arg.endswith('-scheduler') for arg in args):
    print('assignment fence: strict-nonce')
    print('RELOGIN qa-worker(x86_64): [] cache=127.0.0.1:11001 cache_wire=v1 cache_protocol=1 cache_profiles=p29v1 zstd_tu zstd_route')
    raise SystemExit(0)
if args[:1] == ['run'] and '--network-alias' not in args:
    mounts = [a for a in args if a.startswith('type=bind,src=') and ',dst=/qa-logs' in a]
    assert mounts, args
    log_dir = pathlib.Path(mounts[-1].split('src=', 1)[1].split(',dst=', 1)[0])
    host = '172.18.0.2' if {str(remote)} else '172.18.0.99'
    is_p50 = 'ICECC_P50_C1F1_REQUIRED=1' in ' '.join(args)
    profile = next((arg.split('=', 1)[1] for arg in args if arg.startswith('ICECC_P50_PROFILE=')), 'P29V1')
    p50 = ('P50 assignment identity bound for job 1\\n' + profile + ' source committed for P50 CompileFile\\n'
           if is_p50 else '')
    (log_dir / 'icecc.log').write_text('Have to use host ' + host + ':10245 - Job ID: 1\\n' + p50)
    print('ICECREAM_MIXED_OK')
    raise SystemExit(0)
if args[:1] == ['network'] and len(args) > 1 and args[1] == 'create':
    print(args[-1])
raise SystemExit(0)
"""
    path.write_text(code, encoding="utf-8")
    path.chmod(0o755)
    return path, calls


def _run_cli(tmp_path: Path, *, failure: str = "", remote: bool = True,
             wrong_profile: bool = False) -> tuple[subprocess.CompletedProcess[str], Path, list[list[str]]]:
    docker, calls = _fake_docker(tmp_path, failure=failure, remote=remote,
                                 wrong_profile=wrong_profile)
    output = tmp_path / "mixed-results"
    env = os.environ.copy()
    env.update(ICEFARM_DOCKER=str(docker), FAKE_DOCKER_CALLS=str(calls),
               FAKE_DOCKER_FAILURE=failure, PYTHONDONTWRITEBYTECODE="1")
    result = subprocess.run(
        [sys.executable, str(ROOT / "dev/mixed.py"),
         "--current-image", "icecream-dev:current", "--legacy-image", "icecream-dev:p43",
         "--output", str(output), "--jobs", "2"],
        cwd=ROOT, env=env, text=True, capture_output=True, timeout=20,
    )
    records = [json.loads(line) for line in calls.read_text().splitlines()] if calls.exists() else []
    return result, output, records


def test_mixed_runner_covers_native_and_both_cross_generation_roles(tmp_path: Path) -> None:
    result, output, commands = _run_cli(tmp_path)

    assert result.returncode == 0, result.stderr
    summary = json.loads((output / "summary.json").read_text())
    assert summary["status"] == "PASS"
    assert [case["name"] for case in summary["cases"]] == [
        "p50-p29v1", "p50-zstd-tu", "p50-zstd-route", "p43-worker", "p43-client",
    ]
    assert all(case["status"] == "PASS" for case in summary["cases"])
    assert [case.get("selected_profile") for case in summary["cases"][:3]] == [
        "P29V1", "ZSTD_TU", "ZSTD_ROUTE",
    ]
    assert all(case["strict_assignment_fence"] is True
               and case["cache_sidecar_advertisement"] is True
               and case["p50_source_transfer"] is True for case in summary["cases"][:3])
    worker_images = [
        next(arg for arg in command if arg.startswith("icecream-dev:"))
        for command in commands
        if command[:1] == ["run"] and "--network-alias" in command
        and command[command.index("--network-alias") + 1] == "worker"
    ]
    assert worker_images == ["icecream-dev:current", "icecream-dev:current",
                             "icecream-dev:current", "icecream-dev:p43", "icecream-dev:current"]
    client_runs = [
        command for command in commands
        if command[:1] == ["run"] and "--network-alias" not in command
    ]
    assert len(client_runs) == 5
    assert all("--pull=never" in command for command in commands if command[:1] == ["run"])
    for command in (item for item in commands if item[:1] == ["run"]):
        image_index = next(i for i, arg in enumerate(command) if arg.startswith("icecream-dev:"))
        assert "-p" not in command[:image_index]
        assert "--publish" not in command[:image_index]
        assert "--cpus" in command[:image_index]
        assert "--memory" in command[:image_index]
    memory_caps = [
        int(command[command.index("--memory") + 1][:-1])
        for command in commands if command[:1] == ["run"]
    ]
    assert len(memory_caps) == 15
    assert all(sum(memory_caps[offset:offset + 3]) <= 8 * 1024
               for offset in range(0, len(memory_caps), 3))
    assert all("--cap-add" in command and "SYS_CHROOT" in command
               for command in commands if command[:1] == ["run"] and "worker" in command)
    assert all("--no-remote" in " ".join(command) and "-m 0" in " ".join(command)
               for command in client_runs)
    current_case_commands = [
        " ".join(command) for command in commands
        if command[:1] == ["run"] and "icecream-qa-" in " ".join(command)
        and any(name in " ".join(command)
                for name in ("p50-p29v1", "p50-zstd-tu", "p50-zstd-route"))
    ]
    assert any("--assignment-fence-mode strict-nonce" in command
               for command in current_case_commands)
    assert any("--cache-service /opt/icecream/sbin/icecc-cache-service" in command
               for command in current_case_commands)
    assert any("ICECC_P50_C1F1_REQUIRED=1" in command
               and "ICECC_P50_C1F1_WORKER_SCHEDULER_HOST=" in command
               for command in current_case_commands)
    assert not any(command[:2] in (["system", "prune"], ["container", "prune"])
                   for command in commands)
    assert len([command for command in commands if command[:2] == ["network", "create"]]) == 5
    assert len([command for command in commands if command[:2] == ["network", "rm"]]) == 5
    assert len([command for command in commands if command[:1] == ["rm"]]) == 15
    assert all((output / name / "source/probe.cpp").is_file()
               for name in ("p50-p29v1", "p50-zstd-tu", "p50-zstd-route",
                            "p43-worker", "p43-client"))


def test_docker_failure_is_recorded_and_only_owned_resources_are_cleaned(tmp_path: Path) -> None:
    result, output, commands = _run_cli(tmp_path, failure="client-compile")

    assert result.returncode != 0
    summary = json.loads((output / "summary.json").read_text())
    assert summary["status"] == "FAIL"
    assert summary["cases"][0]["status"] == "FAIL"
    assert "injected docker failure" in result.stderr or any(
        "client-compile" in step["log"] for step in summary["steps"]
    )
    assert [command[-1] for command in commands if command[:1] == ["rm"]] == [
        command[command.index("--name") + 1]
        for command in commands
        if command[:1] == ["run"] and "--name" in command
    ][::-1]
    assert any(command[:2] == ["network", "rm"] for command in commands)
    assert not any("prune" in command for command in commands)


def test_scheduler_start_failure_still_cleans_all_owned_names_and_network(tmp_path: Path) -> None:
    result, output, commands = _run_cli(tmp_path, failure="scheduler-start")

    assert result.returncode != 0
    summary = json.loads((output / "summary.json").read_text())
    assert summary["status"] == "FAIL"
    assert summary["cases"][0]["status"] == "FAIL"
    attempted = [
        command[command.index("--name") + 1]
        for command in commands if command[:1] == ["run"] and "--name" in command
    ]
    removed = [command[-1] for command in commands if command[:1] == ["rm"]]
    assert len(attempted) == 1
    scheduler = attempted[0]
    base = scheduler.removesuffix("-scheduler")
    assert removed == [base + "-client", base + "-worker", scheduler]
    assert any(command[:2] == ["network", "rm"] for command in commands)


def test_remote_host_must_be_proven_not_inferred_from_compile_success(tmp_path: Path) -> None:
    result, output, commands = _run_cli(tmp_path, remote=False)

    assert result.returncode != 0
    summary = json.loads((output / "summary.json").read_text())
    assert summary["status"] == "FAIL"
    assert "does not prove compilation on worker" in summary["cases"][0]["error"]
    assert any(command[:1] == ["rm"] for command in commands)
    assert any(command[:2] == ["network", "rm"] for command in commands)


def test_profile_mismatch_is_rejected(tmp_path: Path) -> None:
    result, output, _ = _run_cli(tmp_path, wrong_profile=True)

    assert result.returncode != 0
    summary = json.loads((output / "summary.json").read_text())
    assert summary["cases"][0]["status"] == "FAIL"
    assert "P29V1 input attachment" in summary["cases"][0]["error"]


def test_invalid_jobs_and_output_are_rejected_before_docker(tmp_path: Path) -> None:
    docker, calls = _fake_docker(tmp_path)
    env = os.environ | {"ICEFARM_DOCKER": str(docker), "FAKE_DOCKER_CALLS": str(calls)}
    result = subprocess.run(
        [sys.executable, str(ROOT / "dev/mixed.py"),
         "--current-image", "current", "--legacy-image", "legacy",
         "--output", str(tmp_path / "bad-jobs"), "--jobs", "0"],
        cwd=ROOT, env=env, text=True, capture_output=True,
    )
    assert result.returncode != 0
    assert not calls.exists()
