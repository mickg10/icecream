from __future__ import annotations

import hashlib
import json
import subprocess
from pathlib import Path

import pytest

from s8_live_metric_producer import produce
from s8_retained_s7_package import PackageError, build


def _canonical(value: object) -> bytes:
    return json.dumps(value, sort_keys=True, separators=(",", ":")).encode()


def _git_repo(root: Path) -> tuple[Path, str]:
    repository = root / "source-repository"
    repository.mkdir()
    subprocess.run(["git", "init", "-q", str(repository)], check=True)
    subprocess.run(["git", "-C", str(repository), "config", "user.email", "fixture@example.invalid"], check=True)
    subprocess.run(["git", "-C", str(repository), "config", "user.name", "fixture"], check=True)
    (repository / "source.cc").write_text("int value;\n")
    subprocess.run(["git", "-C", str(repository), "add", "source.cc"], check=True)
    subprocess.run(["git", "-C", str(repository), "commit", "-q", "-m", "fixture"], check=True)
    commit = subprocess.run(
        ["git", "-C", str(repository), "rev-parse", "HEAD"], check=True,
        capture_output=True, text=True,
    ).stdout.strip()
    return repository, commit


def _fixture(root: Path) -> tuple[Path, Path, Path]:
    repository, commit = _git_repo(root)
    package = root / "s7"
    replay = package / "replay"
    runtime = package / "runtime" / "run"
    replay.mkdir(parents=True)
    runtime.mkdir(parents=True)
    input_raw = b"preprocessed translation unit\n" * 4
    input_sha = hashlib.sha256(input_raw).hexdigest()
    identity = {
        "c_store_guid": "1" * 32, "f_store_guid": "2" * 32,
        "history_nonce": 1, "measured_tu_seq": 0,
    }
    begin = {
        "action": "TX_BEGIN", "c_store_guid": identity["c_store_guid"],
        "f_store_guid": identity["f_store_guid"], "history_nonce": 1,
        "tu_seq": 0, "transaction_digest": "3" * 32,
        "raw_digest": "4" * 32, "stage_bytes": 123,
    }
    c_trace = _canonical({**begin, "actor": "C"}) + b"\n"
    f_trace = _canonical({**begin, "actor": "F"}) + b"\n"
    stage = _canonical({"action": "TX_BEGIN", "actor": "C", "stage_bytes": 123}) + b"\n"
    manifest = {
        "schema": "icecream-s7-fmt-zstd-tu-cold-conformance-v2",
        "cell": "fmt/ZSTD_TU/cold", "status": "PASS",
        "input_sha256": input_sha, "identity": identity,
        "deletion_control": {"status": "PASS", "returncode": 1},
    }
    retained = {
        "manifest.json": _canonical(manifest) + b"\n",
        "identities.json": _canonical(identity) + b"\n",
        "stage-ledger.jsonl": stage,
        "measured-c-action-trace.jsonl": c_trace,
        "measured-f-action-trace.jsonl": f_trace,
        "measured.ii": input_raw,
    }
    for name, raw in retained.items():
        (replay / name).write_bytes(raw)
    (runtime / "client-compile-measured.log").write_text(
        "</wait for cs: 456ms>\nICECC: got 78 bytes (12%)\n"
    )
    conformance = {
        "status": "PASS",
        "manifest_sha256": hashlib.sha256(retained["manifest.json"]).hexdigest(),
        "identity_sha256": hashlib.sha256(retained["identities.json"]).hexdigest(),
        "stage_ledger_sha256": hashlib.sha256(stage).hexdigest(),
    }
    result = {
        "schema": "icecream-s7-live-cell-v1", "cell": "fmt/ZSTD_TU/cold",
        "corpus": "fmt", "profile": "ZSTD_TU", "regime": "cold",
        "status": "PASS", "live_status": "PASS", "acceptance_status": "PASS",
        "conformance_status": "PASS", "conformance": conformance,
        "live_source_commit": commit, "run_id": "fixture-run", "runtime": "runtime/run",
        "binary_sha256": {"client": "5" * 64, "daemon": "6" * 64},
        "measured": {
            "input_sha256": input_sha, "tu_seq": 0,
            "remote_bytes": 999999, "remote_compile_ms": 999999,
        },
    }
    (package / "results.jsonl").write_bytes(_canonical(result) + b"\n")
    return package, replay, repository


def test_builds_authentic_package_and_live_curve(tmp_path: Path) -> None:
    package, replay, repository = _fixture(tmp_path)
    out = tmp_path / "out"
    manifest = build(package, replay, repository, out)
    assert json.loads(manifest.read_bytes())["status"] == "PASS"
    timing = json.loads((out / "timing.jsonl").read_bytes())
    assert timing["c_to_f_bytes"] == 123
    assert timing["f_to_c_bytes"] == 78
    assert timing["channel_bytes"] == 201
    assert timing["elapsed_ns"] == 456_000_000
    # Raw object size and the aggregate result timer are intentionally ignored.
    assert timing["channel_bytes"] != 999999
    assert timing["elapsed_ns"] != 999999 * 1_000_000
    producer = json.loads(produce(out, out / "live").read_bytes())
    assert producer["status"] == "PASS"
    assert producer["observations"] == 1


def test_trace_identity_mismatch_fails_before_output(tmp_path: Path) -> None:
    package, replay, repository = _fixture(tmp_path)
    rows = json.loads((replay / "measured-f-action-trace.jsonl").read_bytes())
    rows["stage_bytes"] += 1
    (replay / "measured-f-action-trace.jsonl").write_bytes(_canonical(rows) + b"\n")
    with pytest.raises(PackageError, match="tx_begin_identity_mismatch"):
        build(package, replay, repository, tmp_path / "out")
    assert not (tmp_path / "out").exists()


def test_ambiguous_client_observation_fails_before_output(tmp_path: Path) -> None:
    package, replay, repository = _fixture(tmp_path)
    log = package / "runtime" / "run" / "client-compile-measured.log"
    log.write_text(log.read_text() + "ICECC: got 79 bytes (13%)\n")
    with pytest.raises(PackageError, match="wire_or_wait_observation_ambiguous"):
        build(package, replay, repository, tmp_path / "out")
    assert not (tmp_path / "out").exists()


def test_live_producer_authenticates_raw_derivation_files(tmp_path: Path) -> None:
    package, replay, repository = _fixture(tmp_path)
    out = tmp_path / "out"
    build(package, replay, repository, out)
    (out / "client-compile-measured.log").write_bytes(
        (out / "client-compile-measured.log").read_bytes() + b"mutated\n"
    )
    result = json.loads(produce(out, out / "live").read_bytes())
    assert result["status"] == "HOLD"
    assert result["reason"] == "derivation_evidence.client_log:digest_mismatch"
