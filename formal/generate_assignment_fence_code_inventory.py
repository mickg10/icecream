#!/usr/bin/env python3
"""Generate an exact repository-derived model-to-code inventory.

The generator records real source matches at one revision.  Planned protocol
messages are listed separately and are never represented as existing seams.
"""

from __future__ import annotations

import argparse
import json
import re
import subprocess
from dataclasses import asdict, dataclass
from pathlib import Path


@dataclass(frozen=True)
class Match:
    path: str
    line: int
    text: str


@dataclass(frozen=True)
class Query:
    transition: str
    purpose: str
    pattern: str
    required: bool


QUERIES = (
    Query(
        "AssignmentRequest",
        "scheduler admission and assignment request handling",
        r"GetCS(Msg)?|handle_get_cs|handleGetCS|get_cs",
        True,
    ),
    Query(
        "UseCSExposed",
        "assignment delivery to the client-facing daemon path",
        r"UseCS(Msg)?|USE_CS|use_cs",
        True,
    ),
    Query(
        "Started",
        "worker/daemon begin linearization",
        r"JobBegin(Msg)?|handle_job_begin|handleJobBegin",
        True,
    ),
    Query(
        "Terminal",
        "completion and cancellation terminal handling",
        r"JobDone(Msg)?|handle_job_done|handleJobDone|MonJobDone",
        True,
    ),
    Query(
        "SubmitterDetach",
        "started-job ownership transfer after submitter loss",
        r"detachSubmitter|submitterDetached|m_submitterDetached",
        True,
    ),
    Query(
        "PeerLoss",
        "submitter or worker disconnect cleanup",
        r"handle_end|handleEnd|remove_daemon|removeDaemon|toremove",
        True,
    ),
    Query(
        "DispatchCredit",
        "reservation/debit release and accounting conservation",
        r"credit_dispatch_credit|dispatch.?credit|submittedJobsDecrement",
        True,
    ),
    Query(
        "ProtocolNegotiation",
        "per-link protocol-version capability selection",
        r"protocol.?version|min(imum)?_remote_version|max(imum)?_remote_version|IS_PROTOCOL_VERSION",
        True,
    ),
    Query(
        "MessageCodec",
        "wire encoding and decoding seam",
        r"fill_from_channel|send_to_channel|class [A-Za-z0-9_]*Msg|struct [A-Za-z0-9_]*Msg",
        True,
    ),
    Query(
        "LifecycleRegression",
        "deterministic scheduler lifecycle and race gates",
        r"schedbp|schedstress|schedcredit|teardown|retention",
        True,
    ),
    Query(
        "PreparePlanned",
        "new per-assignment prepare control; absence is expected before product work",
        r"PrepareAssignment(Msg)?|AssignmentPrepare(Msg)?|PREPARE_ASSIGNMENT",
        False,
    ),
    Query(
        "ReadyPlanned",
        "new per-assignment ready control; absence is expected before product work",
        r"AssignmentReady(Msg)?|ReadyAssignment(Msg)?|ASSIGNMENT_READY",
        False,
    ),
    Query(
        "RevokePlanned",
        "new per-assignment revoke control; absence is expected before product work",
        r"RevokeAssignment(Msg)?|AssignmentRevoke(Msg)?|REVOKE_ASSIGNMENT",
        False,
    ),
    Query(
        "PendingClaimPlanned",
        "optional bounded pipelined-enforcing pending table",
        r"pending.?claim|PendingClaim|pending_assignments",
        False,
    ),
)

SOURCE_ROOT_CANDIDATES = (
    "scheduler",
    "daemon",
    "services",
    "client",
    "unittests",
    "tests",
)


def git(repo: Path, *args: str, check: bool = True) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        ["git", *args],
        cwd=repo,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=check,
    )


def roots(repo: Path) -> list[str]:
    result = [name for name in SOURCE_ROOT_CANDIDATES if (repo / name).exists()]
    if not result:
        raise SystemExit("no expected product source roots exist")
    return result


def search(repo: Path, query: Query, source_roots: list[str]) -> list[Match]:
    command = [
        "grep",
        "-n",
        "-I",
        "-E",
        query.pattern,
        "--",
        *source_roots,
    ]
    completed = git(repo, *command, check=False)
    if completed.returncode not in (0, 1):
        raise SystemExit(
            f"git grep failed for {query.transition}: {completed.stderr}"
        )
    matches: list[Match] = []
    for raw in completed.stdout.splitlines():
        first, second, text = raw.split(":", 2)
        matches.append(
            Match(path=first, line=int(second), text=text.strip())
        )
    matches.sort(key=lambda item: (item.path, item.line, item.text))
    return matches[:80]


def markdown(
    revision: str,
    source_roots: list[str],
    inventory: dict[str, list[Match]],
) -> str:
    lines = [
        "# Assignment-fence model-to-code inventory",
        "",
        f"Generated from exact revision `{revision}`.",
        "",
        "This is a repository-derived inventory, not an assertion that the new",
        "assignment-fence protocol is implemented. Existing seams and planned",
        "product additions are deliberately separated.",
        "",
        "Source roots searched:",
        "",
        "```text",
        *source_roots,
        "```",
        "",
        "## Existing transition seams",
        "",
    ]
    by_name = {query.transition: query for query in QUERIES}
    for query in QUERIES:
        if not query.required:
            continue
        matches = inventory[query.transition]
        lines.extend(
            [
                f"### `{query.transition}`",
                "",
                query.purpose + ".",
                "",
            ]
        )
        for match in matches:
            snippet = match.text.replace("`", "\\`")
            lines.append(
                f"- `{match.path}:{match.line}` — `{snippet}`"
            )
        lines.append("")

    lines.extend(
        [
            "## Planned protocol delta",
            "",
            "The following categories are searched only to detect accidental or",
            "partial product work. Absence is expected on this formal branch.",
            "",
        ]
    )
    for query in QUERIES:
        if query.required:
            continue
        matches = inventory[query.transition]
        state = "ABSENT" if not matches else "PRESENT — requires review"
        lines.extend(
            [
                f"### `{query.transition}` — **{state}**",
                "",
                query.purpose + ".",
                "",
            ]
        )
        for match in matches:
            snippet = match.text.replace("`", "\\`")
            lines.append(
                f"- `{match.path}:{match.line}` — `{snippet}`"
            )
        if not matches:
            lines.append("- No product-source symbol matched.")
        lines.append("")

    lines.extend(
        [
            "## Mandatory product mapping before behavior is enabled",
            "",
            "Each new product commit must replace a planned category with exact",
            "message fields, encoder/decoder locations, handlers, state fields,",
            "linearization point, cleanup path, and a deterministic regression",
            "whose barrier comes from a named formal trace manifest.",
            "",
            "The minimum concrete mapping is:",
            "",
            "| Formal transition | Product obligation |",
            "|---|---|",
            "| `QueuePrepare` | version-gated new-peer control encoding and ordered S->F enqueue |",
            "| `ConsumePrepare` | exact-token installation at F; no client/compiler side effect |",
            "| `ConsumeReady` | strict-mode scheduler authorization to expose UseCS |",
            "| `ClientClaim` | exact identity and client-session-generation lookup |",
            "| `QueueRevoke` | ordered enqueue without scheduler ownership release |",
            "| `ConsumeRevoke` | F-side rejection-fence linearization |",
            "| `ConsumeTerminal` | scheduler terminal-token consumption and ownership release |",
            "| `SessionLost` | generation invalidation, pending cleanup, and one terminal outcome |",
            "| `Compact` | retained rejection summary adequate for the advertised delay bound |",
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
    revision = git(repo, "rev-parse", "HEAD").stdout.strip()
    source_roots = roots(repo)
    inventory = {
        query.transition: search(repo, query, source_roots)
        for query in QUERIES
    }
    missing = [
        query.transition
        for query in QUERIES
        if query.required and not inventory[query.transition]
    ]
    if missing:
        raise SystemExit(
            "required repository seams have no source match: " + ", ".join(missing)
        )

    rendered = markdown(revision, source_roots, inventory)
    if args.output:
        args.output.write_text(rendered + "\n", encoding="utf-8")
    else:
        print(rendered)

    if args.json_output:
        document = {
            "schema": 1,
            "revision": revision,
            "source_roots": source_roots,
            "queries": [asdict(query) for query in QUERIES],
            "inventory": {
                name: [asdict(match) for match in matches]
                for name, matches in inventory.items()
            },
        }
        args.json_output.write_text(
            json.dumps(document, indent=2) + "\n", encoding="utf-8"
        )


if __name__ == "__main__":
    main()
