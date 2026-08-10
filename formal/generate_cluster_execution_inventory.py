#!/usr/bin/env python3
"""Generate a complete repository-derived execution and cluster inventory."""

from __future__ import annotations

import argparse
import json
import os
import re
import shlex
import stat
import subprocess
from dataclasses import asdict, dataclass
from pathlib import Path
from typing import Any


@dataclass(frozen=True)
class TestFile:
    path: str
    executable: bool
    kind: str


@dataclass(frozen=True)
class MakeVariable:
    file: str
    variable: str
    values: tuple[str, ...]


ACCEPTED_SCHEDCREDIT_MODES = (
    "credit",
    "report",
    "retention",
    "noreader",
    "clientstall",
    "mixedrole",
    "clamp",
    "relisten",
)


def run(repo: Path, *command: str, check: bool = True) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        list(command),
        cwd=repo,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=check,
    )


def revision(repo: Path) -> str:
    return run(repo, "git", "rev-parse", "HEAD").stdout.strip()


def logical_make_lines(text: str) -> list[str]:
    lines: list[str] = []
    pending = ""
    for raw in text.splitlines():
        line = raw.split("#", 1)[0].rstrip()
        if not line and not pending:
            continue
        if line.endswith("\\"):
            pending += line[:-1] + " "
            continue
        lines.append((pending + line).strip())
        pending = ""
    if pending:
        lines.append(pending.strip())
    return lines


def make_variables(repo: Path) -> list[MakeVariable]:
    interesting = re.compile(
        r"^([A-Za-z0-9_]*(?:TESTS|check_PROGRAMS|noinst_PROGRAMS|bin_PROGRAMS))\s*(?:\+?=)\s*(.*)$"
    )
    results: list[MakeVariable] = []
    for path in sorted(repo.rglob("Makefile.am")):
        if ".git" in path.parts:
            continue
        for line in logical_make_lines(path.read_text(encoding="utf-8", errors="replace")):
            match = interesting.match(line)
            if not match:
                continue
            values = tuple(
                token
                for token in shlex.split(match.group(2))
                if token and not token.startswith("$(")
            )
            if values:
                results.append(
                    MakeVariable(
                        file=str(path.relative_to(repo)),
                        variable=match.group(1),
                        values=values,
                    )
                )
    return results


def test_files(repo: Path) -> list[TestFile]:
    roots = [repo / name for name in ("unittests", "tests") if (repo / name).is_dir()]
    results: list[TestFile] = []
    for root in roots:
        for path in sorted(root.rglob("*")):
            if not path.is_file():
                continue
            suffix = path.suffix.lower()
            name = path.name.lower()
            kind = ""
            if suffix == ".sh":
                kind = "shell"
            elif suffix in (".py", ".py3"):
                kind = "python"
            elif suffix in (".cpp", ".cc", ".cxx", ".c"):
                kind = "source"
            elif "test" in name or "quick" in name:
                kind = "test-data-or-runner"
            if not kind:
                continue
            mode = path.stat().st_mode
            results.append(
                TestFile(
                    path=str(path.relative_to(repo)),
                    executable=bool(mode & stat.S_IXUSR),
                    kind=kind,
                )
            )
    return results


def formal_manifests(repo: Path) -> list[dict[str, Any]]:
    formal = repo / "formal"
    results: list[dict[str, Any]] = []
    for path in sorted(formal.glob("*.json")):
        try:
            document = json.loads(path.read_text(encoding="utf-8"))
        except (json.JSONDecodeError, UnicodeDecodeError):
            continue
        if not isinstance(document, dict) or "checks" not in document:
            continue
        checks = document.get("checks", [])
        proofs = document.get("proofs", [])
        results.append(
            {
                "path": str(path.relative_to(repo)),
                "checks": len(checks) if isinstance(checks, list) else None,
                "proofs": len(proofs) if isinstance(proofs, list) else None,
                "groups": sorted(
                    {
                        str(row.get("id", "")).split("-", 1)[0]
                        for row in checks
                        if isinstance(row, dict) and row.get("id")
                    }
                ),
            }
        )
    return results


def discovered_modes(repo: Path) -> dict[str, list[str]]:
    roots = [name for name in ("unittests", "tests") if (repo / name).exists()]
    results: dict[str, list[str]] = {}
    for mode in ACCEPTED_SCHEDCREDIT_MODES:
        completed = run(
            repo,
            "git",
            "grep",
            "-n",
            "-I",
            "-F",
            mode,
            "--",
            *roots,
            check=False,
        )
        if completed.returncode not in (0, 1):
            raise SystemExit(completed.stderr)
        results[mode] = completed.stdout.splitlines()[:30]
    return results


def build_commands(repo: Path) -> list[list[str]]:
    commands: list[list[str]] = []
    if (repo / "configure.ac").exists():
        if (repo / "autogen.sh").exists():
            commands.append(["./autogen.sh"])
        commands.extend(
            [
                ["./configure", "--prefix=$PWD/_install"],
                ["make", "-j$(nproc)"],
                ["make", "check"],
            ]
        )
    if (repo / "CMakeLists.txt").exists():
        commands.extend(
            [
                ["cmake", "-S", ".", "-B", "build", "-DCMAKE_BUILD_TYPE=RelWithDebInfo"],
                ["cmake", "--build", "build", "-j$(nproc)"],
                ["ctest", "--test-dir", "build", "--output-on-failure"],
            ]
        )
    if not commands:
        raise SystemExit("no supported repository build system detected")
    return commands


def shell(command: list[str]) -> str:
    return " ".join(command)


def markdown(document: dict[str, Any]) -> str:
    lines = [
        "# mickg10 cluster execution inventory",
        "",
        f"Generated from exact revision `{document['revision']}`.",
        "",
        "The inventory separates repository-discovered commands from topology",
        "templates. A command is not reported as having passed merely because it",
        "appears here.",
        "",
        "## 1. Detected build entry points",
        "",
        "```sh",
        *[shell(command) for command in document["build_commands"]],
        "```",
        "",
        "## 2. Makefile test/program inventory",
        "",
    ]
    for item in document["make_variables"]:
        lines.append(
            f"- `{item['file']}` `{item['variable']}`: "
            + ", ".join(f"`{value}`" for value in item["values"])
        )
    lines.extend(["", "## 3. Test scripts and sources", ""])
    for item in document["test_files"]:
        marker = "executable" if item["executable"] else "not executable"
        lines.append(
            f"- `{item['path']}` — {item['kind']}, {marker}"
        )

    lines.extend(["", "## 4. Scheduler integration modes", ""])
    for mode, matches in document["modes"].items():
        state = "present" if matches else "missing"
        lines.append(f"- `{mode}` — **{state}**, {len(matches)} source references")

    lines.extend(["", "## 5. Formal acceptance entry points", ""])
    for manifest in document["formal_manifests"]:
        lines.append(
            f"- `{manifest['path']}` — {manifest['checks']} checks, "
            f"{manifest['proofs']} proof rows, groups "
            + ", ".join(f"`{group}`" for group in manifest["groups"])
        )

    lines.extend(
        [
            "",
            "The proof-bearing assignment-fence command is intentionally",
            "executed through `formal/run_assignment_fence_focused_gate.py`,",
            "which adapts to the generation-4 runner's pinned TLAPM/backend",
            "interface and refuses unmapped required proof inputs.",
            "",
            "## 6. Required cluster topology matrix",
            "",
            "| Tier | S | F/F′ | C/C′ | Purpose |",
            "|---|---:|---:|---:|---|",
            "| smoke | 1 | 2 | 4 | wiring, version negotiation, exact traces |",
            "| small | 1 | 10 | 100 | deterministic mixed-role and disconnect gates |",
            "| medium | 1 | 25 | 1,000 | latency, throughput, management responsiveness |",
            "| high | 1 | 50 | 5,000 | sustained queue/pending/state bounds |",
            "| saturation | 1 | 50 | 10,000+ | capacity, reconnect storm, retained-state plateau |",
            "",
            "Every tier must execute these version rows:",
            "",
            "```text",
            "S'FC",
            "S'FC'",
            "S'F'C",
            "S'F'C'",
            "S'F[F']C[C']",
            "S'F'[CC']",
            "```",
            "",
            "`StrictEnforcing` is measured first. `PipelinedEnforcing` is run",
            "only for negotiated new-S/new-F/new-C Token assignments and only",
            "as the performance contingency described in",
            "`ASSIGNMENT_FENCE_THEORY.md`.",
            "",
            "## 7. Host and process setup contract",
            "",
            "Each run directory must contain:",
            "",
            "```text",
            "revision.txt                 exact git SHA and dirty-state check",
            "binary-sha256.txt            scheduler/daemon/client binary hashes",
            "host-inventory.json          hostname, role, CPU, RAM, kernel, NIC",
            "version-topology.json        S/F/F'/C/C' versions per host",
            "command-lines.txt            exact argv and environment",
            "clock-check.txt              monotonic and wall-clock sanity",
            "start-barrier.json           participating processes and generations",
            "event-log.jsonl              assignment identity and barrier events",
            "metrics.csv                  one-second resource/latency samples",
            "result.json                  pass/fail plus first exact blocker",
            "```",
            "",
            "Recommended role separation:",
            "",
            "- one scheduler host with no compiler workload;",
            "- dedicated old and new fulfillment-daemon pools;",
            "- separate client-generator hosts for C and C′;",
            "- one observer host collecting management queries and process metrics;",
            "- synchronized start barriers, but all correctness deadlines use a",
            "  monotonic clock local to the observing process.",
            "",
            "## 8. Per-tier scenario inventory",
            "",
            "Run all of the following at every applicable tier:",
            "",
            "1. normal assignment/begin/completion;",
            "2. cancellation before UseCS delivery;",
            "3. claim wins while REVOKE remains queued;",
            "4. REVOKE consumption wins before a delayed claim;",
            "5. submitter disconnect before Begin;",
            "6. submitter disconnect after Begin, followed by worker completion;",
            "7. submitter disconnect after Begin, followed by worker disconnect;",
            "8. scheduler restart with delayed Legacy, FencedLegacy, and Token claims;",
            "9. F and C reconnect/relogin generation changes;",
            "10. duplicate Begin and duplicate terminal messages;",
            "11. partial UseCS delivery at every deterministic byte cut;",
            "12. pending-claim overflow and unknown-token rejection in pipelined mode;",
            "13. terminal-record compaction followed by a delayed claim;",
            "14. sustained management queries during assignment saturation;",
            "15. connection storm and repeated inbound-connectivity failures.",
            "",
            "## 9. Required measurements",
            "",
            "For baseline, strict, and—only if needed—pipelined modes, retain:",
            "",
            "- assignments and completions per second;",
            "- request-to-UseCS and request-to-Begin p50/p95/p99/max latency;",
            "- scheduler user/system CPU, RSS, virtual memory, and descriptor count;",
            "- per-F queue depth, pending-claim high-water mark, and tombstone count;",
            "- management listjobs/listcs response p50/p95/p99/max and failure count;",
            "- S->F and F->S bytes/messages per assignment;",
            "- reconnect and relogin duration and generation transitions;",
            "- terminal uniqueness, live-identity uniqueness, and accounting",
            "  conservation assertions;",
            "- first exact failed barrier with retained process logs.",
            "",
            "## 10. Acceptance comparison",
            "",
            "A mode is acceptable only if correctness gates are green and its",
            "performance comparison is reported against the same exact baseline",
            "binary, host allocation, workload seed, warmup, and measurement",
            "window. Strict mode remains selected when it meets the budget. A",
            "pipelined mode cannot be justified by throughput alone if it violates",
            "pending bounds, management responsiveness, mixed-version isolation,",
            "or any formal premise.",
            "",
        ]
    )
    return "\n".join(lines)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo", default=".", type=Path)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--json-output", type=Path)
    args = parser.parse_args()
    repo = args.repo.resolve()

    variables = make_variables(repo)
    files = test_files(repo)
    manifests = formal_manifests(repo)
    modes = discovered_modes(repo)
    document: dict[str, Any] = {
        "schema": 1,
        "revision": revision(repo),
        "build_commands": build_commands(repo),
        "make_variables": [asdict(item) for item in variables],
        "test_files": [asdict(item) for item in files],
        "formal_manifests": manifests,
        "modes": modes,
    }
    if not variables:
        raise SystemExit("no Makefile test/program variables discovered")
    if not files:
        raise SystemExit("no test files discovered")
    if not manifests:
        raise SystemExit("no formal acceptance manifests discovered")
    missing_modes = [mode for mode, matches in modes.items() if not matches]
    if missing_modes:
        raise SystemExit(
            "accepted scheduler modes absent from repository test sources: "
            + ", ".join(missing_modes)
        )

    rendered = markdown(document)
    if args.output:
        args.output.write_text(rendered + "\n", encoding="utf-8")
    else:
        print(rendered)
    if args.json_output:
        args.json_output.write_text(
            json.dumps(document, indent=2) + "\n", encoding="utf-8"
        )


if __name__ == "__main__":
    main()
