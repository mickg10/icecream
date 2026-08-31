from __future__ import annotations

import hashlib
import json
import os
import subprocess
from pathlib import Path

import pytest

import s8_real_c1f1_live_runner as runner
from s8_first_triple_driver import _load_live_curve
from s8_predictive_live_normalizer import MANIFEST_SCHEMA, canonical_bytes


def _product(tmp_path: Path) -> tuple[Path, Path]:
    root = tmp_path / "product"
    for role in ("scheduler/icecc-scheduler", "daemon/iceccd", "client/icecc",
                 "cache/icecc-cache-service"):
        path = root / role
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(role.encode())
        path.chmod(0o755)
    script = root / "unittests/p50compilee2e-run.sh"
    script.parent.mkdir(parents=True)
    script.write_text("#!/bin/sh\nexit 0\n")
    script.chmod(0o755)
    subprocess.run(["git", "init", "-q", str(root)], check=True)
    subprocess.run(["git", "-C", str(root), "config", "user.email", "test@example.invalid"], check=True)
    subprocess.run(["git", "-C", str(root), "config", "user.name", "S8 test"], check=True)
    subprocess.run(["git", "-C", str(root), "add", "."], check=True)
    subprocess.run(["git", "-C", str(root), "commit", "-qm", "product"], check=True)
    return root, script


def _batch(tmp_path: Path, count: int = 100) -> Path:
    rows = []
    payload_root = tmp_path / "translation-units"
    payload_root.mkdir(exist_ok=True)
    for index in range(count):
        source = payload_root / f"tu-{index:03d}.ii"
        raw = f"int duck_{index}() {{ return {index}; }}\n".encode()
        source.write_bytes(raw)
        source_relative = f"translation-units/tu-{index:03d}.ii"
        rows.append({"tu_id": f"duck-tu-{index:03d}", "source": str(source),
                     "source_relative": source_relative,
                     "sha256": hashlib.sha256(raw).hexdigest(),
                     "predictive_input": {"ordinal": index, "path": str(source),
                                          "source_relative": source_relative,
                                          "sha256": hashlib.sha256(raw).hexdigest(),
                                          "bytes": len(raw)}})
    path = tmp_path / "batch.jsonl"
    path.write_text("".join(json.dumps(row, sort_keys=True) + "\n" for row in rows))
    return path


def _compile_batch(tmp_path: Path, count: int = 100) -> Path:
    source_root = tmp_path / "source"
    output_root = source_root / "build"
    payload_root = tmp_path / "payload"
    output_root.mkdir(parents=True)
    payload_root.mkdir()
    entries = []
    rows = []
    for ordinal in range(count):
        source = source_root / f"source-{ordinal:03d}.cc"
        source.write_text(f"int source_{ordinal}() {{ return {ordinal}; }}\n")
        payload = payload_root / f"source-{ordinal:03d}.ii"
        payload.write_text(f"int source_{ordinal}();\n")
        output = output_root / f"source-{ordinal:03d}.o"
        entries.append({"directory": str(output_root), "file": str(source),
                        "output": str(output),
                        "command": f"/usr/bin/c++ -DORDINAL={ordinal} -o {output} -c {source}"})
        payload_raw = payload.read_bytes()
        rows.append({"tu_id": f"duck-tu-{ordinal:03d}", "source": str(source),
                     "source_relative": f"source-{ordinal:03d}.ii",
                     "sha256": hashlib.sha256(source.read_bytes()).hexdigest(),
                     "predictive_input": {
                         "ordinal": ordinal, "path": str(payload),
                         "source_relative": f"source-{ordinal:03d}.ii",
                         "sha256": hashlib.sha256(payload_raw).hexdigest(),
                         "bytes": len(payload_raw)},
                     "compile_db": str(tmp_path / "compile_commands.json"),
                     "compile_db_sha256": "", "compile_source": str(source),
                     "compile_output": str(output)})
    compile_db = tmp_path / "compile_commands.json"
    compile_db.write_text(json.dumps(entries, sort_keys=True))
    db_sha = hashlib.sha256(compile_db.read_bytes()).hexdigest()
    for row in rows:
        row["compile_db_sha256"] = db_sha
    batch = tmp_path / "compile-batch.jsonl"
    batch.write_text("".join(json.dumps(row, sort_keys=True) + "\n" for row in rows))
    return batch


def _topology(tmp_path: Path, batch: Path) -> Path:
    rows = [json.loads(line) for line in batch.read_text().splitlines()]
    path = tmp_path / "topology.json"
    path.write_text(json.dumps({"schema": "icecream-s8-topology-assignment-v1",
                                "suite": runner.TOPOLOGY,
                                "assignments": [{"ordinal": i, "tu_id": row["tu_id"],
                                                 "relationship": 0, "f_slot": 0}
                                                for i, row in enumerate(rows)]}, sort_keys=True))
    return path


def _plan(tmp_path: Path, batch: Path, count: int = 100, *, profile: str = "ZSTD_TU",
          regime: str = "cold") -> Path:
    rows = [json.loads(line) for line in batch.read_text().splitlines()]
    source_manifest = tmp_path / "source-manifest.txt"
    source_manifest.write_text("".join(row["source"] + "\n" for row in rows))
    source_raw = source_manifest.read_bytes()
    inputs = [row["predictive_input"] for row in rows]
    value = {"schema": "icecream-s8-depth-run-plan-v1", "semantics": "s8-current-semantics-v1",
             "cell": {"corpus": "DuckDB", "profile": profile, "regime": regime},
             "split": "held_out_validation", "request": {"depth": count, "requested_curve_points": count},
             "source_manifest": {"path": str(source_manifest), "sha256": hashlib.sha256(source_raw).hexdigest(),
                                 "bytes": len(source_raw), "entries": count},
             "source_root": str(tmp_path),
             "inputs": inputs}
    path = tmp_path / "predictive-plan.json"
    path.write_text(json.dumps(value, sort_keys=True))
    return path


def test_batch_manifest_and_c1f1_topology_are_authenticated(tmp_path: Path) -> None:
    batch = _batch(tmp_path)
    rows = runner.load_batch_manifest(batch)
    assert len(rows) == 100
    topology = _topology(tmp_path, batch)
    digest = runner.load_topology(topology, rows)
    assert len(digest) == 64


@pytest.mark.parametrize(("depth", "count"), [("100", 100), ("200", 200), ("full", 7)])
def test_depth_binds_manifest_count(tmp_path: Path, depth: str, count: int) -> None:
    batch = _batch(tmp_path, count)
    rows = runner.load_batch_manifest(batch, runner.selected_count(depth, count if depth == "full" else None))
    digest = runner.payload_descriptor_digest(rows, "a" * 64)
    rows[0]["predictive_input"]["bytes"] += 1
    assert runner.payload_descriptor_digest(rows, "a" * 64) != digest
    with pytest.raises(runner.LiveRunnerError):
        runner.load_batch_manifest(batch, count + 1)


def test_input_digest_matches_source_manifest_plus_ordered_payload_contract(tmp_path: Path) -> None:
    batch = _batch(tmp_path, 3)
    rows = runner.load_batch_manifest(batch, 3)
    source_manifest_sha, _ = runner._sha(batch)
    expected = {"source_manifest_sha256": source_manifest_sha,
                "inputs": runner.payload_descriptors(rows)}
    expected_digest = hashlib.sha256(runner._canonical(expected)).hexdigest()
    assert runner.payload_descriptor_digest(rows, source_manifest_sha) == expected_digest
    assert runner.payload_descriptors(rows)[0]["source_relative"] == "translation-units/tu-000.ii"


def test_compile_binding_parses_each_database_command_once(tmp_path: Path,
                                                           monkeypatch: pytest.MonkeyPatch) -> None:
    batch = _compile_batch(tmp_path)
    calls = 0
    original = runner.compile_entry_output

    def counted(entry: object) -> Path | None:
        nonlocal calls
        calls += 1
        return original(entry)

    monkeypatch.setattr(runner, "compile_entry_output", counted)
    rows = runner.load_batch_manifest(batch)
    assert len(rows) == 100
    assert calls == 100


def test_predictive_plan_path_swap_is_rejected(tmp_path: Path) -> None:
    batch = _batch(tmp_path)
    plan_path = _plan(tmp_path, batch)
    _plan_value, plan_inputs, _sha = runner.load_predictive_plan(
        plan_path, corpus="DuckDB", profile="ZSTD_TU", regime="cold", depth="100")
    rows = runner.load_batch_manifest(batch)
    rows[0]["predictive_input"], rows[1]["predictive_input"] = (
        rows[1]["predictive_input"], rows[0]["predictive_input"])
    with pytest.raises(runner.LiveRunnerError, match="batch_input_mismatch:0"):
        runner.bind_batch_to_plan(rows, plan_inputs)


def test_predictive_plan_source_relative_mutation_is_rejected(tmp_path: Path) -> None:
    batch = _batch(tmp_path)
    plan_path = _plan(tmp_path, batch)
    value = json.loads(plan_path.read_text())
    value["inputs"][0]["source_relative"] = "other/tu-000.ii"
    plan_path.write_text(json.dumps(value, sort_keys=True))
    with pytest.raises(runner.LiveRunnerError, match="input_root_mismatch:0"):
        runner.load_predictive_plan(plan_path, corpus="DuckDB", profile="ZSTD_TU",
                                    regime="cold", depth="100")


def test_repeat_full_plan_binds_exact_first_plan_and_owns_full_2_descriptor(
        tmp_path: Path) -> None:
    batch = _batch(tmp_path, 3)
    first_path = _plan(tmp_path, batch, count=3)
    first_value = json.loads(first_path.read_text())
    first_value["request"]["depth"] = "full"
    first_value["request"]["requested_curve_points"] = "full"
    first_value["matrix_precondition"] = {"status": "PASS", "sha256": "a" * 64}
    first_value["scheduling"] = {
        "topology": "C1F1",
        "assignments": [{"ordinal": ordinal, "global_slot": 0}
                        for ordinal in range(3)],
    }
    first_path.write_text(json.dumps(first_value, sort_keys=True))
    first_plan, first_inputs, first_sha = runner.load_predictive_plan(
        first_path, corpus="DuckDB", profile="ZSTD_TU", regime="cold", depth="full")
    _first_sha, first_bytes = runner._sha(first_path)
    repeat_value = json.loads(json.dumps(first_value))
    repeat_value["request"]["depth"] = "repeat-full"
    repeat_value["request"]["requested_curve_points"] = "repeat-full"
    repeat_value["repeat_of"] = {"path": str(first_path.resolve()),
                                 "sha256": first_sha, "bytes": first_bytes}
    repeat_path = tmp_path / "repeat-predictive-plan.json"
    repeat_path.write_text(json.dumps(repeat_value, sort_keys=True))

    repeat_plan, repeat_inputs, repeat_sha = runner.load_repeat_predictive_plan(
        repeat_path, first_path=first_path, first_plan=first_plan,
        first_inputs=first_inputs, first_sha=first_sha, corpus="DuckDB",
        profile="ZSTD_TU", regime="cold")
    assert repeat_inputs == first_inputs
    first_descriptor = runner.normalizer.comparison_descriptor(
        first_sha, first_plan["scheduling"])
    repeat_descriptor = runner.normalizer.comparison_descriptor(
        repeat_sha, repeat_plan["scheduling"])
    assert first_descriptor["comparison_id"] != repeat_descriptor["comparison_id"]

    repeat_value["scheduling"]["assignments"][0]["global_slot"] = 1
    repeat_path.write_text(json.dumps(repeat_value, sort_keys=True))
    with pytest.raises(runner.LiveRunnerError, match="scheduling_mismatch"):
        runner.load_repeat_predictive_plan(
            repeat_path, first_path=first_path, first_plan=first_plan,
            first_inputs=first_inputs, first_sha=first_sha, corpus="DuckDB",
            profile="ZSTD_TU", regime="cold")


def _action_row(actor: str, seq: int, *, history: int = 9) -> dict[str, object]:
    digest = "a" * 32
    return {
        "action": "TX_BEGIN", "actor": actor, "c_store_guid": "b" * 32,
        "f_store_guid": "c" * 32, "previous_f_store_guid": "d" * 32,
        "session_serial": 1, "history_nonce": history, "rel_seq": seq,
        "tu_seq": seq, "transaction_digest": digest, "raw_digest": "e" * 32,
        "state_digest": "f" * 32, "content_digest": "1" * 32, "key64": 1,
        "need_keys": [], "remaining_need": 0, "duplicate": False, "stage_bytes": 17,
    }


def test_warm_prewarm_trace_requires_continuing_product_state(tmp_path: Path) -> None:
    prewarm_c = tmp_path / "pre-c.jsonl"
    prewarm_f = tmp_path / "pre-f.jsonl"
    measured_c = tmp_path / "measured-c.jsonl"
    measured_f = tmp_path / "measured-f.jsonl"
    prewarm_c.write_text(json.dumps(_action_row("C", 0)) + "\n")
    prewarm_f.write_text(json.dumps(_action_row("F", 0)) + "\n")
    measured_c.write_text(json.dumps(_action_row("C", 1)) + "\n")
    measured_f.write_text(json.dumps(_action_row("F", 1)) + "\n")
    prewarm = runner._action_stage_paths(prewarm_c, prewarm_f, 1)
    measured = runner._action_stage_paths(measured_c, measured_f, 1)
    runner._validate_warm_continuation(prewarm, measured, 1)
    measured[0]["history_nonce"] += 1
    with pytest.raises(runner.LiveRunnerError, match="does_not_continue_prewarm"):
        runner._validate_warm_continuation(prewarm, measured, 1)


def test_parallel_action_trace_binds_by_observed_service_not_global_arrival(
        tmp_path: Path) -> None:
    c_path = tmp_path / "c.jsonl"
    f_path = tmp_path / "f.jsonl"
    assignments = []
    by_relationship: dict[tuple[int, int], tuple[dict[str, object], dict[str, object]]] = {}
    for wave in range(2):
        for relationship in range(20):
            ordinal = wave * 20 + relationship
            assignments.append({"relationship": relationship, "f_slot": wave})
            # Transaction digests are allowed to collide across independent
            # relationships, as they do for repeated build occurrences.
            digest = f"{wave + 1:032x}"
            f_guid = f"{relationship + 1:032x}"
            c_row = _action_row("C", wave)
            f_row = _action_row("F", wave)
            for row in (c_row, f_row):
                row["f_store_guid"] = f_guid
                row["transaction_digest"] = digest
                row["raw_digest"] = f"{ordinal + 101:032x}"
            by_relationship[(relationship, wave)] = c_row, f_row
    # Relationships are intentionally observed in reverse order; only each
    # relationship's own source sequence remains ordered.
    observed = [(relationship, wave) for wave in range(2)
                for relationship in reversed(range(20))]
    c_path.write_text("".join(json.dumps(by_relationship[key][0]) + "\n" for key in observed))
    f_path.write_text("".join(json.dumps(by_relationship[key][1]) + "\n" for key in reversed(observed)))
    (tmp_path / "s8-f-service-map.tsv").write_text("".join(
        f"{relationship}\tp50-f-{relationship}\t{relationship + 1:032x}\n"
        for relationship in range(20)))
    stages = runner._action_stage_paths(
        c_path, f_path, 40, assignments, runner.PARALLEL_TOPOLOGY)
    assert [(row["planned_relationship"], row["planned_admission_lane"])
            for row in stages] == [(index % 20, index // 20) for index in range(40)]
    assert [row["tu_seq"] for row in stages] == [index // 20 for index in range(40)]
    assert stages[0]["observed_f_service_identity"] == "p50-f-0"


def test_action_trace_rejects_duplicate_within_one_relationship(tmp_path: Path) -> None:
    c_path = tmp_path / "c.jsonl"
    f_path = tmp_path / "f.jsonl"
    c_row = _action_row("C", 0)
    f_row = _action_row("F", 0)
    c_path.write_text(json.dumps(c_row) + "\n" + json.dumps(c_row) + "\n")
    f_path.write_text(json.dumps(f_row) + "\n" + json.dumps(f_row) + "\n")
    with pytest.raises(runner.LiveRunnerError, match="duplicate_transaction"):
        runner._action_stage_paths(c_path, f_path, 2)


def test_per_tu_profile_evidence_is_required(tmp_path: Path) -> None:
    log = tmp_path / "client-compile-full-1-0.log"
    log.write_text("P29 CACHE_SESSION\n")
    (tmp_path / "f.log").write_text("CACHE_SESSION\n")
    observation = [{"run": "full-1", "ordinal": 0, "planned_relationship": 0,
                    "observed_f_service_identity": "p50-f"}]
    runner._validate_product_log_evidence(tmp_path, observation, "P29")
    log.write_text("CACHE_SESSION\n")
    with pytest.raises(runner.LiveRunnerError, match="profile_evidence_missing"):
        runner._validate_product_log_evidence(tmp_path, observation, "P29")


def test_environment_preparation_requires_each_private_relationship(tmp_path: Path) -> None:
    work = tmp_path / "p50compilee2e.ready"
    work.mkdir()
    digest = "a" * 64
    stdout = ""
    for relationship in range(20):
        warmup, ready = f"env-warm-{relationship}", f"env-ready-{relationship}"
        (work / f"client-compile-{warmup}.log").write_text("has env: false\n")
        (work / f"client-compile-{ready}.log").write_text("has env: true\n")
        stdout += (f"S8_ENV_WARMUP relationship={relationship} warmup_label={warmup} "
                   f"ready_label={ready} warmup_has_env=false ready_has_env=true "
                   "preparation_measured=0 cache_state=pre_rotation\n")
    def frame(pid: int, c_guid: str, f_guid: str) -> str:
        return (f"READY v2 generation=1 attempt={pid} pid={pid} "
                f"C_STORE_GUID={c_guid} F_STORE_GUID={f_guid}\n")
    rotations = []
    for role, relationship in [("C", 0)] + [("F", index) for index in range(20)]:
        before_pid, after_pid = 1000 + relationship, 2000 + relationship
        before_c, after_c = f"{relationship + 1:032x}", f"{relationship + 101:032x}"
        before_f, after_f = f"{relationship + 201:032x}", f"{relationship + 301:032x}"
        path = work / ("ready-c.trace" if role == "C" else f"ready-f-{relationship}.trace")
        path.write_text(frame(before_pid, before_c, before_f) + frame(after_pid, after_c, after_f))
        rotations.append(
            f"S8_SIDECAR_ROTATION role={role} relationship={relationship} "
            f"before_pid={before_pid} after_pid={after_pid} "
            f"before_c_store_guid={before_c} after_c_store_guid={after_c} "
            f"before_f_store_guid={before_f} after_f_store_guid={after_f}\n")
    stdout += "".join(rotations)
    old_ready = "".join(f"RELOGIN p50-f-{relationship} cache=old cache_profiles=zstd_tu\n"
                         for relationship in range(20))
    new_ready = "".join(f"RELOGIN p50-f-{relationship} cache=new cache_profiles=zstd_tu\n"
                         for relationship in range(20))
    (work / "scheduler.log").write_bytes((old_ready + new_ready).encode())
    stdout += f"S8_ENV_POST_ROTATION_READY relationships=20 log_offset={len(old_ready.encode())}\n"
    stdout += ("S8_ENV_PREPARATION relationships=20 archive_sha256=" + digest +
               " archive_bytes=31 start_ns=10 end_ns=20 measured=0 cache_state=rotated\n"
               "S8_ENV_MEASURED_NO_INSTALL checked=1\n")
    preparation = runner._environment_preparation(stdout, work, runner.PARALLEL_TOPOLOGY)
    assert preparation["relationships"] == 20
    assert preparation["measured"] is False
    assert preparation["cache_state"] == "rotated"
    (work / "scheduler.log").write_bytes(old_ready.encode())
    with pytest.raises(runner.LiveRunnerError, match="post_rotation_ready_incomplete"):
        runner._environment_preparation(stdout, work, runner.PARALLEL_TOPOLOGY)
    missing = "\n".join(line for line in stdout.splitlines()
                       if not line.startswith("S8_ENV_WARMUP relationship=19")) + "\n"
    with pytest.raises(runner.LiveRunnerError, match="warmup_count_mismatch"):
        runner._environment_preparation(missing, work, runner.PARALLEL_TOPOLOGY)


def test_environment_preparation_accepts_real_single_scheduler_service_name(
        tmp_path: Path) -> None:
    work = tmp_path / "p50compilee2e.ready"
    work.mkdir()
    (work / "client-compile-env-warm.log").write_text("has env: false\n")
    (work / "client-compile-env-ready.log").write_text("has env: true\n")
    before_c, after_c = "1" * 32, "2" * 32
    before_f, after_f = "3" * 32, "4" * 32

    def frame(pid: int, c_guid: str, f_guid: str) -> str:
        return (f"READY v2 generation=1 attempt={pid} pid={pid} "
                f"C_STORE_GUID={c_guid} F_STORE_GUID={f_guid}\n")

    for role, before_pid, after_pid in (("F", 1001, 2001), ("C", 1002, 2002)):
        (work / f"ready-{role.lower()}.trace").write_text(
            frame(before_pid, before_c, before_f) + frame(after_pid, after_c, after_f))
    old_ready = b"RELOGIN p50-f(x86_64): [] cache=off\n"
    new_ready = (b"RELOGIN p50-f(x86_64): [env(x86_64), ] cache=127.0.0.1:54320 "
                 b"cache_wire=v1 cache_protocol=50 cache_profiles=p29 zstd_tu grz z3_long\n")
    (work / "scheduler.log").write_bytes(old_ready + new_ready)
    stdout = (
        "S8_ENV_WARMUP relationship=0 warmup_label=env-warm ready_label=env-ready "
        "warmup_has_env=false ready_has_env=true preparation_measured=0 "
        "cache_state=pre_rotation\n"
        f"S8_SIDECAR_ROTATION role=F relationship=0 before_pid=1001 after_pid=2001 "
        f"before_c_store_guid={before_c} after_c_store_guid={after_c} "
        f"before_f_store_guid={before_f} after_f_store_guid={after_f}\n"
        f"S8_SIDECAR_ROTATION role=C relationship=0 before_pid=1002 after_pid=2002 "
        f"before_c_store_guid={before_c} after_c_store_guid={after_c} "
        f"before_f_store_guid={before_f} after_f_store_guid={after_f}\n"
        f"S8_ENV_POST_ROTATION_READY relationships=1 log_offset={len(old_ready)}\n"
        f"S8_ENV_PREPARATION relationships=1 archive_sha256={'a' * 64} archive_bytes=31 "
        "start_ns=10 end_ns=20 measured=0 cache_state=rotated\n"
        "S8_ENV_MEASURED_NO_INSTALL checked=1\n"
    )
    preparation = runner._environment_preparation(stdout, work, runner.TOPOLOGY)
    assert preparation["post_rotation_ready"]["relationships"] == 1

    (work / "scheduler.log").write_bytes(
        old_ready + new_ready.replace(b"p50-f(x86_64)", b"p50-f-0(x86_64)"))
    with pytest.raises(runner.LiveRunnerError, match="post_rotation_ready_absent"):
        runner._environment_preparation(stdout, work, runner.TOPOLOGY)


def test_raw_environment_preparation_requires_each_relationship_readiness(
        tmp_path: Path) -> None:
    work = tmp_path / "p50compilee2e.ready"
    work.mkdir()
    stdout = ""
    for relationship in range(20):
        label = f"raw-env-ready-{relationship}"
        (work / f"client-compile-{label}.log").write_text("has env: false\n")
        stdout += (f"S8_RAW_ENV_READY relationship={relationship} label={label} "
                   "measured=0 cache_state=disabled\n")
    stdout += ("S8_ENV_PREPARATION relationships=20 readiness_compiles=20 "
               f"archive_sha256={'a' * 64} archive_bytes=31 start_ns=10 end_ns=20 "
               "measured=0 cache_state=disabled\n")
    preparation = runner._environment_preparation(
        stdout, work, runner.PARALLEL_TOPOLOGY)
    assert preparation["relationships"] == 20
    assert preparation["readiness_compiles"] == 20
    assert preparation["cache_state"] == "disabled"
    with pytest.raises(runner.LiveRunnerError, match="disabled_invalid"):
        runner._environment_preparation(
            stdout.replace("readiness_compiles=20", "readiness_compiles=19"),
            work, runner.PARALLEL_TOPOLOGY)
    with pytest.raises(runner.LiveRunnerError, match="raw_readiness_count_mismatch"):
        missing = "\n".join(line for line in stdout.splitlines()
                              if "S8_RAW_ENV_READY relationship=19" not in line) + "\n"
        runner._environment_preparation(
            missing,
            work, runner.PARALLEL_TOPOLOGY)


def test_parallel_cache_session_evidence_comes_from_observed_f_log(tmp_path: Path) -> None:
    (tmp_path / "client-compile-full-1-0.log").write_text("ZSTD_ROUTE\n")
    (tmp_path / "f-7.log").write_text("CACHE_SESSION\n")
    observation = [{"run": "full-1", "ordinal": 0, "planned_relationship": 7,
                    "observed_f_service_identity": "p50-f-7"}]
    runner._validate_product_log_evidence(tmp_path, observation, "ZSTD_ROUTE")
    (tmp_path / "f-7.log").write_text("no cache evidence\n")
    with pytest.raises(runner.LiveRunnerError, match="cache_session_evidence_missing"):
        runner._validate_product_log_evidence(tmp_path, observation, "ZSTD_ROUTE")


@pytest.mark.parametrize("profile", ["ZSTD_TU", "ZSTD_ROUTE", "P29", "GRZ_RESIDUAL"])
def test_dry_run_command_targets_real_single_lifecycle_for_all_profiles(tmp_path: Path,
                                                                          profile: str) -> None:
    batch = _batch(tmp_path)
    plan = _plan(tmp_path, batch, profile=profile, regime="warm")
    product, script = _product(tmp_path)
    command = runner.build_command(batch, profile,
                                   product_root=product, script=script, corpus="DuckDB",
                                   regime="warm", depth="100", passes=2,
                                   predictive_plan=plan)
    assert command[0] == "env"
    assert "ICECC_CARET_WORKAROUND=0" in command
    expected_product_profile = "GRZ" if profile == "GRZ_RESIDUAL" else profile
    assert f"ICECC_P50_PROFILE={expected_product_profile}" in command
    assert "ICECC_P50_C1F1_WARM=1" in command
    assert "ICECC_P50_C1F1_PASSES=2" in command
    assert "ICECC_P50_C1F1_EXPECTED_COUNT=100" in command
    assert any(item.startswith("ICECC_P50_PREDICTIVE_PLAN=") for item in command)
    assert "ICECC_P50_C1F1_TIMEOUT=3630" in command
    assert "ICECC_P50_C1F1_KEEP_WORK=1" in command
    assert any(item.startswith("ICECC_P50_C1F1_BATCH_MANIFEST=") for item in command)
    assert command[-1].endswith("unittests/p50compilee2e-run.sh")


def test_timeout_scales_and_is_capped() -> None:
    assert runner.derive_timeout(100, 1, False) < runner.derive_timeout(100, 2, True)
    assert runner.derive_timeout(100000, 2, True) == runner.MAX_TIMEOUT_SECONDS


def test_timeout_default_and_explicit_override_are_bounded() -> None:
    assert runner.derive_timeout(100, 2, False) == 2430
    assert runner.derive_timeout(100, 2, False, 900) == 900
    for invalid in (runner.MIN_TIMEOUT_SECONDS - 1, runner.MAX_TIMEOUT_SECONDS + 1, "900", 900.0):
        with pytest.raises(runner.LiveRunnerError, match="timeout:override_invalid"):
            runner.derive_timeout(100, 2, False, invalid)  # type: ignore[arg-type]


def test_timeout_override_propagates_to_dry_run_command(tmp_path: Path) -> None:
    batch = _batch(tmp_path)
    plan = _plan(tmp_path, batch)
    product, script = _product(tmp_path)
    command = runner.build_command(
        batch, "ZSTD_TU", product_root=product, script=script,
        predictive_plan=plan, timeout_seconds=901)
    assert "ICECC_P50_C1F1_TIMEOUT=901" in command


def test_build_command_reuses_authenticated_rows_for_topology(
        tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> None:
    batch = _batch(tmp_path)
    plan = _plan(tmp_path, batch)
    topology = _topology(tmp_path, batch)
    product, script = _product(tmp_path)
    calls = 0
    original = runner.load_batch_manifest

    def counted(path: Path, expected_count: int = 100) -> list[dict[str, object]]:
        nonlocal calls
        calls += 1
        return original(path, expected_count)

    monkeypatch.setattr(runner, "load_batch_manifest", counted)
    command = runner.build_command(
        batch, "ZSTD_TU", product_root=product, script=script,
        predictive_plan=plan, topology=topology)
    assert command[-1] == str(script)
    assert calls == 1


def test_input_ready_publication_precedes_lane_release_and_predecessor_failure_propagates() -> None:
    shell = (Path(__file__).resolve().parents[1] / "unittests" /
             "p50compilee2e-run.sh").read_text()
    source_commit = shell.index("source committed for P50 CompileFile")
    publish = shell.index("mv -- \"$input_ready_tmp\" \"$input_ready_marker\"")
    release = shell.index("release_planned_lane\n        if ! wait")
    predecessor_wait = shell.index('while test ! -e "$predecessor_marker"; do')
    predecessor_failure = shell.index('test ! -e "$predecessor_failure" || {')
    assert source_commit < publish < release
    assert predecessor_wait < predecessor_failure < predecessor_wait + 200
    assert 'echo "FAIL: predecessor source admission failed' in shell


def test_container_command_uses_resolved_image_and_read_only_product_mount(
        tmp_path: Path) -> None:
    bind_root = tmp_path / "tanksmall"
    product = bind_root / "product"
    product.mkdir(parents=True)
    required = product / "batch.jsonl"
    required.write_text("{}\n")
    work_parent = Path("/tmp") / f"p5.pytest-{os.getpid()}-{tmp_path.name}"
    work_parent.mkdir()
    try:
        identity = {"reference": runner.PINNED_IMAGE,
                    "image_id": "sha256:" + "a" * 64,
                    "architecture": "amd64", "os": "linux",
                    "created": "2026-08-21T21:11:43Z"}
        command = runner.build_container_command(
            ["env", "ICECC_P50_PROFILE=ZSTD_TU", str(product / "run.sh")],
            image_identity=identity, bind_root=bind_root,
            work_parent=work_parent, required_paths=[required, product])
        assert command[:7] == ["docker", "run", "--rm", "--user", "0", "--network", "host"]
        assert command[7:9] == ["--name", runner.container_name(work_parent)]
        assert "--oom-score-adj=-1000" in command
        assert identity["image_id"] in command
        assert identity["reference"] not in command
        assert f"{bind_root.resolve()}:{bind_root.resolve()}:ro" in command
        assert f"{work_parent}:{runner.DEFAULT_CONTAINER_WORK_ROOT}:rw" in command
        assert f"ICECC_TEST_DAEMON_UID={runner.CONTAINER_DAEMON_USER}" in command
        assert f"ICECC_TEST_DAEMON_GID={runner.CONTAINER_DAEMON_GROUP}" in command
        shell = command[-1]
        account_ready = shell.index("set +e")
        assert shell.index("groupadd --system icecc") < account_ready
        assert shell.index("useradd --system --gid icecc") < account_ready
        assert shell.index("getent passwd icecc") < account_ready
        assert (f"chown -R {os.geteuid()}:{os.getegid()} "
                f"{runner.DEFAULT_CONTAINER_WORK_ROOT}") in command[-1]
    finally:
        work_parent.rmdir()


def test_container_temp_root_must_be_a_real_directory(tmp_path: Path) -> None:
    root = tmp_path / "container-work"
    root.mkdir()
    assert runner.validated_container_temp_root(root) == root.absolute()
    work_parent = root / "p5.custom"
    work_parent.mkdir()
    required = tmp_path / "required"
    required.write_text("input\n")
    identity = {"reference": runner.PINNED_IMAGE,
                "image_id": "sha256:" + "a" * 64,
                "architecture": "amd64", "os": "linux",
                "created": "2026-08-21T21:11:43Z"}
    command = runner.build_container_command(
        ["env", "true"], image_identity=identity, bind_root=tmp_path,
        work_parent=work_parent, required_paths=[required], temp_root=root)
    assert (f"{work_parent.resolve()}:{runner.DEFAULT_CONTAINER_WORK_ROOT}:rw"
            in command)
    socket_path = (runner.DEFAULT_CONTAINER_WORK_ROOT / "p50compilee2e.run" /
                   "cache-runtime-f-19" /
                   ("attempt-100000-" + "a" * 32) / "cache.sock")
    assert len(str(socket_path)) < 108
    alias = tmp_path / "container-work-alias"
    alias.symlink_to(root, target_is_directory=True)
    with pytest.raises(runner.LiveRunnerError, match="container_temp_root:invalid"):
        runner.validated_container_temp_root(alias)


def test_container_account_preparation_is_deletion_sensitive(tmp_path: Path) -> None:
    """Removing the pre-start account preparation restores daemon fallback."""
    bind_root = tmp_path / "root"
    bind_root.mkdir()
    work_parent = Path("/tmp") / f"p5.account-{os.getpid()}-{tmp_path.name}"
    work_parent.mkdir()
    identity = {"reference": runner.PINNED_IMAGE,
                "image_id": "sha256:" + "a" * 64,
                "architecture": "amd64", "os": "linux",
                "created": "2026-08-21T21:11:43Z"}
    try:
        command = runner.build_container_command(
            ["env", "true"], image_identity=identity, bind_root=bind_root,
            work_parent=work_parent, required_paths=[bind_root])
        shell = command[-1]
        prelude, product = shell.split("set +e\n", 1)
        assert "getent passwd icecc" in prelude
        assert "groupadd --system icecc" in prelude
        assert "useradd --system --gid icecc" in prelude
        # The daemon source emits this exact warning when its constructor
        # cannot resolve icecc.  A deletion mutant has no account preparation
        # before product startup, so that constructor-time condition returns.
        daemon_source = (Path(__file__).resolve().parents[1] /
                         "daemon/main.cpp").read_text()
        assert 'getpwnam("icecc")' in daemon_source
        assert "No icecc user on system. Falling back to nobody." in daemon_source
        deletion_mutant = product
        assert "getent passwd icecc" not in deletion_mutant
        assert "groupadd --system icecc" not in deletion_mutant
        assert "useradd --system --gid icecc" not in deletion_mutant
    finally:
        work_parent.rmdir()


def test_container_reported_workdir_maps_to_retained_host_tree(tmp_path: Path) -> None:
    host = tmp_path / "p50compilee2e.run"
    host.mkdir()
    reported = runner.DEFAULT_REPORTED_WORKDIR
    assert runner._retained_workdir(
        f"S7_WORKDIR={reported}\n", host_workdir=host,
        reported_workdir=reported) == host
    with pytest.raises(runner.LiveRunnerError, match="workdir_mapping_mismatch"):
        runner._retained_workdir(
            "S7_WORKDIR=/wrong/p50compilee2e.run\n", host_workdir=host,
            reported_workdir=reported)
    real_parent = tmp_path / "real"
    real_parent.mkdir()
    (real_parent / "p50compilee2e.run").mkdir()
    alias = tmp_path / "alias"
    alias.symlink_to(real_parent, target_is_directory=True)
    with pytest.raises(runner.LiveRunnerError, match="workdir_unavailable"):
        runner._retained_workdir(
            f"S7_WORKDIR={reported}\n",
            host_workdir=alias / "p50compilee2e.run",
            reported_workdir=reported)


def test_container_reported_artifacts_map_only_beneath_exact_workdir(
        tmp_path: Path) -> None:
    host = tmp_path / "p50compilee2e.run"
    reported = runner.DEFAULT_REPORTED_WORKDIR
    mapped = runner._reported_artifact_path(
        str(reported / "out" / "remote-full-1-0.o"), work=host,
        reported_work=reported, reason="artifact_path_invalid")
    assert mapped == host / "out" / "remote-full-1-0.o"
    for invalid in (
            Path("out/remote-full-1-0.o"),
            reported.parent / "other" / "remote-full-1-0.o",
            reported / ".." / "other" / "remote-full-1-0.o"):
        with pytest.raises(runner.LiveRunnerError, match="artifact_path_invalid"):
            runner._reported_artifact_path(
                str(invalid), work=host, reported_work=reported,
                reason="artifact_path_invalid")
    with pytest.raises(runner.LiveRunnerError, match="artifact_path_invalid"):
        runner._reported_artifact_path(
            "/untrusted-prefix/out/remote-full-1-0.o", work=host,
            reported_work=Path("/untrusted-prefix"),
            reason="artifact_path_invalid")


def test_timing_rows_read_container_artifacts_from_retained_host_tree(
        tmp_path: Path) -> None:
    host = tmp_path / "p50compilee2e.run"
    (host / "out").mkdir(parents=True)
    reported = runner.DEFAULT_REPORTED_WORKDIR
    preprocessed = host / "s7-full-1-0-preprocessed.ii"
    remote = host / "out" / "remote-full-1-0.o"
    local = host / "out" / "local-full-1-0.o"
    preprocessed.write_bytes(b"predictive input\n")
    remote.write_bytes(b"object\n")
    local.write_bytes(remote.read_bytes())
    def digest(path: Path) -> str:
        return hashlib.sha256(path.read_bytes()).hexdigest()
    source_sha = hashlib.sha256(b"source\n").hexdigest()
    rows = [{"tu_id": "tu-0", "sha256": source_sha,
             "predictive_input": {"sha256": digest(preprocessed),
                                  "bytes": preprocessed.stat().st_size}}]
    fields = {
        "run": "full-1", "ordinal": "0", "tu_id": "tu-0",
        "source_sha256": source_sha,
        "preprocessed_path": str(reported / preprocessed.name),
        "preprocessed_sha256": digest(preprocessed),
        "preprocessed_bytes": str(preprocessed.stat().st_size),
        "remote_path": str(reported / "out" / remote.name),
        "remote_sha256": digest(remote), "remote_bytes": str(remote.stat().st_size),
        "local_path": str(reported / "out" / local.name),
        "local_sha256": digest(local), "local_bytes": str(local.stat().st_size),
        "admission_start_ns": "10", "compile_start_ns": "20",
        "input_ready_ns": "30", "compile_end_ns": "40", "witness_end_ns": "50",
        "wait_for_cs_ns": "1", "planned_assignment_ordinal": "0",
        "planned_relationship": "0", "planned_admission_lane": "0",
        "observed_scheduler_job_id": "1", "observed_f_service_identity": "p50-f",
        "observed_source_tu_seq": "0",
    }
    stdout = "S8_BATCH_TU " + " ".join(f"{key}={value}" for key, value in fields.items())
    observations = runner._timing_rows(
        stdout, rows, host, 1, [{"relationship": 0, "f_slot": 0}],
        runner.TOPOLOGY, reported)
    assert observations[0]["preprocessed_path"] == str(preprocessed)
    assert observations[0]["remote_path"] == str(remote)
    assert observations[0]["local_path"] == str(local)
    assert observations[0]["reported_remote_path"] == fields["remote_path"]
    bad = stdout.replace(fields["remote_path"], "/p5/other/remote-full-1-0.o")
    with pytest.raises(runner.LiveRunnerError, match="remote_object_path_invalid"):
        runner._timing_rows(
            bad, rows, host, 1, [{"relationship": 0, "f_slot": 0}],
            runner.TOPOLOGY, reported)


def test_container_cleanup_is_exact_and_tolerates_already_removed(
        monkeypatch: pytest.MonkeyPatch) -> None:
    work_parent = Path("/tmp") / f"p5.cleanup-{os.getpid()}"
    calls: list[list[str]] = []

    def fake_run(command: list[str], **_kwargs: object) -> subprocess.CompletedProcess[str]:
        calls.append(command)
        return subprocess.CompletedProcess(command, 1, "", "Error: No such container")

    monkeypatch.setattr(runner.subprocess, "run", fake_run)
    name = runner.container_name(work_parent)
    runner.stop_container(name)
    assert calls == [["docker", "container", "rm", "--force", name]]
    with pytest.raises(runner.LiveRunnerError, match="name_invalid"):
        runner.stop_container("unrelated-container")


def test_interrupted_product_removes_its_exact_container(
        tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> None:
    work_parent = Path("/tmp") / f"p5.cleanup-main-{os.getpid()}"
    work_parent.mkdir()
    stopped: list[str] = []
    image = {"reference": runner.PINNED_IMAGE,
             "image_id": "sha256:" + "a" * 64,
             "architecture": "amd64", "os": "linux",
             "created": "2026-08-21T21:11:43Z"}

    monkeypatch.setattr(runner.tempfile, "mkdtemp", lambda **_kwargs: str(work_parent))
    monkeypatch.setattr(runner, "load_predictive_plan",
                        lambda *_args, **_kwargs: ({}, [], "b" * 64))
    monkeypatch.setattr(runner, "load_batch_manifest", lambda *_args, **_kwargs: [])
    monkeypatch.setattr(runner, "load_topology", lambda *_args, **_kwargs: "c" * 64)
    monkeypatch.setattr(runner, "container_image_identity", lambda *_args: image)
    monkeypatch.setattr(
        runner, "product_identity",
        lambda *_args: ("d" * 40, "e" * 40, {"client/icecc": "f" * 64}, "1" * 64))
    monkeypatch.setattr(runner, "build_command", lambda *_args, **_kwargs: ["env", "true"])
    monkeypatch.setattr(runner, "build_container_command",
                        lambda *_args, **_kwargs: ["docker", "run"])

    def interrupted(_command: list[str], _timeout: int) -> tuple[str, int]:
        raise runner.LiveRunnerError("product_run:interrupted")

    monkeypatch.setattr(runner, "_run_product", interrupted)
    monkeypatch.setattr(runner, "stop_container", stopped.append)
    try:
        status = runner.main([
            "--batch-manifest", str(tmp_path / "batch.jsonl"),
            "--predictive-plan", str(tmp_path / "plan.json"),
            "--topology", str(tmp_path / "topology.json"),
            "--profile", "ZSTD_TU", "--product-root", str(tmp_path / "product"),
            "--output", str(tmp_path / "output"), "--execute",
        ])
        assert status == 77
        assert stopped == [runner.container_name(work_parent)]
    finally:
        work_parent.rmdir()


def test_container_command_rejects_input_outside_read_only_root(tmp_path: Path) -> None:
    bind_root = tmp_path / "root"
    bind_root.mkdir()
    work_parent = Path("/tmp") / f"p5.pytest-outside-{os.getpid()}-{tmp_path.name}"
    work_parent.mkdir()
    identity = {"reference": runner.PINNED_IMAGE,
                "image_id": "sha256:" + "b" * 64,
                "architecture": "amd64", "os": "linux",
                "created": "2026-08-21T21:11:43Z"}
    try:
        with pytest.raises(runner.LiveRunnerError, match="required_path_outside"):
            runner.build_container_command(
                ["env", "true"], image_identity=identity, bind_root=bind_root,
                work_parent=work_parent, required_paths=[tmp_path.parent])
    finally:
        work_parent.rmdir()


def test_topology_requires_exact_tu_identity(tmp_path: Path) -> None:
    batch = _batch(tmp_path)
    rows = runner.load_batch_manifest(batch)
    topology = _topology(tmp_path, batch)
    value = json.loads(topology.read_text())
    value["assignments"][0].pop("tu_id")
    topology.write_text(json.dumps(value))
    with pytest.raises(runner.LiveRunnerError, match="identity_mismatch"):
        runner.load_topology(topology, rows)


def test_predictive_schedule_slot_mutation_fails_before_live_descriptor(tmp_path: Path) -> None:
    batch = _batch(tmp_path, 1)
    rows = runner.load_batch_manifest(batch, 1)
    topology = _topology(tmp_path, batch)
    scheduling = {"topology": "C1F1", "assignments": [
        {"ordinal": 0, "global_slot": 1, "f_relationship": 0, "per_f_slot": 0}
    ]}
    with pytest.raises(runner.LiveRunnerError, match="predictive_schedule_mismatch"):
        runner.load_topology(topology, rows, scheduling=scheduling)


def test_product_identity_rejects_tracked_edit(tmp_path: Path) -> None:
    product, script = _product(tmp_path)
    (product / "unittests/p50compilee2e-run.sh").write_text("#!/bin/sh\nexit 1\n")
    with pytest.raises(runner.LiveRunnerError, match="tracked_or_index_dirty"):
        runner.product_identity(product, script)


def test_batch_shell_excludes_warm_prewarm_and_carries_optional_repeat() -> None:
    shell = (Path(__file__).resolve().parents[1] / "unittests/p50compilee2e-run.sh").read_text()
    runner_source = Path(__file__).resolve().parent.joinpath(
        "s8_real_c1f1_live_runner.py").read_text()
    cache_service = (Path(__file__).resolve().parents[1] /
                     "cache/p50_cache_service.cpp").read_text()
    sidecar_adapter = (Path(__file__).resolve().parents[1] /
                       "cache/p50_daemon_sidecar_adapter.cpp").read_text()
    assert "run_batch prewarm 0" in shell
    assert "run_batch full-1 1" in shell
    assert 'staged="$work/src/$run_label-$ordinal.ii"' in shell
    assert 'cp -- "$predictive_path" "$staged"' in shell
    assert 'compile_args_for "$item_compile_db" "$item_compile_source" "$item_compile_output" "$input_path" "$remote_obj"' in shell
    assert 'compile_args_for "$item_compile_db" "$item_compile_source" "$item_compile_output" "$input_path" "$local_obj"' in shell
    assert 'eval "g++ $local_compile_args"' in shell
    assert '"$input_path" -o "$local_obj"' in shell
    assert 'g++ -x c++ -std=c++17' not in shell
    assert 'stdin_mode' not in shell
    assert 'if test "$passes" = 2; then' in shell
    assert "s7-measured-c-action-trace.jsonl" in runner_source
    assert "shutil.rmtree(work)" in runner_source
    assert '"comparison": manifest_value["comparison"]' in runner_source
    assert 'product-output.log' in runner_source
    assert 'C1F20/40) relationship_count=20; slots_per_f=2; execution_slots=40' in shell
    assert '"$build/daemon/iceccd" "$@" -p "$worker_port" -m 2' in shell
    assert '"$work/envs-f-$relationship"' in shell
    assert 'S8_ENV_WARMUP relationship=$relationship' in shell
    assert 'S8_ENV_PREPARATION relationships=$environment_warmup_count' in shell
    assert 'raw-env-ready-$relationship' in shell
    assert ': >"$c_legacy_wire_trace"' in shell
    assert 's7-measured-f-legacy-wire-trace-$relationship.jsonl' in shell
    assert shell.index('raw-env-ready-$relationship') < shell.index(
        ': >"$c_legacy_wire_trace"') < shell.index('S8_ENV_PREPARATION relationships=')
    assert 'S8_SIDECAR_ROTATION role=$sidecar_role' in shell
    assert 'scheduler_rotation_offset=$(stat -c %s "$work/scheduler.log")' in shell
    assert 'S8_ENV_POST_ROTATION_READY relationships=$ready_count' in shell
    assert 'S8_ENV_MEASURED_NO_INSTALL checked=1' in shell
    assert 'S8_BATCH_WINDOW run=%s start_ns=%s end_ns=%s' in shell
    assert 'run_one "$run_label" "$ordinal" "$relationship" "$f_slot"' in shell
    assert "physical_slot_observed=0" in shell
    assert '"physical_slot_observed": False' in Path(__file__).resolve().parent.joinpath(
        "s8_real_c1f1_live_runner.py").read_text()
    assert '"source_admission": "per_relationship_source_commit_gate"' in (
        Path(__file__).resolve().parent / "s8_real_c1f1_live_runner.py").read_text()
    assert 'staged="$work/src/$run_label-$ordinal.ii"' in shell
    assert 'cp -- "$predictive_path" "$staged"' in shell
    assert 'compile_args_for "$item_compile_db" "$item_compile_source" "$item_compile_output" "$input_path" "$remote_obj"' in shell
    assert 'input-ready/$run_label-$relationship-$ordinal' in shell
    assert "source committed for P50 CompileFile" in shell
    assert 'while ! ln "$owner_path" "$marker" 2>/dev/null; do sleep 0.005; done' in shell
    assert 'if test "$marker_owner" = "$owner_token"; then' in shell
    assert shell.index("release_planned_lane\n        if ! wait") < shell.index(
        'witness_end_ns=$(date +%s%N)')
    assert 'predecessor_marker="$work/input-ready/$run_label-$relationship-$predecessor_ordinal"' in shell
    assert "S8_BATCH_METRICS run=%s" in shell
    assert "planned_admission_lane=%s" in shell
    assert "observed_scheduler_job_id=%s" in shell
    assert "observed_f_service_identity=%s" in shell
    assert "assignment=%s relationship=%s f_slot=%s service_identity=" not in shell
    assert "grep -oE 'p50-f-[0-9]+' | sort -u | wc -l" in shell
    assert "fsession_owner_(64, config_.f_store_generation)" in cache_service
    assert "::listen(outer_launch_listener_fd_, 64)" in sidecar_adapter


def test_unsupported_topology_fails_closed(tmp_path: Path) -> None:
    batch = _batch(tmp_path)
    rows = runner.load_batch_manifest(batch)
    topology = tmp_path / "bad-topology.json"
    topology.write_text(json.dumps({"suite": "C1F20/40", "assignments": []}))
    with pytest.raises(runner.LiveRunnerError, match="unsupported_suite"):
        runner.load_topology(topology, rows)


def test_parallel_topology_binds_all_relationships_and_slots(tmp_path: Path) -> None:
    batch = _batch(tmp_path)
    rows = runner.load_batch_manifest(batch)
    topology = tmp_path / "parallel-topology.json"
    assignments = [{"ordinal": index, "tu_id": row["tu_id"],
                    "relationship": index % 20, "f_slot": (index // 20) % 2}
                   for index, row in enumerate(rows)]
    topology.write_text(json.dumps({"schema": "icecream-s8-topology-assignment-v1",
                                    "suite": runner.PARALLEL_TOPOLOGY,
                                    "assignments": assignments}))
    assert len(runner.load_topology(topology, rows, runner.PARALLEL_TOPOLOGY)) == 64
    assignments[0]["relationship"] = 20
    topology.write_text(json.dumps({"schema": "icecream-s8-topology-assignment-v1",
                                    "suite": runner.PARALLEL_TOPOLOGY,
                                    "assignments": assignments}))
    with pytest.raises(runner.LiveRunnerError, match="assignment_invalid"):
        runner.load_topology(topology, rows, runner.PARALLEL_TOPOLOGY)


def test_parallel_topology_must_match_predictive_schedule(tmp_path: Path) -> None:
    batch = _batch(tmp_path, 40)
    rows = runner.load_batch_manifest(batch, 40)
    topology = tmp_path / "parallel-topology.json"
    assignments = [
        {"ordinal": index, "tu_id": row["tu_id"],
         "relationship": index // 2, "f_slot": index % 2}
        for index, row in enumerate(rows)
    ]
    topology.write_text(json.dumps({
        "schema": "icecream-s8-topology-assignment-v1",
        "suite": runner.PARALLEL_TOPOLOGY,
        "assignments": assignments,
    }))
    scheduling = {
        "topology": "C1F20",
        "assignments": [
            {"ordinal": index, "global_slot": index,
             "f_relationship": index // 2, "per_f_slot": index % 2}
            for index in range(40)
        ],
    }
    assert len(runner.load_topology(
        topology, rows, runner.PARALLEL_TOPOLOGY, scheduling)) == 64
    scheduling["assignments"][1]["per_f_slot"] = 0
    with pytest.raises(runner.LiveRunnerError, match="predictive_schedule_mismatch"):
        runner.load_topology(topology, rows, runner.PARALLEL_TOPOLOGY, scheduling)


def _external_farm(tmp_path: Path, suite: str = runner.TOPOLOGY) -> tuple[Path, str]:
    count = runner.RELATIONSHIP_COUNT[suite]
    c_digest = "1" * 64
    scheduler_digest = c_digest
    physical_digests = [f"{index + 3:064x}" for index in range(3 if suite == runner.PARALLEL_TOPOLOGY else 1)]
    f_digests = [physical_digests[index % len(physical_digests)] for index in range(count)]
    value = {
        "schema": runner.EXTERNAL_FARM_SCHEMA,
        "suite": suite,
        "scheduler": {"host": "10.0.27.101", "port": 54321},
        "client": {"host_digest": c_digest},
        "workers": [{"relationship": index,
                     "service": "p50-f" if suite == runner.TOPOLOGY else f"p50-f-{index}",
                     "host_digest": f_digests[index]} for index in range(count)],
        "role_placement": {
            "schema": runner.ROLE_PLACEMENT_SCHEMA, "mode": "external_farm",
            "c_host_digest": c_digest, "scheduler_host_digest": scheduler_digest,
            "f_host_digests": physical_digests, "roles_disjoint": True,
            "timing_eligible": True,
        },
    }
    path = tmp_path / "external-farm.json"
    path.write_bytes(runner._canonical(value) + b"\n")
    return path, hashlib.sha256(path.read_bytes()).hexdigest()


def test_external_farm_manifest_proves_disjoint_role_hosts(tmp_path: Path) -> None:
    path, digest = _external_farm(tmp_path)
    value, observed, size = runner.load_external_farm_manifest(
        path, digest, runner.TOPOLOGY)
    assert observed == digest and size == path.stat().st_size
    assert value["role_placement"]["timing_eligible"] is True


def test_external_farm_manifest_allows_c_and_scheduler_same_host(tmp_path: Path) -> None:
    path, digest = _external_farm(tmp_path)
    value, _observed, _size = runner.load_external_farm_manifest(
        path, digest, runner.TOPOLOGY)
    placement = value["role_placement"]
    assert placement["c_host_digest"] == placement["scheduler_host_digest"]


def test_parallel_external_farm_maps_twenty_relationships_to_three_hosts(
        tmp_path: Path) -> None:
    path, digest = _external_farm(tmp_path, runner.PARALLEL_TOPOLOGY)
    value, _observed, _size = runner.load_external_farm_manifest(
        path, digest, runner.PARALLEL_TOPOLOGY)
    placement = value["role_placement"]
    assert len(placement["f_host_digests"]) == 3
    assert [row["host_digest"] for row in value["workers"]] == [
        placement["f_host_digests"][index % 3] for index in range(20)]


def test_external_farm_manifest_rejects_client_worker_co_residence(tmp_path: Path) -> None:
    path, _digest = _external_farm(tmp_path)
    value = json.loads(path.read_text())
    value["workers"][0]["host_digest"] = value["client"]["host_digest"]
    value["role_placement"]["f_host_digests"][0] = value["client"]["host_digest"]
    path.write_bytes(runner._canonical(value) + b"\n")
    digest = hashlib.sha256(path.read_bytes()).hexdigest()
    with pytest.raises(runner.LiveRunnerError, match="role_placement:external_identity_invalid"):
        runner.load_external_farm_manifest(path, digest, runner.TOPOLOGY)


def test_external_farm_manifest_rejects_scheduler_worker_co_residence(tmp_path: Path) -> None:
    path, _digest = _external_farm(tmp_path)
    value = json.loads(path.read_text())
    scheduler_digest = "2" * 64
    value["role_placement"]["scheduler_host_digest"] = scheduler_digest
    value["role_placement"]["f_host_digests"][0] = scheduler_digest
    value["workers"][0]["host_digest"] = scheduler_digest
    path.write_bytes(runner._canonical(value) + b"\n")
    digest = hashlib.sha256(path.read_bytes()).hexdigest()
    with pytest.raises(runner.LiveRunnerError, match="role_placement:external_identity_invalid"):
        runner.load_external_farm_manifest(path, digest, runner.TOPOLOGY)


def test_local_runner_declares_non_calibratable_role_placement() -> None:
    placement = runner._co_resident_role_placement("a" * 64, runner.PARALLEL_TOPOLOGY)
    assert placement["schema"] == runner.ROLE_PLACEMENT_SCHEMA
    assert placement["mode"] == "co_resident_loopback"
    assert placement["roles_disjoint"] is False
    assert placement["timing_eligible"] is False
    assert placement["f_host_digests"] == ["a" * 64]


def _external_authority(tmp_path: Path, manifest: Path, manifest_sha: str) -> tuple[Path, str]:
    """Build the standalone host/image authority consumed by the adapter."""
    descriptor = tmp_path / "external-host-descriptor.json"
    descriptor.write_bytes(runner._canonical({
        "schema": runner.HOST_DESCRIPTOR_SCHEMA,
        "facts": {
            "machine_id_sha256": "1" * 64,
            "dmi_source": "unavailable",
            "dmi_identity_sha256": None,
            "cpu_vendor_sha256": "2" * 64,
            "cpu_model_sha256": "3" * 64,
            "cpu_count": 8,
        },
    }) + b"\n")
    descriptor_sha = hashlib.sha256(descriptor.read_bytes()).hexdigest()
    manifest_value = json.loads(manifest.read_text())
    metadata = {"product_image_digest": "7" * 64,
                "toolchain_digest": "5" * 64,
                "output_contract_digest": "6" * 64,
                "host_digest": descriptor_sha,
                "ordered_input_class": "ordered"}
    authority = {
        "schema": runner.EXTERNAL_FARM_AUTHORITY_SCHEMA,
        "suite": manifest_value["suite"], "manifest_sha256": manifest_sha,
        "role_placement": manifest_value["role_placement"],
        "runtime_image": {"reference": runner.PINNED_IMAGE,
                           "image_id": "sha256:" + "7" * 64,
                           "architecture": "amd64", "os": "linux", "created": "now"},
        "host_descriptor": {"path": str(descriptor), "sha256": descriptor_sha,
                             "bytes": descriptor.stat().st_size},
        "calibration_metadata": metadata,
    }
    path = tmp_path / "external-authority.json"
    path.write_bytes(runner._canonical(authority) + b"\n")
    return path, hashlib.sha256(path.read_bytes()).hexdigest()


def test_external_binding_requires_independent_authority_and_binds_hashes(
        tmp_path: Path) -> None:
    manifest, manifest_sha = _external_farm(tmp_path)
    authority, authority_sha = _external_authority(tmp_path, manifest, manifest_sha)
    external = runner.ExternalFarmFinalization(
        manifest, manifest_sha, authority, authority_sha,
        tmp_path / "p50compilee2e.external", f"S7_WORKDIR={tmp_path / 'p50compilee2e.external'}")
    (tmp_path / "p50compilee2e.external").mkdir()
    binding = runner._external_farm_binding(external, runner.TOPOLOGY)
    assert binding["manifest"]["sha256"] == manifest_sha
    assert binding["authority"]["sha256"] == authority_sha
    assert binding["role_placement"] == json.loads(manifest.read_text())["role_placement"]


def test_external_binding_rejects_authority_hash_mutation_and_local_launch(
        tmp_path: Path) -> None:
    manifest, manifest_sha = _external_farm(tmp_path)
    authority, authority_sha = _external_authority(tmp_path, manifest, manifest_sha)
    work = tmp_path / "p50compilee2e.external"; work.mkdir()
    external = runner.ExternalFarmFinalization(
        manifest, manifest_sha, authority, authority_sha, work,
        f"S7_WORKDIR={work}")
    authority.write_bytes(authority.read_bytes() + b"\n")
    with pytest.raises(runner.LiveRunnerError, match="authority:sha256_mismatch"):
        runner._external_farm_binding(external, runner.TOPOLOGY)
    with pytest.raises(runner.LiveRunnerError, match="local_execution_authority_present"):
        # The local identity is intentionally rejected before any product
        # parsing, so a caller cannot relabel a loopback run as external.
        runner.finalize(external.stdout, 0, batch_manifest=manifest, topology=manifest,
                        predictive_plan=manifest, output=tmp_path / "out", profile="ZSTD_TU",
                        product_root=tmp_path, external_farm=external,
                        launch_identity={"source_commit": "a" * 40},
                        execution_environment="external_farm_product_build")


def test_external_binding_rejects_disconnected_worker_host_digest(
        tmp_path: Path) -> None:
    manifest, _digest = _external_farm(tmp_path)
    value = json.loads(manifest.read_text())
    value["workers"][0]["host_digest"] = "9" * 64
    manifest.write_bytes(runner._canonical(value) + b"\n")
    with pytest.raises(runner.LiveRunnerError, match="worker_binding_mismatch|identity_invalid"):
        runner.load_external_farm_manifest(
            manifest, hashlib.sha256(manifest.read_bytes()).hexdigest(), runner.TOPOLOGY)


@pytest.mark.parametrize(("mutation", "message"), [
    (lambda value: value.update({"suite": runner.PARALLEL_TOPOLOGY}), "identity_invalid"),
    (lambda value: value["scheduler"].update({"host": "127.0.0.1"}), "loopback"),
    (lambda value: value["workers"][0].update({"host_digest": "1" * 64}), "identity_invalid"),
])
def test_external_manifest_rejects_transport_identity_mutations(
        tmp_path: Path, mutation: object, message: str) -> None:
    path, _digest = _external_farm(tmp_path)
    value = json.loads(path.read_text())
    mutation(value)  # type: ignore[operator]
    path.write_bytes(runner._canonical(value) + b"\n")
    digest = hashlib.sha256(path.read_bytes()).hexdigest()
    with pytest.raises(runner.LiveRunnerError, match=message):
        runner.load_external_farm_manifest(path, digest, runner.TOPOLOGY)


def test_external_finalizer_propagates_scope_placement_and_authority(
        tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> None:
    batch_manifest = tmp_path / "batch.jsonl"; batch_manifest.write_bytes(b"batch\n")
    topology = tmp_path / "topology.json"
    topology.write_bytes(runner._canonical({"assignments": []}) + b"\n")
    predictive_plan = tmp_path / "predictive-plan.json"; predictive_plan.write_bytes(b"plan\n")
    work = tmp_path / "p50compilee2e.external"; work.mkdir()
    (work / "s7-measured-c-action-trace.jsonl").write_bytes(b"c-action\n")
    (work / "s7-measured-f-action-trace.jsonl").write_bytes(b"f-action\n")
    rows = [{"tu_id": "tu-0"}]
    plan = {"source_manifest": {"sha256": "a" * 64},
            "scheduling": {"topology": runner.TOPOLOGY,
                            "assignments": [{"ordinal": 0, "global_slot": 0}]}}
    plan_sha = hashlib.sha256(predictive_plan.read_bytes()).hexdigest()
    monkeypatch.setattr(runner, "load_predictive_plan",
                        lambda *_args, **_kwargs: (plan, [{"ordinal": 0}], plan_sha))
    monkeypatch.setattr(runner, "load_batch_manifest", lambda *_args: rows)
    monkeypatch.setattr(runner, "bind_batch_to_plan", lambda *_args: None)
    topology_sha = hashlib.sha256(topology.read_bytes()).hexdigest()
    monkeypatch.setattr(runner, "load_topology", lambda *_args: topology_sha)
    monkeypatch.setattr(runner, "_retained_workdir", lambda stdout, **_kwargs: work)
    monkeypatch.setattr(runner, "_environment_preparation", lambda *_args: {})
    monkeypatch.setattr(runner, "_binary_identity", lambda *_args: {"client/icecc": "e" * 64})
    monkeypatch.setattr(runner, "product_identity",
                        lambda *_args: ("f" * 40, "1" * 40, {"client/icecc": "e" * 64}, "2" * 64))
    monkeypatch.setattr(runner, "_timing_rows", lambda *_args: [])
    monkeypatch.setattr(runner, "_batch_windows",
                        lambda *_args: {"full-1": {"start_ns": 1, "end_ns": 2}})
    monkeypatch.setattr(runner, "_validate_product_log_evidence", lambda *_args: None)
    monkeypatch.setattr(runner, "_action_stage", lambda *_args: [])
    metadata = {"product_image_digest": "7" * 64, "toolchain_digest": "5" * 64,
                "output_contract_digest": "6" * 64, "host_digest": "0" * 64,
                "ordered_input_class": "ordered"}
    monkeypatch.setattr(runner, "_calibration_metadata", lambda **_kwargs: metadata)
    monkeypatch.setattr(runner, "_live_curve_rows",
                        lambda *_args: [{"step": 0, "tu_id": "tu-0",
                                         "cumulative": {"channel_bytes": 1, "elapsed_ns": 1}}])
    manifest, manifest_sha = _external_farm(tmp_path)
    authority, authority_sha = _external_authority(tmp_path, manifest, manifest_sha)
    authority_value = json.loads(authority.read_text())
    descriptor = Path(authority_value["host_descriptor"]["path"])
    metadata["host_digest"] = hashlib.sha256(descriptor.read_bytes()).hexdigest()
    authority_value["calibration_metadata"] = metadata
    authority.write_bytes(runner._canonical(authority_value) + b"\n")
    authority_sha = hashlib.sha256(authority.read_bytes()).hexdigest()
    external = runner.ExternalFarmFinalization(
        manifest, manifest_sha, authority, authority_sha, work,
        f"PASS: all-P50 C1F1\nS7_WORKDIR={work}\nS8_BATCH_COUNT=1\n"
        f"S8_SUITE={runner.TOPOLOGY}\nS8_BATCH_PASSES=1\nS8_BATCH_WARM=0\n"
        "S8_SCHEDULING mode=relationship-ordered execution_slots=1 relationships=1 "
        "planned_admission_lanes_per_relationship=1\n")
    output = runner.finalize(
        external.stdout, 0, batch_manifest=batch_manifest, topology=topology,
        predictive_plan=predictive_plan, output=tmp_path / "output", profile="ZSTD_TU",
        product_root=tmp_path, corpus="DuckDB", regime="cold", depth="full", full_count=1,
        passes=1, timestamp="20260831T000000Z", execution_environment="external_farm_product_build",
        external_farm=external)
    evidence = json.loads((output / "evidence.json").read_text())
    curve_manifest = json.loads((output / "live_curve_manifest.json").read_text())
    experiment = json.loads((output / "experiment_manifest.json").read_text())
    assert evidence["execution_scope"] == runner.EXTERNAL_FARM_EXECUTION_SCOPE
    assert curve_manifest["role_placement"] == evidence["role_placement"]
    assert experiment["external_farm"]["manifest"]["sha256"] == manifest_sha
    assert (output / "product-evidence" / "external-farm-authority.json").is_file()


def test_parallel_batch_window_requires_real_overlap() -> None:
    rows = [{} for _ in range(40)]
    observations = []
    for index in range(40):
        wave = index // 20
        observations.append({
            "run": "full-1", "planned_assignment_ordinal": index,
            "planned_relationship": index % 20, "planned_admission_lane": wave,
            "admission_start_ns": 1_000 + wave * 100,
            "compile_start_ns": 1_001 + wave * 100,
            "input_ready_ns": 1_100 + wave * 100,
            "compile_end_ns": 1_300 + wave * 100,
            "witness_end_ns": 1_350 + wave * 100,
            "observed_source_tu_seq": wave,
        })
    stdout = (
        "S8_BATCH_WINDOW run=full-1 start_ns=900 end_ns=1500\n"
        "S8_BATCH_METRICS run=full-1 makespan_ns=500 harness_completion_ns=600 "
        "max_concurrent_source_admissions=20 max_concurrent_compile_result_jobs=40 "
        "max_concurrent_admitted_or_compiling_jobs=40 "
        "max_concurrent_active_per_relationship=2\n"
    )
    windows = runner._batch_windows(stdout, observations, rows, 1,
                                    runner.PARALLEL_TOPOLOGY)
    assert windows["full-1"]["makespan_ns"] == 500
    assert windows["full-1"]["harness_completion_ns"] == 600
    assert windows["full-1"]["max_concurrent_source_admissions"] == 20
    assert windows["full-1"]["max_concurrent_active_per_relationship"] == 2

    serial = []
    for index, row in enumerate(observations):
        start = 1_000 + index * 100
        serial.append({**row, "admission_start_ns": start,
                       "compile_start_ns": start + 1, "input_ready_ns": start + 10,
                       "compile_end_ns": start + 50, "witness_end_ns": start + 60})
    serial_stdout = (
        "S8_BATCH_WINDOW run=full-1 start_ns=900 end_ns=5000\n"
        "S8_BATCH_METRICS run=full-1 makespan_ns=4050 harness_completion_ns=4100 "
        "max_concurrent_source_admissions=1 max_concurrent_compile_result_jobs=1 "
        "max_concurrent_admitted_or_compiling_jobs=1 "
        "max_concurrent_active_per_relationship=1\n"
    )
    with pytest.raises(runner.LiveRunnerError, match="no_observed_compile_overlap"):
        runner._batch_windows(serial_stdout, serial, rows, 1, runner.PARALLEL_TOPOLOGY)


def test_relationship_admission_model_removes_global_source_serialization() -> None:
    """Bounded 40-job model of the old global gate and new relationship gates."""
    global_gate = []
    relationship_gate = []
    for ordinal in range(40):
        relationship, wave = ordinal % 20, ordinal // 20
        global_start = ordinal * 10
        global_gate.append({
            "planned_assignment_ordinal": ordinal,
            "planned_relationship": relationship, "planned_admission_lane": wave,
            "admission_start_ns": global_start, "input_ready_ns": global_start + 10,
            "compile_start_ns": global_start + 1, "compile_end_ns": global_start + 101,
            "witness_end_ns": global_start + 110, "observed_source_tu_seq": wave,
        })
        relationship_start = wave * 10
        relationship_gate.append({
            "planned_assignment_ordinal": ordinal,
            "planned_relationship": relationship, "planned_admission_lane": wave,
            "admission_start_ns": relationship_start,
            "input_ready_ns": relationship_start + 10,
            "compile_start_ns": relationship_start + 1,
            "compile_end_ns": relationship_start + 101,
            "witness_end_ns": relationship_start + 110,
            "observed_source_tu_seq": wave,
        })
    baseline = runner._observed_concurrency(global_gate, runner.PARALLEL_TOPOLOGY)
    prototype = runner._observed_concurrency(relationship_gate, runner.PARALLEL_TOPOLOGY)
    baseline_makespan = max(row["compile_end_ns"] for row in global_gate)
    prototype_makespan = max(row["compile_end_ns"] for row in relationship_gate)
    assert baseline["max_concurrent_source_admissions"] == 1
    assert prototype["max_concurrent_source_admissions"] == 20
    assert prototype["max_concurrent_active_per_relationship"] == 2
    assert prototype_makespan == 111
    assert baseline_makespan == 491


def test_planned_lane_can_reenter_at_remote_result_before_local_witness() -> None:
    rows = [
        {"planned_assignment_ordinal": 0, "planned_relationship": 0,
         "planned_admission_lane": 0, "admission_start_ns": 1,
         "input_ready_ns": 10, "compile_start_ns": 2, "compile_end_ns": 100,
         "witness_end_ns": 180, "observed_source_tu_seq": 0},
        {"planned_assignment_ordinal": 1, "planned_relationship": 0,
         "planned_admission_lane": 0, "admission_start_ns": 100,
         "input_ready_ns": 110, "compile_start_ns": 101, "compile_end_ns": 200,
         "witness_end_ns": 250, "observed_source_tu_seq": 1},
    ]
    observed = runner._observed_concurrency(rows, runner.TOPOLOGY)
    assert observed["max_concurrent_active_per_relationship"] == 1


def test_live_curve_uses_prefix_wall_makespan_not_sum_of_job_durations() -> None:
    selected = [
        {"compile_start_ns": 110, "compile_end_ns": 300,
         "elapsed_ns": 190, "c_to_f_bytes": 4, "f_to_c_bytes": 6,
         "channel_bytes": 10},
        {"compile_start_ns": 120, "compile_end_ns": 220,
         "elapsed_ns": 100, "c_to_f_bytes": 8, "f_to_c_bytes": 12,
         "channel_bytes": 20},
        {"compile_start_ns": 310, "compile_end_ns": 400,
         "elapsed_ns": 90, "c_to_f_bytes": 12, "f_to_c_bytes": 18,
         "channel_bytes": 30},
    ]
    curve = runner._live_curve_rows(
        selected, [{"tu_id": f"tu-{index}"} for index in range(3)],
        {"corpus": "DuckDB", "profile": "ZSTD_ROUTE", "regime": "cold"}, 100)
    assert [row["cumulative"]["elapsed_ns"] for row in curve] == [200, 200, 300]
    assert curve[-1]["cumulative"]["elapsed_ns"] != sum(
        row["elapsed_ns"] for row in selected)
    assert curve[-1]["cumulative"]["C_TO_F_bytes"] == 24
    assert curve[-1]["cumulative"]["F_TO_C_bytes"] == 36
    assert curve[-1]["cumulative"]["channel_bytes"] == 60


def test_source_mutation_is_rejected_before_launch(tmp_path: Path) -> None:
    batch = _batch(tmp_path)
    rows = runner.load_batch_manifest(batch)
    Path(rows[0]["source"]).write_bytes(b"changed\n")
    with pytest.raises(runner.LiveRunnerError, match="source_digest_mismatch"):
        runner.load_batch_manifest(batch)


def test_predictive_payload_descriptor_is_required(tmp_path: Path) -> None:
    batch = _batch(tmp_path)
    values = [json.loads(line) for line in batch.read_text().splitlines()]
    values[0].pop("predictive_input")
    batch.write_text("".join(json.dumps(value) + "\n" for value in values))
    with pytest.raises(runner.LiveRunnerError, match="fields_invalid"):
        runner.load_batch_manifest(batch)


def test_runner_curve_is_accepted_by_current_live_intake(tmp_path: Path) -> None:
    identity = {"corpus": "DuckDB", "profile": "ZSTD_TU", "regime": "cold",
                "split": "held_out_validation", "run_id": "full-1",
                "source_commit": "a" * 40, "source_tree": "b" * 40,
                "input_digest": "c" * 64, "topology_digest": "d" * 64,
                "model_id": "s8-real-live"}
    curve = [{"step": 0, "tu_id": "tu-0",
              "cell": {"corpus": "DuckDB", "profile": "ZSTD_TU", "regime": "cold"},
              "cumulative": {"channel_bytes": 10, "elapsed_ns": 20,
                              "throughput_bytes_per_s": 500000000.0}}]
    curve_path = tmp_path / "live_curve.jsonl"
    curve_raw = b"".join(canonical_bytes(row) + b"\n" for row in curve)
    curve_path.write_bytes(curve_raw)
    manifest = {"schema": MANIFEST_SCHEMA, "identity": identity,
                "units": {"point": "step", "channel_bytes": "bytes", "elapsed_ns": "ns",
                          "throughput_bytes_per_s": "bytes_per_s"},
                "curve": {"path": curve_path.name, "sha256": hashlib.sha256(curve_raw).hexdigest(),
                          "bytes": len(curve_raw)},
                "provenance": {"mode": "live", "producer": "s8_real_c1f1_live_runner",
                               "trace_free": False}}
    manifest_path = tmp_path / "live_curve_manifest.json"
    manifest_path.write_bytes(canonical_bytes(manifest) + b"\n")
    artifact = _load_live_curve(manifest_path,
                                cell={key: identity[key] for key in ("corpus", "profile", "regime")})
    assert artifact["provenance"]["producer"] == "s8_real_c1f1_live_runner"


def test_finalize_supplies_optional_metadata_to_normalized_live_record(
        tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> None:
    """The terminal path is deletion-sensitive at the normalizer boundary."""
    batch_manifest = tmp_path / "batch.jsonl"
    batch_manifest.write_bytes(b"batch\n")
    topology = tmp_path / "topology.json"
    topology.write_bytes(canonical_bytes({"assignments": []}) + b"\n")
    predictive_plan = tmp_path / "predictive-plan.json"
    predictive_plan.write_bytes(b"{}\n")
    work = tmp_path / "p50compilee2e.run"
    work.mkdir()
    (work / "s7-measured-c-action-trace.jsonl").write_bytes(b"c-action\n")
    (work / "s7-measured-f-action-trace.jsonl").write_bytes(b"f-action\n")
    rows = [{"tu_id": "tu-0"}]
    plan = {"source_manifest": {"sha256": "a" * 64},
            "scheduling": {"topology": runner.TOPOLOGY,
                            "assignments": [{"ordinal": 0, "global_slot": 0}]}}
    plan_sha = hashlib.sha256(predictive_plan.read_bytes()).hexdigest()
    topology_sha = hashlib.sha256(topology.read_bytes()).hexdigest()
    monkeypatch.setattr(runner, "load_predictive_plan",
                        lambda *_args, **_kwargs: (plan, [{"ordinal": 0}], plan_sha))
    monkeypatch.setattr(runner, "load_batch_manifest", lambda *_args: rows)
    monkeypatch.setattr(runner, "bind_batch_to_plan", lambda *_args: None)
    monkeypatch.setattr(runner, "load_topology", lambda *_args: topology_sha)
    monkeypatch.setattr(runner, "_retained_workdir", lambda *_args, **_kwargs: work)
    monkeypatch.setattr(runner, "_environment_preparation", lambda *_args: {})
    monkeypatch.setattr(runner, "_binary_identity", lambda *_args: {"client/icecc": "d" * 64})
    monkeypatch.setattr(runner, "product_identity",
                        lambda *_args: ("e" * 40, "f" * 40,
                                        {"client/icecc": "d" * 64}, "1" * 64))
    monkeypatch.setattr(runner, "_timing_rows", lambda *_args: [])
    monkeypatch.setattr(runner, "_batch_windows",
                        lambda *_args: {"full-1": {"start_ns": 1, "end_ns": 2}})
    monkeypatch.setattr(runner, "_validate_product_log_evidence", lambda *_args: None)
    monkeypatch.setattr(runner, "_action_stage", lambda *_args: [])
    monkeypatch.setattr(
        runner, "_live_curve_rows",
        lambda *_args: [{"step": 0, "tu_id": "tu-0",
                         "cumulative": {"channel_bytes": 1, "elapsed_ns": 1}}])
    stdout = (
        "PASS: all-P50 C1F1\n"
        "S8_BATCH_COUNT=1\n"
        f"S8_SUITE={runner.TOPOLOGY}\n"
        "S8_BATCH_PASSES=1\n"
        "S8_BATCH_WARM=0\n"
        "S8_SCHEDULING mode=relationship-ordered execution_slots=1 relationships=1 "
        "planned_admission_lanes_per_relationship=1\n"
    )
    output = runner.finalize(
        stdout, 0, batch_manifest=batch_manifest, topology=topology,
        predictive_plan=predictive_plan, output=tmp_path / "output",
        profile="ZSTD_TU", product_root=tmp_path, corpus="DuckDB", regime="cold",
        depth="full", full_count=1, passes=1, timestamp="20260830T000000Z")
    records = [json.loads(line) for line in (output / "records.jsonl").read_text().splitlines()]
    assert len(records) == 1
    assert records[0]["record_type"] == "live"
    assert records[0]["provenance"]["producer"] == "s8_real_c1f1_live_runner"


def test_host_descriptor_is_redacted_stable_and_content_addressed(
        tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> None:
    """Host identity is stable while raw machine identifiers never persist."""
    facts = {
        runner.HOST_MACHINE_ID: "machine-secret-1\n",
        runner.HOST_DMI_PRODUCT_UUID: "dmi-secret-1\n",
        runner.HOST_CPUINFO: "vendor_id : GenuineTest\nmodel name : Test CPU\n",
    }

    def read(path: Path, label: str) -> str:
        if path not in facts:
            raise runner.LiveRunnerError(f"missing:{label}")
        return facts[path].strip()

    monkeypatch.setattr(runner, "_read_host_fact", read)
    monkeypatch.setattr(runner.os, "cpu_count", lambda: 8)
    monkeypatch.setattr(runner, "HOST_DMI_BOARD_SERIAL", tmp_path / "missing-board")
    first = runner.capture_host_descriptor(tmp_path / "host-1.json")
    second = runner.capture_host_descriptor(tmp_path / "host-2.json")
    assert first["sha256"] == second["sha256"]
    raw = (tmp_path / "host-1.json").read_text()
    assert "machine-secret-1" not in raw
    assert "dmi-secret-1" not in raw
    assert runner._load_host_descriptor(tmp_path / "host-1.json")[1] == first["sha256"]


def test_host_descriptor_deletion_and_change_are_fail_closed(
        tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> None:
    facts = {
        runner.HOST_MACHINE_ID: "machine-secret-2\n",
        runner.HOST_DMI_PRODUCT_UUID: "dmi-secret-2\n",
        runner.HOST_CPUINFO: "vendor_id : GenuineTest\nmodel name : Test CPU\n",
    }
    monkeypatch.setattr(runner, "_read_host_fact",
                        lambda path, _label: facts[path].strip())
    monkeypatch.setattr(runner.os, "cpu_count", lambda: 8)
    descriptor = tmp_path / "host.json"
    binding = runner.capture_host_descriptor(descriptor)
    value = json.loads(descriptor.read_text())
    del value["facts"]["machine_id_sha256"]
    descriptor.write_text(json.dumps(value, sort_keys=True) + "\n")
    with pytest.raises(runner.LiveRunnerError, match="facts_invalid"):
        runner._load_host_descriptor(descriptor)
    descriptor.unlink()
    runner.capture_host_descriptor(descriptor)
    changed = json.loads(descriptor.read_text())
    changed["facts"]["cpu_count"] = 16
    descriptor.write_text(json.dumps(changed, sort_keys=True) + "\n")
    _value, digest, size = runner._load_host_descriptor(descriptor)
    assert digest != binding["sha256"] or size != binding["bytes"]


def test_host_descriptor_binding_rejects_deletion_and_changed_snapshot(
        tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> None:
    monkeypatch.setattr(runner, "_read_host_fact",
                        lambda path, _label: {
                            runner.HOST_MACHINE_ID: "machine-secret-4",
                            runner.HOST_DMI_PRODUCT_UUID: "dmi-secret-4",
                            runner.HOST_CPUINFO: "vendor_id : GenuineTest\nmodel name : Test CPU",
                        }[path])
    monkeypatch.setattr(runner.os, "cpu_count", lambda: 8)
    descriptor = tmp_path / "host.json"
    captured = runner.capture_host_descriptor(descriptor)
    identity = {"host_descriptor": {"sha256": captured["sha256"],
                                     "bytes": captured["bytes"]}}
    assert runner._authenticated_host_binding(descriptor, identity) == (
        captured["sha256"], captured["bytes"])
    descriptor.unlink()
    with pytest.raises(runner.LiveRunnerError, match="host_descriptor:unavailable"):
        runner._authenticated_host_binding(descriptor, identity)
    runner.capture_host_descriptor(descriptor)
    changed = json.loads(descriptor.read_text())
    changed["facts"]["cpu_count"] = 16
    descriptor.write_text(json.dumps(changed, sort_keys=True) + "\n")
    with pytest.raises(runner.LiveRunnerError,
                       match="host_descriptor_changed_during_run"):
        runner._authenticated_host_binding(descriptor, identity)


def test_calibration_metadata_has_exact_fields_and_no_caller_override(
        tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> None:
    facts = {
        runner.HOST_MACHINE_ID: "machine-secret-3\n",
        runner.HOST_DMI_PRODUCT_UUID: "dmi-secret-3\n",
        runner.HOST_CPUINFO: "vendor_id : GenuineTest\nmodel name : Test CPU\n",
    }
    monkeypatch.setattr(runner, "_read_host_fact",
                        lambda path, _label: facts[path].strip())
    monkeypatch.setattr(runner.os, "cpu_count", lambda: 8)
    descriptor = tmp_path / "host.json"
    runner.capture_host_descriptor(descriptor)
    archive = tmp_path / "work" / "toolchain" / "env.tar.gz"
    archive.parent.mkdir(parents=True)
    archive.write_bytes(b"authenticated compiler environment")
    archive_sha = hashlib.sha256(archive.read_bytes()).hexdigest()
    work = archive.parents[1]
    rows = [{"ordinal": 0, "predictive_input": {"ordinal": 0},
             "compile_db": "/src/compile_commands.json",
             "compile_db_sha256": "a" * 64, "compile_source": "/src/a.cc",
             "compile_output": "/build/a.o"}]
    observations = [{"remote_compile": True}]
    metadata = runner._calibration_metadata(
        runtime_image={"reference": runner.PINNED_IMAGE,
                       "image_id": "sha256:" + "b" * 64,
                       "architecture": "amd64", "os": "linux", "created": "now"},
        preparation={"archive_sha256": archive_sha, "archive_bytes": archive.stat().st_size},
        work=work, rows=rows, observations=observations,
        suite=runner.TOPOLOGY, host_descriptor=descriptor)
    assert set(metadata) == runner.CALIBRATION_METADATA_FIELDS
    assert metadata["ordered_input_class"] == "ordered"
    with pytest.raises(TypeError):
        runner._calibration_metadata(  # type: ignore[call-arg]
            metadata=metadata, runtime_image={}, preparation={}, work=work,
            rows=rows, observations=observations, suite=runner.TOPOLOGY,
            host_descriptor=descriptor)


def _write_legacy_wire_fixture(work: Path, *, mutate: dict[str, object] | None = None) -> None:
    base = {
        "schema": "icecream-p50-legacy-wire-v1", "job_id": 71,
        "assignment_epoch": 73, "assignment_nonce": 79, "c_guid": 83,
        "tu_seq": 0, "c_to_f_sent_bytes": 140,
        "c_to_f_received_bytes": 0, "f_to_c_sent_bytes": 96,
        "f_to_c_received_bytes": 96,
    }
    c_row = {**base, "role": "C", "f_to_c_sent_bytes": 0}
    f_row = {**base, "role": "F", "c_to_f_received_bytes": 140,
             "f_to_c_sent_bytes": 96, "f_to_c_received_bytes": 0,
             "c_to_f_sent_bytes": 0}
    if mutate:
        c_row.update(mutate.get("C", {}))
        f_row.update(mutate.get("F", {}))
    (work / "s7-measured-c-legacy-wire-trace.jsonl").write_text(
        json.dumps(c_row, sort_keys=True) + "\n", encoding="ascii")
    (work / "s7-measured-f-legacy-wire-trace.jsonl").write_text(
        json.dumps(f_row, sort_keys=True) + "\n", encoding="ascii")


def test_legacy_wire_stage_joins_authenticated_c_and_f_totals(tmp_path: Path) -> None:
    _write_legacy_wire_fixture(tmp_path)
    observations = [{"observed_scheduler_job_id": 71,
                     "observed_source_tu_seq": 0}]
    assignments = [{"relationship": 0, "f_slot": 0}]
    rows = runner._legacy_wire_stage(tmp_path, observations, assignments)
    assert rows[0]["c_to_f_bytes"] == 140
    assert rows[0]["f_to_c_bytes"] == 96
    assert rows[0]["channel_bytes"] == 236


def test_raw_ii_method_cannot_enter_calibration_as_zstd_tu() -> None:
    method, product, eligible = runner._measurement_descriptor(
        "ZSTD_TU", runner.RAW_II_PROFILE)
    assert method == runner.RAW_II_PROFILE
    assert product == runner.RAW_II_PROFILE
    assert eligible is False


def test_transfer_accounting_distinguishes_raw_and_compressed_bases() -> None:
    raw = runner._transfer_accounting(runner.RAW_II_PROFILE)
    compressed = runner._transfer_accounting("ZSTD_TU")
    assert raw["basis"] == "framed_application_wire_bytes"
    assert compressed["basis"] == "source_stage_plus_returned_object_payload"
    assert raw != compressed


@pytest.mark.parametrize("mutation", [
    {"C": {"job_id": 72}},
    {"F": {"c_to_f_received_bytes": 139}},
    {"F": {"f_to_c_received_bytes": 1}},
])
def test_legacy_wire_stage_rejects_identity_or_direction_mutation(
        tmp_path: Path, mutation: dict[str, object]) -> None:
    _write_legacy_wire_fixture(tmp_path, mutate=mutation)
    observations = [{"observed_scheduler_job_id": 71,
                     "observed_source_tu_seq": 0}]
    assignments = [{"relationship": 0, "f_slot": 0}]
    with pytest.raises(runner.LiveRunnerError, match="legacy_wire:"):
        runner._legacy_wire_stage(tmp_path, observations, assignments)
