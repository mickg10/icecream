from __future__ import annotations

import hashlib
import json
import subprocess
from pathlib import Path


HERE = Path(__file__).resolve().parent


def write_canonical(path: Path, value: object) -> None:
    path.write_bytes(json.dumps(value, sort_keys=True, separators=(",", ":")).encode())


def read_jsonl(path: Path) -> list[dict[str, object]]:
    return [json.loads(line) for line in path.read_bytes().splitlines()]


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
