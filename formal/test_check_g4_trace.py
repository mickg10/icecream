from __future__ import annotations

import copy
import unittest

from check_g4_trace import (
    MODEL_BOUNDARY, SCHEMA, TraceError, blank_entry, calculate_post,
    check_records, check_transition, digest,
)

COMMIT = "d206c7a012b173bd5ad92b5da3ed5a92abcc316c"


def base_state(client_count: int = 2, max_batch: int = 3, capacity: int = 1) -> dict:
    requests = {}
    priority = {}
    for i in range(1, client_count + 1):
        c = f"c{i}"
        requests[c] = {
            "mode": "None", "phase": "Idle", "settlement_cause": "None",
            "generation": 0,
            "expected": 0, "accepted": 0, "delivered": 0,
            "tail_cancel_count": 0,
            "entries": [blank_entry() for _ in range(max_batch)],
        }
        priority[c] = {"nice": 10 if i == 1 else 0, "client_id": i}
    return {
        "model_boundary": MODEL_BOUNDARY,
        "protocol": 48,
        "max_batch": max_batch,
        "capacity": capacity,
        "max_generation": 2,
        "max_request_generation": 2,
        "session": "Disconnected",
        "generation": 0,
        "conf_arrived": False,
        "activated_by": "None",
        "loss_pending": False,
        "requests": requests,
        "priority": priority,
        "outputs": [],
        "completed_tokens": [],
        "cancelled_requests": [],
        "dropped_records": 0,
    }


def step(state: dict, action: str, **fields) -> tuple[dict, dict]:
    event = {"action": action, **fields}
    return calculate_post(state, event), event


def transition(seq: int, pre: dict, event: dict, post: dict) -> dict:
    return {
        "type": "transition", "schema": SCHEMA, "seq": seq,
        "pre": pre, "event": event, "post": post,
        "pre_digest": digest(pre), "post_digest": digest(post),
    }


def records(steps: list[tuple[dict, dict, dict]], *, dropped: int = 0,
            commit: str = COMMIT) -> list[dict]:
    out = [{"type": "header", "schema": SCHEMA, "commit": commit,
            "model_boundary": MODEL_BOUNDARY}]
    for i, (pre, event, post) in enumerate(steps, 1):
        out.append(transition(i, pre, event, post))
    out.append({"type": "footer", "schema": SCHEMA, "records": len(steps),
                "dropped_records": dropped,
                "final_digest": digest(steps[-1][2])})
    return out


class G4TraceTests(unittest.TestCase):
    def active(self, n: int = 2) -> tuple[dict, list[tuple[dict, dict, dict]]]:
        s = base_state(n)
        history = []
        for action, fields in [
            ("Connect", {}), ("ConfArrived", {}), ("ActivateConf", {})
        ]:
            p, e = step(s, action, **fields)
            history.append((s, e, p)); s = p
        return s, history

    def test_01_valid_scalar_local_lifecycle(self):
        s, h = self.active(1)
        for action, fields in [
            ("AcceptScalar1", {"client": "c1"}),
            ("AcceptLocal", {"client": "c1", "decision": 1}),
            ("BindLocal", {"client": "c1", "decision": 1}),
            ("DeliverLocal", {"client": "c1", "decision": 1}),
            ("BeginCommit", {"client": "c1", "decision": 1}),
            ("AcceptDone", {"client": "c1", "decision": 1}),
            ("NormalEnd", {"client": "c1"}),
            ("CloseRequest", {"client": "c1"}),
        ]:
            p, e = step(s, action, **fields); h.append((s, e, p)); s = p
        self.assertEqual(check_records(records(h), COMMIT), s)

    def test_02_valid_nocs_uses_local_lane(self):
        s, h = self.active(1)
        for action, fields in [
            ("AcceptScalar1", {"client": "c1"}),
            ("AcceptNoCS", {"client": "c1", "decision": 1}),
            ("BindLocal", {"client": "c1", "decision": 1}),
            ("DeliverLocal", {"client": "c1", "decision": 1}),
            ("BeginCommit", {"client": "c1", "decision": 1}),
            ("AcceptDone", {"client": "c1", "decision": 1}),
            ("AbnormalEnd", {"client": "c1"}),
            ("CloseRequest", {"client": "c1"}),
        ]:
            p, e = step(s, action, **fields); h.append((s, e, p)); s = p
        self.assertEqual(check_records(records(h), COMMIT), s)

    def test_03_valid_loss_cleanup_reconnect_and_stale_rejection(self):
        s, h = self.active(1)
        sequence = [
            ("AcceptBatchN", {"client": "c1", "count": 2}),
            ("AcceptRemote", {"client": "c1", "decision": 1}),
            ("LoseSession", {}),
            ("SettleJob", {"client": "c1", "decision": 1}),
            ("CancelTail", {"client": "c1"}),
            ("CloseRequest", {"client": "c1"}),
            ("FinishLossCleanup", {}),
            ("ResetClient", {"client": "c1"}),
            ("Connect", {}), ("ConfArrived", {}), ("ActivateConf", {}),
            ("AcceptScalar1", {"client": "c1"}),
            ("RejectStaleDecision", {"client": "c1", "decision": 1,
                                     "incoming_session_generation": 1,
                                     "incoming_request_generation": 1}),
        ]
        for action, fields in sequence:
            p, e = step(s, action, **fields); h.append((s, e, p)); s = p
        self.assertEqual(check_records(records(h), COMMIT), s)

    def test_04_start_before_begin_named_reason(self):
        s, _ = self.active(1)
        for action, fields in [
            ("AcceptScalar1", {"client": "c1"}),
            ("AcceptLocal", {"client": "c1", "decision": 1}),
            ("BindLocal", {"client": "c1", "decision": 1}),
            ("DeliverLocal", {"client": "c1", "decision": 1}),
        ]:
            s, _ = step(s, action, **fields)
        event = {"action": "BeginCommit", "client": "c1", "decision": 1}
        bad = copy.deepcopy(s)
        bad["requests"]["c1"]["entries"][0]["phase"] = "LocalStarted"
        with self.assertRaisesRegex(TraceError, "semantic commit"):
            check_transition(s, event, bad)

    def test_05_nocs_immediate_terminal_named_reason(self):
        s, _ = self.active(1)
        s, _ = step(s, "AcceptScalar1", client="c1")
        event = {"action": "AcceptNoCS", "client": "c1", "decision": 1}
        bad = copy.deepcopy(s)
        e = bad["requests"]["c1"]["entries"][0]
        e.update(kind="NoCS", phase="Terminal",
                 owner_session_generation=bad["generation"],
                 owner_request_generation=bad["requests"]["c1"]["generation"],
                 terminal_attempts=1, terminal_emits=1)
        with self.assertRaisesRegex(TraceError, "NoCS must enter LocalWaiting"):
            check_transition(s, event, bad)

    def test_06_second_bound_same_client_rejected(self):
        s, _ = self.active(1)
        for action, fields in [
            ("AcceptBatchN", {"client": "c1", "count": 2}),
            ("AcceptLocal", {"client": "c1", "decision": 1}),
            ("AcceptNoCS", {"client": "c1", "decision": 2}),
            ("BindLocal", {"client": "c1", "decision": 1}),
        ]:
            s, _ = step(s, action, **fields)
        with self.assertRaisesRegex(TraceError, "empty client lane"):
            step(s, "BindLocal", client="c1", decision=2)

    def test_07_fifo_bind_inversion_rejected(self):
        s, _ = self.active(1)
        for action, fields in [
            ("AcceptBatchN", {"client": "c1", "count": 2}),
            ("AcceptLocal", {"client": "c1", "decision": 1}),
            ("AcceptNoCS", {"client": "c1", "decision": 2}),
        ]:
            s, _ = step(s, action, **fields)
        event = {"action": "BindLocal", "client": "c1", "decision": 2}
        good = copy.deepcopy(s)
        good["requests"]["c1"]["entries"][1]["phase"] = "LocalBound"
        with self.assertRaisesRegex(TraceError, "FIFO head"):
            check_transition(s, event, good)

    def test_08_priority_delivery_inversion_rejected(self):
        s, _ = self.active(2)
        for c in ("c1", "c2"):
            s, _ = step(s, "AcceptScalar1", client=c)
            s, _ = step(s, "AcceptLocal", client=c, decision=1)
            s, _ = step(s, "BindLocal", client=c, decision=1)
        event = {"action": "DeliverLocal", "client": "c1", "decision": 1}
        bad = copy.deepcopy(s)
        e = bad["requests"]["c1"]["entries"][0]
        e["phase"] = "LocalDelivered"; e["delivered"] = True; e["charge_count"] = 1
        bad["requests"]["c1"]["delivered"] = 1
        from check_g4_trace import append_output
        append_output(bad, "UseCS", "c1", 1)
        with self.assertRaisesRegex(TraceError, "lexicographic"):
            check_transition(s, event, bad)

    def test_09_stale_decision_mutation_rejected(self):
        s, _ = self.active(1)
        s, _ = step(s, "AcceptScalar1", client="c1")
        event = {"action": "RejectStaleDecision", "client": "c1", "decision": 1,
                 "incoming_session_generation": 0,
                 "incoming_request_generation": 0}
        bad = copy.deepcopy(s)
        e = bad["requests"]["c1"]["entries"][0]
        e.update(kind="Remote", phase="RemoteDelivered",
                 owner_session_generation=0, owner_request_generation=0)
        bad["requests"]["c1"]["accepted"] = 1
        with self.assertRaises(TraceError):
            check_transition(s, event, bad)

    def test_10_duplicate_terminal_output_rejected(self):
        s, _ = self.active(1)
        for action, fields in [
            ("AcceptScalar1", {"client": "c1"}),
            ("AcceptRemote", {"client": "c1", "decision": 1}),
            ("AcceptDone", {"client": "c1", "decision": 1}),
        ]:
            s, _ = step(s, action, **fields)
        event = {"action": "RejectDuplicateDone", "client": "c1", "decision": 1}
        bad = copy.deepcopy(s)
        bad["requests"]["c1"]["entries"][0]["terminal_attempts"] += 1
        bad["requests"]["c1"]["entries"][0]["terminal_emits"] += 1
        with self.assertRaisesRegex(TraceError, "duplicate terminal output"):
            check_transition(s, event, bad)

    def test_11_double_release_rejected(self):
        s, _ = self.active(1)
        for action, fields in [
            ("AcceptScalar1", {"client": "c1"}),
            ("AcceptLocal", {"client": "c1", "decision": 1}),
            ("BindLocal", {"client": "c1", "decision": 1}),
            ("DeliverLocal", {"client": "c1", "decision": 1}),
            ("BeginCommit", {"client": "c1", "decision": 1}),
            ("AcceptDone", {"client": "c1", "decision": 1}),
        ]:
            s, _ = step(s, action, **fields)
        bad = copy.deepcopy(s)
        bad["requests"]["c1"]["entries"][0]["release_count"] = 2
        with self.assertRaisesRegex(TraceError, "release without charge"):
            check_transition(s, {"action": "RejectDuplicateDone", "client": "c1", "decision": 1}, bad)

    def test_12_duplicate_tail_cancel_rejected(self):
        s, _ = self.active(1)
        for action, fields in [
            ("AcceptBatchN", {"client": "c1", "count": 2}),
            ("AcceptRemote", {"client": "c1", "decision": 1}),
            ("NormalEnd", {"client": "c1"}),
            ("SettleJob", {"client": "c1", "decision": 1}),
            ("CancelTail", {"client": "c1"}),
        ]:
            s, _ = step(s, action, **fields)
        bad = copy.deepcopy(s)
        bad["requests"]["c1"]["tail_cancel_count"] = 2
        with self.assertRaisesRegex(TraceError, "duplicate client tail cancellation"):
            check_transition(s, {"action": "CloseRequest", "client": "c1"}, bad)

    def test_13_cleanup_output_reorder_rejected(self):
        s, _ = self.active(1)
        for action, fields in [
            ("AcceptBatchN", {"client": "c1", "count": 2}),
            ("AcceptRemote", {"client": "c1", "decision": 1}),
            ("AcceptRemote", {"client": "c1", "decision": 2}),
            ("NormalEnd", {"client": "c1"}),
        ]:
            s, _ = step(s, action, **fields)
        event = {"action": "SettleJob", "client": "c1", "decision": 2}
        bad = copy.deepcopy(s)
        e = bad["requests"]["c1"]["entries"][1]
        e.update(phase="Terminal", terminal_cause="Cleanup", terminal_attempts=1, terminal_emits=1)
        from check_g4_trace import append_output
        append_output(bad, "CleanupJob", "c1", 2)
        with self.assertRaisesRegex(TraceError, "ledger order"):
            check_transition(s, event, bad)

    def test_14_sequence_gap_rejected(self):
        s, h = self.active(1)
        rs = records(h)
        rs[2]["seq"] = 7
        with self.assertRaisesRegex(TraceError, "sequence gap"):
            check_records(rs, COMMIT)

    def test_15_dropped_record_footer_rejected(self):
        s, h = self.active(1)
        with self.assertRaisesRegex(TraceError, "dropped records"):
            check_records(records(h, dropped=1), COMMIT)

    def test_16_wrong_commit_and_final_digest_rejected(self):
        _, h = self.active(1)
        with self.assertRaisesRegex(TraceError, "wrong trace commit"):
            check_records(records(h, commit="bad"), COMMIT)
        rs = records(h); rs[-1]["final_digest"] = "0" * 64
        with self.assertRaisesRegex(TraceError, "final digest"):
            check_records(rs, COMMIT)


if __name__ == "__main__":
    unittest.main()
