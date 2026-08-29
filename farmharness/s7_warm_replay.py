#!/usr/bin/env python3
"""Create and replay the exact fmt/ZSTD_TU warm S7 cell without Docker."""
from __future__ import annotations

import argparse
import hashlib
import json
import shutil
import subprocess
import sys
from pathlib import Path

CELL = "fmt/ZSTD_TU/warm"

def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()

def rows(path: Path) -> list[dict[str, object]]:
    return [json.loads(line) for line in path.read_bytes().splitlines()]

def write_json(path: Path, value: object) -> None:
    path.write_bytes((json.dumps(value, sort_keys=True, separators=(",", ":")) + "\n").encode())

def write_rows(path: Path, values: list[dict[str, object]]) -> None:
    path.write_bytes(b"".join((json.dumps(value, sort_keys=True, separators=(",", ":")) + "\n").encode() for value in values))

def require_regular(path: Path, label: str) -> None:
    info = path.lstat()
    if not path.is_file() or path.is_symlink() or info.st_nlink != 1:
        raise ValueError(f"{label} is not a private regular file")

def run_sim(sim: Path, scenario: Path, output: Path) -> subprocess.CompletedProcess[str]:
    return subprocess.run([sys.executable, str(sim), "--s7-cell", CELL,
                           "--s7-artifacts", str(scenario), "--s7-output", str(output)],
                          text=True, capture_output=True, check=False)

def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--artifact-root", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--sim", type=Path,
                        default=Path(__file__).resolve().parents[1] / "cache/sim/p50sim")
    args = parser.parse_args(argv)
    artifact, out = args.artifact_root.resolve(), args.out.resolve()
    try:
        if not artifact.is_dir() or artifact.is_symlink() or out.exists():
            raise ValueError("artifact root is unavailable or output already exists")
        source = artifact / "artifacts/source/preprocessed.ii"
        live_c = artifact / "live/c-action-trace-identity-aware.jsonl"
        live_f = artifact / "live/f-action-trace-identity-aware.jsonl"
        for path, label in ((source, "preprocessed input"), (live_c, "live C trace"), (live_f, "live F trace")):
            require_regular(path, label)
        c_live, f_live = rows(live_c), rows(live_f)
        live_rows = c_live + f_live
        c_guid, f_guid = c_live[0]["c_store_guid"], c_live[0]["f_store_guid"]
        begin = next(row for row in live_rows if row["action"] == "TX_BEGIN")
        if any(row.get("c_store_guid") != c_guid or row.get("f_store_guid") != f_guid for row in live_rows):
            raise ValueError("retained live C/F identities disagree")
        identity = {"c_store_guid": c_guid, "f_store_guid": f_guid,
                    "history_nonce": begin["history_nonce"], "prewarm_tu_seq": 0,
                    "measured_tu_seq": 1}
        sim, binary = args.sim.resolve(), args.sim.resolve().with_name(".p50sim.bin")
        require_regular(binary, "product-linked p50sim binary")
        out.mkdir(parents=True)
        scenario, record = out / "scenario", out / "recorded"
        scenario.mkdir(); record.mkdir()
        shutil.copyfile(source, scenario / "preprocessed.ii")
        command = [str(binary), "--prewarm-input", str(scenario / "preprocessed.ii"),
                   "--measured-input", str(scenario / "preprocessed.ii"),
                   "--actions", str(record / "actions.jsonl"),
                   "--prewarm-actions", str(record / "prewarm-actions.jsonl"),
                   "--measured-actions", str(record / "measured-actions.jsonl"),
                   "--summary", str(record / "summary.json"), "--c-store-guid", str(c_guid),
                   "--f-store-guid", str(f_guid), "--history-nonce", str(begin["history_nonce"])]
        recorded = subprocess.run(command, text=True, capture_output=True, check=False)
        if recorded.returncode:
            raise ValueError(f"product warm recording failed: {recorded.stderr.strip()}")
        all_rows, prewarm_rows = rows(record / "actions.jsonl"), rows(record / "prewarm-actions.jsonl")
        measured_rows = rows(record / "measured-actions.jsonl")
        paths = {"actions.jsonl": all_rows, "prewarm-actions.jsonl": prewarm_rows,
                 "measured-actions.jsonl": measured_rows,
                 "prewarm-c-action-trace.jsonl": [r for r in prewarm_rows if r["actor"] == "C"],
                 "prewarm-f-action-trace.jsonl": [r for r in prewarm_rows if r["actor"] == "F"],
                 "measured-c-action-trace.jsonl": [r for r in measured_rows if r["actor"] == "C"],
                 "measured-f-action-trace.jsonl": [r for r in measured_rows if r["actor"] == "F"]}
        for name, value in paths.items():
            write_rows(scenario / name, value)
        ledger = [{"sequence": n, "action": r["action"], "actor": r["actor"], "stage_bytes": r["stage_bytes"],
                   "transaction_digest": r["transaction_digest"], "raw_digest": r["raw_digest"]}
                  for n, r in enumerate(all_rows, 1)]
        write_rows(scenario / "stage-ledger.jsonl", ledger)
        route = {"actions": [r["action"] for r in all_rows], "origin": "live",
                 "schema": "icecream-s7-live-route-trace-v1",
                 "trace": [{"action": r["action"], "sequence": n} for n, r in enumerate(all_rows, 1)]}
        write_json(scenario / "route_trace.json", route)
        scenario_value = {"cell": CELL, "regime": "warm", "schema": "icecream-s7-p50sim-scenario-v1",
                          "input": "preprocessed.ii", "input_sha256": sha256(scenario / "preprocessed.ii"),
                          "prewarm_input": "preprocessed.ii", "prewarm_input_sha256": sha256(scenario / "preprocessed.ii"),
                          "action_trace": "actions.jsonl", "action_trace_sha256": sha256(scenario / "actions.jsonl"),
                          "prewarm_action_trace": "prewarm-actions.jsonl", "measured_action_trace": "measured-actions.jsonl",
                          "prewarm_c_action_trace": "prewarm-c-action-trace.jsonl", "prewarm_f_action_trace": "prewarm-f-action-trace.jsonl",
                          "measured_c_action_trace": "measured-c-action-trace.jsonl", "measured_f_action_trace": "measured-f-action-trace.jsonl",
                          "stage_ledger": "stage-ledger.jsonl", "route_trace": "route_trace.json",
                          "route_trace_sha256": sha256(scenario / "route_trace.json"), "identity": identity}
        write_json(scenario / "scenario.json", scenario_value)
        replay = out / "replay"
        result = run_sim(sim, scenario, replay)
        if result.returncode:
            raise ValueError(f"warm replay failed: {result.stderr.strip()}")
        shutil.copyfile(replay / "identities.json", out / "identities.json")
        shutil.copyfile(replay / "stage-ledger.jsonl", out / "stage-ledger.jsonl")
        shutil.copyfile(scenario / "preprocessed.ii", out / "preprocessed.ii")
        control_scenario = out / "control-scenario"
        shutil.copytree(scenario, control_scenario)
        control_path = control_scenario / "measured-actions.jsonl"
        write_rows(control_path, [r for r in rows(control_path) if r["action"] != "NEED_RECORDED"])
        control = run_sim(sim, control_scenario, out / "control-replay")
        control_log = out / "controls/deletion-measured-trace.log"
        control_log.parent.mkdir()
        control_log.write_text("command=warm p50sim replay with measured NEED_RECORDED deletion\n" +
                               f"returncode={control.returncode}\n{control.stderr}")
        if control.returncode == 0:
            raise ValueError("deletion control unexpectedly passed")
        (out / "commands.log").write_text(
            "record=" + " ".join(command) + "\n"
            "replay=" + " ".join([sys.executable, str(sim), "--s7-cell", CELL,
                                    "--s7-artifacts", str(scenario), "--s7-output", str(replay)]) + "\n"
            "control=warm p50sim replay with measured NEED_RECORDED deletion\n")
        write_json(out / "manifest.json", {"schema": "icecream-s7-fmt-zstd-tu-warm-conformance-v1",
                                            "cell": CELL, "status": "PASS", "input_sha256": sha256(source),
                                            "identity": identity, "action_count": len(all_rows),
                                            "prewarm_action_count": len(prewarm_rows), "measured_action_count": len(measured_rows),
                                            "stage_ledger_count": len(ledger), "deletion_control": {"status": "PASS", "returncode": control.returncode}})
        return 0
    except (OSError, StopIteration, ValueError, json.JSONDecodeError) as error:
        print(f"s7_warm_replay: {error}", file=sys.stderr)
        return 1

if __name__ == "__main__":
    raise SystemExit(main())
