#!/usr/bin/env python3
from __future__ import annotations

import copy
import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

import check_g4_trace as g4


def entry() -> dict:
    return {"kind": "None", "phase": "Absent", "owner_generation": 0,
            "owner_request_generation": 0, "slot_charged": False,
            "begin_committed": False, "terminal_count": 0}


def base_state() -> dict:
    return {
        "protocol": 48, "max_batch": 2,
        "client_priority": {"c1": {"nice": 10, "client_id": 1},
                            "c2": {"nice": 0, "client_id": 2}},
        "session": "Disconnected", "generation": 0,
        "conf_arrived": False, "activated_by": "None",
        "loss_pending": False, "capacity": 1,
        "requests": {
            client: {"state": "Idle", "generation": 0,
                     "expected": 0, "accepted": 0,
                     "decisions": {"e1": entry(), "e2": entry()}}
            for client in ("c1", "c2")
        },
    }


def transition(seq: int, action: str, pre: dict, **fields: object) -> tuple[dict, dict]:
    event = {"type": "transition", "seq": seq, "action": action,
             "client": fields.pop("client", None),
             "decision": fields.pop("decision", None), **fields}
    post = g4.expected_post(pre, event)
    event.update(pre=copy.deepcopy(pre), post=copy.deepcopy(post),
                 pre_digest=g4.state_digest(pre), post_digest=g4.state_digest(post))
    return event, post


def valid_trace() -> list[dict]:
    state = base_state()
    records: list[dict] = [{"type": "header", "schema": g4.SCHEMA,
                            "commit": "a" * 40, "run": "unit", "role": "daemon",
                            "instance": "d1", "initial_state": copy.deepcopy(state)}]
    plan = [
        ("Connect", {}), ("ConfArrived", {}), ("ConfActivated", {}),
        ("BatchAccepted", {"client": "c2", "count": 1}),
        ("LocalAccepted", {"client": "c2", "decision": "e1"}),
        ("LocalBound", {"client": "c2", "decision": "e1"}),
        ("LocalDelivered", {"client": "c2", "decision": "e1"}),
        ("BeginCommitted", {"client": "c2", "decision": "e1"}),
        ("Completed", {"client": "c2", "decision": "e1"}),
        ("RequestClosed", {"client": "c2"}),
        ("ClientReset", {"client": "c2"}),
        ("SessionLost", {}), ("SessionCleaned", {}),
        ("Connect", {}), ("ConfArrived", {}), ("ConfActivated", {}),
    ]
    for seq, (action, fields) in enumerate(plan, 1):
        event, state = transition(seq, action, state, **fields)
        records.append(event)
    records.append({"type": "footer", "schema": g4.SCHEMA,
                    "records": len(plan), "dropped_records": 0,
                    "final_digest": g4.state_digest(state),
                    "final_state": copy.deepcopy(state)})
    return records


def waiting_two() -> dict:
    state = base_state()
    state.update(session="Active", generation=1, activated_by="Conf")
    for client in ("c1", "c2"):
        req = state["requests"][client]
        req.update(state="Waiting", generation=1, expected=1, accepted=1)
        req["decisions"]["e1"].update(kind="Local", phase="LocalWaiting",
                                         owner_generation=1,
                                         owner_request_generation=1)
    g4.validate_state(state)
    return state


class TraceTests(unittest.TestCase):
    def assertRejected(self, records: list[dict], contains: str | None = None) -> None:
        with self.assertRaises(g4.TraceError) as caught:
            g4.check_records(records)
        if contains:
            self.assertIn(contains, str(caught.exception))

    def test_01_valid_reconnect_trace(self) -> None:
        result = g4.check_records(valid_trace())
        self.assertEqual(result["result"], "PASS")
        self.assertEqual(result["records"], 16)

    def test_02_unknown_action(self) -> None:
        records = valid_trace()
        records[1]["action"] = "Mystery"
        self.assertRejected(records, "unknown action")

    def test_03_sequence_gap(self) -> None:
        records = valid_trace()
        records[2]["seq"] = 9
        self.assertRejected(records, "sequence gap")

    def test_04_start_before_begin(self) -> None:
        records = valid_trace()
        event = records[7]  # LocalDelivered
        event["post"]["requests"]["c2"]["decisions"]["e1"].update(
            phase="LocalStarted", begin_committed=False)
        event["post_digest"] = g4.state_digest(event["post"])
        self.assertRejected(records, "LocalStarted before BeginCommitted")

    def test_05_stale_owner_generation(self) -> None:
        records = valid_trace()
        event = records[5]  # LocalAccepted
        event["post"]["requests"]["c2"]["decisions"]["e1"]["owner_generation"] = 0
        event["post_digest"] = g4.state_digest(event["post"])
        self.assertRejected(records, "stale session generation")

    def test_06_priority_inversion(self) -> None:
        pre = waiting_two()
        event = {"action": "LocalBound", "client": "c1", "decision": "e1"}
        with self.assertRaisesRegex(g4.TraceError, "priority inversion"):
            g4.expected_post(pre, event)

    def test_07_second_bound(self) -> None:
        pre = waiting_two()
        pre["requests"]["c1"]["decisions"]["e1"]["phase"] = "LocalBound"
        g4.validate_state(pre)
        with self.assertRaisesRegex(g4.TraceError, "second LocalBound"):
            g4.expected_post(pre, {"action": "LocalBound", "client": "c2",
                                   "decision": "e1"})

    def test_08_capacity_overflow(self) -> None:
        pre = waiting_two()
        one = pre["requests"]["c1"]["decisions"]["e1"]
        two = pre["requests"]["c2"]["decisions"]["e1"]
        one.update(phase="LocalDelivered", slot_charged=True)
        two["phase"] = "LocalBound"
        g4.validate_state(pre)
        with self.assertRaisesRegex(g4.TraceError, "LocalDelivered precondition"):
            g4.expected_post(pre, {"action": "LocalDelivered", "client": "c2",
                                   "decision": "e1"})

    def test_09_duplicate_terminal(self) -> None:
        state = valid_trace()[9]["post"]
        state["requests"]["c2"]["decisions"]["e1"]["terminal_count"] = 2
        with self.assertRaisesRegex(g4.TraceError, "duplicate terminal"):
            g4.validate_state(state)

    def test_10_accepted_ledger_regression(self) -> None:
        state = valid_trace()[5]["post"]
        state["requests"]["c2"]["accepted"] = 0
        with self.assertRaisesRegex(g4.TraceError, "accepted ledger mismatch"):
            g4.validate_state(state)

    def test_11_partial_cleanup(self) -> None:
        pre = waiting_two()
        pre.update(session="Disconnected", activated_by="None", loss_pending=True)
        event, correct = transition(1, "SessionCleaned", pre)
        bad = copy.deepcopy(correct)
        bad_entry = bad["requests"]["c2"]["decisions"]["e1"]
        bad_entry.update(phase="LocalWaiting", terminal_count=0)
        bad["requests"]["c2"]["state"] = "Closed"
        event["post"] = bad
        event["post_digest"] = g4.state_digest(bad)
        records = [{"type": "header", "schema": g4.SCHEMA,
                    "initial_state": pre}, event,
                   {"type": "footer", "schema": g4.SCHEMA, "records": 1,
                    "dropped_records": 0, "final_digest": g4.state_digest(bad)}]
        self.assertRejected(records, "impossible post-state")

    def test_12_stale_rejection_requires_stale_identity(self) -> None:
        state = valid_trace()[4]["post"]  # batch accepted
        with self.assertRaisesRegex(g4.TraceError, "current identity"):
            g4.expected_post(state, {"action": "StaleDecisionRejected",
                                     "client": "c2", "decision": "e1",
                                     "incoming_session_generation": 1,
                                     "incoming_request_generation": 1})

    def test_13_begin_failure_classification(self) -> None:
        state = valid_trace()[7]["post"]  # LocalDelivered
        with self.assertRaisesRegex(g4.TraceError, "classification"):
            g4.expected_post(state, {"action": "BeginNoCommitFailure",
                                     "client": "c2", "decision": "e1",
                                     "commit_class": "ambiguous"})

    def test_14_footer_digest(self) -> None:
        records = valid_trace()
        records[-1]["final_digest"] = "0" * 64
        self.assertRejected(records, "footer final digest")

    def test_15_dropped_record(self) -> None:
        records = valid_trace()
        records[-1]["dropped_records"] = 1
        self.assertRejected(records, "dropped records")

    def test_16_cli_jsonl(self) -> None:
        records = valid_trace()
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "trace.jsonl"
            path.write_text("".join(json.dumps(r) + "\n" for r in records),
                            encoding="utf-8")
            script = Path(g4.__file__).resolve()
            proc = subprocess.run([sys.executable, str(script), str(path)],
                                  text=True, stdout=subprocess.PIPE,
                                  stderr=subprocess.PIPE)
        self.assertEqual(proc.returncode, 0, proc.stderr)
        self.assertEqual(json.loads(proc.stdout)["result"], "PASS")


if __name__ == "__main__":
    unittest.main()
