from __future__ import annotations

from pathlib import Path

import pytest

from farmharness.integration.collect import (
    CollectError,
    _scheduler_retry_decisions,
)


def _write_scheduler(tmp_path: Path, lines: list[str]) -> tuple[Path, dict[str, object]]:
    evidence = tmp_path / "evidence"
    path = evidence / "diagnostics" / "sched-host" / "S1.log" / "scheduler.log"
    path.parent.mkdir(parents=True)
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")
    plan: dict[str, object] = {
        "topology": {"instances": [{"name": "S1", "host": "sched-host", "role": "S"}]}
    }
    return evidence, plan


def _frame(message: str, second: int = 0) -> str:
    return f"[1] 2026-09-17 00:00:{second:02d}: {message}"


def _decision(*, job: int = 7, epoch: int = 1, nonce: int = 1,
              profile: int = 1, compatible: int = 0) -> str:
    return _frame(
        f"P50_RETRY_DECISION job={job} epoch={epoch} nonce={nonce} "
        "failed=F2:23004 selected=F2:23004 "
        f"profile={profile} compatible_alternative={compatible}"
    )


def test_retry_decision_binds_known_new_and_precedes_put(tmp_path: Path) -> None:
    evidence, plan = _write_scheduler(
        tmp_path,
        [
            _frame("ICECREAM scheduler test starting up, port 8765"),
            _frame("NEW 7 client=C1"),
            _decision(),
            _frame("put 7 in joblist of F2"),
        ],
    )
    records = _scheduler_retry_decisions(evidence, plan)
    assert records[0]["job"] == 7
    assert records[0]["epoch"] == 1
    assert records[0]["nonce"] == 1
    assert records[0]["profile"] == 1
    assert records[0]["line"] < records[0]["put_line"]
    assert records[0]["put_ms"] >= records[0]["timestamp_ms"]


def test_retry_decision_allows_new_nonce_on_preexposure_redispatch(tmp_path: Path) -> None:
    evidence, plan = _write_scheduler(
        tmp_path,
        [
            _frame("ICECREAM scheduler test starting up, port 8765"),
            _frame("NEW 7 client=C1"),
            _decision(),
            _frame("put 7 in joblist of F1"),
            _frame("redispatch unexposed assignment 7 after worker loss F1"),
            _decision(epoch=2, nonce=2),
            _frame("put 7 in joblist of F2"),
        ],
    )
    records = _scheduler_retry_decisions(evidence, plan)
    assert [(item["epoch"], item["nonce"]) for item in records] == [(1, 1), (2, 2)]
    assert all(item["line"] < item["put_line"] for item in records)


@pytest.mark.parametrize(
    "mutation",
    (
        "unknown",
        "after-put",
        "duplicate",
        "bad-profile",
        "zero-epoch",
        "unframed",
        "malformed",
        "multiple-before-put",
        "no-put",
        "time-inversion",
        "oversized",
        "stale-generation",
    ),
)
def test_retry_decision_malformed_or_misordered_fails_closed(
    tmp_path: Path, mutation: str
) -> None:
    lines = [
        _frame("ICECREAM scheduler test starting up, port 8765"),
        _frame("NEW 7 client=C1"),
    ]
    if mutation == "unknown":
        lines.append(_decision(job=99))
    elif mutation == "after-put":
        lines.extend([_frame("put 7 in joblist of F2"), _decision()])
    elif mutation == "duplicate":
        lines.extend([_decision(), _decision()])
    elif mutation == "multiple-before-put":
        lines.extend([_decision(), _decision(epoch=2, nonce=2)])
    elif mutation == "no-put":
        lines.append(_decision())
    elif mutation == "bad-profile":
        lines.append(_decision(profile=3))
    elif mutation == "zero-epoch":
        lines.append(_decision(epoch=0))
    elif mutation == "time-inversion":
        lines = [
            _frame("ICECREAM scheduler test starting up, port 8765"),
            _frame("NEW 7 client=C1", second=1),
            _decision(),
        ]
    elif mutation == "oversized":
        lines.append(_decision(epoch=2**64, nonce=2**64))
    elif mutation == "stale-generation":
        lines.extend([
            _frame("ICECREAM scheduler test starting up, port 8765"),
            _decision(),
        ])
    elif mutation == "unframed":
        lines.append(
            "2026-09-17 00:00:00: P50_RETRY_DECISION job=7 epoch=1 nonce=1 "
            "failed=F2:23004 selected=F2:23004 profile=1 compatible_alternative=0"
        )
    else:
        lines.append(_frame("P50_RETRY_DECISION job=7 epoch=1 nonce=1 malformed"))
    evidence, plan = _write_scheduler(tmp_path, lines)
    with pytest.raises(CollectError):
        _scheduler_retry_decisions(evidence, plan)


def test_historical_scheduler_without_decisions_remains_accepted(tmp_path: Path) -> None:
    evidence, plan = _write_scheduler(
        tmp_path,
        [
            _frame("ICECREAM scheduler test starting up, port 8765"),
            _frame("NEW 7 client=C1"),
            _frame("put 7 in joblist of F2"),
        ],
    )
    assert _scheduler_retry_decisions(evidence, plan) == []
