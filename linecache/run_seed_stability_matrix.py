#!/usr/bin/env python3
"""Run the balanced C-only-seed reorder and input-change matrix."""

from __future__ import annotations

import argparse
import concurrent.futures
import json
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Sequence


CORPORA = (
    "llvm",
    "rocksdb",
    "duckdb",
    "abseil",
    "opencv",
    "godot",
    "fmt",
    "spdlog",
    "catch2",
    "nlohmann-json",
    "range-v3",
    "eigen",
    "re2",
    "leveldb",
    "simdjson",
    "cereal",
)

SCENARIOS = {
    "standard": (),
    "reverse": ("--order", "reverse"),
    "shuffle1": ("--order", "shuffled", "--order-seed", "1"),
    "shuffle2": ("--order", "shuffled", "--order-seed", "2"),
    "shuffle3": ("--order", "shuffled", "--order-seed", "3"),
    "perturb": ("--perturb-shared-region",),
}


@dataclass(frozen=True)
class Task:
    corpus: str
    scenario: str
    report: Path
    curve: Path
    log: Path


def package_for(repo: Path, corpus: str) -> Path:
    artifact = (
        "online-bootstrap-common-llvm-godot.zst"
        if corpus in ("rocksdb", "opencv")
        else "online-bootstrap-common-rocks-opencv.zst"
    )
    return repo / "linecache" / "ml-artifacts" / artifact


def report_is_complete(task: Task, expected_tus: int) -> bool:
    if not task.report.exists() or not task.curve.exists():
        return False
    try:
        report = json.loads(task.report.read_text())
    except (OSError, json.JSONDecodeError):
        return False
    rows = {row["name"]: row for row in report.get("rows", ())}
    required = ("empty-online-k2", "pretrained-seed-online-k2")
    target = report.get("target", {})
    expected_order = (
        "reverse"
        if task.scenario == "reverse"
        else "shuffled"
        if task.scenario.startswith("shuffle")
        else "standard"
    )
    expected_seed = (
        int(task.scenario.removeprefix("shuffle"))
        if task.scenario.startswith("shuffle")
        else 5_353_740
    )
    return (
        set(rows) == set(required)
        and all(row.get("exact") for row in rows.values())
        and len({row.get("tus") for row in rows.values()}) == 1
        and next(iter(rows.values())).get("tus") == target.get("tus")
        and target.get("tus", expected_tus + 1) <= expected_tus
        and report.get("max_tus") == expected_tus
        and target.get("order") == expected_order
        and target.get("order_seed") == expected_seed
        and target.get("perturb_shared_region")
        == (task.scenario == "perturb")
        and report.get("row_set") == "online"
        and report.get("pretrained_mode") == "seed-only"
    )


def command(repo: Path, task: Task, max_tus: int) -> list[str]:
    return [
        sys.executable,
        str(repo / "linecache" / "online_bootstrap_curves.py"),
        "--package-in",
        str(package_for(repo, task.corpus)),
        "--test",
        str(repo / "linecache" / "traces" / f"ml-{task.corpus}.bin"),
        "--online-budget",
        "524288",
        "--thresholds",
        "2",
        "--level",
        "3",
        "--model-level",
        "3",
        "--publication",
        "first-use",
        "--budget-basis",
        "ids32",
        "--row-set",
        "online",
        "--pretrained-mode",
        "seed-only",
        "--max-tus",
        str(max_tus),
        *SCENARIOS[task.scenario],
        "--curve-tsv",
        str(task.curve),
        "--report",
        str(task.report),
    ]


def run_task(repo: Path, task: Task, max_tus: int, force: bool) -> str:
    if not force and report_is_complete(task, max_tus):
        return f"SKIP {task.corpus}/{task.scenario} {task.report}"
    with task.log.open("w") as output:
        result = subprocess.run(
            command(repo, task, max_tus),
            cwd=repo,
            stdout=output,
            stderr=subprocess.STDOUT,
            text=True,
            check=False,
        )
    if result.returncode:
        raise RuntimeError(
            f"{task.corpus}/{task.scenario} exited {result.returncode}; "
            f"see {task.log}"
        )
    if not report_is_complete(task, max_tus):
        raise RuntimeError(
            f"{task.corpus}/{task.scenario} produced an incomplete report; "
            f"see {task.log}"
        )
    return f"PASS {task.corpus}/{task.scenario} {task.report}"


def parse_args(argv: Sequence[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--corpora", nargs="+", choices=CORPORA, default=CORPORA)
    parser.add_argument(
        "--scenarios",
        nargs="+",
        choices=tuple(SCENARIOS),
        default=tuple(SCENARIOS),
    )
    parser.add_argument(
        "--output-dir",
        type=Path,
        default=Path("/tmp/online-bootstrap-seed-stability"),
    )
    parser.add_argument("--max-tus", type=int, default=200)
    parser.add_argument("--jobs", type=int, default=4)
    parser.add_argument("--force", action="store_true")
    return parser.parse_args(argv)


def main(argv: Sequence[str] | None = None) -> int:
    args = parse_args(argv)
    if args.max_tus <= 0 or args.jobs <= 0:
        raise ValueError("max-tus and jobs must be positive")
    repo = Path(__file__).resolve().parent.parent
    args.output_dir.mkdir(parents=True, exist_ok=True)
    tasks = [
        Task(
            corpus,
            scenario,
            args.output_dir / f"{corpus}-{scenario}.json",
            args.output_dir / f"{corpus}-{scenario}.tsv",
            args.output_dir / f"{corpus}-{scenario}.log",
        )
        for scenario in args.scenarios
        for corpus in args.corpora
    ]
    failures: list[str] = []
    with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as executor:
        pending = {
            executor.submit(run_task, repo, task, args.max_tus, args.force): task
            for task in tasks
        }
        for future in concurrent.futures.as_completed(pending):
            task = pending[future]
            try:
                print(future.result(), flush=True)
            except Exception as error:  # report all completed lanes before exiting
                message = f"FAIL {task.corpus}/{task.scenario}: {error}"
                failures.append(message)
                print(message, flush=True)
    if failures:
        print(json.dumps({"failures": failures}, indent=2), file=sys.stderr)
        return 1
    print(
        json.dumps(
            {
                "corpora": len(args.corpora),
                "scenarios": list(args.scenarios),
                "runs": len(tasks),
                "output_dir": str(args.output_dir),
            },
            sort_keys=True,
        )
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
