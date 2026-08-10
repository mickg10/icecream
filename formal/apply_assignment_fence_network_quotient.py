#!/usr/bin/env python3
"""Apply and verify the finite FIFO quotient for AssignmentFenceNetwork."""

from __future__ import annotations

import json
import re
from pathlib import Path
from typing import Any

HERE = Path(__file__).resolve().parent
MODEL = HERE / "AssignmentFenceNetwork.tla"


def rewrite_json(value: Any) -> Any:
    if isinstance(value, list):
        result = []
        for item in value:
            if isinstance(item, dict) and item.get("path") == "nextF2SSeq":
                continue
            result.append(rewrite_json(item))
        return result
    if isinstance(value, dict):
        result = {key: rewrite_json(item) for key, item in value.items()}
        if result.get("path") == "lastF2SConsumed":
            result["path"] = "lastF2SIndex"
        return result
    if isinstance(value, str):
        return (
            value.replace("lastF2SConsumed", "lastF2SIndex")
            .replace("nextF2SSeq", "finite FIFO position")
        )
    return value


def apply_quotient(text: str) -> str:
    if "nextF2SSeq" not in text:
        verify(text)
        return text

    required = [
        "Msg(kind, assignment, seq)",
        "lastF2SConsumed",
        "ChosenF2S.seq",
        "F2SStrictlyIncreasing ==",
    ]
    missing = [fragment for fragment in required if fragment not in text]
    if missing:
        raise SystemExit(f"finite-quotient precondition missing: {missing}")

    text = text.replace(
        "Msg(kind, assignment, seq) ==",
        "Msg(kind, assignment) ==",
        1,
    )
    text = text.replace(", seq |-> seq", "")
    text = text.replace(", seq : Nat", "")
    text = re.sub(
        r"\bMsg\(([^,\n()]+),\s*([^,\n()]+),\s*(?:0|nextF2SSeq)\)",
        r"Msg(\1, \2)",
        text,
    )
    text = text.replace("lastF2SConsumed", "lastF2SIndex")
    text = text.replace("ChosenF2S.seq", "ChosenF2SIndex")

    lines: list[str] = []
    for line in text.splitlines():
        stripped = line.strip()
        if re.fullmatch(r"nextF2SSeq,?", stripped):
            continue
        if re.fullmatch(r"/\\ nextF2SSeq = .+", stripped):
            continue
        if re.fullmatch(r"/\\ nextF2SSeq' = .+", stripped):
            continue
        if re.fullmatch(r"/\\ nextF2SSeq \\in .+", stripped):
            continue
        line = re.sub(r"\bnextF2SSeq\s*,\s*", "", line)
        line = re.sub(r",\s*nextF2SSeq\b", "", line)
        lines.append(line)
    text = "\n".join(lines) + "\n"

    text, type_count = re.subn(
        r"(?m)^\s*/\\ lastF2SIndex \\in .*?$",
        "    /\\ lastF2SIndex \\in 0..2",
        text,
    )
    if type_count != 1:
        raise SystemExit(
            f"expected one lastF2SIndex type clause, got {type_count}"
        )

    op_start = text.index("F2SStrictlyIncreasing ==")
    op_end = text.find("\n\n", op_start)
    if op_end < 0:
        raise SystemExit("F2SStrictlyIncreasing operator has no boundary")
    text = (
        text[:op_start]
        + "F2SStrictlyIncreasing == lastF2SIndex <= 1"
        + text[op_end:]
    )

    marker = "ChosenF2SIndex ==\n"
    marker_at = text.index(marker)
    comment = """(***************************************************************************
Finite FIFO quotient: queue position carries all relative-order information.
lastF2SIndex is only a bounded observation used to discriminate the explicit
index-2 bypass mutant; it is not a protocol sequence number.
***************************************************************************)
"""
    if "Finite FIFO quotient:" not in text:
        text = text[:marker_at] + comment + text[marker_at:]
    verify(text)
    return text


def verify(text: str) -> None:
    forbidden = [
        "nextF2SSeq",
        "lastF2SConsumed",
        "ChosenF2S.seq",
        "seq |-> seq",
        "seq : Nat",
    ]
    residue = [fragment for fragment in forbidden if fragment in text]
    if residue:
        raise SystemExit(f"finite-quotient residue: {residue}")
    required = [
        "Msg(kind, assignment) ==",
        "lastF2SIndex \\in 0..2",
        "lastF2SIndex' = ChosenF2SIndex",
        "F2SStrictlyIncreasing == lastF2SIndex <= 1",
    ]
    missing = [fragment for fragment in required if fragment not in text]
    if missing:
        raise SystemExit(f"finite-quotient contract missing: {missing}")


def write_test() -> None:
    path = HERE / "assignment_fence_network_quotient_test.py"
    path.write_text(
        '''#!/usr/bin/env python3
"""Static regression for the finite F-to-S FIFO quotient."""

from pathlib import Path
import re
import unittest

MODEL = Path(__file__).with_name("AssignmentFenceNetwork.tla")


class FiniteF2SQuotientTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.text = MODEL.read_text(encoding="utf-8")

    def test_absolute_sequence_state_is_absent(self) -> None:
        for fragment in (
            "nextF2SSeq",
            "lastF2SConsumed",
            "ChosenF2S.seq",
            "seq |-> seq",
            "seq : Nat",
        ):
            self.assertNotIn(fragment, self.text)

    def test_queue_position_is_the_only_order_observer(self) -> None:
        self.assertIn("lastF2SIndex \\in 0..2", self.text)
        self.assertIn("lastF2SIndex' = ChosenF2SIndex", self.text)
        self.assertRegex(
            self.text,
            re.compile(r"F2SStrictlyIncreasing\s*==\s*lastF2SIndex\s*<=\s*1"),
        )

    def test_messages_carry_no_synthetic_order_field(self) -> None:
        self.assertIn("Msg(kind, assignment) ==", self.text)
        self.assertNotRegex(
            self.text,
            re.compile(r"\bMsg\([^\n]*,[^\n]*,[^\n]*\)"),
        )


if __name__ == "__main__":
    unittest.main()
''',
        encoding="utf-8",
    )


def main() -> None:
    original = MODEL.read_text(encoding="utf-8")
    rewritten = apply_quotient(original)
    MODEL.write_text(rewritten, encoding="utf-8")

    trace_dir = HERE / "trace-manifests"
    for path in sorted(trace_dir.glob("*.json")):
        document = json.loads(path.read_text(encoding="utf-8"))
        rewritten_document = rewrite_json(document)
        path.write_text(
            json.dumps(rewritten_document, indent=2) + "\n",
            encoding="utf-8",
        )
    write_test()


if __name__ == "__main__":
    main()
