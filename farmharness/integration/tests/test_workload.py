from __future__ import annotations

import hashlib
import base64
import concurrent.futures
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import threading
import time
import textwrap
import tempfile
from types import SimpleNamespace

import pytest

from farmharness.integration.tests import farm_fixture

from farmharness.integration import farmtest, workload as workload_module
from farmharness.integration import scenario_spec as scenario_spec_module
from farmharness.integration.farm_spec import load_farm_spec
from farmharness.integration.layout import oracle_root
from farmharness.integration.images import CommandFactory, RecordingTransport
from farmharness.integration.events import EventProducer
from farmharness.integration.remote import (
    CommandResult,
    PlannedCommand,
    SubprocessTransport,
    decode_ssh_payload,
)
from farmharness.integration.scenario_spec import ScenarioSpecError, load_scenario_spec
from farmharness.integration.workload import (
    MANIFEST_DRIVER,
    D18_PROCESS_OBSERVER,
    WORKLOAD_SCHEMA,
    WorkloadError,
    _d18_barrier_and_observe,
    _active_loss_serial_through,
    _d18_concurrent_witness,
    _driver_command,
    _parse_summary,
    _p51_check_held_marker,
    _p51_check_scoped_identity_marker,
    _p51_link_output_check_argv,
    _p51_link_outputs_complete,
    _p51_require_sibling_held,
    _strict_p50_required,
    run_workload,
)


INTEGRATION = Path(__file__).resolve().parents[1]


def test_manifest_driver_file_keeps_the_reviewed_script_bytes() -> None:
    driver_path = INTEGRATION / "workers" / "manifest_driver.sh"

    assert MANIFEST_DRIVER == driver_path.read_text(encoding="utf-8")
    assert hashlib.sha256(MANIFEST_DRIVER.encode("utf-8")).hexdigest() == (
        "729b90901e55bd5d794e4e3985454d54bf7c4e37e027cbed38e1c55643c7ca01"
    )


def test_p51_shell_route_mapping_is_exact_in_child_bash() -> None:
    match = re.search(
        r"(?ms)^p51_expected_worker\(\) \{.*?^\}", MANIFEST_DRIVER
    )
    assert match is not None
    prefix = match.group(0)
    good = subprocess.run(
        ["/bin/bash", "-c", prefix + '\np51_link_map="1-2=F1,3-5=F2"; p51_expected_worker 4'],
        text=True, capture_output=True, check=False,
    )
    assert good.returncode == 0
    assert good.stdout == "F2"
    named_worker = subprocess.run(
        ["/bin/bash", "-c", prefix + '\np51_link_map="1-2=F_R1,3-5=F_R2"; p51_expected_worker 2'],
        text=True, capture_output=True, check=False,
    )
    assert named_worker.returncode == 0
    assert named_worker.stdout == "F_R1"
    exported_child = subprocess.run(
        [
            "/bin/bash", "-c",
            prefix + '\np51_link_map="1-2=F_R1,3-5=F_R2"; '
            'export p51_link_map; export -f p51_expected_worker; '
            '/bin/bash -c "p51_expected_worker 4"',
        ],
        text=True, capture_output=True, check=False,
    )
    assert exported_child.returncode == 0
    assert exported_child.stdout == "F_R2"
    for link_map in ("1-3=F1,3-5=F2", "1-2=F1,4-5=F2", "1-2=F1,bad"):
        overlap_or_hole = subprocess.run(
            ["/bin/bash", "-c", prefix + f'\np51_link_map="{link_map}"; p51_expected_worker 3'],
            text=True, capture_output=True, check=False,
        )
        assert overlap_or_hole.returncode != 0
    assert "export -f read_boundary_release p51_expected_worker p51_expected_endpoint compile_one" in MANIFEST_DRIVER


def test_p51_shell_endpoint_mapping_is_exact_in_child_bash() -> None:
    match = re.search(
        r"(?ms)^p51_expected_endpoint\(\) \{.*?^\}", MANIFEST_DRIVER
    )
    assert match is not None
    prefix = match.group(0)
    good = subprocess.run(
        [
            "/bin/bash", "-c",
            prefix + '\np51_link_endpoint_map="1-2=10.0.0.2:23005,3-5=10.0.0.3:23006"; p51_expected_endpoint 4',
        ], text=True, capture_output=True, check=False,
    )
    assert good.returncode == 0
    assert good.stdout == "10.0.0.3:23006"
    exported_child = subprocess.run(
        [
            "/bin/bash", "-c",
            prefix + '\np51_link_endpoint_map="1-2=10.0.0.2:23005,3-5=10.0.0.3:23006"; '
            'export p51_link_endpoint_map; export -f p51_expected_endpoint; '
            '/bin/bash -c "p51_expected_endpoint 1"',
        ], text=True, capture_output=True, check=False,
    )
    assert exported_child.returncode == 0
    assert exported_child.stdout == "10.0.0.2:23005"

    validator = re.search(
        r"(?ms)^p51_validate_link_endpoint_map\(\) \{.*?^\}", MANIFEST_DRIVER
    )
    assert validator is not None
    validation_script = prefix + "\n" + validator.group(0)
    for endpoint_map, should_fail in (
        ("1-2=10.0.0.2:23005,3-5=10.0.0.3:23006", False),
        ("1-3=10.0.0.2:23005,3-5=10.0.0.3:23006", True),
        ("1-2=10.0.0.2:23005,4-5=10.0.0.3:23006", True),
        ("1-5=10.0.0.2:65536", True),
        ("1-5=not-an-endpoint", True),
    ):
        checked = subprocess.run(
            [
                "/bin/bash", "-c",
                validation_script
                + f'\np51_link_endpoint_map="{endpoint_map}"; expected_jobs=5; '
                + "p51_validate_link_endpoint_map",
            ], text=True, capture_output=True, check=False,
        )
        assert (checked.returncode != 0) is should_fail


@pytest.mark.parametrize(
    ("window", "jobs", "concurrency", "routes"),
    [
        (1, 31, 2, "1-16=F1,17-31=F2"),
        (30, 62, 60, "1-31=F1,32-62=F2"),
    ],
)
def test_multilink_dispatch_prefills_each_gate_before_suffix_jobs(
    tmp_path: Path, window: int, jobs: int, concurrency: int, routes: str,
) -> None:
    match = re.search(
        r"(?ms)^    python3 - \"\$worklist\" \"\$dispatch_worklist\" "
        r"\"\$expected_jobs\" \"\$jobs\" \\\n        \"\$p51_link_window\" "
        r"\"\$p51_link_map\" <<'PY'\n(.*?)\nPY$",
        MANIFEST_DRIVER,
    )
    assert match is not None
    source = tmp_path / "worklist.bin"
    destination = tmp_path / "dispatch.bin"
    records = [
        "\0".join((str(ordinal), "A", "1", f"TU/{ordinal:03d}.cpp", "a" * 64)) + "\0"
        for ordinal in range(1, jobs + 1)
    ]
    source.write_bytes("".join(records).encode())
    completed = subprocess.run(
        [
            sys.executable, "-c", match.group(1), str(source), str(destination),
            str(jobs), str(concurrency), str(window), routes,
        ],
        text=True, capture_output=True, check=False,
    )
    assert completed.returncode == 0, completed.stderr
    assert f"initial={2 * window} jobs={jobs}" in completed.stdout
    fields = destination.read_bytes().split(b"\0")
    assert fields[-1] == b""
    dispatched = [
        fields[index:index + 5]
        for index in range(0, len(fields) - 1, 5)
    ]
    ordinals = [int(row[0]) for row in dispatched]
    assert len(ordinals) == jobs and len(set(ordinals)) == jobs
    route_by_ordinal = {
        **{index: "F1" for index in range(1, 17 if window == 1 else 32)},
        **{index: "F2" for index in range(17 if window == 1 else 32, jobs + 1)},
    }
    first_wave = ordinals[: 2 * window]
    assert sum(route_by_ordinal[index] == "F1" for index in first_wave) == window
    assert sum(route_by_ordinal[index] == "F2" for index in first_wave) == window
    assert ordinals[2 * window:] == sorted(ordinals[2 * window:])
    if window == 30:
        undersized = subprocess.run(
            [
                sys.executable, "-c", match.group(1), str(source), str(destination),
                str(jobs), str(concurrency - 1), str(window), routes,
            ],
            text=True, capture_output=True, check=False,
        )
        assert undersized.returncode != 0


def test_multilink_dispatch_validation_failure_never_runs_xargs(
    tmp_path: Path,
) -> None:
    start = MANIFEST_DRIVER.index("dispatch_worklist=$worklist")
    end = MANIFEST_DRIVER.index("\nxargs -0 -n 5", start)
    dispatch_shell = MANIFEST_DRIVER[start:end]
    source = tmp_path / "worklist.bin"
    destination = tmp_path / "dispatch.bin"
    source.write_bytes(
        b"".join(
            b"\0".join(
                (str(ordinal).encode(), b"A", b"1", f"TU/{ordinal}.cpp".encode(), b"digest")
            ) + b"\0"
            for ordinal in range(1, 5)
        )
    )
    destination.write_bytes(b"preexisting-dispatch\n")
    xargs_marker = tmp_path / "xargs-invoked"
    bin_dir = tmp_path / "bin"
    bin_dir.mkdir()
    (bin_dir / "python3").write_text(
        f"#!/bin/sh\nexec {sys.executable} \"$@\"\n", encoding="utf-8"
    )
    (bin_dir / "xargs").write_text(
        "#!/bin/sh\nprintf invoked > \"$XARGS_MARKER\"\n", encoding="utf-8"
    )
    (bin_dir / "python3").chmod(0o755)
    (bin_dir / "xargs").chmod(0o755)
    script = f'''set +e
worklist={source}
dispatch_worklist={destination}
result_root={tmp_path}
expected_jobs=4
jobs=2
p51_link_window=1
p51_link_map=1-1=F1,3-4=F2
export PATH={bin_dir}:"$PATH"
export XARGS_MARKER={xargs_marker}
{dispatch_shell}
xargs -0 -n 5 /bin/bash -c 'echo compile' fixture <"$dispatch_worklist"
'''
    result = subprocess.run(
        ["/bin/bash", "-c", script], text=True, capture_output=True, check=False
    )
    assert result.returncode != 0, result.stdout + result.stderr
    assert "dispatch-worklist validation failed" in result.stderr
    assert not xargs_marker.exists(), "invalid worklist must never start compiler jobs"
    assert destination.read_bytes() == b"preexisting-dispatch\n"


def test_multilink_receipt_markers_keep_scoped_identity_and_sibling_hold() -> None:
    held = (
        "count=30 first_ordinal=1 last_ordinal=30 profile=3 window=30 "
        f"relationship={'a' * 32} reservation={'b' * 32} epoch=8 generation=2\n"
    )
    parsed = _p51_check_held_marker(
        held, expected=30, profile="ZSTD_ROUTE", negotiated_window=30
    )
    identity = (
        f"c_store={'c' * 32} c_store_generation=4 "
        f"f_store={'d' * 32} f_store_generation=7 "
        f"relationship={'a' * 32} reservation={'b' * 32} "
        "epoch=8 generation=2 profile=3 window=30\n"
    )
    assert _p51_check_scoped_identity_marker(identity, parsed)["f_store_generation"] == 7
    with pytest.raises(WorkloadError, match="scoped identity"):
        _p51_check_scoped_identity_marker(identity.replace("window=30", "window=1"), parsed)
    with pytest.raises(WorkloadError, match="scoped identity"):
        _p51_check_scoped_identity_marker(
            identity.replace(f"relationship={'a' * 32}", f"relationship={'e' * 32}"),
            parsed,
        )

    _p51_require_sibling_held(
        gate_done=False, released_marker=None, exit_marker=None, outputs_complete=False
    )
    for state in (
        {"gate_done": True},
        {"released_marker": "present"},
        {"exit_marker": "present"},
        {"outputs_complete": True},
    ):
        args = {
            "gate_done": False, "released_marker": None,
            "exit_marker": None, "outputs_complete": False,
        }
        args.update(state)
        with pytest.raises(WorkloadError, match="sibling"):
            _p51_require_sibling_held(**args)


def test_multilink_output_checker_polls_pending_to_complete_with_real_transport(
    tmp_path: Path,
) -> None:
    result = tmp_path / "jobs" / "000001" / "result.tsv"
    argv = _p51_link_output_check_argv(
        "C1", 1, 1, "F_R1", "10.0.0.2:23005", "A"
    )
    command_argv = list(argv)
    command_argv[4] = str(tmp_path)
    command = PlannedCommand(
        sequence=1, phase="test.p51-output-check", host="localhost",
        instance="C1", transport="local", timeout_s=5,
        argv=tuple(command_argv),
    )
    transport = SubprocessTransport()

    # A missing result is an expected poll state, not a nonzero command
    # status that SubprocessTransport would turn into RemoteError.
    pending = transport.invoke(command)
    assert pending.returncode == 0
    assert _p51_link_outputs_complete(
        pending.stdout, client="C1", first_job=1, last_job=1,
        expected_worker="F_R1", expected_endpoint="10.0.0.2:23005",
    ) is False
    _p51_require_sibling_held(
        gate_done=False, released_marker=None, exit_marker=None,
        outputs_complete=False,
    )

    result.parent.mkdir(parents=True)
    fields = ["field"] * 14
    fields[0] = "1"
    fields[5] = "10.0.0.2:23005"
    fields[8] = "0"
    fields[9] = "a" * 64
    fields[10] = "a" * 64
    fields[11] = "1"
    fields[12] = "1"
    result.write_text("\t".join(fields) + "\n", encoding="utf-8")
    complete = transport.invoke(command)
    assert complete.returncode == 0
    assert _p51_link_outputs_complete(
        complete.stdout, client="C1", first_job=1, last_job=1,
        expected_worker="F_R1", expected_endpoint="10.0.0.2:23005",
    ) is True
    with pytest.raises(WorkloadError, match="sibling"):
        _p51_require_sibling_held(
            gate_done=False, released_marker=None, exit_marker=None,
            outputs_complete=True,
        )

    # A published but malformed row is terminal BAD, not an endlessly
    # pending observation.
    result.write_text("truncated\n", encoding="utf-8")
    malformed = transport.invoke(command)
    assert malformed.returncode == 0
    with pytest.raises(WorkloadError, match="output validation"):
        _p51_link_outputs_complete(
            malformed.stdout, client="C1", first_job=1, last_job=1,
            expected_worker="F_R1", expected_endpoint="10.0.0.2:23005",
        )

    invalid_rows = []
    wrong_index = fields.copy()
    wrong_index[0] = "2"
    invalid_rows.append(wrong_index)
    wrong_hash = fields.copy()
    wrong_hash[9] = "b" * 64
    invalid_rows.append(wrong_hash)
    for wrong_endpoint in ("10.0.0.2:23006", "10.0.0.3:23005"):
        wrong = fields.copy()
        wrong[5] = wrong_endpoint
        invalid_rows.append(wrong)
    for row in invalid_rows:
        result.write_text("\t".join(row) + "\n", encoding="utf-8")
        wrong = transport.invoke(command)
        assert wrong.returncode == 0
        with pytest.raises(WorkloadError, match="output validation"):
            _p51_link_outputs_complete(
                wrong.stdout, client="C1", first_job=1, last_job=1,
                expected_worker="F_R1", expected_endpoint="10.0.0.2:23005",
            )

    result.write_text(
        "\t".join(fields) + "\n" + "\t".join(fields) + "\n",
        encoding="utf-8",
    )
    extra = transport.invoke(command)
    with pytest.raises(WorkloadError, match="output validation"):
        _p51_link_outputs_complete(
            extra.stdout, client="C1", first_job=1, last_job=1,
            expected_worker="F_R1", expected_endpoint="10.0.0.2:23005",
        )


def test_manifest_result_publication_transitions_output_probe_atomically(
    tmp_path: Path,
) -> None:
    job_dir = tmp_path / "jobs" / "000001"
    job_dir.mkdir(parents=True)
    argv = list(_p51_link_output_check_argv(
        "C1", 1, 1, "F_R1", "10.0.0.2:23005", "A"
    ))
    argv[4] = str(tmp_path)
    probe = PlannedCommand(
        sequence=1, phase="test.output-probe", host="localhost", instance="C1",
        transport="local", timeout_s=5, argv=tuple(argv),
    )
    initial = SubprocessTransport().invoke(probe)
    assert initial.returncode == 0
    assert initial.stdout == "P51_LINK_OUTPUTS_PENDING\n"
    temporary = job_dir / ".result.tsv.tmp"
    temporary.write_text("partial publication", encoding="utf-8")
    while_publishing = SubprocessTransport().invoke(probe)
    assert while_publishing.returncode == 0
    assert while_publishing.stdout == "P51_LINK_OUTPUTS_PENDING\n"
    temporary.unlink()

    start = MANIFEST_DRIVER.index('    result_temporary="$job_dir/.result.tsv.tmp"')
    end = MANIFEST_DRIVER.index("    # Keep a completed prefix boundary", start)
    publish_script = textwrap.dedent(MANIFEST_DRIVER[start:end])
    env = {
        **os.environ,
        "job_dir": str(job_dir), "index": "1", "turn": "A",
        "occurrence": "1", "relative": "src/tiny.cc",
        "scheduler_job": "7", "worker": "10.0.0.2:23005",
        "started": "100", "finished": "101", "compile_rc": "0",
        "remote_sha": "a" * 64, "local_sha": "a" * 64,
        "exact": "1", "remote": "1", "retries": "0",
    }
    published = subprocess.run(
        ["/bin/bash", "-c", publish_script], env=env,
        text=True, capture_output=True, check=False,
    )
    assert published.returncode == 0, published.stderr
    assert not (job_dir / ".result.tsv.tmp").exists()
    complete = SubprocessTransport().invoke(probe)
    assert complete.returncode == 0
    assert _p51_link_outputs_complete(
        complete.stdout, client="C1", first_job=1, last_job=1,
        expected_worker="F_R1", expected_endpoint="10.0.0.2:23005",
    ) is True


def test_driver_keeps_worker_alias_preference_and_passes_endpoint_map() -> None:
    farm, scenario, plan, clients = _multilink_orchestrator_fixture("C1F2")
    command = _driver_command(
        farm, scenario, plan, clients[0], "A", CommandFactory()
    )
    assert "ICEFARM_P51_LINK_MAP=1-2=F1,3-4=F2" in command.argv
    assert "ICEFARM_P51_LINK_ENDPOINT_MAP=1-2=10.0.0.2:23003,3-4=10.0.0.3:23004" in command.argv
    assert 'preferred=(ICECC_PREFERRED_HOST="$mapped_worker" ICECC_REMOTE_REQUIRED=1)' in MANIFEST_DRIVER
    assert '"$worker" != "$mapped_endpoint"' in MANIFEST_DRIVER


def _multilink_orchestrator_fixture(topology: str):
    clients_count, workers_count = (1, 2) if topology == "C1F2" else (2, 1)
    names_c = [f"C{i + 1}" for i in range(clients_count)]
    names_f = [f"F{i + 1}" for i in range(workers_count)]
    instances = [
        {"role": "S", "name": "S", "env": {"ICECC_P50_PROFILE": "P29V1"}}
    ]
    instances += [
        {
            "role": "C", "name": name, "host": "host",
            "container_image": "sha256:" + "a" * 64,
            "compiler_recipe": {
                "executable": "/usr/bin/c++", "binary_sha256": "b" * 64,
                "arguments": ["-O2"],
            },
        }
        for name in names_c
    ]
    instances += [
        {"role": "F", "name": name, "address": f"10.0.0.{i + 2}", "slots": 1}
        for i, name in enumerate(names_f)
    ]
    links = []
    if topology == "C1F2":
        links = [
            {"client": "C1", "worker": "F1", "first_job": 1, "last_job": 2},
            {"client": "C1", "worker": "F2", "first_job": 3, "last_job": 4},
        ]
        instances[-2]["slots"] = instances[-1]["slots"] = 1
        jobs, tus = 4, 4
    else:
        links = [
            {"client": name, "worker": "F1", "first_job": 1, "last_job": 2}
            for name in names_c
        ]
        instances[-1]["slots"] = 2
        jobs, tus = 2, 2
    scenario = SimpleNamespace(data={
        "id": "multilink-receipt-test",
        "shape": "S'C'F'",
        "controls": [],
        "timeline": [],
        "workload": {
            "driver": "p51-receipt-window",
            "corpus": "tiny", "repeat": 1, "jobs": jobs, "clients": names_c,
            "turns": ["A"],
            "receipt_gate": {
                "links": links, "expected_commits": 1,
                "negotiated_window": 1, "expect_observed": True,
                "command_timeout_s": 1800,
            },
        },
        "instances": instances,
        "timeouts": {"turn_s": 1800},
    })
    clients = [{"role": "C", "name": name, "host": "host"} for name in names_c]
    for client in clients:
        client.update({
            "container_image": "sha256:" + "a" * 64,
            "compiler_recipe": {
                "executable": "/usr/bin/c++", "binary_sha256": "b" * 64,
                "arguments": ["-O2"],
            },
        })
    plan = {
        "topology": {"instances": [dict(item, version=50) for item in instances]},
        "ports": {"instances": {name: 23003 + i for i, name in enumerate(names_f)}},
        "p51_receipt_gate": {
            "container_path": "/results/p50daemonpositive", "binary_sha256": "a" * 64,
        },
        "run_id": "multilink-test",
    }
    farm = SimpleNamespace(
        hosts={"host": {"docker_context": "test-context"}},
        data={"corpora": {"tiny": {"tus": tus, "repeat": 1}}},
    )
    return farm, scenario, plan, clients


@pytest.mark.parametrize(
    ("links", "clients", "window", "expected"),
    (
        (None, ["C1"], 30, None),
        ([{"client": "C1", "worker": "F1"}, {"client": "C1", "worker": "F2"}], ["C1"], 30, 60),
        ([{"client": "C1", "worker": "F1"}, {"client": "C2", "worker": "F1"}], ["C1", "C2"], 30, None),
        ([{"client": "C1", "worker": "F1"}, {"client": "C1", "worker": "F2"}], ["C1"], 1, None),
    ),
)
def test_receipt_window_derives_only_needed_scheduler_dispatch_credit(
    links: list[dict[str, str]] | None, clients: list[str],
    window: int, expected: int | None
) -> None:
    gate = {"expect_observed": True, "negotiated_window": window}
    if links is not None:
        gate["links"] = links
    scenario = SimpleNamespace(data={"workload": {
        "driver": "p51-receipt-window", "receipt_gate": gate, "clients": clients,
    }})
    assert farmtest._receipt_dispatch_credit(scenario) == expected


def test_receipt_scheduler_launch_keeps_wrapper_or_pinned_binary_defaults() -> None:
    wrapper, wrapper_args = farmtest._scheduler_launch(
        port=36000, netname="run", assignment_fence_mode="strict-nonce",
        receipt_dispatch_credit=None,
    )
    assert wrapper == "/opt/icecream/entry-scheduler.sh"
    assert wrapper_args == [
        "--port", "36000", "--netname", "run",
        "--assignment-fence-mode", "strict-nonce",
    ]

    binary, direct_args = farmtest._scheduler_launch(
        port=36000, netname="run", assignment_fence_mode="strict-nonce",
        receipt_dispatch_credit=60,
    )
    assert binary == "/opt/icecream/sbin/icecc-scheduler"
    assert direct_args == [
        "-p", "36000", "-n", "run", "-u", "nobody",
        "-l", "/var/log/icecream/scheduler.log", "-vvv",
        "--assignment-fence-mode", "strict-nonce",
        "--max-outstanding-dispatches", "60",
    ]


def test_receipt_window_requires_worker_capacity_beyond_credit_clamp(
    tmp_path: Path,
) -> None:
    binary = Path("/bin/true")
    gate = {
        "binary": str(binary),
        "binary_sha256": hashlib.sha256(binary.read_bytes()).hexdigest(),
        "expect_observed": True,
        "expected_commits": 30,
        "negotiated_window": 30,
    }
    links = [
        {"client": "C1", "worker": "F1", "first_job": 1, "last_job": 30},
        {"client": "C1", "worker": "F2", "first_job": 31, "last_job": 60},
    ]
    workload = {
        "receipt_gate": {**gate, "links": links},
        "clients": ["C1"], "turns": ["A"], "repeat": 1, "jobs": 60,
    }
    value = {"timeline": [], "controls": []}
    role_instances = {
        "S": [{"name": "S1", "env": {"ICECC_P51_MODE": "on", "ICECC_P50_PROFILE": "P29V1"}}],
        "C": [{"name": "C1", "env": {"ICECC_P50_MODE": "on", "ICECC_P51_MODE": "on"}}],
        "F": [
            {"name": "F1", "slots": 30, "env": {"ICECC_P51_MODE": "on"}},
            {"name": "F2", "slots": 30, "env": {"ICECC_P51_MODE": "on"}},
        ],
    }
    with pytest.raises(ScenarioSpecError, match="aggregate F slots must exceed"):
        scenario_spec_module._validate_p51_receipt_window(
            value, workload, role_instances, manifest_jobs=60
        )

    role_instances["F"][1]["slots"] = 31
    scenario_spec_module._validate_p51_receipt_window(
        value, workload, role_instances, manifest_jobs=60
    )


@pytest.mark.parametrize("topology", ["C1F2", "C2F1"])
def test_receipt_window_prepares_oracle_before_starting_gate_timer(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch, topology: str,
) -> None:
    farm, scenario, plan, clients = _multilink_orchestrator_fixture(topology)
    farm.digest = "farm-digest"
    farm.data["hub"] = {"results_root": str(tmp_path)}
    scenario.digest = "scenario-digest"
    plan["topology_digest"] = "topology-digest"
    order: list[str] = []

    class Events:
        def __init__(self, *_args, **_kwargs):
            pass

        def start(self):
            order.append("events-start")

        def raise_if_failed(self):
            pass

        def signal_turn_start(self, _turn):
            order.append("turn-start")

        def signal_turn_complete(self, _turn):
            order.append("turn-complete")

        def wait(self):
            order.append("events-wait")

        def stop(self):
            order.append("events-stop")

    prepared_clients: set[str] = set()

    class PreparationRecorder:
        def invoke(self, command: PlannedCommand) -> CommandResult:
            assert command.phase == "run.oracle-prepare"
            assert "ICEFARM_D18_PHASE=prepare" in command.argv
            assert "ICEFARM_D18_BARRIER=1" not in command.argv
            assert command.argv[command.argv.index("--user") + 1] == "1:1"
            prepared_clients.add(command.instance)
            return CommandResult(0, "", "")

    run_oracle_preparation = workload_module._run_oracle_preparation

    def observe_preparation(commands, transport, timeout):
        run_oracle_preparation(commands, transport, timeout)
        order.append("oracle-prepared")

    def run_gate(*_args, **_kwargs):
        assert order.index("oracle-prepared") < order.index("turn-start")
        assert prepared_clients == {client["name"] for client in clients}
        assert "receipt-gate-start" not in order
        order.append("receipt-gate-start")
        return (
            [
                {
                    "client": client["name"],
                    "jobs": scenario.data["workload"]["jobs"],
                    "failures": 0,
                    "samples": 1,
                }
                for client in clients
            ],
            {"links": []},
        )

    monkeypatch.setattr(workload_module, "EventProducer", Events)
    monkeypatch.setattr(workload_module, "_run_oracle_preparation", observe_preparation)
    monkeypatch.setattr(
        workload_module, "_run_p51_receipt_window_multilink", run_gate
    )
    monkeypatch.setattr(workload_module, "_atomic_json", lambda *_args, **_kwargs: None)

    receipt = run_workload(
        farm, scenario, plan,
        recorder=RecordingTransport(PreparationRecorder()), require_up=False
    )

    assert receipt["status"] == "COMPLETE"
    assert order == [
        "events-start", "oracle-prepared", "turn-start", "receipt-gate-start",
        "turn-complete", "events-wait", "events-stop",
    ]
    for client in clients:
        prepared = _driver_command(
            farm, scenario, plan, client, "A", CommandFactory(),
            exec_uid="1:1", prepared_oracle=True,
        )
        assert "ICEFARM_ORACLE_PREPARED=1" in prepared.argv
        assert "ICEFARM_D18_BARRIER=1" not in prepared.argv
        assert f"ICEFARM_D18_RUN_ID={plan['run_id']}" in prepared.argv


def test_receipt_prepared_driver_validates_certificate_before_remote_dispatch() -> None:
    # The existing D18 certificate carries exact run/client/turn and oracle
    # identity (manifest, compiler digest/configuration, sample hashes). The
    # receipt driver reuses that barrier, whose shell validator runs before
    # compile_one can dispatch any remote job.
    marker_validation = MANIFEST_DRIVER.index(
        "d18_prepared_sample_total=$(d18_validate_preparation"
    )
    compile_dispatch = MANIFEST_DRIVER.index("compile_one() {")
    prepare_exit = MANIFEST_DRIVER.index('if test "$d18_phase" = prepare\nthen')
    assert marker_validation < compile_dispatch
    assert prepare_exit < compile_dispatch
    assert 'echo "D18 measured phase has an incomplete preparation marker"' in MANIFEST_DRIVER


@pytest.mark.parametrize("marker_state", ["valid", "missing", "stale"])
def test_receipt_prepared_shell_path_validates_then_dispatches_without_d18_go(
    tmp_path: Path, marker_state: str,
) -> None:
    function = re.search(
        r"(?ms)^d18_validate_preparation\(\) \{\n.*?^\}", MANIFEST_DRIVER
    )
    validation = re.search(
        r'(?ms)^if test "\$d18_barrier" = 1 -o "\$oracle_prepared" = 1\n'
        r"then\n.*?^fi\n",
        MANIFEST_DRIVER,
    )
    go_wait = re.search(
        r'(?ms)^if test "\$\{ICEFARM_D18_BARRIER:-0\}" = 1\n'
        r"then\n.*?^fi\n",
        MANIFEST_DRIVER,
    )
    assert function is not None and validation is not None and go_wait is not None

    prep = tmp_path / marker_state
    prep.mkdir()
    samples = prep / "oracle-samples.tsv"
    summary = prep / "oracle-summary.tsv"
    samples.write_text("tiny.cc\t" + "a" * 64 + "\t" + "a" * 64 + "\t1\n")
    summary.write_text("sample_total\t1\nsample_mismatches\t0\n")
    identity = "b" * 64
    if marker_state != "missing":
        marker_identity = "c" * 64 if marker_state == "stale" else identity
        (prep / "PREPARED.tsv").write_text(
            "\t".join((
                "D18_PREPARED_V1", marker_identity, "1",
                hashlib.sha256(samples.read_bytes()).hexdigest(),
                hashlib.sha256(summary.read_bytes()).hexdigest(),
            )) + "\n",
            encoding="ascii",
        )

    script = "\n".join((
        "set -euo pipefail",
        "d18_phase=run",
        "d18_barrier=0",
        "oracle_prepared=1",
        "d18_prep_root=$1",
        "d18_prep_identity=$2",
        "cache_ready=1",
        function.group(0),
        validation.group(0),
        go_wait.group(0),
        "printf 'RECEIPT_DISPATCH_REACHED\\n'",
    ))
    result = subprocess.run(
        ["/bin/bash", "-c", script, "prepared-receipt-test", str(prep), identity],
        env={**os.environ, "ICEFARM_D18_BARRIER": "0", "ICEFARM_ORACLE_PREPARED": "1"},
        check=False,
        capture_output=True,
        text=True,
        timeout=5,
    )
    if marker_state == "valid":
        assert result.returncode == 0, result.stderr
        assert result.stdout.endswith("RECEIPT_DISPATCH_REACHED\n")
    else:
        assert result.returncode != 0
        assert "RECEIPT_DISPATCH_REACHED" not in result.stdout


def test_receipt_oracle_preparation_failure_never_starts_gate(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch,
) -> None:
    farm, scenario, plan, _clients = _multilink_orchestrator_fixture("C1F2")
    farm.digest = "farm-digest"
    farm.data["hub"] = {"results_root": str(tmp_path)}
    scenario.digest = "scenario-digest"
    plan["topology_digest"] = "topology-digest"
    events_stopped: list[bool] = []

    class Events:
        def __init__(self, *_args, **_kwargs):
            pass

        def start(self):
            pass

        def raise_if_failed(self):
            pass

        def stop(self):
            events_stopped.append(True)

    def failed_preparation(*_args, **_kwargs):
        raise WorkloadError("local-oracle preparation failed for C1: sample mismatch")

    def unexpected_gate(*_args, **_kwargs):
        raise AssertionError("receipt gate started despite preparation failure")

    monkeypatch.setattr(workload_module, "EventProducer", Events)
    monkeypatch.setattr(workload_module, "_run_oracle_preparation", failed_preparation)
    monkeypatch.setattr(
        workload_module, "_run_p51_receipt_window_multilink", unexpected_gate
    )
    monkeypatch.setattr(workload_module, "_atomic_json", lambda *_args, **_kwargs: None)

    with pytest.raises(WorkloadError, match="local-oracle preparation failed"):
        run_workload(
            farm, scenario, plan, recorder=RecordingTransport(), require_up=False
        )
    assert events_stopped == [True]


@pytest.mark.parametrize("topology", ["C1F2", "C2F1"])
def test_multilink_receipt_orchestrator_runs_and_orders_real_link_checks(
    monkeypatch: pytest.MonkeyPatch, topology: str,
) -> None:
    farm, scenario, plan, clients = _multilink_orchestrator_fixture(topology)
    links = scenario.data["workload"]["receipt_gate"]["links"]
    releases: set[tuple[str, str]] = set()
    finishes: set[tuple[str, str]] = set()
    gate_calls: list[tuple[str, str]] = []
    output_checks: list[tuple[str, str, bool]] = []
    gate_events = {
        (link["client"], link["worker"]): threading.Event() for link in links
    }
    gate_held = {
        (link["client"], link["worker"]): threading.Event() for link in links
    }
    def gate_call(_farm, _plan, client, _factory, _transport, phase, argv, **_kw):
        client_name = client["name"]
        gate_calls.append((client_name, phase))
        if phase == "verify-staged-helper":
            return CommandResult(0, "a" * 64 + "\n", "")
        if phase == "sidecar-uid":
            return CommandResult(0, "65534\n", "")
        if phase.startswith("release-"):
            # Worker names in fixtures are simple F1/F2; extract from link table.
            worker = next(
                item["worker"] for item in links
                if item["client"] == client_name and phase.endswith(item["worker"])
            )
            key = (client_name, worker)
            releases.add(key)
        if phase == "finish-link-gate" or phase.startswith("cleanup-"):
            for link in links:
                if link["client"] == client_name:
                    finishes.add((client_name, link["worker"]))
                    gate_events[(client_name, link["worker"])].set()
        if phase.startswith("verify-sibling-still-held-"):
            worker = phase.rsplit("-", 1)[1]
            key = (client_name, worker)
            released = int(key in releases)
            exited = int(gate_events[key].is_set())
            return CommandResult(
                0, f"held=1 released={released} exited={exited}\n", ""
            )
        return CommandResult(0, "", "")

    class Factory:
        def make(self, **kwargs):
            return SimpleNamespace(**kwargs)

    class Transport:
        def invoke(self, command):
            phase = command.phase
            if ".start-gate." in phase:
                link = next(
                    item for item in links
                    if f".{item['client']}.{item['worker']}" in phase
                )
                key = (link["client"], link["worker"])
                gate_held[key].set()
                gate_events[key].wait(timeout=3)
                return CommandResult(
                    0,
                    "",
                    "P51_RECEIPT_GATE_NEGOTIATED profile=1 window=1 epoch=1 generation=1\n",
                )
            if phase.startswith("run.p51-receipt-window.check-link-output."):
                _, client_name, worker = phase.rsplit(".", 2)
                key = (client_name, worker)
                output_checks.append((client_name, worker, key in releases))
                if key not in releases:
                    return CommandResult(0, "P51_LINK_OUTPUTS_PENDING\n", "")
                link = next(
                    item for item in links
                    if item["client"] == client_name and item["worker"] == worker
                )
                worker_instance = next(
                    item for item in plan["topology"]["instances"]
                    if item["name"] == worker
                )
                endpoint = f"{worker_instance['address']}:{plan['ports']['instances'][worker]}"
                return CommandResult(
                    0,
                    f"P51_LINK_OUTPUTS_OK client={client_name} worker={worker} "
                    f"endpoint={endpoint} first={link['first_job']} last={link['last_job']}\n",
                    "",
                )
            if phase.startswith("run.p51-receipt-window.probe-sibling-output."):
                _, client_name, worker = phase.rsplit(".", 2)
                output_checks.append((client_name, worker, False))
                return CommandResult(0, "P51_LINK_OUTPUTS_PENDING\n", "")
            if phase.startswith("driver-"):
                return CommandResult(0, "synthetic summary", "")
            raise AssertionError(f"unexpected transport command {phase}")

    def driver_command(_farm, _scenario, _plan, client, _turn, _factory, **_kwargs):
        return SimpleNamespace(phase=f"driver-{client['name']}")

    monkeypatch.setattr(workload_module, "_p51_gate_call", gate_call)
    monkeypatch.setattr(workload_module, "_stage_p51_iptables_bundle", lambda *_a, **_k: None)
    monkeypatch.setattr(workload_module, "_driver_command", driver_command)
    monkeypatch.setattr(workload_module, "_parse_summary", lambda _r, name: {
        "client": name, "jobs": scenario.data["workload"]["jobs"], "failures": 0,
    })
    monkeypatch.setattr(workload_module, "_docker_transport", lambda *_a, **_k: "docker")
    monkeypatch.setattr(workload_module, "docker_argv", lambda _f, _h, argv: tuple(argv))
    def link_ids(marker: str):
        for index, link in enumerate(links, start=1):
            link_dir = f"/{link['client']}-{link['worker']}/"
            if link_dir in marker:
                client_index = int(link["client"][1:])
                worker_index = int(link["worker"][1:])
                return (
                    link, f"{client_index:032x}", f"{100 + worker_index:032x}",
                    f"{200 + index:032x}", f"{300 + index:032x}",
                )
        raise AssertionError(f"marker has no route identity: {marker}")

    def wait_marker(_farm, _plan, client, _factory, _transport, marker, **_kwargs):
        if marker.endswith("/ready"):
            return "ready\n"
        if marker.endswith("/held-1"):
            link, _c_guid, _f_guid, relationship, reservation = link_ids(marker)
            key = (link["client"], link["worker"])
            if key not in gate_held or not gate_held[key].wait(timeout=1):
                return None
            return (
                "count=1 first_ordinal=1 last_ordinal=1 profile=1 window=1 "
                f"relationship={relationship} reservation={reservation} epoch=1 generation=1\n"
            )
        if marker.endswith("/identity-1"):
            _link, c_guid, f_guid, relationship, reservation = link_ids(marker)
            return (
                f"c_store={c_guid} c_store_generation=1 f_store={f_guid} "
                f"f_store_generation=1 relationship={relationship} reservation={reservation} "
                "epoch=1 generation=1 profile=1 window=1\n"
            )
        return "released\n" if marker.endswith("/released-1") else "0\n" if marker.endswith("/exit") else None

    monkeypatch.setattr(workload_module, "_p51_wait_marker", wait_marker)
    clock = [0.0]
    monkeypatch.setattr(workload_module, "time", SimpleNamespace(
        monotonic=lambda: clock[0],
        sleep=lambda duration: clock.__setitem__(0, clock[0] + max(1.0, duration)),
    ))

    summaries, evidence = workload_module._run_p51_receipt_window_multilink(
        farm, scenario, plan, clients, Factory(), Transport(), "A"
    )
    assert len(summaries) == len(clients)
    assert len(evidence["links"]) == len(links)
    assert len(evidence["release_order_output_progress"]) == len(links)
    assert releases == set(gate_events)
    assert all(gate_held[key].is_set() for key in gate_held)
    assert finishes == set(gate_events)
    assert all(done for _, _, done in output_checks if done)
    assert any(not done for _, _, done in output_checks)
    assert evidence["restart_extension"] == "pending-not-run"


@pytest.mark.parametrize("topology", ["C1F2", "C2F1"])
@pytest.mark.parametrize(
    "failure", ["misrouted-output", "early-gate-exit", "duplicate-identity"]
)
def test_multilink_receipt_orchestrator_rejects_failure_and_cleans_owned_processes(
    monkeypatch: pytest.MonkeyPatch, topology: str, failure: str,
) -> None:
    farm, scenario, plan, clients = _multilink_orchestrator_fixture(topology)
    links = scenario.data["workload"]["receipt_gate"]["links"]
    releases: set[tuple[str, str]] = set()
    cleanup: list[tuple[str, str]] = []
    kill_clients: list[str] = []
    gate_events = {(row["client"], row["worker"]): threading.Event() for row in links}

    def gate_call(_farm, _plan, client, _factory, _transport, phase, _argv, **_kw):
        if phase == "verify-staged-helper":
            return CommandResult(0, "a" * 64 + "\n", "")
        if phase == "sidecar-uid":
            return CommandResult(0, "65534\n", "")
        if phase.startswith(("cleanup-", "abort-")) or phase in ("finish-link-gate",):
            cleanup.append((client["name"], phase))
            for row in links:
                if row["client"] == client["name"]:
                    gate_events[(row["client"], row["worker"])].set()
        return CommandResult(0, "held=1 released=0 exited=0\n" if phase.startswith("verify-sibling") else "", "")

    class Factory:
        def make(self, **kwargs):
            return SimpleNamespace(**kwargs)

    class Transport:
        def invoke(self, command):
            if command.phase.startswith("run.p51-receipt-window.start-gate."):
                if failure == "early-gate-exit":
                    return CommandResult(1, "", "forced helper failure")
                client_name, worker = command.phase.rsplit(".", 2)[-2:]
                gate_events[(client_name, worker)].wait(timeout=2)
                return CommandResult(0, "", "P51_RECEIPT_GATE_NEGOTIATED profile=1 window=1 epoch=1 generation=1")
            if command.phase.startswith("run.p51-receipt-window.check-link-output."):
                client_name, worker = command.phase.rsplit(".", 2)[-2:]
                releases.add((client_name, worker))
                return CommandResult(
                    0,
                    f"P51_LINK_OUTPUTS_BAD client={client_name} worker={worker} "
                    f"endpoint=wrong job=1\n",
                    "",
                )
            if command.phase.startswith("run.p51-receipt-window.probe-sibling-output."):
                return CommandResult(0, "P51_LINK_OUTPUTS_PENDING\n", "")
            if command.phase.startswith("driver-"):
                return CommandResult(0, "synthetic summary", "")
            if command.argv[:1] == ("kill",):
                kill_clients.append(command.instance)
                return CommandResult(0, "", "")
            raise AssertionError(f"unexpected transport command {command.phase}")

    def driver_command(_farm, _scenario, _plan, client, _turn, _factory, **_kwargs):
        return SimpleNamespace(phase=f"driver-{client['name']}")

    def wait_marker(_farm, _plan, client, _factory, _transport, marker, **_kwargs):
        if failure == "early-gate-exit" and marker.endswith("/held-1"):
            return None
        if marker.endswith("/ready"):
            return "ready\n"
        if marker.endswith("/held-1"):
            return (
                "count=1 first_ordinal=1 last_ordinal=1 profile=1 window=1 "
                f"relationship={'a' * 32} reservation={'b' * 32} epoch=1 generation=1\n"
            )
        if marker.endswith("/identity-1"):
            return (
                f"c_store={'c' * 32} c_store_generation=1 f_store={'d' * 32} "
                f"f_store_generation=1 relationship={'a' * 32} reservation={'b' * 32} "
                "epoch=1 generation=1 profile=1 window=1\n"
            )
        return "released\n" if marker.endswith("/released-1") else None

    monkeypatch.setattr(workload_module, "_p51_gate_call", gate_call)
    monkeypatch.setattr(workload_module, "_stage_p51_iptables_bundle", lambda *_a, **_k: None)
    monkeypatch.setattr(workload_module, "_driver_command", driver_command)
    monkeypatch.setattr(workload_module, "_parse_summary", lambda _r, name: {
        "client": name, "jobs": scenario.data["workload"]["jobs"], "failures": 0,
    })
    monkeypatch.setattr(workload_module, "_docker_transport", lambda *_a, **_k: "docker")
    monkeypatch.setattr(workload_module, "docker_argv", lambda _f, _h, argv: tuple(argv))
    monkeypatch.setattr(workload_module, "_p51_wait_marker", wait_marker)
    clock = [0.0]
    monkeypatch.setattr(workload_module, "time", SimpleNamespace(
        monotonic=lambda: clock[0],
        sleep=lambda duration: clock.__setitem__(0, clock[0] + max(1.0, duration)),
    ))

    with pytest.raises(WorkloadError):
        workload_module._run_p51_receipt_window_multilink(
            farm, scenario, plan, clients, Factory(), Transport(), "A"
        )
    assert len(kill_clients) == len(clients)
    assert {name for name, _phase in cleanup} == {row["client"] for row in links}


def test_d18_overlap_requires_exact_persistent_worker_process_sets() -> None:
    expected = {"F_R1": 2, "F_R2": 1}
    first = {"F_R1": {101: 9001, 102: 9002}, "F_R2": {201: 9010}}
    second = {"F_R1": {101: 9001, 102: 9002}, "F_R2": {201: 9010}}

    witness = _d18_concurrent_witness(first, second, expected)
    assert witness is not None
    assert len(witness["F_R1"]) == 2
    assert len(witness["F_R2"]) == 1

    # Sequential-only activity cannot pass merely because every worker was
    # observed during some part of the collection window.
    sequential_first = {"F_R1": {101: 9001, 102: 9002}, "F_R2": {}}
    sequential_second = {"F_R1": {}, "F_R2": {201: 9010}}
    assert _d18_concurrent_witness(sequential_first, sequential_second, expected) is None

    # Unattributed compiler work is rejected rather than selecting an arbitrary
    # subset of PIDs to make the expected topology fit.
    extra = {"F_R1": {101: 9001, 102: 9002, 103: 9003}, "F_R2": {201: 9010}}
    assert _d18_concurrent_witness(extra, extra, expected) is None


def test_d18_remote_row_failure_reports_exact_worker_and_result_fields(
    tmp_path: Path,
) -> None:
    jobs = tmp_path / "jobs"
    job = jobs / "000007"
    job.mkdir(parents=True)
    (job / "result.tsv").write_text(
        "7\tA\t0\tfiles/example.cc.ii\tjob-007\t10.0.27.212:23005\t10\t11\t0\t"
        "remote-sha\tlocal-sha\t1\t1\t2\n",
        encoding="ascii",
    )

    result = subprocess.run(
        [
            "/bin/bash", "-c", workload_module._D18_VERIFY_REMOTE_ROWS_SCRIPT,
            "d18-verify", str(jobs), "F_R2", "10.0.27.212:23006", "1",
        ],
        check=False,
        capture_output=True,
        encoding="utf-8",
    )

    assert result.returncode != 0
    assert "index=7 job_id=job-007" in result.stderr
    assert "expected_worker='F_R2' expected_endpoint='10.0.27.212:23006' actual_worker='10.0.27.212:23005'" in result.stderr
    assert "rc=0 remote_sha=remote-sha local_sha=local-sha exact=1 remote=1 retries=2" in result.stderr


@pytest.mark.parametrize(
    ("expected_endpoint", "error_fragment"),
    [
        ("10.0.27.212:23006", "expected_endpoint='10.0.27.212:23006'"),
        ("10.0.27.213:23005", "expected_endpoint='10.0.27.213:23005'"),
        ("worker-one", "malformed expected worker endpoint"),
    ],
)
def test_d18_remote_row_verifier_rejects_wrong_or_malformed_worker_endpoint(
    tmp_path: Path, expected_endpoint: str, error_fragment: str,
) -> None:
    jobs = tmp_path / "jobs"
    job = jobs / "000001"
    job.mkdir(parents=True)
    (job / "result.tsv").write_text(
        "1\tA\t0\tfiles/example.cc.ii\tjob-001\t10.0.27.212:23005\t10\t11\t0\t"
        "remote-sha\tlocal-sha\t1\t1\t0\n",
        encoding="ascii",
    )

    result = subprocess.run(
        [
            "/bin/bash", "-c", workload_module._D18_VERIFY_REMOTE_ROWS_SCRIPT,
            "d18-verify", str(jobs), "F_R1", expected_endpoint, "1",
        ],
        check=False,
        capture_output=True,
        encoding="utf-8",
    )

    assert result.returncode != 0
    assert error_fragment in result.stderr


def test_d18_remote_row_verifier_accepts_the_exact_planned_worker_endpoint(
    tmp_path: Path,
) -> None:
    jobs = tmp_path / "jobs"
    job = jobs / "000001"
    job.mkdir(parents=True)
    (job / "result.tsv").write_text(
        "1\tA\t0\tfiles/example.cc.ii\tjob-001\t10.0.27.212:23005\t10\t11\t0\t"
        "remote-sha\tremote-sha\t1\t1\t0\n",
        encoding="ascii",
    )

    result = subprocess.run(
        [
            "/bin/bash", "-c", workload_module._D18_VERIFY_REMOTE_ROWS_SCRIPT,
            "d18-verify", str(jobs), "F_R1", "10.0.27.212:23005", "1",
        ],
        check=False,
        capture_output=True,
        encoding="utf-8",
    )

    assert result.returncode == 0, result.stderr
    assert '"count": 1' in result.stdout
    assert '"worker": "10.0.27.212:23005"' in result.stdout


def test_d18_driver_uses_shared_barrier_without_changing_compiler_identity(
    tmp_path: Path,
) -> None:
    farm = load_farm_spec(farm_fixture.example_farm_path())
    farm.data["hub"]["results_root"] = str(tmp_path)
    scenario = load_scenario_spec(
        INTEGRATION / "scenarios" / "D18-P29V1.json", farm
    )
    plan = farmtest.build_plan(farm, scenario, run_id="d18-driver-workload-unit")
    r1_name = scenario.data["workload"]["d18_roles"]["clients"]["R1"]
    client = next(item for item in plan["topology"]["instances"] if item["name"] == r1_name)

    command = _driver_command(
        farm, scenario, plan, client, "A", CommandFactory()
    )
    assert "ICEFARM_D18_BARRIER=1" in command.argv
    assert not any("ICEFARM_D18_ROLE_" in argument for argument in command.argv)
    assert "-O2" in command.argv

    preparation = _driver_command(
        farm, scenario, plan, client, "A", CommandFactory(), d18_phase="prepare"
    )
    assert preparation.phase == "run.d18-prepare"
    assert "ICEFARM_D18_PHASE=prepare" in preparation.argv
    assert "ICEFARM_D18_BARRIER=1" not in preparation.argv
    assert f"ICEFARM_D18_RUN_ID={plan['run_id']}" in preparation.argv
    assert any(
        argument.endswith(f"/A/{client['name']}")
        for argument in preparation.argv
        if argument.startswith("ICEFARM_D18_PREP_ROOT=")
    )


def test_receipt_oracle_plan_uses_uid_scoped_root_for_prepare_and_mount(
    tmp_path: Path,
) -> None:
    farm = load_farm_spec(farm_fixture.example_farm_path())
    farm.data["hub"]["results_root"] = str(tmp_path)
    scenario = load_scenario_spec(
        INTEGRATION / "scenarios" / "D18-P29V1.json", farm
    )
    base_plan = farmtest.build_plan(farm, scenario, run_id="receipt-oracle-uid-plan")
    scenario.data["workload"]["driver"] = "p51-receipt-window"
    scenario.data["workload"]["receipt_gate"] = {
        "binary": "/bin/true",
        "binary_sha256": hashlib.sha256(Path("/bin/true").read_bytes()).hexdigest(),
        "expect_observed": True,
        "expected_commits": 1,
        "negotiated_window": 1,
        "command_timeout_s": 1800,
    }
    scenario.data["workload"].pop("d18_roles", None)
    commands = farmtest._planned_commands(
        farm,
        scenario,
        base_plan["topology"],
        base_plan["ports"],
        "receipt-oracle-uid-plan",
    )
    client = next(
        item for item in base_plan["topology"]["instances"] if item["role"] == "C"
    )
    legacy = oracle_root(farm, client, scenario.data["workload"]["corpus"])
    receipt = oracle_root(
        farm, client, scenario.data["workload"]["corpus"], writer_uid=1
    )

    assert receipt == legacy / "uid-1"
    for invalid_uid in (True, -1, "1"):
        with pytest.raises(ValueError, match="writer UID"):
            oracle_root(
                farm, client, scenario.data["workload"]["corpus"],
                writer_uid=invalid_uid,
            )
    prepare = next(
        command.as_dict() for command in commands
        if command.phase == "up.prepare-persistent"
        and command.instance == client["name"]
    )
    start = next(
        command.as_dict() for command in commands
        if command.phase == "up.start-c" and command.instance == client["name"]
    )
    prepare_argv = json.dumps(decode_ssh_payload(prepare["argv"]))
    start_argv = json.dumps(start["argv"])
    assert str(receipt) in prepare_argv
    assert "0777" in prepare_argv
    assert f"src={receipt},dst=/oracle" in start_argv
    assert f"src={legacy},dst=/oracle" not in start_argv


def test_receipt_uid_oracle_can_be_created_and_reused_without_mutating_legacy(
) -> None:
    if os.geteuid() != 0 or shutil.which("setpriv") is None:
        pytest.skip("requires root, setpriv, and the existing daemon UID 1")
    with tempfile.TemporaryDirectory(
        prefix="p51-oracle-uid1-", dir=tempfile.gettempdir()
    ) as root:
        scratch = Path(root)
        scratch.chmod(0o755)
        farm = SimpleNamespace(hosts={"host": {"scratch_root": str(scratch)}})
        client = {
            "host": "host",
            "container_image": "sha256:" + "a" * 64,
            "compiler_recipe": {"executable": "/usr/bin/g++", "arguments": ["-O2"]},
        }
        legacy_root = Path(str(oracle_root(farm, client, "tiny")))
        legacy = legacy_root / "A"
        receipt = Path(str(oracle_root(farm, client, "tiny", writer_uid=1)))
        legacy.mkdir(parents=True)
        old_lock = legacy / ".lock"
        old_lock.touch()
        old_lock.chmod(0o644)
        os.chown(legacy, 65534, 65534)
        os.chown(old_lock, 65534, 65534)
        old_lock_stat = old_lock.stat()
        legacy_stat = legacy.stat()

        # This is the same parent preparation mode emitted by up.prepare-persistent.
        subprocess.run(
            ["install", "-d", "-m", "0777", "--", str(receipt)],
            check=True,
            capture_output=True,
            text=True,
        )
        smoke = (
            'set -eu; root=$1; mkdir -p "$root/A"; exec 9>"$root/A/.lock"; '
            'flock -x 9; printf "reuse\\n" >> "$root/A/probe"'
        )
        command = [
            shutil.which("setpriv"), "--reuid=1", "--regid=1", "--clear-groups",
            "/bin/bash", "-c", smoke, "uid-one-oracle", str(receipt),
        ]
        for _ in range(2):
            subprocess.run(command, check=True, capture_output=True, text=True)

        assert (receipt / "A" / "probe").read_text(encoding="ascii") == "reuse\nreuse\n"
        assert (receipt / "A" / ".lock").stat().st_uid == 1
        legacy_attempt = subprocess.run(
            [
                shutil.which("setpriv"), "--reuid=1", "--regid=1", "--clear-groups",
                "/bin/bash", "-c", 'exec 9>"$1/.lock"', "legacy-oracle", str(legacy),
            ],
            check=False,
            capture_output=True,
            text=True,
        )
        assert legacy_attempt.returncode != 0
        assert old_lock.read_bytes() == b""
        assert (old_lock.stat().st_uid, old_lock.stat().st_gid, old_lock.stat().st_mode) == (
            old_lock_stat.st_uid, old_lock_stat.st_gid, old_lock_stat.st_mode
        )
        assert (legacy.stat().st_uid, legacy.stat().st_gid, legacy.stat().st_mode) == (
            legacy_stat.st_uid, legacy_stat.st_gid, legacy_stat.st_mode
        )


def _d18_fixture_plan(tmp_path: Path):
    farm = load_farm_spec(farm_fixture.example_farm_path())
    farm.data["hub"]["results_root"] = str(tmp_path)
    scenario = load_scenario_spec(INTEGRATION / "scenarios" / "D18-P29V1.json", farm)
    plan = farmtest.build_plan(farm, scenario, run_id="d18-phase-unit")
    return farm, scenario, plan


class _IdleEvents:
    def start(self) -> None:
        pass

    def raise_if_failed(self) -> None:
        pass

    def signal_turn_start(self, _turn: str) -> None:
        pass

    def signal_turn_complete(self, _turn: str) -> None:
        pass

    def wait(self) -> None:
        pass

    def stop(self) -> None:
        pass


def test_d18_delayed_preparation_finishes_before_measured_commands(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    farm, scenario, plan = _d18_fixture_plan(tmp_path)
    started = threading.Event()
    release = threading.Event()
    calls: list[str] = []

    class DelayedRecorder:
        def invoke(self, command: PlannedCommand) -> CommandResult:
            calls.append(command.phase)
            if command.phase == "run.d18-prepare" and command.instance == "C_R1":
                started.set()
                if not release.wait(timeout=3):
                    return CommandResult(124, "", "preparation test timed out")
            if command.phase == "run.d18-prepare":
                return CommandResult(0, "ICEFARM_D18_PREPARED\n", "")
            return CommandResult(0, "ICEFARM_WORKLOAD jobs=1 failures=0 samples=1\n", "")

    recorder = RecordingTransport(DelayedRecorder())
    monkeypatch.setattr(workload_module, "EventProducer", lambda *a, **kw: _IdleEvents())
    monkeypatch.setattr(
        workload_module,
        "_d18_barrier_and_observe",
        lambda *a, **kw: {"workers": {"F_R1": [], "F_R2": []}},
    )
    monkeypatch.setattr(workload_module, "_d18_verify_remote_rows", lambda *a, **kw: {})
    monkeypatch.setattr(workload_module, "_d18_verify_r2_link_adoption", lambda *a, **kw: {})

    failures: list[BaseException] = []

    def run() -> None:
        try:
            run_workload(farm, scenario, plan, recorder=recorder, require_up=False)
        except BaseException as exc:
            failures.append(exc)

    thread = threading.Thread(target=run, daemon=True)
    thread.start()
    try:
        assert started.wait(timeout=2)
        time.sleep(0.05)
        assert "run.workload" not in calls
    finally:
        release.set()
    thread.join(timeout=5)
    assert not thread.is_alive()
    assert failures == []
    assert calls.count("run.d18-prepare") == 3
    assert calls.count("run.workload") == 3


def test_d18_preparation_failure_prevents_measured_launch(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    farm, scenario, plan = _d18_fixture_plan(tmp_path)
    calls: list[str] = []

    class FailingPreparation:
        def invoke(self, command: PlannedCommand) -> CommandResult:
            calls.append(command.phase)
            if command.phase == "run.d18-prepare" and command.instance == "C_R1":
                return CommandResult(1, "", "oracle sample mismatch")
            return CommandResult(0, "ICEFARM_D18_PREPARED\n", "")

    monkeypatch.setattr(workload_module, "EventProducer", lambda *a, **kw: _IdleEvents())
    with pytest.raises(WorkloadError, match="local-oracle preparation failed"):
        run_workload(
            farm,
            scenario,
            plan,
            recorder=RecordingTransport(FailingPreparation()),
            require_up=False,
        )
    assert calls
    assert all(phase == "run.d18-prepare" for phase in calls)


def test_d18_missing_ready_fails_without_releasing_clients(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    farm, scenario, plan = _d18_fixture_plan(tmp_path)
    clients = [
        item for item in plan["topology"]["instances"] if item["role"] == "C"
    ]
    release_calls: list[str] = []

    def docker_call(_farm, _plan, instance, _factory, _transport, phase, _argv):
        if phase.startswith("barrier-release-"):
            release_calls.append(instance["name"])
        return CommandResult(0, "", "")

    monkeypatch.setattr(workload_module, "_d18_docker_call", docker_call)
    pending = [concurrent.futures.Future()]
    with pytest.raises(WorkloadError, match="did not all reach"):
        _d18_barrier_and_observe(
            farm,
            scenario,
            plan,
            clients,
            pending,
            CommandFactory(),
            RecordingTransport(),
            deadline_s=0,
        )
    assert release_calls == []


@pytest.mark.parametrize(
    ("case", "valid"),
    [
        ("valid", True),
        ("missing-marker", False),
        ("cache-not-ready", False),
        ("sample-changed", False),
        ("wrong-identity", False),
    ],
)
def test_d18_preparation_marker_validation(
    tmp_path: Path, case: str, valid: bool
) -> None:
    match = re.search(
        r"(?ms)^d18_validate_preparation\(\) \{\n.*?^\}", MANIFEST_DRIVER
    )
    assert match is not None
    root = tmp_path / "prep"
    root.mkdir()
    samples = root / "oracle-samples.tsv"
    summary = root / "oracle-summary.tsv"
    samples.write_text("tiny.cc\t" + "a" * 64 + "\t" + "a" * 64 + "\t1\n")
    summary.write_text("sample_total\t1\nsample_mismatches\t0\n")
    identity = "b" * 64
    if case != "missing-marker":
        marker_identity = "c" * 64 if case == "wrong-identity" else identity
        (root / "PREPARED.tsv").write_text(
            "\t".join(
                (
                    "D18_PREPARED_V1",
                    marker_identity,
                    "1",
                    hashlib.sha256(samples.read_bytes()).hexdigest(),
                    hashlib.sha256(summary.read_bytes()).hexdigest(),
                )
            )
            + "\n"
        )
    if case == "sample-changed":
        samples.write_text("corrupted after the preparation certificate\n")
    cache_is_ready = "0" if case == "cache-not-ready" else "1"
    script = (
        match.group(0)
        + "\nd18_validate_preparation \"$1\" \"$2\" \"$3\"\n"
    )
    result = subprocess.run(
        [
            "/bin/bash", "-c", script, "d18-marker-test", str(root), identity,
            cache_is_ready,
        ],
        check=False,
        capture_output=True,
        text=True,
    )
    if valid:
        assert result.returncode == 0
        assert result.stdout == "1\n"
    else:
        assert result.returncode != 0
        assert result.stdout == ""


def test_d18_preparation_certificate_follows_zero_mismatch_check() -> None:
    zero_mismatch_check = MANIFEST_DRIVER.index('test "$sample_mismatches" -eq 0')
    certificate_write = MANIFEST_DRIVER.index('marker_tmp="$d18_prep_root/.PREPARED.tsv.')
    certificate_publish = MANIFEST_DRIVER.index(
        'mv -f -- "$marker_tmp" "$d18_prep_root/PREPARED.tsv"'
    )
    assert zero_mismatch_check < certificate_write < certificate_publish


def test_d18_no_timeline_event_producer_has_no_deadline_thread(
    tmp_path: Path,
) -> None:
    farm, scenario, plan = _d18_fixture_plan(tmp_path)
    producer = EventProducer(
        farm,
        scenario,
        plan,
        recorder=RecordingTransport(),
        factory=CommandFactory(),
        deadline_s=0.01,
    )
    producer.start()
    time.sleep(0.02)
    producer.raise_if_failed()
    producer.stop()
    assert producer._thread is None


def test_d18_r2_adoption_evidence_requires_sidecar_handoff() -> None:
    from farmharness.integration.workload import D18_R2_ADOPTION_RE

    log = (
        "ordinary worker startup\n"
        "P51 cache-link descriptor adopted by sidecar (generation 7/2)\n"
    )
    assert D18_R2_ADOPTION_RE.findall(log) == [("7", "2")]
    assert not D18_R2_ADOPTION_RE.findall("P51 cache-link setup refused: unavailable\n")


def test_d18_process_observer_uses_comm_not_its_script_text() -> None:
    if not Path("/proc/self/comm").exists():
        pytest.skip("the D18 process observer inspects Linux /proc")
    child = subprocess.Popen(
        [
            sys.executable,
            "-c",
            "import ctypes,time; ctypes.CDLL(None).prctl(15, ctypes.c_char_p(b'cc1plus'), 0, 0, 0); time.sleep(5)",
        ],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )
    observer = None
    try:
        deadline = time.monotonic() + 2
        comm_path = Path(f"/proc/{child.pid}/comm")
        while time.monotonic() < deadline:
            if comm_path.exists() and comm_path.read_text().strip() == "cc1plus":
                break
            time.sleep(0.01)
        else:
            pytest.fail("compiler-named observer fixture did not start")

        observer = subprocess.Popen(
            ["/bin/bash", "-c", D18_PROCESS_OBSERVER],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )
        stdout, stderr = observer.communicate(timeout=2)
        assert observer.returncode == 0, stderr
        identities = [line.split() for line in stdout.splitlines()]
        pids = {int(fields[0]) for fields in identities if len(fields) == 2}
        assert child.pid in pids
        assert observer.pid not in pids
    finally:
        if observer is not None and observer.poll() is None:
            observer.kill()
            observer.wait(timeout=2)
        child.terminate()
        child.wait(timeout=2)


def _farm_scenario_plan(tmp_path: Path):
    farm = load_farm_spec(farm_fixture.example_farm_path())
    farm.data["hub"]["results_root"] = str(tmp_path)
    scenario = load_scenario_spec(INTEGRATION / "scenarios" / "S00-smoke.json", farm)
    plan = farmtest.build_plan(farm, scenario, run_id="workload-unit")
    return farm, scenario, plan


class WorkloadRecorder:
    def __init__(
        self, stdout: str = "ICEFARM_WORKLOAD jobs=100 failures=0 samples=3\n"
    ) -> None:
        self.stdout = stdout
        self.commands: list[PlannedCommand] = []

    def invoke(self, command: PlannedCommand) -> CommandResult:
        self.commands.append(command)
        return CommandResult(0, self.stdout, "")


class PairedWorkloadRecorder(WorkloadRecorder):
    def invoke(self, command: PlannedCommand) -> CommandResult:
        self.commands.append(command)
        if command.phase == "run.corpus-clear":
            return CommandResult(0, f"cleared {command.argv[-1]}\n", "")
        if command.phase == "run.corpus-materialize":
            return CommandResult(0, "materialized /icefarm-corpus-cache/active\n", "")
        if command.phase == "run.corpus-activate":
            return CommandResult(0, f"activated {command.argv[-5]}\n", "")
        return CommandResult(0, self.stdout, "")


def test_plan_mounts_authenticated_corpus_read_only_and_closure_scoped_oracle(
    tmp_path: Path,
) -> None:
    _farm, _scenario, plan = _farm_scenario_plan(tmp_path)
    client = next(item for item in plan["commands"] if item["phase"] == "up.start-c")
    argv = client["argv"]
    corpus_mount = next(item for item in argv if item.endswith("dst=/corpus,readonly"))
    oracle_mount = next(item for item in argv if item.endswith("dst=/oracle"))
    assert "/workload-unit/C1/input," in corpus_mount
    client = next(item for item in plan["topology"]["instances"] if item["role"] == "C")
    assert client["container_image"]["closure_sha256"] not in oracle_mount
    assert "/oracle/fmt-100/" in oracle_mount
    for environment in (
        "ICECC_P50_COMPILE_IDENTITY_TRACE=/results/compile-identity.jsonl",
        "ICECC_P50_C_ACTION_TRACE=/results/c-action.jsonl",
        "ICECC_P50_C_LEGACY_WIRE_TRACE=/results/c-legacy-wire.jsonl",
    ):
        assert environment in argv


def test_manifest_driver_is_one_fixed_program_with_all_spec_values_in_argv(
    tmp_path: Path,
) -> None:
    farm, scenario, plan = _farm_scenario_plan(tmp_path)
    scripted = WorkloadRecorder()
    transport = RecordingTransport(scripted)
    receipt = run_workload(
        farm,
        scenario,
        plan,
        recorder=transport,
        require_up=False,
    )
    assert receipt["schema"] == WORKLOAD_SCHEMA
    assert receipt["status"] == "COMPLETE"
    assert receipt["clients"] == [
        {"client": "C1", "failures": 0, "jobs": 100, "samples": 3}
    ]
    assert len(scripted.commands) == 1
    argv = scripted.commands[0].argv
    assert MANIFEST_DRIVER in argv
    driver_index = argv.index(MANIFEST_DRIVER)
    assert argv[driver_index + 12] == "1"
    assert argv[-4:] == ("A", "", "", "0")
    assert "/usr/bin/g++" in argv
    assert argv[-6:-4] == ("-O2", "-fdiagnostics-color=never")
    assert "fmt-100" not in MANIFEST_DRIVER
    assert "/usr/bin/g++-11" not in MANIFEST_DRIVER
    assert "oracle_recipe=icecream-clang-remote-v1" in MANIFEST_DRIVER
    assert '-Xclang -main-file-name -Xclang "$source"' in MANIFEST_DRIVER
    assert '-Xclang -fdebug-compilation-dir -Xclang "$PWD"' in MANIFEST_DRIVER
    assert '-c -target "$compiler_target" - -o "$object"' in MANIFEST_DRIVER
    assert '"$compiler" "${oracle_compiler_args[@]}" -c "$source"' in MANIFEST_DRIVER
    assert MANIFEST_DRIVER.count('oracle_compile "$source" "$object"') == 2
    assert "'building myself, but telling localhost'" in MANIFEST_DRIVER
    assert "'<building_local>'" in MANIFEST_DRIVER
    assert '"$remote" -eq 1' in MANIFEST_DRIVER
    assert "printf 'OPEN\\t0\\n' >\"$gate_state\"" in MANIFEST_DRIVER
    assert 'flock -x 8' in MANIFEST_DRIVER
    assert 'mv -- "$temporary_marker" "$marker"' in MANIFEST_DRIVER
    assert 'case "$gate_mode" in' in MANIFEST_DRIVER
    assert 'QUIESCE)' in MANIFEST_DRIVER
    assert 'write_checkpoint' in MANIFEST_DRIVER
    assert 'checkpoint_sha256' in MANIFEST_DRIVER
    assert 'resume_mode' in MANIFEST_DRIVER
    assert 'event gate aborted at epoch $gate_epoch' in MANIFEST_DRIVER
    assert 'rm -f -- "$marker"' in MANIFEST_DRIVER
    assert "event_serial_through=${ICEFARM_EVENT_SERIAL_THROUGH:-0}" in MANIFEST_DRIVER
    assert 'predecessor_ready=0' in MANIFEST_DRIVER
    assert 'active_count=$(find "$gate_active"' in MANIFEST_DRIVER
    assert 'serial_boundary_released=0' in MANIFEST_DRIVER
    assert 'event release wait expired for serial boundary job' in MANIFEST_DRIVER
    assert 'active-compiler-boundary-$index.ready.tsv' in MANIFEST_DRIVER
    assert 'active-compiler-boundary-$index.control' in MANIFEST_DRIVER
    assert 'boundary_release="$boundary_control/release.tsv"' in MANIFEST_DRIVER
    assert 'icefarm-active-compiler-boundary-v1' in MANIFEST_DRIVER
    assert 'icefarm-active-compiler-release-v1' in MANIFEST_DRIVER
    assert MANIFEST_DRIVER.index('>"$boundary_ready"') < MANIFEST_DRIVER.index(
        'mkdir "$job_dir"'
    )
    assert MANIFEST_DRIVER.index('serial_boundary_released=0') < MANIFEST_DRIVER.index(
        'mv -- "$result_temporary" "$job_dir/result.tsv"'
    )
    assert MANIFEST_DRIVER.index('>"$result_temporary"') < MANIFEST_DRIVER.index(
        'mv -- "$result_temporary" "$job_dir/result.tsv"'
    )
    assert 'xargs -0 -r -n 3 -P "$oracle_jobs"' in MANIFEST_DRIVER
    assert 'xargs -0 -n 5 -P "$jobs"' in MANIFEST_DRIVER
    assert 'object="$oracle_root/.build-$key-$BASHPID.o"' in MANIFEST_DRIVER
    assert MANIFEST_DRIVER.index("xargs -0 -r -n 3") < MANIFEST_DRIVER.index(
        'printf \'%s\\n\' "$oracle_identity"'
    )
    persisted = json.loads(
        (tmp_path / "results" / "workload-unit" / "workload.json").read_text()
    )
    assert persisted == receipt


def _receipt_gate_stub_inputs():
    farm = SimpleNamespace(hosts={"C-host": {"docker_context": "q5"}})
    scenario = SimpleNamespace(
        data={
            "workload": {
                "receipt_gate": {
                    "expected_commits": 1,
                    "negotiated_window": 1,
                    "expect_observed": True,
                    "command_timeout_s": 1800,
                },
                "corpus": "tiny",
                "repeat": 1,
            },
            "instances": [
                {"role": "S", "env": {"ICECC_P50_PROFILE": "P29V1"}}
            ],
        }
    )
    plan = {
        "topology": {
            "instances": [
                {"role": "F", "name": "F1", "address": "10.0.0.2"}
            ]
        },
        "ports": {"instances": {"F1": 23003}},
        "p51_receipt_gate": {
            "container_path": "/results/p50daemonpositive",
            "binary_sha256": "a" * 64,
        },
        "run_id": "receipt-gate-unit",
    }
    client = {"name": "C1", "host": "C-host"}
    return farm, scenario, plan, client


@pytest.mark.parametrize(
    ("line", "profile", "window", "expected"),
    [
        ("P51_RECEIPT_GATE_NEGOTIATED profile=1 window=1 epoch=1 generation=1", "P29V1", 1, True),
        ("P51_RECEIPT_GATE_NEGOTIATED profile=2 window=1 epoch=1 generation=1", "P29V1", 1, False),
        ("P51_RECEIPT_GATE_NEGOTIATED profile=1 window=30 epoch=1 generation=1", "P29V1", 1, False),
        ("P51_RECEIPT_GATE_NEGOTIATED profile=3 window=1 epoch=1 generation=1", "ZSTD_ROUTE", 1, True),
        ("not a negotiation record", "P29V1", 1, False),
    ],
)
def test_p51_negotiation_matches_expected_profile_and_window(
    line: str, profile: str, window: int, expected: bool
) -> None:
    match = workload_module.P51_NEGOTIATED_RE.search(line)
    assert workload_module._p51_negotiation_matches(
        match, profile=profile, window=window
    ) is expected


def test_p51_receipt_window_refuses_unverified_staged_helper(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    farm, scenario, plan, client = _receipt_gate_stub_inputs()
    phases: list[str] = []

    def gate_call(_farm, _plan, _client, _factory, _transport, phase, _argv):
        phases.append(phase)
        return CommandResult(0, "b" * 64 + "\n", "")

    monkeypatch.setattr(workload_module, "_p51_gate_call", gate_call)
    with pytest.raises(WorkloadError, match="staged P51 receipt-gate helper hash"):
        workload_module._run_p51_receipt_window(
            farm, scenario, plan, client, CommandFactory(), RecordingTransport(WorkloadRecorder()), "A"
        )

    # Hash verification is the first remote action; a bad or missing staged
    # helper must fail before starting either the helper or manifest driver.
    assert phases == ["verify-staged-helper"]


def test_p51_receipt_window_separates_driver_and_sidecar_uids(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    farm, scenario, plan, client = _receipt_gate_stub_inputs()
    phases: list[str] = []
    gate_commands: list[dict[str, object]] = []
    driver_uids: list[str] = []
    staged_bundles: list[tuple[dict[str, object], dict[str, object]]] = []

    def gate_call(_farm, _plan, _client, _factory, _transport, phase, argv, **_kwargs):
        phases.append(phase)
        if phase == "verify-staged-helper":
            return CommandResult(0, "a" * 64 + "\n", "")
        if phase == "sidecar-uid":
            assert argv == ("/usr/bin/id", "-u", "nobody")
            return CommandResult(0, "65534\n", "")
        if phase == "install-test-iptables":
            assert argv[:2] == ("/bin/sh", "-c")
            assert len(argv) == 4 and argv[3] == "install-test-iptables"
            assert "dpkg -i ./*.deb" in argv[2]
            assert "dpkg-query" in argv[2]
            return CommandResult(0, "iptables v1.8.9 (nf_tables)\n", "")
        raise AssertionError(f"unexpected gate call before driver launch: {phase}")

    class CapturingFactory:
        def make(self, **kwargs):
            gate_commands.append(kwargs)
            return object()

    class DriverReached(Exception):
        pass

    def driver_command(*_args, **kwargs):
        driver_uids.append(kwargs["exec_uid"])
        raise DriverReached

    def stage_bundle(_farm, _plan, _client, _factory, _transport):
        staged_bundles.append((_plan, _client))

    monkeypatch.setattr(workload_module, "_p51_gate_call", gate_call)
    monkeypatch.setattr(workload_module, "_stage_p51_iptables_bundle", stage_bundle)
    monkeypatch.setattr(workload_module, "_driver_command", driver_command)
    with pytest.raises(DriverReached):
        workload_module._run_p51_receipt_window(
            farm, scenario, plan, client, CapturingFactory(), RecordingTransport(WorkloadRecorder()), "A"
        )

    assert phases == ["verify-staged-helper", "sidecar-uid", "install-test-iptables"]
    assert staged_bundles == [(plan, client)]
    assert driver_uids == ["1:1"]
    argv = gate_commands[0]["argv"]
    assert isinstance(argv, tuple)
    assert argv[argv.index("--user") + 1] == "0"
    assert argv[argv.index("--p51-commit-receipt-gate-remote") + 3] == "65534"
    remote_gate_index = argv.index("--p51-commit-receipt-gate-remote")
    assert argv[remote_gate_index + 7] == "1770"
    assert gate_commands[0]["timeout_s"] == 1800


@pytest.mark.parametrize(
    ("gate_spec", "expected"),
    [
        ({}, (260, None)),
        ({"command_timeout_s": 1800}, (1800, 1770)),
    ],
)
def test_p51_receipt_gate_budget_preserves_legacy_and_reserves_cleanup(
    gate_spec, expected
) -> None:
    assert workload_module._p51_gate_budget(gate_spec) == expected


@pytest.mark.parametrize("value", [True, 30, 86401, "1800", 0])
def test_p51_receipt_gate_budget_rejects_invalid_explicit_limits(value) -> None:
    with pytest.raises(WorkloadError, match="command_timeout_s"):
        workload_module._p51_gate_budget({"command_timeout_s": value})


def test_p51_iptables_bundle_is_hash_checked_and_staged_in_bounded_chunks(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    bundle = tmp_path / "debs"
    bundle.mkdir()
    payloads = {
        name: (f"fixture:{name}:".encode() + b"x" * 100_000)
        for name in workload_module.P51_IPTABLES_BUNDLE_FILES
    }
    rows = []
    for name, payload in payloads.items():
        (bundle / name).write_bytes(payload)
        rows.append(f"{hashlib.sha256(payload).hexdigest()}  {name}")
    manifest = ("\n".join(rows) + "\n").encode()
    (bundle / "SHA256SUMS").write_bytes(manifest)
    monkeypatch.setenv("ICEFARM_P51_IPTABLES_BUNDLE", str(bundle))
    monkeypatch.setattr(
        workload_module,
        "P51_IPTABLES_BUNDLE_MANIFEST_SHA256",
        hashlib.sha256(manifest).hexdigest(),
    )

    farm, _scenario, plan, client = _receipt_gate_stub_inputs()
    reconstructed: dict[str, bytearray] = {}
    current_path = ""
    wrong_remote_hash = False

    def gate_call(_farm, _plan, _client, _factory, _transport, phase, argv, **_kwargs):
        nonlocal current_path
        if phase == "stage-iptables-bundle-file":
            current_path = argv[-1].rsplit("/", 1)[-1]
            reconstructed[current_path] = bytearray()
            return CommandResult(0, "", "")
        if phase == "stage-iptables-bundle-chunk":
            chunk, remote_path = argv[-2:]
            current_path = remote_path.rsplit("/", 1)[-1]
            reconstructed[current_path].extend(base64.b64decode(chunk))
            assert len(chunk) <= 42_000
            return CommandResult(0, "", "")
        if phase == "verify-iptables-bundle-file":
            name = argv[-1].rsplit("/", 1)[-1]
            if wrong_remote_hash:
                return CommandResult(0, f"{'0' * 64}\n", "")
            return CommandResult(
                0,
                f"{hashlib.sha256(reconstructed[name]).hexdigest()}\n",
                "",
            )
        assert phase == "stage-iptables-bundle-dir"
        return CommandResult(0, "", "")

    monkeypatch.setattr(workload_module, "_p51_gate_call", gate_call)
    workload_module._stage_p51_iptables_bundle(
        farm, plan, client, CommandFactory(), RecordingTransport(WorkloadRecorder())
    )
    for name, payload in payloads.items():
        assert bytes(reconstructed[name]) == payload
    assert bytes(reconstructed["SHA256SUMS"]) == manifest

    wrong_remote_hash = True
    with pytest.raises(WorkloadError, match="staged P51 iptables package hash mismatch"):
        workload_module._stage_p51_iptables_bundle(
            farm, plan, client, CommandFactory(), RecordingTransport(WorkloadRecorder())
        )


def test_manifest_driver_exports_boundary_dependencies_to_compiler_children(
    tmp_path: Path,
) -> None:
    variable_export_line = next(
        line
        for line in MANIFEST_DRIVER.splitlines()
        if line.startswith("export gate_root ")
    )
    function_export_line = next(
        line
        for line in MANIFEST_DRIVER.splitlines()
        if line.startswith("export -f ") and "compile_one" in line
    )
    completed = subprocess.run(
        (
            "/bin/bash",
            "-c",
            "read_boundary_release() { printf 'release:%s\\n' \"$1\"; }; "
            "compile_one() { :; }; "
            f"gate_root=$1; {function_export_line}; {variable_export_line}; "
            "/bin/bash -c 'declare -F read_boundary_release >/dev/null && "
            "read_boundary_release \"$gate_root\"'",
            "icefarm-gate-root-probe",
            str(tmp_path / "event-root"),
        ),
        check=True,
        capture_output=True,
        text=True,
    )

    assert completed.stdout == f"release:{tmp_path / 'event-root'}\n"


def test_s30_mutant_workload_enables_the_legacy_recovery_under_test(
    tmp_path: Path,
) -> None:
    farm = load_farm_spec(farm_fixture.example_farm_path())
    farm.data["hub"]["results_root"] = str(tmp_path)
    scenario = load_scenario_spec(
        INTEGRATION / "scenarios" / "S30-mutant-f-refusal.json", farm
    )
    plan = farmtest.build_plan(farm, scenario, run_id="s30-mutant-workload-unit")
    scripted = WorkloadRecorder()
    run_workload(
        farm,
        scenario,
        plan,
        recorder=RecordingTransport(scripted),
        require_up=False,
    )
    argv = scripted.commands[0].argv
    driver_index = argv.index(MANIFEST_DRIVER)
    assert argv[driver_index + 12] == "0"


@pytest.mark.parametrize(
    ("scenario_id", "expected"),
    (
        ("S80-legacy", False),
        ("S80-p29v1", True),
        ("S70-b6-kill-switch", False),
        ("S70-b5-interner-failure", True),
        ("S95-cache-disk-full", True),
        ("S40-full-newgen-engagement", True),
        ("S60-11-warm-f2-down", False),
        ("S60-13-warm-s-down", False),
        ("H2-client-kill-switch", False),
        ("S70-b4-scheduler-active-loss", False),
    ),
)
def test_strict_p50_tracks_whole_run_engagement_contract(
    tmp_path: Path, scenario_id: str, expected: bool
) -> None:
    farm = load_farm_spec(farm_fixture.example_farm_path())
    farm.data["hub"]["results_root"] = str(tmp_path)
    scenario = load_scenario_spec(
        INTEGRATION / "scenarios" / f"{scenario_id}.json", farm
    )
    plan = farmtest.build_plan(farm, scenario, run_id=f"strict-{scenario_id}")

    assert _strict_p50_required(scenario, plan) is expected


def test_strict_p50_requires_the_s70_b6_off_transition() -> None:
    farm = load_farm_spec(farm_fixture.example_farm_path())
    scenario = load_scenario_spec(
        INTEGRATION / "scenarios" / "S70-b6-kill-switch.json", farm
    )
    plan = farmtest.build_plan(farm, scenario, run_id="strict-b6-mutation")
    scenario.data["timeline"][0]["env"]["ICECC_P50_PROFILE"] = "P29V1"

    assert _strict_p50_required(scenario, plan) is True


@pytest.mark.parametrize("topology", ["C1F2", "C2F1"])
def test_all_new_multilink_receipt_driver_keeps_strict_p50_retry(
    tmp_path: Path, topology: str
) -> None:
    """Receipt-link shape is not a reason to downgrade P50 retries to legacy."""
    farm = load_farm_spec(farm_fixture.example_farm_path())
    farm.data["hub"]["results_root"] = str(tmp_path)
    scenario = load_scenario_spec(
        INTEGRATION / "scenarios" / "S40-full-newgen-engagement.json", farm
    )
    plan = farmtest.build_plan(farm, scenario, run_id=f"strict-{topology}")
    scenario.data["workload"]["driver"] = "p51-receipt-window"
    scenario.data["workload"]["turns"] = ["A"]
    if topology == "C1F2":
        scenario.data["workload"]["receipt_gate"] = {
            "links": [
                {"client": "C1", "worker": "F1", "first_job": 1, "last_job": 24},
                {"client": "C1", "worker": "F2", "first_job": 25, "last_job": 48},
            ],
            "expected_commits": 1,
            "negotiated_window": 1,
            "expect_observed": True,
        }
        client_names = ["C1"]
    else:
        c1_scenario = next(
            item for item in scenario.data["instances"]
            if item["role"] == "C" and item["name"] == "C1"
        )
        scenario.data["instances"].append({**c1_scenario, "name": "C2"})
        scenario.data["workload"]["clients"] = ["C1", "C2"]
        scenario.data["workload"]["receipt_gate"] = {
            "links": [
                {"client": name, "worker": "F1", "first_job": 1, "last_job": 48}
                for name in ("C1", "C2")
            ],
            "expected_commits": 1,
            "negotiated_window": 1,
            "expect_observed": True,
        }
        c1_plan = next(
            item for item in plan["topology"]["instances"]
            if item["role"] == "C" and item["name"] == "C1"
        )
        plan["topology"]["instances"].append({**c1_plan, "name": "C2"})
        scenario.data["instances"] = [
            item for item in scenario.data["instances"]
            if item["role"] != "F" or item["name"] == "F1"
        ]
        plan["topology"]["instances"] = [
            item for item in plan["topology"]["instances"]
            if item["role"] != "F" or item["name"] == "F1"
        ]
        client_names = ["C1", "C2"]

    assert _strict_p50_required(scenario, plan) is True
    for client_name in client_names:
        client = next(
            item for item in plan["topology"]["instances"]
            if item["role"] == "C" and item["name"] == client_name
        )
        command = _driver_command(
            farm, scenario, plan, client, "A", CommandFactory()
        )
        driver_index = command.argv.index(MANIFEST_DRIVER)
        assert command.argv[driver_index + 12] == "1"


@pytest.mark.parametrize("mutation", ["old-worker", "profile-off", "control"])
def test_multilink_receipt_driver_downgrades_when_p50_is_not_run_wide(
    mutation: str,
) -> None:
    farm, scenario, plan, clients = _multilink_orchestrator_fixture("C1F2")
    scenario.data["workload"]["driver"] = "p51-receipt-window"
    scenario.data["workload"]["turns"] = ["A"]
    if mutation == "old-worker":
        next(item for item in plan["topology"]["instances"] if item["role"] == "F")["version"] = 49
    elif mutation == "profile-off":
        scenario.data["instances"][0]["env"]["ICECC_P50_PROFILE"] = "OFF"
    else:
        scenario.data["controls"] = [{"kind": "bounded-control"}]

    assert _strict_p50_required(scenario, plan) is False
    command = _driver_command(
        farm, scenario, plan, clients[0], "A", CommandFactory()
    )
    driver_index = command.argv.index(MANIFEST_DRIVER)
    assert command.argv[driver_index + 12] == "0"


def test_active_loss_serializes_exact_trigger_prefix_and_requires_remote(
    tmp_path: Path,
) -> None:
    farm = load_farm_spec(farm_fixture.example_farm_path())
    farm.data["hub"]["results_root"] = str(tmp_path)
    scenario = load_scenario_spec(
        INTEGRATION / "scenarios" / "S70-b4-scheduler-active-loss.json", farm
    )
    plan = farmtest.build_plan(farm, scenario, run_id="active-loss-workload-unit")
    assert _active_loss_serial_through(scenario) == 2

    client = next(
        item for item in plan["topology"]["instances"] if item["role"] == "C"
    )
    command = _driver_command(
        farm,
        scenario,
        plan,
        client,
        "A",
        CommandFactory(),
    )
    argv = command.argv
    assert "ICEFARM_EVENT_SERIAL_THROUGH=2" in argv
    assert "ICECC_REMOTE_REQUIRED=1" in argv

    ordinary = load_scenario_spec(
        INTEGRATION / "scenarios" / "S00-smoke.json", farm
    )
    assert _active_loss_serial_through(ordinary) == 0


def test_s95_disk_full_prefers_the_declared_filled_worker() -> None:
    farm = load_farm_spec(farm_fixture.example_farm_path())
    scenario = load_scenario_spec(
        INTEGRATION / "scenarios" / "S95-cache-disk-full.json", farm
    )
    plan = farmtest.build_plan(farm, scenario, run_id="s95-preferred-worker-unit")
    client = next(
        item for item in plan["topology"]["instances"] if item["role"] == "C"
    )

    command = _driver_command(
        farm, scenario, plan, client, "A", CommandFactory()
    )
    assert "ICEFARM_DISK_FILL_WORKER=F2" in command.argv
    assert "ICEFARM_DISK_FILL_TRIGGER=12" in command.argv
    assert "ICECC_PREFERRED_HOST=F2" not in command.argv
    assert 'preferred=(ICECC_PREFERRED_HOST="$disk_fill_worker")' in MANIFEST_DRIVER

    ordinary = load_scenario_spec(
        INTEGRATION / "scenarios" / "S00-smoke.json", farm
    )
    ordinary_plan = farmtest.build_plan(
        farm, ordinary, run_id="ordinary-no-preferred-worker-unit"
    )
    ordinary_command = _driver_command(
        farm, ordinary, ordinary_plan, client, "A", CommandFactory()
    )
    assert not any(
        item.startswith("ICEFARM_DISK_FILL_") for item in ordinary_command.argv
    )


def test_manifest_driver_shell_is_syntactically_valid() -> None:
    subprocess.run(
        ["/bin/bash", "-n"],
        input=MANIFEST_DRIVER,
        text=True,
        check=True,
        capture_output=True,
    )


@pytest.mark.parametrize(
    ("configured", "expected", "valid"),
    [(None, "1", True), ("1", "1", True), ("4", "4", True),
     ("000000001", "1", True),
     ("0", "", False), ("-1", "", False), ("1x", "", False),
     ("61", "", False), ("999999999999", "", False)],
)
def test_oracle_prepare_fanout_is_separate_from_measured_jobs(
    configured: str | None, expected: str, valid: bool,
) -> None:
    resolver = re.search(
        r"(?ms)^resolve_oracle_jobs\(\) \{.*?^\}", MANIFEST_DRIVER
    )
    assert resolver is not None
    env = {key: value for key, value in os.environ.items() if key != "ICEFARM_ORACLE_JOBS"}
    if configured is not None:
        env["ICEFARM_ORACLE_JOBS"] = configured
    result = subprocess.run(
        ["/bin/bash", "-c", "jobs=60\n" + resolver.group(0) + "\nresolve_oracle_jobs"],
        env=env, text=True, capture_output=True, check=False, timeout=5,
    )
    if valid:
        assert result.returncode == 0, result.stderr
        assert result.stdout.strip() == expected
    else:
        assert result.returncode == 65
        assert "invalid ICEFARM_ORACLE_JOBS" in result.stderr

    # The first xargs is oracle preparation; the later one remains measured
    # workload dispatch and must continue to use the scenario's jobs value.
    oracle_dispatch = re.search(
        r"(?m)^\s*\| xargs -0 -r -n 3 -P \"\$oracle_jobs\" .*oracle_one", MANIFEST_DRIVER
    )
    measured_dispatch = re.search(
        r"(?m)^xargs -0 -n 5 -P \"\$jobs\" .*compile_one", MANIFEST_DRIVER
    )
    assert oracle_dispatch is not None
    assert measured_dispatch is not None


@pytest.mark.parametrize(
    ("epoch", "index", "boundary", "expected"),
    [(0, 24, 24, 1), (0, 25, 24, 0), (0, 100, 24, 0),
     (1, 25, 24, 1), (1, 100, 24, 1), (0, 100, 0, 1)],
)
def test_s60_admission_reserves_suffix_even_when_controller_is_delayed(
    epoch: int, index: int, boundary: int, expected: int,
) -> None:
    # Execute the actual admission decision, not a Python copy. The controller
    # may remain at epoch zero indefinitely: finishing the prefix must not
    # admit the suffix until its authenticated resume opens epoch one.
    start = MANIFEST_DRIVER.index("                admit_now=0\n")
    end = MANIFEST_DRIVER.index('                if test "$admit_now" -eq 1', start)
    program = (
        'event_serial_through=0; gate_epoch=$1; index=$2; s60_admit_through=$3;\n'
        + MANIFEST_DRIVER[start:end]
        + '\nprintf "%s\\n" "$admit_now"\n'
    )
    result = subprocess.run(
        ["/bin/bash", "-eu", "-c", program, "admission-probe",
         str(epoch), str(index), str(boundary)],
        text=True, capture_output=True, check=True,
    )
    assert int(result.stdout) == expected


@pytest.mark.parametrize("path", sorted((INTEGRATION / "scenarios").glob("S60-*.json")))
def test_s60_driver_binds_admission_limit_for_initial_and_resumed_clients(
    tmp_path: Path, path: Path,
) -> None:
    farm = load_farm_spec(farm_fixture.example_farm_path())
    farm.data["hub"]["results_root"] = str(tmp_path)
    scenario = load_scenario_spec(path, farm)
    plan = farmtest.build_plan(farm, scenario, run_id="s60-admission-unit")
    for client in plan["topology"]["instances"]:
        if client["role"] != "C":
            continue
        for resume in (False, True):
            command = _driver_command(
                farm, scenario, plan, client, "A", CommandFactory(),
                resume=resume, checkpoint_sha256="a" * 64 if resume else None,
            )
            assert "ICEFARM_S60_ADMIT_THROUGH=24" in command.argv
            assert not any(arg.startswith("ICEFARM_EVENT_SERIAL_THROUGH=")
                           for arg in command.argv)
    corpus = farm.data["corpora"][scenario.data["workload"]["corpus"]]
    scenario.data["timeline"][0]["trigger"] = "job 1000000"
    with pytest.raises(WorkloadError, match="preserve a client suffix"):
        workload_module._s60_admit_through(scenario, corpus)


@pytest.mark.parametrize("release", ["OPEN", "ABORT", "QUIESCE"])
def test_s60_suffix_waits_at_real_gate_then_resumes_or_aborts(
    tmp_path: Path, release: str,
) -> None:
    active = tmp_path / "active"
    active.mkdir()
    state = tmp_path / "state.tsv"
    state.write_text("OPEN\t0\n")
    start = MANIFEST_DRIVER.index("    gate_deadline=$((SECONDS + per_job_timeout))")
    end = MANIFEST_DRIVER.index("    trap 'rm -f", start)
    program = (
        'gate_active=$1/active; gate_state=$1/state.tsv; gate_lock=$1/lock; '
        'marker=$gate_active/probe.tsv; index=25; per_job_timeout=5; '
        'event_serial_through=0; s60_admit_through=24; '
        'probe() {\n' + MANIFEST_DRIVER[start:end] + '\n}; probe\n'
    )
    process = subprocess.Popen(
        ["/bin/bash", "-eu", "-c", program, "gate-probe", str(tmp_path)],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
    )
    try:
        time.sleep(0.15)
        assert process.poll() is None, "suffix escaped before transition resume"
        assert not list(active.iterdir())
        # Publish using the same lock/atomic-state discipline as the controller.
        subprocess.run(
            ["/bin/bash", "-eu", "-c",
             'exec 8>"$1/lock"; flock -x 8; printf "%s\\t1\\n" "$2" >"$1/next"; '
             'mv "$1/next" "$1/state.tsv"', "release", str(tmp_path), release],
            check=True,
        )
        _, stderr = process.communicate(timeout=6)
        assert process.returncode == (0 if release == "OPEN" else 75), stderr
        assert bool(list(active.iterdir())) is (release == "OPEN")
    finally:
        if process.poll() is None:
            process.kill()
            process.communicate()


@pytest.mark.parametrize("action", ["pause", "quiesce"])
def test_s60_completed_boundary_survives_delayed_controller_and_drains(
    tmp_path: Path, action: str,
) -> None:
    from farmharness.integration.events import GATE_CONTROL_SCRIPT

    gate = tmp_path / "event-gate"
    active = gate / "active"
    active.mkdir(parents=True)
    (gate / "state.tsv").write_text("OPEN\t0\n")
    (gate / "state.lock").touch()
    marker = active / "job-24-99.tsv"
    marker.write_text("24\t99\t0\t1\n")
    start = MANIFEST_DRIVER.index("    # Keep a completed prefix boundary visible")
    end = MANIFEST_DRIVER.index('    rm -f -- "$remote_object"', start)
    program = (
        'gate_lock=$1/event-gate/state.lock; gate_state=$1/event-gate/state.tsv; '
        's60_admit_through=24; index=24; gate_epoch=0; per_job_timeout=5; '
        'finish() {\n' + MANIFEST_DRIVER[start:end]
        + '\n}; finish; rm "$1/event-gate/active/job-24-99.tsv"\n'
    )
    boundary = subprocess.Popen(
        ["/bin/bash", "-eu", "-c", program, "boundary", str(tmp_path)],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
    )
    try:
        time.sleep(0.15)
        assert boundary.poll() is None
        assert marker.exists()
        result = subprocess.run(
            ["python3", "-c", GATE_CONTROL_SCRIPT, action, str(tmp_path),
             "C1", "A", "1", "3"],
            text=True, capture_output=True, timeout=5, check=True,
        )
        receipt = json.loads(result.stdout)
        assert receipt["active_before"] == 1
        assert receipt["active_after"] == 0
        _, stderr = boundary.communicate(timeout=2)
        assert boundary.returncode == 0, stderr
    finally:
        if boundary.poll() is None:
            boundary.kill()
            boundary.communicate()
    assert "scenario.data" not in MANIFEST_DRIVER


def test_recording_transport_orders_delayed_concurrent_invocation_by_sequence() -> None:
    class Delegate:
        def invoke(self, _command: PlannedCommand) -> CommandResult:
            return CommandResult(0, "", "")

    transport = RecordingTransport(Delegate())
    factory = CommandFactory()
    low = factory.make(
        phase="low",
        host="hub",
        transport="local",
        timeout_s=1,
        argv=("true",),
    )
    high = factory.make(
        phase="high",
        host="hub",
        transport="local",
        timeout_s=1,
        argv=("true",),
    )
    release_low = threading.Event()

    thread = threading.Thread(
        target=lambda: (release_low.wait(timeout=2), transport.invoke(low)),
        daemon=True,
    )
    thread.start()
    transport.invoke(high)
    release_low.set()
    thread.join(timeout=2)

    assert not thread.is_alive()
    assert [command.sequence for command in transport.commands] == [0, 1]


def test_manifest_driver_relaunch_replaces_oracle_samples_atomically(
    tmp_path: Path,
) -> None:
    start = MANIFEST_DRIVER.index("sample_bucket=$((16#")
    end = MANIFEST_DRIVER.index("\nflock -u 9", start)
    sample_program = MANIFEST_DRIVER[start:end]
    result_root = tmp_path / "results"
    oracle_root = tmp_path / "oracle"
    result_root.mkdir()
    (result_root / "oracle-samples").mkdir()
    oracle_root.mkdir()
    source = tmp_path / "source.ii"
    source.write_bytes(b"canonical object bytes\n")
    relative = "files/source.ii"
    digest = "a" * 64
    unique = tmp_path / "unique.tsv"
    unique.write_text(
        f"{digest}\t{relative}\t{source}\n",
        encoding="utf-8",
    )
    key = subprocess.run(
        ["sha256sum"],
        input=f"{digest}\n{relative}\n",
        text=True,
        check=True,
        capture_output=True,
    ).stdout.split()[0]
    observed = subprocess.run(
        ["sha256sum", str(source)],
        text=True,
        check=True,
        capture_output=True,
    ).stdout.split()[0]
    (oracle_root / f"{key}.sha256").write_text(observed + "\n", encoding="ascii")
    prefix = f"""
set -eu
oracle_prepared=0
d18_barrier=0
client_name=C1
manifest_digest={'b' * 64}
result_root={result_root}
oracle_root={oracle_root}
unique={unique}
oracle_compile() {{ cp -- "$1" "$2"; }}
"""

    for _ in range(2):
        subprocess.run(
            ["/bin/bash"],
            input=prefix + sample_program,
            text=True,
            check=True,
            capture_output=True,
        )

    samples = (result_root / "oracle-samples.tsv").read_text(
        encoding="utf-8"
    ).splitlines()
    assert len(samples) == 1
    assert samples[0].split("\t") == [relative, observed, observed, "1"]
    assert (result_root / "oracle-summary.tsv").read_text(encoding="utf-8") == (
        "sample_total\t1\nsample_mismatches\t0\n"
    )


def test_workload_summary_is_fail_closed(tmp_path: Path) -> None:
    farm, scenario, plan = _farm_scenario_plan(tmp_path)
    transport = RecordingTransport(WorkloadRecorder("not a summary\n"))
    with pytest.raises(WorkloadError, match="no unique workload summary"):
        run_workload(
            farm,
            scenario,
            plan,
            recorder=transport,
            require_up=False,
        )


def test_nonzero_workload_result_cannot_be_accepted_with_forged_summary() -> None:
    with pytest.raises(WorkloadError, match=r"failed rc=7: compiler root cause"):
        _parse_summary(
            CommandResult(7, "ICEFARM_WORKLOAD jobs=100 failures=0 samples=3\n", "compiler root cause"),
            "C1",
        )


def _checkpoint_writer_python() -> str:
    marker = 'python3 - "$checkpoint_tmp" "$result_root" "$worklist" "$client_name" "$turn" "$expected_jobs" <<\'PY\'\n'
    start = MANIFEST_DRIVER.index(marker) + len(marker)
    end = MANIFEST_DRIVER.index("\nPY\n", start)
    return MANIFEST_DRIVER[start:end]


def _run_checkpoint_writer(tmp_path: Path, relative: str, *, symlink: bool = False):
    result_root = tmp_path / "results"
    jobs = result_root / "jobs"
    jobs.mkdir(parents=True)
    worklist = tmp_path / "worklist"
    worklist.write_text("work\n", encoding="utf-8")
    path = result_root / relative
    path.parent.mkdir(parents=True, exist_ok=True)
    if symlink:
        target = tmp_path / "outside.tsv"
        target.write_text("1\tA\tx\tx\tx\tx\tx\tx\t0\tx\tx\t1\t1\tx\n", encoding="utf-8")
        path.symlink_to(target)
    else:
        path.write_text("1\tA\tx\tx\tx\tx\tx\tx\t0\tx\tx\t1\t1\tx\n", encoding="utf-8")
    output = result_root / "checkpoint.json"
    result = subprocess.run(
        ["python3", "-c", _checkpoint_writer_python(), str(output), str(result_root), str(worklist), "C1", "A", "1"],
        text=True,
        capture_output=True,
    )
    return result, output


@pytest.mark.parametrize(
    ("relative", "symlink"),
    (("jobs/evil/result.tsv", False), ("jobs/000001/result.tsv", True)),
)
def test_checkpoint_writer_refuses_malicious_result_identity(
    tmp_path: Path, relative: str, symlink: bool
) -> None:
    result, output = _run_checkpoint_writer(tmp_path, relative, symlink=symlink)
    assert result.returncode != 0
    assert not output.exists()


def test_checkpoint_writer_accepts_only_exact_result_identity(tmp_path: Path) -> None:
    result, output = _run_checkpoint_writer(tmp_path, "jobs/000001/result.tsv")
    assert result.returncode == 0, result.stderr
    document = json.loads(output.read_text(encoding="utf-8"))
    assert document["completed_rows"][0]["path"] == "jobs/000001/result.tsv"


def test_control_workload_disables_strict_mode_to_observe_the_fault(
    tmp_path: Path,
) -> None:
    farm = load_farm_spec(farm_fixture.example_farm_path())
    farm.data["hub"]["results_root"] = str(tmp_path)
    scenario = load_scenario_spec(
        INTEGRATION / "scenarios" / "H2-client-kill-switch.json", farm
    )
    plan = farmtest.build_plan(farm, scenario, run_id="control-workload-unit")
    scripted = WorkloadRecorder()

    run_workload(
        farm,
        scenario,
        plan,
        recorder=RecordingTransport(scripted),
        require_up=False,
    )

    argv = scripted.commands[0].argv
    driver_argv0 = argv.index("icefarm-manifest-driver")
    assert argv[driver_argv0 + 11] == "0"


def test_h4_passes_one_typed_object_corruption_fault_in_driver_argv(
    tmp_path: Path,
) -> None:
    farm = load_farm_spec(farm_fixture.example_farm_path())
    farm.data["hub"]["results_root"] = str(tmp_path)
    scenario = load_scenario_spec(
        INTEGRATION / "scenarios" / "H4-corrupt-object.json", farm
    )
    plan = farmtest.build_plan(farm, scenario, run_id="h4-workload-unit")
    scripted = WorkloadRecorder("ICEFARM_WORKLOAD jobs=100 failures=1 samples=3\n")

    receipt = run_workload(
        farm,
        scenario,
        plan,
        recorder=RecordingTransport(scripted),
        require_up=False,
    )

    assert receipt["status"] == "COMPLETE_WITH_JOB_FAILURES"
    assert scripted.commands[0].argv[-4:] == ("A", "corrupt-object", "C1", "1")
    assert "before_sha=$(sha256sum" in MANIFEST_DRIVER
    assert "icefarm-h4-object-fault-v1" in MANIFEST_DRIVER


def test_workload_requires_authenticated_up_receipt(tmp_path: Path) -> None:
    farm, scenario, plan = _farm_scenario_plan(tmp_path)
    with pytest.raises(WorkloadError, match="cannot load UP lifecycle receipt"):
        run_workload(
            farm,
            scenario,
            plan,
            recorder=RecordingTransport(WorkloadRecorder()),
        )


def test_single_manifest_refuses_an_undefined_turn(tmp_path: Path) -> None:
    farm = load_farm_spec(farm_fixture.example_farm_path())
    scenario = json.loads((INTEGRATION / "scenarios" / "S00-smoke.json").read_text())
    scenario["workload"]["turns"] = ["B"]
    path = tmp_path / "scenario.json"
    path.write_text(json.dumps(scenario), encoding="utf-8")
    with pytest.raises(ScenarioSpecError, match="does not define turns"):
        load_scenario_spec(path, farm)


def test_ssh_transport_keeps_driver_values_inside_encoded_argv(tmp_path: Path) -> None:
    farm, scenario, plan = _farm_scenario_plan(tmp_path)
    farm.hosts["tt-quietbox3"].pop("docker_context")
    plan = farmtest.build_plan(farm, scenario, run_id="workload-ssh")
    scripted = WorkloadRecorder()
    run_workload(
        farm,
        scenario,
        plan,
        recorder=RecordingTransport(scripted),
        require_up=False,
    )
    decoded = decode_ssh_payload(scripted.commands[0].argv)
    assert decoded[:3] == ("docker", "exec", "--user")
    assert MANIFEST_DRIVER in decoded


def test_paired_workload_materializes_only_b_between_sequential_turns(
    tmp_path: Path,
) -> None:
    farm = load_farm_spec(farm_fixture.example_farm_path())
    farm.data["hub"]["results_root"] = str(tmp_path)
    scenario = load_scenario_spec(
        INTEGRATION / "scenarios" / "S80-p29v1.json", farm
    )
    plan = farmtest.build_plan(farm, scenario, run_id="paired-unit")
    scripted = PairedWorkloadRecorder()
    receipt = run_workload(
        farm,
        scenario,
        plan,
        recorder=RecordingTransport(scripted),
        require_up=False,
    )
    assert receipt["clients"] == [
        {"client": "C1", "failures": 0, "jobs": 200, "samples": 6}
    ]
    assert [item["turn"] for item in receipt["turns"]] == ["A", "B"]
    assert [command.phase for command in scripted.commands] == [
        "run.workload",
        "run.corpus-input-mkdir",
        "run.corpus-clear",
        "run.corpus-materialize",
        "run.corpus-activate",
        "run.workload",
    ]
    drivers = [
        command for command in scripted.commands if command.phase == "run.workload"
    ]
    assert drivers[0].argv[-4] == "A"
    assert drivers[1].argv[-4] == "B"
    assert "/results/workload/A" in drivers[0].argv
    assert "/results/workload/B" in drivers[1].argv
    assert 'printf \'%s\\n%s\\n\' "$digest" "$relative"' in MANIFEST_DRIVER


def test_s50_runs_old_and_new_clients_concurrently_in_one_turn(
    tmp_path: Path,
) -> None:
    farm = load_farm_spec(farm_fixture.example_farm_path())
    farm.data["hub"]["results_root"] = str(tmp_path)
    scenario = load_scenario_spec(
        INTEGRATION / "scenarios" / "S50-mixed-pool-fmt.json", farm
    )
    plan = farmtest.build_plan(farm, scenario, run_id="s50-workload-unit")
    scripted = WorkloadRecorder()
    receipt = run_workload(
        farm,
        scenario,
        plan,
        recorder=RecordingTransport(scripted),
        require_up=False,
    )
    assert receipt["clients"] == [
        {"client": "C1", "failures": 0, "jobs": 100, "samples": 3},
        {"client": "C2", "failures": 0, "jobs": 100, "samples": 3},
    ]
    commands = [
        command for command in scripted.commands if command.phase == "run.workload"
    ]
    assert len(commands) == 2
    assert {command.instance for command in commands} == {"C1", "C2"}
    assert all(command.argv[-4] == "A" for command in commands)


@pytest.mark.parametrize("failure", (False, True))
def test_checkpointed_turn_stays_active_after_initial_futures_finish(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch, failure: bool
) -> None:
    farm = load_farm_spec(farm_fixture.example_farm_path())
    farm.data["hub"]["results_root"] = str(tmp_path)
    scenario = load_scenario_spec(
        INTEGRATION / "scenarios" / "S60-06-c2-down.json", farm
    )
    plan = farmtest.build_plan(farm, scenario, run_id="checkpoint-turn-lease")

    class RaceRecorder(WorkloadRecorder):
        def __init__(self) -> None:
            super().__init__()
            self.initial = 0
            self.initial_done = threading.Event()
            self.lock = threading.Lock()

        def invoke(self, command: PlannedCommand) -> CommandResult:
            self.commands.append(command)
            if command.phase == "event.checkpoint":
                return CommandResult(
                    0,
                    json.dumps(
                        {
                            "checkpoint_sha256": "a" * 64,
                            "expected_jobs": 100,
                        }
                    ),
                    "",
                )
            if command.phase == "run.workload":
                with self.lock:
                    if not self.initial_done.is_set():
                        self.initial += 1
                        if self.initial == 2:
                            self.initial_done.set()
                return CommandResult(
                    0, "ICEFARM_WORKLOAD jobs=100 failures=0 samples=3\n", ""
                )
            return CommandResult(0, "", "")

    recorder = RaceRecorder()

    class DelayedCheckpointProducer:
        def __init__(self, _farm, _scenario, event_plan, **kwargs) -> None:
            self.plan = event_plan
            self.quiesce = kwargs["quiesce_workload"]
            self.relaunch = kwargs["relaunch_workload"]
            self.error: BaseException | None = None
            self.thread: threading.Thread | None = None

        def start(self) -> None:
            return None

        def signal_turn_start(self, turn: str) -> None:
            clients = tuple(
                item
                for item in self.plan["topology"]["instances"]
                if item["role"] == "C"
            )

            def delayed() -> None:
                try:
                    assert recorder.initial_done.wait(timeout=1)
                    # Reproduce A6's ordering: workload futures have become
                    # terminal while the event callback is still delayed.
                    time.sleep(0.05)
                    checkpoints = self.quiesce(turn, clients)
                    if failure:
                        raise WorkloadError("injected delayed checkpoint failure")
                    self.relaunch(turn, checkpoints)
                except BaseException as exc:
                    self.error = exc

            self.thread = threading.Thread(target=delayed)
            self.thread.start()

        def signal_turn_complete(self, _turn: str) -> None:
            return None

        def raise_if_failed(self) -> None:
            if self.error is not None:
                raise self.error

        def wait(self) -> None:
            assert self.thread is not None
            self.thread.join(timeout=2)
            assert not self.thread.is_alive()
            self.raise_if_failed()

        def stop(self) -> None:
            if self.thread is not None:
                self.thread.join(timeout=2)
                assert not self.thread.is_alive()
            self.raise_if_failed()

    monkeypatch.setattr(workload_module, "EventProducer", DelayedCheckpointProducer)
    if failure:
        with pytest.raises(WorkloadError, match="injected delayed checkpoint failure"):
            run_workload(
                farm,
                scenario,
                plan,
                recorder=RecordingTransport(recorder),
                require_up=False,
            )
        return

    receipt = run_workload(
        farm, scenario, plan, recorder=RecordingTransport(recorder), require_up=False
    )
    assert receipt["status"] == "COMPLETE"
    assert [item["client"] for item in receipt["clients"]] == ["C1", "C2"]
    assert sum(command.phase == "event.checkpoint" for command in recorder.commands) == 2


def test_checkpointed_client_transition_refuses_ambiguous_multiple_turns(
    tmp_path: Path,
) -> None:
    farm = load_farm_spec(farm_fixture.example_farm_path())
    farm.data["hub"]["results_root"] = str(tmp_path)
    scenario = load_scenario_spec(
        INTEGRATION / "scenarios" / "S60-06-c2-down.json", farm
    )
    plan = farmtest.build_plan(farm, scenario, run_id="checkpoint-multiple-turns")
    scenario.data["workload"]["turns"] = ["A", "B"]

    with pytest.raises(WorkloadError, match="one unambiguous workload turn"):
        run_workload(
            farm,
            scenario,
            plan,
            recorder=RecordingTransport(WorkloadRecorder()),
            require_up=False,
        )


def test_checkpointed_client_transition_missing_trigger_times_out(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    farm = load_farm_spec(farm_fixture.example_farm_path())
    farm.data["hub"]["results_root"] = str(tmp_path)
    scenario = load_scenario_spec(
        INTEGRATION / "scenarios" / "S60-06-c2-down.json", farm
    )
    scenario.data["timeouts"]["turn_s"] = 1
    plan = farmtest.build_plan(farm, scenario, run_id="checkpoint-missing-trigger")

    class NeverTriggeredProducer:
        def __init__(self, *_args, **_kwargs) -> None:
            return None

        def start(self) -> None:
            return None

        def signal_turn_start(self, _turn: str) -> None:
            return None

        def signal_turn_complete(self, _turn: str) -> None:
            return None

        def raise_if_failed(self) -> None:
            return None

        def wait(self) -> None:
            return None

        def stop(self) -> None:
            return None

    monkeypatch.setattr(workload_module, "EventProducer", NeverTriggeredProducer)
    with pytest.raises(
        WorkloadError, match="checkpointed C transition did not relaunch"
    ):
        run_workload(
            farm,
            scenario,
            plan,
            recorder=RecordingTransport(WorkloadRecorder()),
            require_up=False,
        )
