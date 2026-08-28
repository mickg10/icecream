from __future__ import annotations

import hashlib
import json
import subprocess
from pathlib import Path


HERE = Path(__file__).resolve().parent


def write_canonical(path: Path, value: object) -> None:
    path.write_bytes(json.dumps(value, sort_keys=True, separators=(",", ":")).encode())


def scenario_tree(tmp_path: Path) -> Path:
    tmp_path.mkdir(parents=True)
    payload = b"an authenticated production Protocol-50 input\n"
    route = {"origin": "live", "schema": "icecream-s7-live-route-trace-v1", "trace": [{"rel_seq": 0}]}
    (tmp_path / "input.bin").write_bytes(payload)
    write_canonical(tmp_path / "route_trace.json", route)
    scenario = {
        "cell": "fmt/ZSTD_TU/cold",
        "input": "input.bin",
        "input_sha256": hashlib.sha256(payload).hexdigest(),
        "route_trace": "route_trace.json",
        "route_trace_sha256": hashlib.sha256((tmp_path / "route_trace.json").read_bytes()).hexdigest(),
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


def test_input_digest_mismatch_rejects_execution(tmp_path: Path) -> None:
    artifacts = scenario_tree(tmp_path / "artifacts")
    (artifacts / "input.bin").write_bytes(b"tampered\n")
    result = invoke(artifacts, tmp_path / "output")
    assert result.returncode != 0
    assert "scenario input digest mismatch" in result.stderr
