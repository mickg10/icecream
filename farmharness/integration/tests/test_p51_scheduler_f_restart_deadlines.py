from __future__ import annotations

import re
import subprocess
from pathlib import Path

import pytest


ROOT = Path(__file__).resolve().parents[3]
RUNNER = ROOT / "unittests" / "p50compilee2e-run.sh"


def _deadline_function() -> str:
    text = RUNNER.read_text(encoding="utf-8")
    match = re.search(
        r"(?ms)^    verify_f_restart_failure_deadlines\(\) \{\n.*?^    \}",
        text,
    )
    assert match is not None, "P50 runner must expose its F-restart deadline checker"
    return match.group(0)


def _single_attempt_log(start: str = "2026-09-26 20:00:00",
                        finish: str = "2026-09-26 20:00:05") -> str:
    return (
        f"{start}: P50 assignment identity bound for job 1 epoch 7 nonce 9\n"
        f"{finish}: P29V1 cache source transfer failed closed "
        "(status 2, error 7, attempts 0)\n"
    )


def _two_attempt_log(*, retry_job: int = 31, retry_failure: str = "20:04:00",
                     include_transition: bool = True,
                     include_retry_failure: bool = True) -> str:
    lines = [
        "2026-09-26 20:00:00: P50 assignment identity bound for job 1 epoch 7 nonce 9",
        "2026-09-26 20:02:00: P29V1 cache source transfer failed closed (status 2, error 7, attempts 0)",
    ]
    if include_transition:
        lines.append(
            "2026-09-26 20:02:00: assignment failed; requesting one fresh strict-P50 remote assignment"
        )
    lines.append(
        f"2026-09-26 20:02:01: P50 assignment identity bound for job {retry_job} epoch 7 nonce 10"
    )
    if include_retry_failure:
        lines.append(
            f"2026-09-26 {retry_failure}: ZSTD_ROUTE cache source transfer failed closed "
            "(status 2, error 7, attempts 0)"
        )
    return "\n".join(lines) + "\n"


CASES = (
    ("single-attempt-valid", "single", 5_000, True),
    ("two-attempt-valid", "two", 240_000, True),
    ("negative-span", "single", -1, False),
    ("nonnumeric-span", "single", "not-a-number", False),
    ("missing-timestamp", "single-missing", 5_000, False),
    ("invalid-date", "single-invalid-date", 5_000, False),
    ("single-c-deadline-overbudget", "single-overbudget", 123_000, False),
    ("retry-reuses-job-identity", "retry-same-job", 240_000, False),
    ("retry-transition-missing", "retry-no-transition", 240_000, False),
    ("retry-terminal-missing", "retry-no-terminal", 240_000, False),
    ("retry-c-deadline-overbudget", "retry-overbudget", 247_000, False),
    ("f-arm-overbudget", "f-arm-overbudget", 240_000, False),
    ("aggregate-shorter-than-attempts", "two", 236_999, False),
    ("aggregate-overbudget", "two", 249_001, False),
)


@pytest.mark.parametrize("case_name,kind,elapsed_ms,accepted", CASES,
                         ids=[case[0] for case in CASES])
def test_scheduler_f_restart_deadline_parser(
    tmp_path: Path, case_name: str, kind: str, elapsed_ms: int | str, accepted: bool
) -> None:
    del case_name
    client_log = tmp_path / "client.log"
    f_log = tmp_path / "f.log"
    if kind.startswith("single"):
        content = _single_attempt_log()
        if kind == "single-missing":
            content = re.sub(r"2026-09-26 [0-9:]+: ", "", content)
        elif kind == "single-invalid-date":
            content = content.replace("2026-09-26", "2026-99-99")
        elif kind == "single-overbudget":
            content = _single_attempt_log(finish="2026-09-26 20:02:03")
        client_log.write_text(content, encoding="utf-8")
    else:
        kwargs: dict[str, object] = {}
        if kind == "retry-same-job":
            kwargs["retry_job"] = 1
        elif kind == "retry-no-transition":
            kwargs["include_transition"] = False
        elif kind == "retry-no-terminal":
            kwargs["include_retry_failure"] = False
        elif kind == "retry-overbudget":
            kwargs["retry_failure"] = "20:04:04"
        client_log.write_text(_two_attempt_log(**kwargs), encoding="utf-8")

    arm_age = 61_001 if kind == "f-arm-overbudget" else 60_000
    f_log.write_text(
        "P50 source arm deadline expired for client 99\n"
        f"JobDoneMsg: waitp50input ClientID: 99 Job ID: 31, age_msec={arm_age}\n",
        encoding="utf-8",
    )
    script = (
        _deadline_function()
        + '\nwork="$1"; verify_f_restart_failure_deadlines "$2" "$3"\n'
    )
    result = subprocess.run(
        ["sh", "-c", script, "deadline-check", str(tmp_path), str(client_log), str(elapsed_ms)],
        capture_output=True,
        text=True,
        check=False,
    )
    assert (result.returncode == 0) is accepted, (
        f"deadline parser acceptance mismatch for {kind}: rc={result.returncode}; "
        f"stdout={result.stdout!r}; stderr={result.stderr!r}"
    )
