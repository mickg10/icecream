from __future__ import annotations

import hashlib
import json
import subprocess
from pathlib import Path

import pytest

from s7_grz_results import ResultsError, build, canonical
from s8_retained_s7_package import build as build_package


def _git_repo(root: Path) -> tuple[Path, str]:
    repo = root / "source"
    repo.mkdir()
    for args in (("git", "init", "-q", str(repo)),
                 ("git", "-C", str(repo), "config", "user.email", "fixture@example.invalid"),
                 ("git", "-C", str(repo), "config", "user.name", "fixture")):
        subprocess.run(args, check=True)
    source = repo / "format.cc"
    source.write_text("int value;\n")
    subprocess.run(["git", "-C", str(repo), "add", "format.cc"], check=True)
    subprocess.run(["git", "-C", str(repo), "commit", "-q", "-m", "fixture"], check=True)
    commit = subprocess.check_output(["git", "-C", str(repo), "rev-parse", "HEAD"], text=True).strip()
    return repo, commit


def _write(path: Path, value: object) -> bytes:
    raw = canonical(value) + b"\n"
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(raw)
    return raw


def _fixture(root: Path, regime: str = "cold") -> dict[str, object]:
    repo, commit = _git_repo(root)
    experiment = root / "experiment"
    replay = experiment / "exact-replay"
    runtime = experiment / "runtime" / "run"
    build_root = root / "build"
    replay.mkdir(parents=True); runtime.mkdir(parents=True); build_root.mkdir()
    source_file = repo / "format.cc"
    input_raw = b"preprocessed input\n"
    (replay / "measured.ii").write_bytes(input_raw)
    input_sha = hashlib.sha256(input_raw).hexdigest()
    ident = {"c_store_guid": "1" * 32, "f_store_guid": "2" * 32,
             "history_nonce": 1, "measured_tu_seq": 1 if regime == "warm" else 0}
    begin = {"action": "TX_BEGIN", "c_store_guid": ident["c_store_guid"],
             "f_store_guid": ident["f_store_guid"], "history_nonce": 1,
             "tu_seq": ident["measured_tu_seq"], "transaction_digest": "3" * 32,
             "raw_digest": "4" * 32, "stage_bytes": 123}
    c_raw = _write(replay / "measured-c-action-trace.jsonl", {**begin, "actor": "C"})
    f_raw = _write(replay / "measured-f-action-trace.jsonl", {**begin, "actor": "F"})
    stage_rows = [{"action": "TX_BEGIN", "actor": actor, "stage_bytes": 123,
                   "transaction_digest": "3" * 32, "raw_digest": "4" * 32}
                  for actor in ("C", "F")]
    if regime == "warm":
        stage_rows.extend({"action": "TX_BEGIN", "actor": actor, "stage_bytes": 99,
                           "transaction_digest": "5" * 32, "raw_digest": "6" * 32}
                          for actor in ("C", "F"))
    stage_raw = b"".join(canonical(row) + b"\n" for row in stage_rows)
    (replay / "stage-ledger.jsonl").write_bytes(stage_raw)
    identity_raw = _write(replay / "identities.json", ident)
    manifest = {"schema": f"icecream-s7-fmt-grz-residual-{regime}-conformance-v2",
                "cell": f"fmt/GRZ_RESIDUAL/{regime}", "status": "PASS",
                "input_sha256": input_sha, "identity": ident,
                "deletion_control": {"status": "PASS", "returncode": 1},
                "mutation_control": {"status": "PASS", "returncode": 1},
                "action_count": 11 if regime == "cold" else 21,
                "measured_action_count": 11 if regime == "cold" else 10,
                "prewarm_action_count": 0 if regime == "cold" else 11}
    if regime == "warm":
        manifest["prewarm_input_sha256"] = hashlib.sha256(b"prewarm input\n").hexdigest()
        manifest["identity"]["prewarm_tu_seq"] = 0
    manifest_raw = _write(replay / "manifest.json", manifest)
    controls = replay / "controls"
    _write(controls / "deletion-measured-trace.log", {"status": "PASS"})
    _write(controls / "mutation-measured-input.log", {"status": "PASS"})
    (runtime / "client-compile-measured.log").write_text("</wait for cs: 456ms>\nICECC: got 78 bytes (12%)\n")
    for name in ("local.o", "remote.o"):
        (runtime / name).write_bytes(b"object bytes")
    binaries = []
    for name in ("client", "daemon"):
        path = build_root / name
        path.write_bytes((name + " binary").encode())
        binaries.append(f"{name}={path}")
    if regime == "warm":
        prewarm_raw = b"prewarm input\n"
        (replay / "prewarm.ii").write_bytes(prewarm_raw)
        pre_begin = {**begin, "tu_seq": 0, "transaction_digest": "5" * 32,
                     "raw_digest": "6" * 32, "stage_bytes": 99}
        _write(replay / "prewarm-c-action-trace.jsonl", {**pre_begin, "actor": "C"})
        _write(replay / "prewarm-f-action-trace.jsonl", {**pre_begin, "actor": "F"})
    return {"experiment": experiment, "runtime": runtime, "replay": replay,
            "repo": repo, "commit": commit, "source": source_file,
            "build": build_root, "binaries": binaries}


def _run(root: Path, regime: str = "cold") -> Path:
    f = _fixture(root, regime)
    return build(experiment=f["experiment"], runtime=f["runtime"], replay=f["replay"],
                 corpus="fmt", regime=regime, source_repository=f["repo"],
                 source_commit=f["commit"], source_file=f["source"], build_root=f["build"],
                 local_object=f["runtime"] / "local.o", remote_object=f["runtime"] / "remote.o",
                 binaries=f["binaries"], out=root / "out",
                 **({"prewarm_input": f["replay"] / "prewarm.ii",
                     "prewarm_c_trace": f["replay"] / "prewarm-c-action-trace.jsonl",
                     "prewarm_f_trace": f["replay"] / "prewarm-f-action-trace.jsonl"}
                    if regime == "warm" else {}))


@pytest.mark.parametrize("regime", ("cold", "warm"))
def test_canonical_row_binds_real_layout_and_warm_prewarm(tmp_path: Path, regime: str) -> None:
    output = _run(tmp_path, regime)
    row = json.loads(output.read_bytes())
    assert row["schema"] == "icecream-s7-live-cell-v1"
    assert row["cell"] == f"fmt/GRZ_RESIDUAL/{regime}"
    assert row["measured"]["f_to_c_wire_bytes"] == 78
    assert row["measured"]["source_transfer_bytes"] == 123
    assert row["measured"]["remote_bytes"] == len(b"object bytes")
    assert row["measured"]["byte_identical"] is True
    assert row["evidence"]["prewarm_bound"] is (regime == "warm")
    assert len(output.read_text().splitlines()) == 1


def test_object_mutation_is_rejected_without_output(tmp_path: Path) -> None:
    f = _fixture(tmp_path)
    remote = f["runtime"] / "remote.o"
    remote.write_bytes(b"mutated")
    with pytest.raises(ResultsError, match="not_byte_identical"):
        build(experiment=f["experiment"], runtime=f["runtime"], replay=f["replay"],
              corpus="fmt", regime="cold", source_repository=f["repo"],
              source_commit=f["commit"], source_file=f["source"], build_root=f["build"],
              local_object=f["runtime"] / "local.o", remote_object=remote,
              binaries=f["binaries"], out=tmp_path / "out")
    assert not (tmp_path / "out").exists()


def test_warm_without_prewarm_is_rejected(tmp_path: Path) -> None:
    f = _fixture(tmp_path, "warm")
    with pytest.raises(ResultsError, match="prewarm_evidence_required"):
        build(experiment=f["experiment"], runtime=f["runtime"], replay=f["replay"],
              corpus="fmt", regime="warm", source_repository=f["repo"],
              source_commit=f["commit"], source_file=f["source"], build_root=f["build"],
              local_object=f["runtime"] / "local.o", remote_object=f["runtime"] / "remote.o",
              binaries=f["binaries"], out=tmp_path / "out")


def test_duplicate_stage_transaction_match_is_rejected(tmp_path: Path) -> None:
    f = _fixture(tmp_path, "warm")
    stage = f["replay"] / "stage-ledger.jsonl"
    original = stage.read_bytes()
    stage.write_bytes(original.rstrip(b"\n") + b"\n" + original.splitlines()[0] + b"\n")
    with pytest.raises(ResultsError, match="stage_ledger:measured_c_match_count=2"):
        build(experiment=f["experiment"], runtime=f["runtime"], replay=f["replay"],
              corpus="fmt", regime="warm", source_repository=f["repo"],
              source_commit=f["commit"], source_file=f["source"], build_root=f["build"],
              local_object=f["runtime"] / "local.o", remote_object=f["runtime"] / "remote.o",
              binaries=f["binaries"], out=tmp_path / "out",
              prewarm_input=f["replay"] / "prewarm.ii",
              prewarm_c_trace=f["replay"] / "prewarm-c-action-trace.jsonl",
              prewarm_f_trace=f["replay"] / "prewarm-f-action-trace.jsonl")


def test_normalized_output_is_consumable_by_retained_package_builder(tmp_path: Path) -> None:
    f = _fixture(tmp_path)
    normalized_root = tmp_path / "normalized"
    normalized_root.mkdir()
    normalized = _run(normalized_root)
    package = build_package(normalized.parent, f["replay"], f["repo"], tmp_path / "package")
    assert package.exists()
    assert (tmp_path / "normalized" / "out" / "runtime" / "run" /
            "client-compile-measured.log").exists()
