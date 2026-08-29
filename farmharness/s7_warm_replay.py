#!/usr/bin/env python3
"""Replay explicit live captures for the ZSTD_TU S7 cell matrix.

The runner is intentionally replay-only: it never invokes the product binary
to manufacture an expected trace.  Its capture arguments must point at the
retained preprocessed input(s) and role-local C/F traces from one live run.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import shutil
import subprocess
import sys
from pathlib import Path


SUPPORTED_CELLS = frozenset(
    f"{corpus}/ZSTD_TU/{regime}"
    for corpus in ("fmt", "RocksDB")
    for regime in ("cold", "warm")
)


def schema_for_cell(cell: str) -> str:
    corpus, profile, regime = cell.split("/")
    return f"icecream-s7-{corpus.lower()}-{profile.lower().replace('_', '-')}-{regime}-conformance-v2"


class Hold(Exception):
    pass


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


def read_input(path: Path, label: str) -> tuple[str, bytes]:
    """Snapshot one explicit live input before constructing the scenario."""
    try:
        info = path.lstat()
    except OSError as error:
        raise Hold(f"missing explicit live {label}: {path}") from error
    if not path.is_file() or path.is_symlink() or info.st_nlink != 1:
        raise Hold(f"live {label} is not a private regular file: {path}")
    try:
        value = path.read_bytes()
    except OSError as error:
        raise Hold(f"cannot read explicit live {label}: {path}") from error
    return hashlib.sha256(value).hexdigest(), value


def read_rows(path: Path, label: str, actor: str | None) -> list[dict[str, object]]:
    try:
        info = path.lstat()
    except OSError as error:
        raise Hold(f"missing explicit live {label}: {path}") from error
    if not path.is_file() or path.is_symlink() or info.st_nlink != 1:
        raise Hold(f"live {label} is not a private regular file: {path}")
    try:
        values = [json.loads(line) for line in path.read_bytes().splitlines()]
    except (OSError, json.JSONDecodeError) as error:
        raise ValueError(f"live {label} is not valid JSONL") from error
    if not values or any(not isinstance(value, dict) for value in values):
        raise ValueError(f"live {label} is empty or not object JSONL")
    if actor is not None and any(value.get("actor") != actor for value in values):
        raise ValueError(f"live {label} contains a non-{actor} action")
    return values


def combine(c_rows: list[dict[str, object]], f_rows: list[dict[str, object]]) -> list[dict[str, object]]:
    """Merge two already-captured role traces at Protocol-50 boundaries."""
    merged: list[dict[str, object]] = []
    ci = fi = 0

    def transaction(row: dict[str, object]) -> bool:
        return row.get("action") in {
            "TX_BEGIN", "ACTIVE_REPLAYED", "DICT_COMPLETE", "NEED_RECORDED",
            "OBJECT_APPLIED", "BODY_COMPLETE", "INPUT_MATERIALIZED",
            "INPUT_COMMITTED", "COMMIT_ACCEPTED", "LOST_COMMIT_ACCEPTED",
        }

    while ci < len(c_rows) or fi < len(f_rows):
        c = c_rows[ci] if ci < len(c_rows) else None
        f = f_rows[fi] if fi < len(f_rows) else None
        if f is None:
            merged.append(c); ci += 1; continue
        if c is None:
            merged.append(f); fi += 1; continue
        ca, fa = c.get("action"), f.get("action")
        if ca == "TX_BEGIN" and fa == "SESSION_OPENED" and f.get("session_serial", 0) > 1:
            merged.append(c); ci += 1; continue
        if fa in {"SESSION_OPENED", "SESSION_REPLACED", "HISTORY_RESET"}:
            merged.append(f); fi += 1; continue
        if fa == "SESSION_DISCONNECTED":
            if ca in {"COMMIT_ACCEPTED", "LOST_COMMIT_ACCEPTED"}:
                merged.append(c); ci += 1
            else:
                merged.append(f); fi += 1
            continue
        if ca == "TX_BEGIN" and fa == "TX_BEGIN":
            if c.get("transaction_digest") == f.get("transaction_digest"):
                merged.append(c); ci += 1
            else:
                merged.append(f); fi += 1
            continue
        if ca in {"COMMIT_ACCEPTED", "LOST_COMMIT_ACCEPTED"} and transaction(f):
            merged.append(f); fi += 1; continue
        if ca == "TX_BEGIN" and transaction(f):
            merged.append(f); fi += 1; continue
        if ca in {"COMMIT_ACCEPTED", "LOST_COMMIT_ACCEPTED"}:
            merged.append(c); ci += 1; continue
        if ca == "TX_BEGIN":
            merged.append(c); ci += 1
        else:
            merged.append(f); fi += 1
    return merged


def write_json(path: Path, value: object) -> None:
    path.write_bytes((json.dumps(value, sort_keys=True, separators=(",", ":")) + "\n").encode())


def write_rows(path: Path, values: list[dict[str, object]]) -> None:
    path.write_bytes(b"".join((json.dumps(value, sort_keys=True, separators=(",", ":")) + "\n").encode() for value in values))


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cell", choices=sorted(SUPPORTED_CELLS), required=True)
    parser.add_argument("--input", type=Path, help="explicit measured input for a cold cell")
    parser.add_argument("--c-trace", type=Path, help="explicit measured C trace for a cold cell")
    parser.add_argument("--f-trace", type=Path, help="explicit measured F trace for a cold cell")
    for name in ("prewarm-input", "measured-input", "prewarm-c-trace", "prewarm-f-trace",
                 "measured-c-trace", "measured-f-trace"):
        parser.add_argument("--" + name, type=Path)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--sim", type=Path,
                        default=Path(__file__).resolve().parents[1] / "cache/sim/p50sim")
    args = parser.parse_args(argv)
    corpus, _profile, regime = args.cell.split("/")
    warm = regime == "warm"
    warm_names = ("prewarm_input", "measured_input", "prewarm_c_trace", "prewarm_f_trace",
                  "measured_c_trace", "measured_f_trace")
    cold_names = ("input", "c_trace", "f_trace")
    required = warm_names if warm else cold_names
    supplied = {name: getattr(args, name) for name in warm_names + cold_names}
    missing = [name for name in required if supplied[name] is None]
    extra = [name for name in (cold_names if warm else warm_names) if supplied[name] is not None]
    if missing or extra:
        parser.error(f"{args.cell} requires {', '.join(required)}" +
                     (f"; unexpected {', '.join(extra)}" if extra else ""))
    out = args.out.resolve()
    try:
        if out.exists():
            raise ValueError("output already exists")
        if warm:
            pre_c = read_rows(args.prewarm_c_trace, "prewarm C trace", "C")
            pre_f = read_rows(args.prewarm_f_trace, "prewarm F trace", "F")
            meas_c = read_rows(args.measured_c_trace, "measured C trace", "C")
            meas_f = read_rows(args.measured_f_trace, "measured F trace", "F")
            prewarm_input_sha, prewarm_input_bytes = read_input(args.prewarm_input, "prewarm input")
            measured_input_sha, measured_input_bytes = read_input(args.measured_input, "measured input")
            prewarm = combine(pre_c, pre_f)
        else:
            pre_c = pre_f = []
            meas_c = read_rows(args.c_trace, "measured C trace", "C")
            meas_f = read_rows(args.f_trace, "measured F trace", "F")
            prewarm_input_sha = prewarm_input_bytes = None
            measured_input_sha, measured_input_bytes = read_input(args.input, "measured input")
            prewarm = []
        measured = combine(meas_c, meas_f)
        all_rows = prewarm + measured
        if not all_rows:
            raise ValueError("explicit live traces produced no actions")
        begins = [row for row in all_rows if row.get("action") == "TX_BEGIN"]
        expected_tu = [0, 0, 1, 1] if warm else [0, 0]
        if len(begins) != len(expected_tu) or [row.get("tu_seq") for row in begins] != expected_tu:
            raise ValueError("explicit live traces do not contain the required TU sequence")
        c_guid, f_guid = begins[0].get("c_store_guid"), begins[0].get("f_store_guid")
        nonce = begins[0].get("history_nonce")
        if not isinstance(c_guid, str) or not isinstance(f_guid, str) or not isinstance(nonce, int) or nonce <= 0:
            raise ValueError("explicit live traces have incomplete identity")
        if any(row.get("c_store_guid") != c_guid or row.get("f_store_guid") != f_guid for row in all_rows):
            raise ValueError("explicit live traces disagree on C/F identity")
        sim, binary = args.sim.resolve(), args.sim.resolve().with_name(".p50sim.bin")
        if not binary.is_file() or binary.is_symlink():
            raise Hold(f"product-linked simulator is not built: {binary}")
        out.mkdir(parents=True)
        scenario = out / "scenario"
        scenario.mkdir()
        if warm:
            (scenario / "prewarm.ii").write_bytes(prewarm_input_bytes)
        (scenario / "measured.ii").write_bytes(measured_input_bytes)
        paths = {"actions.jsonl": all_rows, "prewarm-actions.jsonl": prewarm,
                 "measured-actions.jsonl": measured, "measured-c-action-trace.jsonl": meas_c,
                 "measured-f-action-trace.jsonl": meas_f}
        if warm:
            paths.update({"prewarm-c-action-trace.jsonl": pre_c, "prewarm-f-action-trace.jsonl": pre_f})
        for name, value in paths.items():
            write_rows(scenario / name, value)
        ledger = [{"sequence": n, "action": row.get("action"), "actor": row.get("actor"),
                   "stage_bytes": row.get("stage_bytes"), "transaction_digest": row.get("transaction_digest"),
                   "raw_digest": row.get("raw_digest")} for n, row in enumerate(all_rows, 1)]
        write_rows(scenario / "stage-ledger.jsonl", ledger)
        route = {"actions": [row.get("action") for row in all_rows], "origin": "live",
                 "schema": "icecream-s7-live-route-trace-v1",
                 "trace": [{"action": row.get("action"), "sequence": n} for n, row in enumerate(all_rows, 1)]}
        write_json(scenario / "route_trace.json", route)
        identity = {"c_store_guid": c_guid, "f_store_guid": f_guid, "history_nonce": nonce,
                    "prewarm_tu_seq": 0, "measured_tu_seq": 1 if warm else 0}
        scenario_value = {
            "cell": args.cell, "regime": regime, "schema": "icecream-s7-p50sim-scenario-v1",
            "input": "measured.ii", "input_sha256": measured_input_sha,
            "action_trace": "actions.jsonl", "action_trace_sha256": sha256(scenario / "actions.jsonl"),
            "measured_action_trace": "measured-actions.jsonl",
            "stage_ledger": "stage-ledger.jsonl", "route_trace": "route_trace.json",
            "route_trace_sha256": sha256(scenario / "route_trace.json"), "identity": identity,
        }
        if warm:
            scenario_value.update({
                "prewarm_input": "prewarm.ii", "prewarm_input_sha256": prewarm_input_sha,
                "prewarm_action_trace": "prewarm-actions.jsonl",
                "prewarm_c_action_trace": "prewarm-c-action-trace.jsonl",
                "prewarm_f_action_trace": "prewarm-f-action-trace.jsonl",
            })
        scenario_value.update({"measured_c_action_trace": "measured-c-action-trace.jsonl",
                               "measured_f_action_trace": "measured-f-action-trace.jsonl"})
        write_json(scenario / "scenario.json", scenario_value)
        replay = out / "replay"
        replay_command = [sys.executable, str(sim), "--s7-cell", args.cell,
                          "--s7-artifacts", str(scenario), "--s7-output", str(replay)]
        result = subprocess.run(replay_command, text=True, capture_output=True, check=False)
        if result.returncode:
            raise ValueError(f"{regime} replay failed: {result.stderr.strip()}")
        shutil.copyfile(replay / "identities.json", out / "identities.json")
        shutil.copyfile(replay / "stage-ledger.jsonl", out / "stage-ledger.jsonl")
        if warm:
            (out / "prewarm.ii").write_bytes(prewarm_input_bytes)
        (out / "measured.ii").write_bytes(measured_input_bytes)
        for name in ("measured-c-action-trace.jsonl", "measured-f-action-trace.jsonl"):
            shutil.copyfile(scenario / name, out / name)
        if warm:
            for name in ("prewarm-c-action-trace.jsonl", "prewarm-f-action-trace.jsonl"):
                shutil.copyfile(scenario / name, out / name)
        control_scenario = out / "control-scenario"
        shutil.copytree(scenario, control_scenario)
        control_path = control_scenario / "measured-actions.jsonl"
        write_rows(control_path, [row for row in read_rows(control_path, "control measured trace", None)
                                  if row.get("action") != "NEED_RECORDED"])
        control_command = [sys.executable, str(sim), "--s7-cell", args.cell,
                           "--s7-artifacts", str(control_scenario),
                           "--s7-output", str(out / "control-replay")]
        control = subprocess.run(control_command, text=True, capture_output=True, check=False)
        (out / "controls").mkdir()
        (out / "controls/deletion-measured-trace.log").write_text(
            f"command={' '.join(control_command)}\nreturncode={control.returncode}\n{control.stderr}")
        if control.returncode == 0:
            raise ValueError("deletion control unexpectedly passed")
        (out / "commands.log").write_text(
            f"replay={' '.join(replay_command)}\ncontrol={' '.join(control_command)}\n")
        write_json(out / "manifest.json", {
            "schema": schema_for_cell(args.cell), "cell": args.cell, "profile": "ZSTD_TU",
            "regime": regime, "status": "PASS", "corpus": corpus,
            "input_sha256": measured_input_sha,
            **({"prewarm_input_sha256": prewarm_input_sha} if warm else {}),
            "identity": identity, "action_count": len(all_rows),
            "prewarm_action_count": len(prewarm), "measured_action_count": len(measured),
            "stage_ledger_count": len(ledger),
            "deletion_control": {"status": "PASS", "returncode": control.returncode},
        })
        return 0
    except Hold as error:
        print(f"HOLD: {error}", file=sys.stderr)
        return 77
    except (OSError, StopIteration, ValueError, json.JSONDecodeError) as error:
        print(f"s7_warm_replay: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
