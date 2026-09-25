from __future__ import annotations

import json
import hashlib
import os
from pathlib import Path
import subprocess
import sys
import pytest

ROOT = Path(__file__).resolve().parents[3]


def _fake_docker(tmp_path: Path, *, failure: str = "", remote: bool = True,
                 wrong_profile: bool = False, strip_measured_role: str = "",
                 reuse_pid: bool = False, wrong_job_prefix: bool = False
                 ) -> tuple[Path, Path]:
    path = tmp_path / "docker"
    calls = tmp_path / "docker-calls.jsonl"
    code = f"""#!{sys.executable}
import json, os, pathlib, sys
args = sys.argv[1:]
with open(os.environ['FAKE_DOCKER_CALLS'], 'a', encoding='utf-8') as out:
    out.write(json.dumps(args) + '\\n')
fail = os.environ.get('FAKE_DOCKER_FAILURE')
strip_measured = os.environ.get('FAKE_DOCKER_STRIP_MEASURED')
reuse_pid = os.environ.get('FAKE_DOCKER_REUSE_PID') == '1'
wrong_job_prefix = os.environ.get('FAKE_DOCKER_WRONG_JOB_PREFIX') == '1'
d18_profile = os.environ.get('FAKE_D18_PROFILE', 'P29V1')
if fail and ((fail == 'scheduler-start' and args[:1] == ['run'] and '--network-alias' in args and 'scheduler' in args) or
             (fail == 'client-compile' and args[:1] == ['run'] and '--network-alias' not in args)):
    print('injected docker failure', file=sys.stderr)
    raise SystemExit(17)
if args[:1] == ['exec'] and args[1].endswith('-scheduler'):
    if 'nc -z' in args[-1]:
        raise SystemExit(0)
    if 'p51-old50-scheduler-fallback' in args[1]:
        print(' qa-worker (172.18.0.2:10245) 1 1 cache=off')
    elif 'd18-' in args[1]:
        print(' qa-d18-r1 (172.18.0.2:10245) 1 1 cache=127.0.0.1:11001 cache_wire=v1 cache_protocol=1 cache_profiles=p29v1')
        print(' qa-d18-r2 (172.18.0.3:10245) 1 1 cache=127.0.0.1:11001 cache_wire=v1 cache_protocol=2 cache_profiles=p29v1')
    else:
        print(' qa-worker (172.18.0.2:10245) 1 1')
    raise SystemExit(0)
if args[:2] == ['inspect', '--format']:
    print('172.18.0.3' if '-r2-worker' in args[-1] else '172.18.0.2')
    raise SystemExit(0)
if args[:2] == ['image', 'inspect']:
    image_id = 'sha256:' + ('a' * 64 if 'p43' not in args[-1] else 'b' * 64)
    labels = {{'icecream.source.identity': 'fake-source',
              'org.icecream.dev.recipe': 'fake-recipe',
              'org.icecream.dev.profile': 'ubuntu24.04'}}
    print(image_id + ' ' + json.dumps(labels))
    raise SystemExit(0)
if args[:1] == ['logs'] and any(arg.endswith('-worker') for arg in args):
    if 'd18-' in ' '.join(args):
        joined = ' '.join(args)
        if '-r2-worker' in joined:
            attachment_job = '30' if wrong_job_prefix else '3'
            print('[81] remote compile for file /source/r2.cpp' + chr(10) +
                  '[81] Remote compilation completed with exit code 0' + chr(10) +
                  '[1] P50 CompileFile attached exact ' + d18_profile + ' input for job ' + attachment_job + chr(10) +
                  'P51 cache-link descriptor adopted by sidecar')
        else:
            print(('[39] remote compile for file /source/p43.cpp' + chr(10) +
                   '[39] Remote compilation completed with exit code 0' + chr(10) +
                   '[157] remote compile for file /source/r1.cpp' + chr(10) +
                   '[157] Remote compilation completed with exit code 0' + chr(10) +
                   '[1] P50 CompileFile attached exact ' + d18_profile + ' input for job 20' + chr(10) +
                   '[1] P50 CompileFile attached exact ' + d18_profile + ' input for job 2'))
        raise SystemExit(0)
    print('Remote compilation completed with exit code 0')
    joined = ' '.join(args)
    profile = ('ZSTD_TU' if {str(wrong_profile)} else
               'ZSTD_ROUTE' if ('zstd-route' in joined) else
               'ZSTD_TU' if ('zstd-tu' in joined) else 'P29V1')
    print('P50 CompileFile attached exact ' + profile + ' input for job 1')
    if 'p51-r2-' in joined and 'old50' not in joined:
        print('P51 cache-link descriptor adopted by sidecar')
    raise SystemExit(0)
if args[:1] == ['logs'] and any(arg.endswith('-scheduler') for arg in args):
    print('assignment fence: strict-nonce')
    joined = ' '.join(args)
    if 'p51-old50-scheduler-fallback' in joined:
        print('RELOGIN qa-worker(x86_64): [] cache=off')
    elif 'p51-r2-' in joined:
        print('RELOGIN qa-worker(x86_64): [] cache=127.0.0.1:11001 cache_wire=v1 cache_protocol=2 cache_profiles=p29v1 zstd_tu zstd_route')
    else:
        print('RELOGIN qa-worker(x86_64): [] cache=127.0.0.1:11001 cache_wire=v1 cache_protocol=1 cache_profiles=p29v1 zstd_tu zstd_route')
    raise SystemExit(0)
if args[:1] == ['logs'] and any(arg.endswith(('-p43', '-r1', '-r2')) for arg in args):
    role = next(arg.rsplit('-', 1)[1] for arg in args if arg.endswith(('-p43', '-r1', '-r2')))
    output = dict(p43='ICECREAM_D18_P43_OK', r1='ICECREAM_D18_R1_OK',
                  r2='ICECREAM_D18_R2_OK')[role]
    print('D18_RESULT role=' + role + ' output=' + output)
    raise SystemExit(0)
if args[:1] == ['exec'] and args[1].endswith('-worker'):
    joined = ' '.join(args)
    standard = next((std for std in ('11', '14', '17')
                     if f'gnu++{{std}}' in joined or f'c++{{std}}' in joined), '11')
    pid = {{'11': '123', '14': '124', '17': '125'}}[standard]
    start = '999' if reuse_pid and ('wanted=' + pid) in joined else '100' + pid
    print(pid + '|' + start + '|cc1plus -std=gnu++' + standard + ' /source/probe.cpp')
    raise SystemExit(0)
if args[:1] == ['run'] and '--network-alias' not in args:
    if '--detach' in args and 'd18-' in ' '.join(args):
        name = args[args.index('--name') + 1]
        role = name.rsplit('-', 1)[1]
        mounts = [a for a in args if a.startswith('type=bind,src=')]
        log_mount = next(a for a in mounts if ',dst=/qa-logs' in a)
        log_dir = pathlib.Path(log_mount.split('src=', 1)[1].split(',dst=', 1)[0])
        log_dir.mkdir(parents=True, exist_ok=True)
        output = dict(p43='ICECREAM_D18_P43_OK', r1='ICECREAM_D18_R1_OK',
                      r2='ICECREAM_D18_R2_OK')[role]
        host = '172.18.0.3' if role == 'r2' else '172.18.0.2'
        job_id = dict(p43='1', r1='2', r2='3')[role]
        profile_log = ('P50 assignment identity bound for job ' + job_id + chr(10) +
                       d18_profile + ' source committed for P50 CompileFile' + chr(10)) if role != 'p43' else ''
        if strip_measured == role:
            profile_log = ''
        (log_dir / 'icecc.log').write_text(
            'Have to use host ' + host + ':10245 - Job ID: ' + job_id + '\\n' + profile_log)
        (log_dir / 'warm-icecc.log').write_text(
            'P50 assignment identity bound for job ' + job_id + chr(10) +
            'P29V1 source committed for P50 CompileFile' + chr(10))
        (log_dir / 'client-daemon.log').write_text(
            'P51 C-cache source-control lease delivered for assignment 3 profile 1 window 30' + chr(10)
            if role == 'r2' else '')
        control_mount = next(a for a in mounts if ',dst=/qa-control' in a)
        control_dir = pathlib.Path(control_mount.split('src=', 1)[1].split(',dst=', 1)[0])
        control_dir.mkdir(parents=True, exist_ok=True)
        (control_dir / ('ready-' + role)).touch()
        raise SystemExit(0)
    mounts = [a for a in args if a.startswith('type=bind,src=') and ',dst=/qa-logs' in a]
    assert mounts, args
    log_dir = pathlib.Path(mounts[-1].split('src=', 1)[1].split(',dst=', 1)[0])
    host = '172.18.0.2' if {str(remote)} else '172.18.0.99'
    is_p50 = 'ICECC_P50_C1F1_REQUIRED=1' in ' '.join(args)
    profile = next((arg.split('=', 1)[1] for arg in args if arg.startswith('ICECC_P50_PROFILE=')), 'P29V1')
    p50 = ('P50 assignment identity bound for job 1\\n' + profile + ' source committed for P50 CompileFile\\n'
           if is_p50 else '')
    (log_dir / 'icecc.log').write_text('Have to use host ' + host + ':10245 - Job ID: 1\\n' + p50)
    is_p51 = 'ICECC_P51_MODE=on' in ' '.join(args)
    is_old50 = 'p51-old50-scheduler-fallback' in ' '.join(args)
    if is_p51 and not is_old50:
        (log_dir / 'client-daemon.log').write_text(
            'P51 C-cache source-control lease delivered for assignment 1\\n'
        )
    print('ICECREAM_MIXED_OK')
    raise SystemExit(0)
if args[:1] == ['network'] and len(args) > 1 and args[1] == 'create':
    print(args[-1])
if args[:1] == ['wait']:
    print('0')
    raise SystemExit(0)
raise SystemExit(0)
"""
    path.write_text(code, encoding="utf-8")
    path.chmod(0o755)
    return path, calls


def _run_cli(tmp_path: Path, *, failure: str = "", remote: bool = True,
             wrong_profile: bool = False, p51_r2: bool = False,
             concurrent_mixed: bool = False,
             concurrent_profile: str | None = None,
             d18_reported_profile: str = "P29V1",
             strip_measured_role: str = "", reuse_pid: bool = False,
             wrong_job_prefix: bool = False,
             only_p51_r2: bool = False,
             only_old50_scheduler_fallback: bool = False,
             old50_binary: Path | None = None,
             old50_sha256: str | None = None) -> tuple[subprocess.CompletedProcess[str], Path, list[list[str]]]:
    docker, calls = _fake_docker(tmp_path, failure=failure, remote=remote,
                                 wrong_profile=wrong_profile,
                                 strip_measured_role=strip_measured_role,
                                 reuse_pid=reuse_pid,
                                 wrong_job_prefix=wrong_job_prefix)
    output = tmp_path / "mixed-results"
    env = os.environ.copy()
    env.update(ICEFARM_DOCKER=str(docker), FAKE_DOCKER_CALLS=str(calls),
               FAKE_DOCKER_FAILURE=failure,
               FAKE_DOCKER_STRIP_MEASURED=strip_measured_role,
               FAKE_DOCKER_REUSE_PID="1" if reuse_pid else "0",
               FAKE_DOCKER_WRONG_JOB_PREFIX="1" if wrong_job_prefix else "0",
               FAKE_D18_PROFILE=d18_reported_profile,
               PYTHONDONTWRITEBYTECODE="1")
    if concurrent_mixed:
        env["ICEFARM_TMPDIR"] = str(tmp_path)
    command = [
        sys.executable, str(ROOT / "dev/mixed.py"),
        "--current-image", "icecream-dev:current", "--legacy-image", "icecream-dev:p43",
        "--output", str(output), "--jobs", "3" if concurrent_mixed else "2",
    ]
    if concurrent_mixed:
        command += ["--memory-gb", "4", "--concurrent-mixed"]
    if concurrent_profile is not None:
        command += ["--concurrent-profile", concurrent_profile]
    if p51_r2:
        command.append("--p51-r2")
    if only_p51_r2:
        command.append("--only-p51-r2")
    if only_old50_scheduler_fallback:
        command.append("--only-old50-scheduler-fallback")
    if old50_binary is not None:
        command += [
            "--ordinary50-scheduler-binary", str(old50_binary),
            "--ordinary50-scheduler-sha256", old50_sha256 or "",
            "--ordinary50-scheduler-source-commit", "94e9b44025887412c70c1c46c35fc588d6dec776",
        ]
    result = subprocess.run(
        command,
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


def test_opt_in_r2_profiles_and_pinned_old50_scheduler_fallback(tmp_path: Path) -> None:
    old50 = tmp_path / "icecc-scheduler-50"
    old50.write_bytes(b"pinned protocol 50 scheduler fixture\n")
    old50.chmod(0o755)
    digest = hashlib.sha256(old50.read_bytes()).hexdigest()
    result, output, commands = _run_cli(
        tmp_path, p51_r2=True, old50_binary=old50, old50_sha256=digest,
    )

    assert result.returncode == 0, result.stderr
    summary = json.loads((output / "summary.json").read_text())
    assert summary["status"] == "PASS"
    assert [case["name"] for case in summary["cases"]] == [
        "p50-p29v1", "p50-zstd-tu", "p50-zstd-route", "p43-worker", "p43-client",
        "p51-r2-p29v1", "p51-r2-zstd-tu", "p51-r2-zstd-route",
        "p51-old50-scheduler-fallback",
    ]
    current_r2 = summary["cases"][5:8]
    assert [case["selected_profile"] for case in current_r2] == [
        "P29V1", "ZSTD_TU", "ZSTD_ROUTE",
    ]
    assert all(case["p51_opt_in"] is True and case["r2_selected"] is True
               and case["r2_source_lease"] is True and case["r2_link_adopted"] is True
               for case in current_r2)
    fallback = summary["cases"][8]
    assert fallback["ordinary_scheduler_protocol"] == 50
    assert fallback["ordinary_scheduler_source_commit"] == (
        "94e9b44025887412c70c1c46c35fc588d6dec776"
    )
    assert fallback["ordinary_scheduler_binary_sha256"] == digest
    assert fallback["p51_opt_in"] is True
    assert fallback["r2_selected"] is False
    assert fallback["cache_fallback"] == "R1-or-disabled"

    fallback_scheduler = next(
        command for command in commands
        if command[:1] == ["run"] and "--network-alias" in command
        and command[command.index("--network-alias") + 1] == "scheduler"
        and "p51-old50-scheduler-fallback" in " ".join(command)
    )
    assert any(
        arg == f"type=bind,src={old50},dst=/qa-bin/icecc-scheduler,readonly"
        for arg in fallback_scheduler
    )
    assert "/qa-bin/icecc-scheduler -n" in " ".join(fallback_scheduler)
    assert 'chown "$runtime_uid:$runtime_gid" /qa-logs/scheduler.log' in " ".join(fallback_scheduler)
    assert "ICECC_P51_MODE=on" not in fallback_scheduler
    fallback_worker = next(
        command for command in commands
        if command[:1] == ["run"] and "--network-alias" in command
        and command[command.index("--network-alias") + 1] == "worker"
        and "p51-old50-scheduler-fallback" in " ".join(command)
    )
    fallback_client = next(
        command for command in commands
        if command[:1] == ["run"] and "--network-alias" not in command
        and "p51-old50-scheduler-fallback" in " ".join(command)
    )
    assert "ICECC_P51_MODE=on" in fallback_worker
    assert "ICECC_P51_MODE=on" in fallback_client
    assert "ICECC_P50_C1F1_REQUIRED=1" not in " ".join(fallback_client)
    r2_role_runs = [
        command for command in commands
        if command[:1] == ["run"] and "p51-r2-" in " ".join(command)
        and "--network-alias" in command
        and command[command.index("--network-alias") + 1] in ("worker", "scheduler")
    ]
    assert r2_role_runs
    assert all("ICECC_P51_MODE=on" in command for command in r2_role_runs
               if command[command.index("--network-alias") + 1] in ("scheduler", "worker"))
    r2_client = next(
        command for command in commands
        if command[:1] == ["run"] and "--network-alias" not in command
        and "p51-r2-p29v1" in " ".join(command)
    )
    client_script = r2_client[-1]
    assert "install -m 0644 /dev/null /qa-logs/client-daemon.log" in client_script
    assert 'chown "$runtime_uid:$runtime_gid" /qa-logs/client-daemon.log' in client_script


def test_p51_only_selector_skips_unchanged_legacy_rows(tmp_path: Path) -> None:
    result, output, commands = _run_cli(tmp_path, p51_r2=True, only_p51_r2=True)
    assert result.returncode == 0, result.stderr
    summary = json.loads((output / "summary.json").read_text())
    assert [case["name"] for case in summary["cases"]] == [
        "p51-r2-p29v1", "p51-r2-zstd-tu", "p51-r2-zstd-route",
    ]
    assert all(case["r2_selected"] for case in summary["cases"])
    assert len([command for command in commands if command[:2] == ["network", "create"]]) == 3


def test_concurrent_mixed_uses_one_scheduler_and_observes_all_role_compilers(
    tmp_path: Path,
) -> None:
    result, output, commands = _run_cli(tmp_path, concurrent_mixed=True)
    assert result.returncode == 0, result.stderr
    summary = json.loads((output / "summary.json").read_text())
    assert summary["status"] == "PASS"
    assert summary["concurrent_mixed_requested"] is True
    assert len(summary["cases"]) == 1
    case = summary["cases"][0]
    assert case["status"] == "PASS"
    assert case["assignment_policy"] == "enforcing-compat"
    assert case["image_provenance"]["current"]["image_id"].startswith("sha256:")
    assert case["image_provenance"]["current"]["source_identity"]
    assert case["image_provenance"]["p43"]["image_id"].startswith("sha256:")
    assert case["worker_modes"] == {"r1": "P29V1/R1", "r2": "P29V1/R2"}
    assert set(case["concurrent_role_cc1plus"]) == {"p43", "r1", "r2"}
    assert all(item["revalidated"] == "true"
               for item in case["concurrent_role_cc1plus"].values())
    assert case["concurrent_role_cc1plus"]["p43"]["worker"] == "r1"
    assert case["concurrent_role_cc1plus"]["r1"]["worker"] == "r1"
    assert case["concurrent_role_cc1plus"]["r2"]["worker"] == "r2"
    assert case["p43_remote"] and case["r1_remote"] and case["r2_remote"]
    assert case["selected_profile"] == "P29V1"
    assert "r1_remote_p29v1" not in case and "r2_remote_p29v1" not in case
    assert case["r2_source_lease"] and case["r2_link_adopted"]
    assert len([command for command in commands
                if command[:2] == ["network", "create"]]) == 1
    scheduler_runs = [command for command in commands
                      if command[:1] == ["run"] and "--network-alias" in command
                      and command[command.index("--network-alias") + 1] == "scheduler"]
    worker_runs = [command for command in commands
                   if command[:1] == ["run"] and "--network-alias" in command
                   and command[command.index("--network-alias") + 1].startswith("worker-")]
    client_runs = [command for command in commands
                   if command[:1] == ["run"] and "--network-alias" not in command]
    assert len(scheduler_runs) == 1 and len(worker_runs) == 2
    assert len(client_runs) == 3
    assert all("--detach" in command for command in client_runs)
    assert all("--network" in command and command[command.index("--network") + 1]
               == scheduler_runs[0][scheduler_runs[0].index("--network") + 1]
               for command in [*worker_runs, *client_runs])
    assert any("ICECC_D18_ROLE_P43=1" in " ".join(command) for command in client_runs)
    assert any("ICECC_D18_ROLE_R1=1" in " ".join(command) for command in client_runs)
    assert any("ICECC_D18_ROLE_R2=1" in " ".join(command) for command in client_runs)
    worker_env = [" ".join(command) for command in worker_runs]
    assert any("ICECC_P51_MODE=off" in command for command in worker_env)
    assert any("ICECC_P51_MODE=on" in command for command in worker_env)


@pytest.mark.parametrize("profile", ["ZSTD_TU", "ZSTD_ROUTE"])
def test_concurrent_profile_rejects_p29_attachment_evidence(
    tmp_path: Path, profile: str,
) -> None:
    # This fake worker deliberately still reports P29V1. Selecting a different
    # profile must change the evidence check, not just the container environment.
    result, output, commands = _run_cli(
        tmp_path, concurrent_mixed=True, concurrent_profile=profile,
    )
    assert result.returncode != 0
    summary = json.loads((output / "summary.json").read_text())
    assert summary["concurrent_profile"] == profile
    assert "lacks exact F input attachment" in summary["cases"][0]["error"]
    workers = [command for command in commands
               if command[:1] == ["run"] and "--name" in command
               and command[command.index("--name") + 1].endswith("-worker")]
    assert len(workers) == 2
    assert all(f"ICECC_P50_PROFILE={profile}" in command for command in workers)


@pytest.mark.parametrize("profile", ["P29V1", "ZSTD_TU", "ZSTD_ROUTE"])
def test_concurrent_profile_selects_matching_evidence(tmp_path: Path, profile: str) -> None:
    result, output, commands = _run_cli(
        tmp_path, concurrent_mixed=True, concurrent_profile=profile,
        d18_reported_profile=profile,
    )
    assert result.returncode == 0, result.stderr
    summary = json.loads((output / "summary.json").read_text())
    assert summary["status"] == "PASS"
    assert summary["concurrent_profile"] == profile
    case = summary["cases"][0]
    assert case["selected_profile"] == profile
    assert case["p43_remote"] and case["r1_remote"] and case["r2_remote"]
    assert "r1_remote_p29v1" not in case and "r2_remote_p29v1" not in case
    assert case["worker_modes"] == {"r1": f"{profile}/R1", "r2": f"{profile}/R2"}
    for command in commands:
        if command[:1] != ["run"] or "--name" not in command:
            continue
        name = command[command.index("--name") + 1]
        if name.endswith("-p43"):
            assert not any(arg.startswith("ICECC_P50_PROFILE=") for arg in command)
        else:
            assert f"ICECC_P50_PROFILE={profile}" in command
        if name.endswith(("-r1", "-r2")):
            assert f"export ICECC_P50_PROFILE={profile} " in " ".join(command)


def test_concurrent_profile_requires_concurrent_gate(tmp_path: Path) -> None:
    result, output, commands = _run_cli(tmp_path, concurrent_profile="P29V1")
    assert result.returncode != 0
    assert "--concurrent-profile requires --concurrent-mixed" in result.stderr
    assert not commands
    assert not output.exists()


def test_concurrent_profile_rejects_unknown_profile(tmp_path: Path) -> None:
    result, output, commands = _run_cli(
        tmp_path, concurrent_mixed=True, concurrent_profile="ZSTD_UNKNOWN",
    )
    assert result.returncode != 0
    assert "invalid choice" in result.stderr
    assert not commands
    assert not output.exists()


def test_concurrent_mixed_requires_bounded_parallel_worker_and_scratch(
    tmp_path: Path,
) -> None:
    docker, calls = _fake_docker(tmp_path)
    output = tmp_path / "mixed-results"
    env = os.environ.copy()
    env.update(ICEFARM_DOCKER=str(docker), FAKE_DOCKER_CALLS=str(calls),
               PYTHONDONTWRITEBYTECODE="1")
    command = [sys.executable, str(ROOT / "dev/mixed.py"),
               "--current-image", "icecream-dev:current",
               "--legacy-image", "icecream-dev:p43", "--output", str(output),
               "--jobs", "2", "--memory-gb", "4", "--concurrent-mixed"]
    result = subprocess.run(command, cwd=ROOT, env=env, text=True,
                            capture_output=True, timeout=20)
    assert result.returncode != 0
    assert "--jobs >= 3" in result.stderr
    assert not calls.exists()


@pytest.mark.parametrize("role", ["r1", "r2"])
def test_concurrent_mixed_rejects_warmup_only_p50_evidence(
    tmp_path: Path, role: str,
) -> None:
    result, output, _ = _run_cli(
        tmp_path, concurrent_mixed=True, strip_measured_role=role,
    )
    assert result.returncode != 0
    summary = json.loads((output / "summary.json").read_text())
    assert "lacks P50 assignment identity" in summary["cases"][0]["error"]


def test_concurrent_mixed_rejects_reused_compiler_pid_identity(tmp_path: Path) -> None:
    result, output, _ = _run_cli(tmp_path, concurrent_mixed=True, reuse_pid=True)
    assert result.returncode != 0
    summary = json.loads((output / "summary.json").read_text())
    assert "PID/starttime did not survive overlap bracket" in summary["cases"][0]["error"]


def test_concurrent_mixed_rejects_job_id_prefix_collision(tmp_path: Path) -> None:
    result, output, _ = _run_cli(
        tmp_path, concurrent_mixed=True, wrong_job_prefix=True,
    )
    assert result.returncode != 0
    summary = json.loads((output / "summary.json").read_text())
    assert "measured job 3 lacks exact F input attachment" in summary["cases"][0]["error"]


def test_old50_only_selector_runs_pinned_fallback_row(tmp_path: Path) -> None:
    old50 = tmp_path / "icecc-scheduler-50"
    old50.write_bytes(b"pinned protocol 50 scheduler fixture\n")
    old50.chmod(0o755)
    digest = hashlib.sha256(old50.read_bytes()).hexdigest()
    result, output, commands = _run_cli(
        tmp_path, p51_r2=True, only_old50_scheduler_fallback=True,
        old50_binary=old50, old50_sha256=digest,
    )
    assert result.returncode == 0, result.stderr
    summary = json.loads((output / "summary.json").read_text())
    assert [case["name"] for case in summary["cases"]] == [
        "p51-old50-scheduler-fallback",
    ]
    assert summary["cases"][0]["r2_selected"] is False
    assert len([command for command in commands if command[:2] == ["network", "create"]]) == 1


def test_old50_binary_digest_is_checked_before_docker(tmp_path: Path) -> None:
    old50 = tmp_path / "icecc-scheduler-50"
    old50.write_bytes(b"not the pinned binary")
    old50.chmod(0o755)
    result, _, commands = _run_cli(
        tmp_path, p51_r2=True, old50_binary=old50, old50_sha256="0" * 64,
    )
    assert result.returncode != 0
    assert "does not match the scheduler binary" in result.stderr
    assert not commands


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
