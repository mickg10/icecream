#!/usr/bin/env python3
"""Replay explicit live captures for the fmt/ZSTD_TU warm S7 cell.

The runner is intentionally replay-only: it never invokes the product binary
to manufacture an expected trace.  Its six capture arguments must point at
the retained preprocessed inputs and role-local C/F traces from one live run.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import shutil
import subprocess
import sys
from pathlib import Path


CELL = "fmt/ZSTD_TU/warm"
SCHEMA = "icecream-s7-fmt-zstd-tu-warm-conformance-v2"


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
    for name in ("prewarm-input", "measured-input", "prewarm-c-trace", "prewarm-f-trace",
                 "measured-c-trace", "measured-f-trace"):
        parser.add_argument("--" + name, type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--sim", type=Path,
                        default=Path(__file__).resolve().parents[1] / "cache/sim/p50sim")
    args = parser.parse_args(argv)
    out = args.out.resolve()
    try:
        if out.exists():
            raise ValueError("output already exists")
        pre_c = read_rows(args.prewarm_c_trace, "prewarm C trace", "C")
        pre_f = read_rows(args.prewarm_f_trace, "prewarm F trace", "F")
        meas_c = read_rows(args.measured_c_trace, "measured C trace", "C")
        meas_f = read_rows(args.measured_f_trace, "measured F trace", "F")
        prewarm_input_sha, prewarm_input_bytes = read_input(args.prewarm_input, "prewarm input")
        measured_input_sha, measured_input_bytes = read_input(args.measured_input, "measured input")
        prewarm = combine(pre_c, pre_f)
        measured = combine(meas_c, meas_f)
        all_rows = prewarm + measured
        if not all_rows:
            raise ValueError("explicit live traces produced no actions")
        begins = [row for row in all_rows if row.get("action") == "TX_BEGIN"]
        if len(begins) != 4 or [row.get("tu_seq") for row in begins] != [0, 0, 1, 1]:
            raise ValueError("explicit live traces do not contain TU0 then TU1")
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
        (scenario / "prewarm.ii").write_bytes(prewarm_input_bytes)
        (scenario / "measured.ii").write_bytes(measured_input_bytes)
        for name, value in {
            "actions.jsonl": all_rows, "prewarm-actions.jsonl": prewarm, "measured-actions.jsonl": measured,
            "prewarm-c-action-trace.jsonl": pre_c, "prewarm-f-action-trace.jsonl": pre_f,
            "measured-c-action-trace.jsonl": meas_c, "measured-f-action-trace.jsonl": meas_f,
        }.items():
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
                    "prewarm_tu_seq": 0, "measured_tu_seq": 1}
        write_json(scenario / "scenario.json", {
            "cell": CELL, "regime": "warm", "schema": "icecream-s7-p50sim-scenario-v1",
            "input": "measured.ii", "input_sha256": measured_input_sha,
            "prewarm_input": "prewarm.ii", "prewarm_input_sha256": prewarm_input_sha,
            "action_trace": "actions.jsonl", "action_trace_sha256": sha256(scenario / "actions.jsonl"),
            "prewarm_action_trace": "prewarm-actions.jsonl", "measured_action_trace": "measured-actions.jsonl",
            "prewarm_c_action_trace": "prewarm-c-action-trace.jsonl", "prewarm_f_action_trace": "prewarm-f-action-trace.jsonl",
            "measured_c_action_trace": "measured-c-action-trace.jsonl", "measured_f_action_trace": "measured-f-action-trace.jsonl",
            "stage_ledger": "stage-ledger.jsonl", "route_trace": "route_trace.json",
            "route_trace_sha256": sha256(scenario / "route_trace.json"), "identity": identity,
        })
        replay = out / "replay"
        result = subprocess.run([sys.executable, str(sim), "--s7-cell", CELL,
                                 "--s7-artifacts", str(scenario), "--s7-output", str(replay)],
                                text=True, capture_output=True, check=False)
        if result.returncode:
            raise ValueError(f"warm replay failed: {result.stderr.strip()}")
        shutil.copyfile(replay / "identities.json", out / "identities.json")
        shutil.copyfile(replay / "stage-ledger.jsonl", out / "stage-ledger.jsonl")
        (out / "prewarm.ii").write_bytes(prewarm_input_bytes)
        (out / "measured.ii").write_bytes(measured_input_bytes)
        for name in ("prewarm-c-action-trace.jsonl", "prewarm-f-action-trace.jsonl",
                     "measured-c-action-trace.jsonl", "measured-f-action-trace.jsonl"):
            shutil.copyfile(scenario / name, out / name)
        control_scenario = out / "control-scenario"
        shutil.copytree(scenario, control_scenario)
        control_path = control_scenario / "measured-actions.jsonl"
        write_rows(control_path, [row for row in read_rows(control_path, "control measured trace", None)
                                  if row.get("action") != "NEED_RECORDED"])
        # The control is expected to fail at the measured action equality gate.
        control = subprocess.run([sys.executable, str(sim), "--s7-cell", CELL,
                                  "--s7-artifacts", str(control_scenario), "--s7-output", str(out / "control-replay")],
                                 text=True, capture_output=True, check=False)
        (out / "controls").mkdir()
        (out / "controls/deletion-measured-trace.log").write_text(
            "command=warm p50sim replay with measured NEED_RECORDED deletion\n"
            f"returncode={control.returncode}\n{control.stderr}")
        if control.returncode == 0:
            raise ValueError("deletion control unexpectedly passed")
        (out / "commands.log").write_text(
            "replay=" + " ".join([sys.executable, str(sim), "--s7-cell", CELL,
                                    "--s7-artifacts", str(scenario), "--s7-output", str(replay)]) + "\n"
            "control=warm p50sim replay with measured NEED_RECORDED deletion\n")
        write_json(out / "manifest.json", {"schema": SCHEMA, "cell": CELL, "status": "PASS",
                                            "prewarm_input_sha256": prewarm_input_sha,
                                            "measured_input_sha256": measured_input_sha,
                                            "identity": identity, "action_count": len(all_rows),
                                            "prewarm_action_count": len(prewarm), "measured_action_count": len(measured),
                                            "stage_ledger_count": len(ledger),
                                            "deletion_control": {"status": "PASS", "returncode": control.returncode}})
        return 0
    except Hold as error:
        print(f"HOLD: {error}", file=sys.stderr)
        return 77
    except (OSError, StopIteration, ValueError, json.JSONDecodeError) as error:
        print(f"s7_warm_replay: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
