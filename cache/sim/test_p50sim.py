from __future__ import annotations

import hashlib
import json
import os
import subprocess
from pathlib import Path


HERE = Path(__file__).resolve().parent


def write_canonical(path: Path, value: object) -> None:
    path.write_bytes(json.dumps(value, sort_keys=True, separators=(",", ":")).encode())


def read_jsonl(path: Path) -> list[dict[str, object]]:
    return [json.loads(line) for line in path.read_bytes().splitlines()]


def write_role_traces(artifacts: Path, rows: list[dict[str, object]]) -> None:
    for actor in ("C", "F"):
        (artifacts / f"{actor.lower()}-trace.jsonl").write_text(
            "".join(json.dumps(row, sort_keys=True, separators=(",", ":")) + "\n"
                    for row in rows if row["actor"] == actor))


def invoke_explicit_runner(artifacts: Path, cell: str, output: Path) -> subprocess.CompletedProcess[str]:
    root = HERE.parents[1]
    command = [str(root / "farmharness/s7_warm_replay.py"), "--cell", cell, "--out", str(output)]
    if cell.endswith("/warm"):
        command += [
            "--prewarm-input", str(artifacts / "preprocessed.ii"),
            "--measured-input", str(artifacts / "preprocessed.ii"),
            "--prewarm-c-trace", str(artifacts / "prewarm-c-action-trace.jsonl"),
            "--prewarm-f-trace", str(artifacts / "prewarm-f-action-trace.jsonl"),
            "--measured-c-trace", str(artifacts / "measured-c-action-trace.jsonl"),
            "--measured-f-trace", str(artifacts / "measured-f-action-trace.jsonl"),
        ]
    else:
        command += [
            "--input", str(artifacts / "input.bin"),
            "--c-trace", str(artifacts / "c-trace.jsonl"),
            "--f-trace", str(artifacts / "f-trace.jsonl"),
        ]
    return subprocess.run(command, text=True, capture_output=True, check=False)


def scenario_tree(tmp_path: Path) -> Path:
    tmp_path.mkdir(parents=True)
    payload = b"an authenticated production Protocol-50 input\n"
    (tmp_path / "input.bin").write_bytes(payload)
    probe = tmp_path / "probe"
    probe.mkdir()
    binary = HERE / ".p50sim.bin"
    subprocess.run(
        [str(binary), "--input", str(tmp_path / "input.bin"), "--actions", str(probe / "actions.jsonl"), "--summary", str(probe / "summary.json")],
        check=True,
    )
    action_bytes = (probe / "actions.jsonl").read_bytes()
    (tmp_path / "action_trace.jsonl").write_bytes(action_bytes)
    action_rows = [json.loads(line) for line in action_bytes.splitlines()]
    route = {
        "actions": [row["action"] for row in action_rows],
        "origin": "live",
        "schema": "icecream-s7-live-route-trace-v1",
        "trace": [
            {"action": row["action"], "sequence": sequence}
            for sequence, row in enumerate(action_rows, 1)
        ],
    }
    write_canonical(tmp_path / "route_trace.json", route)
    scenario = {
        "cell": "fmt/ZSTD_TU/cold",
        "input": "input.bin",
        "input_sha256": hashlib.sha256(payload).hexdigest(),
        "action_trace": "action_trace.jsonl",
        "action_trace_sha256": hashlib.sha256(action_bytes).hexdigest(),
        "route_trace": "route_trace.json",
        "route_trace_sha256": hashlib.sha256((tmp_path / "route_trace.json").read_bytes()).hexdigest(),
        "identity": {
            "c_store_guid": action_rows[0]["c_store_guid"],
            "f_store_guid": action_rows[0]["f_store_guid"],
            "history_nonce": next(row["history_nonce"] for row in action_rows
                                   if row["action"] in {"TX_BEGIN", "ACTIVE_REPLAYED"}),
        },
        "schema": "icecream-s7-p50sim-scenario-v1",
    }
    write_canonical(tmp_path / "scenario.json", scenario)
    return tmp_path


def invoke(artifacts: Path, output: Path) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [str(HERE / "p50sim"), "--s7-cell", "fmt/ZSTD_TU/cold", "--s7-artifacts", str(artifacts), "--s7-output", str(output)],
        text=True,
        capture_output=True,
        check=False,
    )


def warm_scenario_tree(tmp_path: Path) -> Path:
    tmp_path.mkdir(parents=True)
    payload = b"an authenticated production Protocol-50 warm input\n"
    (tmp_path / "preprocessed.ii").write_bytes(payload)
    probe = tmp_path / "probe"
    probe.mkdir()
    binary = HERE / ".p50sim.bin"
    subprocess.run([
        str(binary), "--prewarm-input", str(tmp_path / "preprocessed.ii"),
        "--measured-input", str(tmp_path / "preprocessed.ii"),
        "--actions", str(probe / "actions.jsonl"),
        "--prewarm-actions", str(probe / "prewarm-actions.jsonl"),
        "--measured-actions", str(probe / "measured-actions.jsonl"),
        "--summary", str(probe / "summary.json"),
        "--c-store-guid", "4eed5e3d7e595ecd4ba9ce014e951583",
        "--f-store-guid", "d3ef72d22168e1fb907a38f80315d178",
        "--history-nonce", "1",
    ], check=True)
    action_bytes = (probe / "actions.jsonl").read_bytes()
    action_rows = [json.loads(line) for line in action_bytes.splitlines()]
    for name in ("actions.jsonl", "prewarm-actions.jsonl", "measured-actions.jsonl"):
        (tmp_path / name).write_bytes((probe / name).read_bytes())
    all_rows = action_rows
    prewarm_rows = read_jsonl(tmp_path / "prewarm-actions.jsonl")
    measured_rows = read_jsonl(tmp_path / "measured-actions.jsonl")
    route = {
        "actions": [row["action"] for row in all_rows],
        "origin": "live",
        "schema": "icecream-s7-live-route-trace-v1",
        "trace": [
            {"action": row["action"], "sequence": sequence}
            for sequence, row in enumerate(all_rows, 1)
        ],
    }
    write_canonical(tmp_path / "route_trace.json", route)
    identity = {
        "c_store_guid": "4eed5e3d7e595ecd4ba9ce014e951583",
        "f_store_guid": "d3ef72d22168e1fb907a38f80315d178",
        "history_nonce": 1,
        "prewarm_tu_seq": 0,
        "measured_tu_seq": 1,
    }
    scenario = {
        "cell": "fmt/ZSTD_TU/warm",
        "input": "preprocessed.ii",
        "input_sha256": hashlib.sha256(payload).hexdigest(),
        "prewarm_input": "preprocessed.ii",
        "prewarm_input_sha256": hashlib.sha256(payload).hexdigest(),
        "action_trace": "actions.jsonl",
        "action_trace_sha256": hashlib.sha256(action_bytes).hexdigest(),
        "prewarm_action_trace": "prewarm-actions.jsonl",
        "measured_action_trace": "measured-actions.jsonl",
        "prewarm_c_action_trace": "prewarm-c-action-trace.jsonl",
        "prewarm_f_action_trace": "prewarm-f-action-trace.jsonl",
        "measured_c_action_trace": "measured-c-action-trace.jsonl",
        "measured_f_action_trace": "measured-f-action-trace.jsonl",
        "stage_ledger": "stage-ledger.jsonl",
        "route_trace": "route_trace.json",
        "route_trace_sha256": hashlib.sha256((tmp_path / "route_trace.json").read_bytes()).hexdigest(),
        "identity": identity,
        "schema": "icecream-s7-p50sim-scenario-v1",
        "regime": "warm",
    }
    # Expected role and stage ledgers are generated from the product trace,
    # then authenticated as scenario inputs for the replay check.
    for name, rows in (
        ("prewarm-c-action-trace.jsonl", [row for row in prewarm_rows if row["actor"] == "C"]),
        ("prewarm-f-action-trace.jsonl", [row for row in prewarm_rows if row["actor"] == "F"]),
        ("measured-c-action-trace.jsonl", [row for row in measured_rows if row["actor"] == "C"]),
        ("measured-f-action-trace.jsonl", [row for row in measured_rows if row["actor"] == "F"]),
    ):
        (tmp_path / name).write_text("".join(json.dumps(row, sort_keys=True, separators=(",", ":")) + "\n" for row in rows))
    ledger = [{
        "sequence": sequence, "action": row["action"], "actor": row["actor"],
        "stage_bytes": row["stage_bytes"],
        "transaction_digest": row["transaction_digest"],
        "raw_digest": row["raw_digest"],
    } for sequence, row in enumerate(all_rows, 1)]
    (tmp_path / "stage-ledger.jsonl").write_text(
        "".join(json.dumps(row, sort_keys=True, separators=(",", ":")) + "\n" for row in ledger))
    write_canonical(tmp_path / "scenario.json", scenario)
    return tmp_path


def test_route_profile_commits_prewarm_before_preparing_measured(tmp_path: Path) -> None:
    payload = b"route-history warm ordering input\n"
    input_path = tmp_path / "input.ii"
    input_path.write_bytes(payload)
    actions = tmp_path / "actions.jsonl"
    prewarm_actions = tmp_path / "prewarm-actions.jsonl"
    measured_actions = tmp_path / "measured-actions.jsonl"
    summary = tmp_path / "summary.json"
    environment = os.environ.copy()
    environment["ICECC_P50_PROFILE"] = "ZSTD_ROUTE"
    result = subprocess.run([
        str(HERE / ".p50sim.bin"),
        "--prewarm-input", str(input_path),
        "--measured-input", str(input_path),
        "--actions", str(actions),
        "--prewarm-actions", str(prewarm_actions),
        "--measured-actions", str(measured_actions),
        "--summary", str(summary),
    ], env=environment, text=True, capture_output=True, check=False)
    assert result.returncode == 0, result.stderr
    begins = [row for row in read_jsonl(actions) if row["action"] == "TX_BEGIN"]
    assert [row["tu_seq"] for row in begins] == [0, 0, 1, 1]


def test_real_endpoint_writes_authenticated_trace(tmp_path: Path) -> None:
    artifacts = scenario_tree(tmp_path / "artifacts")
    result = invoke(artifacts, tmp_path / "output")
    assert result.returncode == 0, result.stderr
    summary = json.loads((tmp_path / "output" / "execution.json").read_text())
    assert summary["schema"] == "icecream-p50sim-execution-v1"
    assert summary["client_status"] == "Committed"
    assert summary["server_status"] == "Completed"
    assert int(summary["action_records"]) > 0
    c_rows = read_jsonl(tmp_path / "output" / "c-action-trace.jsonl")
    f_rows = read_jsonl(tmp_path / "output" / "f-action-trace.jsonl")
    produced_rows = read_jsonl(tmp_path / "output" / "actions.jsonl")
    assert c_rows and f_rows
    assert all(row["actor"] == "C" for row in c_rows)
    assert all(row["actor"] == "F" for row in f_rows)
    assert len(c_rows) + len(f_rows) == len(produced_rows)


def test_input_digest_mismatch_rejects_execution(tmp_path: Path) -> None:
    artifacts = scenario_tree(tmp_path / "artifacts")
    (artifacts / "input.bin").write_bytes(b"tampered\n")
    result = invoke(artifacts, tmp_path / "output")
    assert result.returncode != 0
    assert "scenario input digest mismatch" in result.stderr


def test_action_trace_mutation_rejects_replay(tmp_path: Path) -> None:
    artifacts = scenario_tree(tmp_path / "artifacts")
    action_path = artifacts / "action_trace.jsonl"
    rows = [json.loads(line) for line in action_path.read_bytes().splitlines()]
    rows[0]["actor"] = "C" if rows[0]["actor"] == "F" else "F"
    mutated = b"\n".join(json.dumps(row, separators=(",", ":")).encode() for row in rows) + b"\n"
    action_path.write_bytes(mutated)
    scenario = json.loads((artifacts / "scenario.json").read_bytes())
    scenario["action_trace_sha256"] = hashlib.sha256(mutated).hexdigest()
    write_canonical(artifacts / "scenario.json", scenario)
    result = invoke(artifacts, tmp_path / "output")
    assert result.returncode != 0
    assert "production replay differs from canonical live action trace" in result.stderr


def test_manifest_identity_mismatch_rejects_execution(tmp_path: Path) -> None:
    artifacts = scenario_tree(tmp_path / "artifacts")
    scenario = json.loads((artifacts / "scenario.json").read_bytes())
    identity = dict(scenario["identity"])
    identity["history_nonce"] += 1
    scenario["identity"] = identity
    write_canonical(artifacts / "scenario.json", scenario)
    result = invoke(artifacts, tmp_path / "output")
    assert result.returncode != 0
    assert "scenario identity differs from live action snapshot" in result.stderr


def test_warm_replay_retains_tu_traces_identity_and_stage_ledger(tmp_path: Path) -> None:
    artifacts = warm_scenario_tree(tmp_path / "artifacts")
    result = subprocess.run(
        [str(HERE / "p50sim"), "--s7-cell", "fmt/ZSTD_TU/warm",
         "--s7-artifacts", str(artifacts), "--s7-output", str(tmp_path / "output")],
        text=True, capture_output=True, check=False)
    assert result.returncode == 0, result.stderr
    assert len(read_jsonl(tmp_path / "output" / "prewarm-actions.jsonl")) == 11
    measured = read_jsonl(tmp_path / "output" / "measured-actions.jsonl")
    assert len(measured) == 10
    assert {row["tu_seq"] for row in measured if row["action"] == "TX_BEGIN"} == {1}
    identity = json.loads((tmp_path / "output" / "identities.json").read_text())
    assert identity["prewarm_tu_seq"] == 0
    assert identity["measured_tu_seq"] == 1
    assert len(read_jsonl(tmp_path / "output" / "stage-ledger.jsonl")) == 21


def test_warm_deletion_control_reddens_measured_trace(tmp_path: Path) -> None:
    artifacts = warm_scenario_tree(tmp_path / "artifacts")
    measured = artifacts / "measured-actions.jsonl"
    rows = [json.loads(line) for line in measured.read_bytes().splitlines()]
    rows = [row for row in rows if row["action"] != "NEED_RECORDED"]
    measured.write_text("".join(json.dumps(row, sort_keys=True, separators=(",", ":")) + "\n" for row in rows))
    result = subprocess.run(
        [str(HERE / "p50sim"), "--s7-cell", "fmt/ZSTD_TU/warm",
         "--s7-artifacts", str(artifacts), "--s7-output", str(tmp_path / "output")],
        text=True, capture_output=True, check=False)
    assert result.returncode != 0
    assert "measured TU1 action trace differs from product trace" in result.stderr


def test_explicit_runner_supports_fmt_and_rocksdb_cold_warm(tmp_path: Path) -> None:
    cold = scenario_tree(tmp_path / "cold")
    write_role_traces(cold, read_jsonl(cold / "action_trace.jsonl"))
    warm = warm_scenario_tree(tmp_path / "warm")
    for cell, artifacts in (
        ("fmt/ZSTD_TU/cold", cold),
        ("fmt/ZSTD_TU/warm", warm),
        ("RocksDB/ZSTD_TU/cold", cold),
        ("RocksDB/ZSTD_TU/warm", warm),
    ):
        result = invoke_explicit_runner(artifacts, cell, tmp_path / cell.replace("/", "-"))
        assert result.returncode == 0, f"{cell}: {result.stderr}"
        manifest = json.loads((tmp_path / cell.replace("/", "-") / "manifest.json").read_text())
        assert manifest["cell"] == cell
        assert manifest["schema"] == f"icecream-s7-{cell.split('/')[0].lower()}-zstd-tu-{cell.split('/')[-1]}-conformance-v2"
        assert manifest["deletion_control"]["returncode"] != 0
