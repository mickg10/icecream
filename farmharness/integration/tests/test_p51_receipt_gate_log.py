from __future__ import annotations

import re
import subprocess
from pathlib import Path

import pytest


ROOT = Path(__file__).resolve().parents[3]
RUNNER = ROOT / "unittests" / "p50compilee2e-run.sh"


def _single_link_state_function() -> str:
    text = RUNNER.read_text(encoding="utf-8")
    match = re.search(
        r"(?ms)^receipt_gate_has_single_link_state\(\) \{\n.*?^\}", text
    )
    assert match is not None, "P50 runner must expose its receipt-gate state validator"
    return match.group(0)


@pytest.mark.parametrize(
    ("log", "expected"),
    [
        (
            "P51_RECEIPT_GATE_LINK_STATE_DECODED attempt=1\n"
            "P51_RECEIPT_GATE_LINK_STATE port=55665\n",
            0,
        ),
        (
            "P51_RECEIPT_GATE_LINK_STATE_DECODED attempt=1\n"
            "P51_RECEIPT_GATE_LINK_STATE port=55665\n"
            "P51_RECEIPT_GATE_LINK_STATE port=55666\n",
            1,
        ),
        ("P51_RECEIPT_GATE_LINK_STATE_DECODED attempt=1\n", 1),
        ("", 1),
    ],
)
def test_receipt_gate_requires_exactly_one_decoded_link_state(
    tmp_path: Path, log: str, expected: int
) -> None:
    log_path = tmp_path / "receipt-gate.log"
    log_path.write_text(log, encoding="utf-8")
    result = subprocess.run(
        [
            "/bin/sh",
            "-c",
            _single_link_state_function()
            + '\nreceipt_gate_has_single_link_state "$1"',
            "receipt-gate-fixture",
            str(log_path),
        ],
        check=False,
        text=True,
        capture_output=True,
    )
    assert result.returncode == expected
