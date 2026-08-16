#!/usr/bin/env python3
"""Run exact control/prior-root comparisons across balanced corpora and orders."""

from __future__ import annotations

import argparse
import concurrent.futures
import json
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Sequence

from run_seed_stability_matrix import CORPORA, package_for


SCENARIOS = {
    "standard": (),
    "reverse": ("--order", "reverse"),
    "shuffle1": ("--order", "shuffled", "--order-seed", "1"),
    "perturb": ("--perturb-shared-region",),
}
MODES = ("control", "root")
ROW = "pretrained-seed-online-k2"


@dataclass(frozen=True)
class Task:
    corpus: str
    scenario: str
    mode: str
    report: Path
    curve: Path
    log: Path


def report_is_complete(task: Task, expected_tus: int, stride: int) -> bool:
    if not task.report.exists() or not task.curve.exists():
        return False
    try:
        report = json.loads(task.report.read_text())
    except (OSError, json.JSONDecodeError):
        return False
    rows = {row["name"]: row for row in report.get("rows", ())}
    row = rows.get(ROW)
    target = report.get("target", {})
    expected_order = (
        "reverse"
        if task.scenario == "reverse"
        else "shuffled"
        if task.scenario == "shuffle1"
        else "standard"
    )
    return bool(
        row
        and set(rows) == {ROW}
        and row.get("exact")
        and row.get("tus") == target.get("tus")
        and (expected_tus == 0 or target.get("tus", expected_tus + 1) <= expected_tus)
        and report.get("max_tus") == expected_tus
        and target.get("order") == expected_order
        and target.get("order_seed") == (1 if task.scenario == "shuffle1" else 5_353_740)
        and target.get("perturb_shared_region") == (task.scenario == "perturb")
        and report.get("row_set") == "pretrained"
        and report.get("pretrained_mode") == "seed-only"
        and report.get("phrase_scope") == "run-vs-tu-match-hybrid"
        and report.get("prior_root_copy") == (task.mode == "root")
        and report.get("root_copy_index_stride") == stride
        and row.get("prior_root_copy") == (task.mode == "root")
    )


def command(repo: Path, task: Task, max_tus: int, stride: int) -> list[str]:
    value = [
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
        "pretrained",
        "--pretrained-mode",
        "seed-only",
        "--phrase-scope",
        "run-vs-tu-match-hybrid",
        "--max-tus",
        str(max_tus),
        "--root-copy-index-stride",
        str(stride),
        *SCENARIOS[task.scenario],
        "--curve-tsv",
        str(task.curve),
        "--report",
        str(task.report),
    ]
    if task.mode == "root":
        value.append("--prior-root-copy")
    return value


def run_task(
    repo: Path,
    task: Task,
    max_tus: int,
    stride: int,
    force: bool,
) -> str:
    if not force and report_is_complete(task, max_tus, stride):
        return f"SKIP {task.corpus}/{task.scenario}/{task.mode}"
    with task.log.open("w") as output:
        result = subprocess.run(
            command(repo, task, max_tus, stride),
            cwd=repo,
            stdout=output,
            stderr=subprocess.STDOUT,
            text=True,
            check=False,
        )
    if result.returncode:
        raise RuntimeError(f"exit {result.returncode}; see {task.log}")
    if not report_is_complete(task, max_tus, stride):
        raise RuntimeError(f"incomplete report; see {task.log}")
    return f"PASS {task.corpus}/{task.scenario}/{task.mode}"


def parse_args(argv: Sequence[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--corpora", nargs="+", choices=CORPORA, default=CORPORA)
    parser.add_argument(
        "--scenarios",
        nargs="+",
        choices=tuple(SCENARIOS),
        default=("standard",),
    )
    parser.add_argument("--modes", nargs="+", choices=MODES, default=MODES)
    parser.add_argument("--max-tus", type=int, default=200)
    parser.add_argument("--root-copy-index-stride", type=int, default=8)
    parser.add_argument("--jobs", type=int, default=4)
    parser.add_argument(
        "--output-dir",
        type=Path,
        default=Path("/tmp/issue16-prior-root-matrix"),
    )
    parser.add_argument("--force", action="store_true")
    return parser.parse_args(argv)


def main(argv: Sequence[str] | None = None) -> int:
    args = parse_args(argv)
    if args.max_tus < 0 or args.jobs <= 0 or args.root_copy_index_stride <= 0:
        raise ValueError("max-tus must be nonnegative; job and stride controls positive")
    repo = Path(__file__).resolve().parent.parent
    args.output_dir.mkdir(parents=True, exist_ok=True)
    tasks = [
        Task(
            corpus,
            scenario,
            mode,
            args.output_dir / f"{corpus}-{scenario}-{mode}.json",
            args.output_dir / f"{corpus}-{scenario}-{mode}.tsv",
            args.output_dir / f"{corpus}-{scenario}-{mode}.log",
        )
        for scenario in args.scenarios
        for corpus in args.corpora
        for mode in args.modes
    ]
    failures: list[str] = []
    with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as executor:
        pending = {
            executor.submit(
                run_task,
                repo,
                task,
                args.max_tus,
                args.root_copy_index_stride,
                args.force,
            ): task
            for task in tasks
        }
        for future in concurrent.futures.as_completed(pending):
            task = pending[future]
            try:
                print(future.result(), flush=True)
            except Exception as error:  # finish independent lanes and report every failure
                message = (
                    f"FAIL {task.corpus}/{task.scenario}/{task.mode}: {error}"
                )
                failures.append(message)
                print(message, flush=True)
    if failures:
        print(json.dumps({"failures": failures}, indent=2), file=sys.stderr)
        return 1
    print(
        json.dumps(
            {
                "corpora": len(args.corpora),
                "scenarios": args.scenarios,
                "modes": args.modes,
                "runs": len(tasks),
                "max_tus": args.max_tus,
                "root_copy_index_stride": args.root_copy_index_stride,
                "output_dir": str(args.output_dir),
            },
            sort_keys=True,
        )
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
