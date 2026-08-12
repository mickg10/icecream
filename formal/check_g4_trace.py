#!/usr/bin/env python3
"""Strict JSONL conformance checker for the G4 lifecycle action registry."""
from __future__ import annotations

import argparse
import copy
import hashlib
import json
import sys
from pathlib import Path
from typing import Any

SCHEMA = "icecream.g4.lifecycle.v1"
SESSIONS = {"Disconnected", "LoginAttempt", "Active"}
REQUEST_STATES = {"Idle", "Waiting", "Closed"}
KINDS = {"None", "Remote", "Local", "NoCS"}
PHASES = {"Absent", "RemoteDelivered", "LocalWaiting", "LocalBound",
          "LocalDelivered", "LocalStarted", "Terminal"}
LIVE = PHASES - {"Absent", "Terminal"}
SLOT = {"LocalDelivered", "LocalStarted"}
DONE = {"RemoteDelivered", "LocalStarted"}
ACTIONS = {"Connect", "ConfArrived", "LegacyActivated", "ConfActivated",
           "ZeroNoop", "BatchAccepted", "BatchOverflowRejected",
           "LocalAccepted", "RemoteAccepted", "NoCSAccepted",
           "StaleDecisionRejected", "LocalBound", "LocalDelivered",
           "BeginCommitted", "BeginNoCommitFailure", "BeginAmbiguousFailure",
           "NonOwnedObserved", "NonOwnedRejected", "WrongDoneRejected",
           "Completed", "DuplicateDoneRejected", "SessionLost",
           "SessionCleaned", "RequestClosed", "ClientReset"}


class TraceError(ValueError):
    pass


def require(value: bool, message: str) -> None:
    if not value:
        raise TraceError(message)


def canonical(value: Any) -> bytes:
    return json.dumps(value, sort_keys=True, separators=(",", ":")).encode()


def state_digest(state: dict[str, Any]) -> str:
    return hashlib.sha256(canonical(state)).hexdigest()


def integer(value: Any, name: str) -> int:
    require(isinstance(value, int) and not isinstance(value, bool) and value >= 0,
            f"{name} must be a nonnegative integer")
    return value


def request(state: dict[str, Any], client: str) -> dict[str, Any]:
    require(client in state["requests"], f"unknown client {client}")
    return state["requests"][client]


def decision(state: dict[str, Any], client: str, item: str) -> dict[str, Any]:
    req = request(state, client)
    require(item in req["decisions"], f"unknown decision {client}/{item}")
    return req["decisions"][item]


def validate_state(state: dict[str, Any], where: str = "state") -> None:
    require(isinstance(state, dict), f"{where} must be an object")
    protocol = integer(state.get("protocol"), f"{where}.protocol")
    max_batch = integer(state.get("max_batch"), f"{where}.max_batch")
    capacity = integer(state.get("capacity"), f"{where}.capacity")
    generation = integer(state.get("generation"), f"{where}.generation")
    require(max_batch > 0 and capacity > 0, f"{where}: bounds must be positive")
    require(state.get("session") in SESSIONS, f"{where}: invalid session")
    require(state.get("activated_by") in {"None", "Legacy", "Conf"},
            f"{where}: invalid activation mode")
    require(isinstance(state.get("conf_arrived"), bool), f"{where}: conf_arrived")
    require(isinstance(state.get("loss_pending"), bool), f"{where}: loss_pending")
    requests = state.get("requests")
    priority = state.get("client_priority")
    require(isinstance(requests, dict) and requests, f"{where}: requests")
    require(isinstance(priority, dict) and set(priority) == set(requests),
            f"{where}: priority must cover requests exactly")
    ids: list[int] = []
    for client, rank in priority.items():
        require(isinstance(rank, dict), f"{where}: priority {client}")
        integer(rank.get("nice"), f"{where}.{client}.nice")
        cid = integer(rank.get("client_id"), f"{where}.{client}.client_id")
        require(cid > 0, f"{where}: client_id must be positive")
        ids.append(cid)
    require(len(ids) == len(set(ids)), f"{where}: duplicate client_id")

    slots = 0
    bound = 0
    live = 0
    for client, req in requests.items():
        require(req.get("state") in REQUEST_STATES, f"{where}: request state {client}")
        req_gen = integer(req.get("generation"), f"{where}.{client}.generation")
        expected = integer(req.get("expected"), f"{where}.{client}.expected")
        accepted = integer(req.get("accepted"), f"{where}.{client}.accepted")
        entries = req.get("decisions")
        require(isinstance(entries, dict) and entries, f"{where}: decisions {client}")
        observed = 0
        for item, entry in entries.items():
            require(entry.get("kind") in KINDS, f"{where}: kind {client}/{item}")
            phase = entry.get("phase")
            require(phase in PHASES, f"{where}: phase {client}/{item}")
            owner = integer(entry.get("owner_generation"), f"{where}: owner gen")
            owner_req = integer(entry.get("owner_request_generation"),
                                f"{where}: owner request gen")
            slot = entry.get("slot_charged")
            begin = entry.get("begin_committed")
            terminals = integer(entry.get("terminal_count"), f"{where}: terminal count")
            require(isinstance(slot, bool) and isinstance(begin, bool),
                    f"{where}: boolean lifecycle fields")
            require(terminals <= 1, f"{where}: duplicate terminal {client}/{item}")
            require(slot == (phase in SLOT), f"{where}: slot/phase {client}/{item}")
            require((phase == "Terminal") == (terminals == 1),
                    f"{where}: terminal shape {client}/{item}")
            if phase == "LocalStarted":
                require(begin, f"{where}: LocalStarted before BeginCommitted")
            if begin:
                require(phase in {"LocalStarted", "Terminal"},
                        f"{where}: invalid BeginCommitted shape")
            if phase == "Absent":
                require(entry["kind"] == "None" and owner == 0 and owner_req == 0,
                        f"{where}: absent entry retains identity")
            else:
                observed += 1
                require(entry["kind"] != "None" and owner > 0 and owner_req > 0,
                        f"{where}: accepted entry lacks identity")
            if phase in LIVE:
                live += 1
                if state["session"] == "Active" and not state["loss_pending"]:
                    require(owner == generation, f"{where}: stale session generation")
                    require(owner_req == req_gen, f"{where}: stale request generation")
            slots += int(slot)
            bound += int(phase == "LocalBound")
        require(accepted == observed, f"{where}: accepted ledger mismatch {client}")
        require(accepted <= expected <= max_batch, f"{where}: request bounds {client}")
        require(max_batch <= len(entries), f"{where}: max_batch exceeds ledger")
        if req["state"] == "Idle":
            require(expected == accepted == 0 and observed == 0,
                    f"{where}: idle request retains state")
        if req["state"] == "Closed":
            require(all(e["phase"] in {"Absent", "Terminal"} and not e["slot_charged"]
                        for e in entries.values()), f"{where}: closed request owns work")
    require(slots <= capacity, f"{where}: capacity exceeded")
    require(bound <= 1, f"{where}: more than one LocalBound")
    if state["session"] == "Disconnected" and not state["loss_pending"]:
        require(live == slots == 0, f"{where}: disconnected clean state owns work")
    if state["session"] == "Active":
        expected_mode = "Legacy" if protocol < 24 else "Conf"
        require(state["activated_by"] == expected_mode,
                f"{where}: active session lacks activation evidence")


def identity(event: dict[str, Any], need_decision: bool = False) -> tuple[str, str | None]:
    client = event.get("client")
    require(isinstance(client, str) and client, "transition.client required")
    item = event.get("decision")
    if need_decision:
        require(isinstance(item, str) and item, "transition.decision required")
    return client, item


def eligible_clients(state: dict[str, Any]) -> set[str]:
    result: set[str] = set()
    if state["session"] != "Active" or state["loss_pending"]:
        return result
    for client, req in state["requests"].items():
        if req["state"] != "Waiting":
            continue
        for entry in req["decisions"].values():
            if (entry["phase"] == "LocalWaiting"
                    and entry["owner_generation"] == state["generation"]
                    and entry["owner_request_generation"] == req["generation"]):
                result.add(client)
    return result


def lex_winner(state: dict[str, Any], clients: set[str]) -> str:
    require(clients, "no eligible client")
    return min(clients, key=lambda c: (state["client_priority"][c]["nice"],
                                       state["client_priority"][c]["client_id"]))


def expected_post(pre: dict[str, Any], event: dict[str, Any]) -> dict[str, Any]:
    action = event.get("action")
    require(action in ACTIONS, f"unknown action {action!r}")
    out = copy.deepcopy(pre)
    if action == "Connect":
        require(pre["session"] == "Disconnected" and not pre["loss_pending"],
                "Connect precondition")
        require(all(r["state"] == "Idle" for r in pre["requests"].values()),
                "Connect requires idle clients")
        out.update(session="LoginAttempt", conf_arrived=False, activated_by="None")
    elif action == "ConfArrived":
        require(pre["session"] == "LoginAttempt" and pre["protocol"] >= 24,
                "ConfArrived precondition")
        out["conf_arrived"] = True
    elif action in {"LegacyActivated", "ConfActivated"}:
        require(pre["session"] == "LoginAttempt", "activation precondition")
        if action == "LegacyActivated":
            require(pre["protocol"] < 24, "legacy protocol")
            mode = "Legacy"
        else:
            require(pre["protocol"] >= 24 and pre["conf_arrived"], "Conf not arrived")
            mode = "Conf"
        out.update(session="Active", generation=pre["generation"] + 1,
                   conf_arrived=False, activated_by=mode)
    elif action == "ZeroNoop":
        client, _ = identity(event)
        require(pre["session"] == "Active" and request(pre, client)["state"] == "Idle",
                "ZeroNoop precondition")
    elif action == "BatchAccepted":
        client, _ = identity(event)
        count = integer(event.get("count"), "transition.count")
        req = request(out, client)
        require(pre["session"] == "Active" and req["state"] == "Idle",
                "BatchAccepted precondition")
        require(1 <= count <= pre["max_batch"], "BatchAccepted count")
        req.update(state="Waiting", generation=req["generation"] + 1,
                   expected=count, accepted=0)
    elif action == "BatchOverflowRejected":
        client, _ = identity(event)
        require(integer(event.get("count"), "transition.count") > pre["max_batch"],
                "overflow count")
        require(request(pre, client)["state"] == "Idle", "overflow precondition")
        request(out, client)["state"] = "Closed"
    elif action in {"LocalAccepted", "RemoteAccepted", "NoCSAccepted"}:
        client, item = identity(event, True)
        req = request(out, client)
        entry = decision(out, client, item or "")
        require(pre["session"] == "Active" and req["state"] == "Waiting"
                and entry["phase"] == "Absent" and req["accepted"] < req["expected"],
                "decision acceptance precondition")
        kind = {"LocalAccepted": "Local", "RemoteAccepted": "Remote",
                "NoCSAccepted": "NoCS"}[action]
        phase = {"LocalAccepted": "LocalWaiting", "RemoteAccepted": "RemoteDelivered",
                 "NoCSAccepted": "Terminal"}[action]
        entry.update(kind=kind, phase=phase, owner_generation=pre["generation"],
                     owner_request_generation=req["generation"])
        if action == "NoCSAccepted":
            entry["terminal_count"] = 1
        req["accepted"] += 1
    elif action == "StaleDecisionRejected":
        client, item = identity(event, True)
        sg = integer(event.get("incoming_session_generation"), "incoming session generation")
        rg = integer(event.get("incoming_request_generation"), "incoming request generation")
        req = request(pre, client)
        require(sg != pre["generation"] or rg != req["generation"],
                "stale rejection has current identity")
        require(decision(pre, client, item or "")["phase"] == "Absent",
                "stale rejection altered an existing entry")
    elif action == "LocalBound":
        client, item = identity(event, True)
        entry = decision(out, client, item or "")
        require(entry["phase"] == "LocalWaiting", "LocalBound phase")
        require(not any(e["phase"] == "LocalBound" for r in pre["requests"].values()
                        for e in r["decisions"].values()), "second LocalBound")
        require(client == lex_winner(pre, eligible_clients(pre)), "priority inversion")
        entry["phase"] = "LocalBound"
    elif action == "LocalDelivered":
        client, item = identity(event, True)
        entry = decision(out, client, item or "")
        slots = sum(e["slot_charged"] for r in pre["requests"].values()
                    for e in r["decisions"].values())
        require(pre["session"] == "Active" and entry["phase"] == "LocalBound"
                and slots < pre["capacity"], "LocalDelivered precondition")
        entry.update(phase="LocalDelivered", slot_charged=True)
    elif action == "BeginCommitted":
        client, item = identity(event, True)
        entry = decision(out, client, item or "")
        require(pre["session"] == "Active" and entry["phase"] == "LocalDelivered",
                "BeginCommitted precondition")
        entry.update(phase="LocalStarted", begin_committed=True)
    elif action in {"BeginNoCommitFailure", "BeginAmbiguousFailure"}:
        client, item = identity(event, True)
        entry = decision(pre, client, item or "")
        expected_class = "none" if action == "BeginNoCommitFailure" else "ambiguous"
        require(event.get("commit_class") == expected_class, "begin failure classification")
        require(pre["session"] == "Active" and entry["phase"] == "LocalDelivered",
                "begin failure precondition")
        out.update(session="Disconnected", conf_arrived=False,
                   activated_by="None", loss_pending=True)
    elif action == "NonOwnedObserved":
        client, item = identity(event, True)
        require(decision(pre, client, item or "")["phase"] == "LocalDelivered",
                "NonOwnedObserved precondition")
    elif action == "NonOwnedRejected":
        client, item = identity(event, True)
        require(pre["session"] == "Active"
                and decision(pre, client, item or "")["phase"] == "LocalDelivered",
                "NonOwnedRejected precondition")
        out.update(session="Disconnected", conf_arrived=False,
                   activated_by="None", loss_pending=True)
    elif action == "WrongDoneRejected":
        client, item = identity(event, True)
        require(decision(pre, client, item or "")["phase"]
                in {"LocalWaiting", "LocalBound", "LocalDelivered"},
                "WrongDoneRejected phase")
    elif action == "Completed":
        client, item = identity(event, True)
        req = request(pre, client)
        entry = decision(out, client, item or "")
        require(pre["session"] == "Active" and not pre["loss_pending"]
                and entry["phase"] in DONE
                and entry["owner_generation"] == pre["generation"]
                and entry["owner_request_generation"] == req["generation"],
                "Completed authority")
        entry.update(phase="Terminal", slot_charged=False, terminal_count=1)
    elif action == "DuplicateDoneRejected":
        client, item = identity(event, True)
        require(decision(pre, client, item or "")["phase"] == "Terminal",
                "DuplicateDoneRejected precondition")
    elif action == "SessionLost":
        require(pre["session"] == "Active" and not pre["loss_pending"],
                "SessionLost precondition")
        out.update(session="Disconnected", conf_arrived=False,
                   activated_by="None", loss_pending=True)
    elif action == "SessionCleaned":
        require(pre["session"] == "Disconnected" and pre["loss_pending"],
                "SessionCleaned precondition")
        for req in out["requests"].values():
            for entry in req["decisions"].values():
                if entry["phase"] in LIVE:
                    entry.update(phase="Terminal", slot_charged=False, terminal_count=1)
            if req["state"] != "Idle":
                req["state"] = "Closed"
        out["loss_pending"] = False
    elif action == "RequestClosed":
        client, _ = identity(event)
        req = request(out, client)
        require(req["state"] == "Waiting" and req["accepted"] == req["expected"],
                "RequestClosed precondition")
        require(all(e["phase"] == "Terminal" for e in list(req["decisions"].values())[:req["expected"]]),
                "RequestClosed unsettled entries")
        req["state"] = "Closed"
    elif action == "ClientReset":
        client, _ = identity(event)
        req = request(out, client)
        require(req["state"] == "Closed" and all(e["phase"] in {"Absent", "Terminal"}
                and not e["slot_charged"] for e in req["decisions"].values()),
                "ClientReset precondition")
        req.update(state="Idle", expected=0, accepted=0)
        for entry in req["decisions"].values():
            entry.update(kind="None", phase="Absent", owner_generation=0,
                         owner_request_generation=0, slot_charged=False,
                         begin_committed=False, terminal_count=0)
    return out


def check_records(records: list[dict[str, Any]]) -> dict[str, Any]:
    require(len(records) >= 2, "trace requires header and footer")
    header, footer = records[0], records[-1]
    require(header.get("type") == "header" and header.get("schema") == SCHEMA,
            "invalid header")
    require(footer.get("type") == "footer" and footer.get("schema") == SCHEMA,
            "invalid footer")
    current = header.get("initial_state")
    validate_state(current, "header.initial_state")
    transitions = records[1:-1]
    for expected_seq, event in enumerate(transitions, 1):
        require(event.get("type") == "transition", f"record {expected_seq}: type")
        require(event.get("seq") == expected_seq, f"sequence gap at {expected_seq}")
        pre, post = event.get("pre"), event.get("post")
        require(pre == current, f"record {expected_seq}: pre-state chain mismatch")
        require(event.get("pre_digest") == state_digest(pre),
                f"record {expected_seq}: pre digest")
        require(event.get("post_digest") == state_digest(post),
                f"record {expected_seq}: post digest")
        validate_state(pre, f"record {expected_seq}.pre")
        calculated = expected_post(pre, event)
        require(calculated == post,
                f"record {expected_seq}: action {event.get('action')} has impossible post-state")
        validate_state(post, f"record {expected_seq}.post")
        current = post
    require(footer.get("records") == len(transitions), "footer record count")
    require(footer.get("dropped_records") == 0, "trace dropped records")
    require(footer.get("final_digest") == state_digest(current), "footer final digest")
    if "final_state" in footer:
        require(footer["final_state"] == current, "footer final state")
    return {"result": "PASS", "records": len(transitions),
            "final_digest": state_digest(current)}


def load_jsonl(path: Path) -> list[dict[str, Any]]:
    result: list[dict[str, Any]] = []
    for line_no, line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        if not line.strip():
            continue
        try:
            value = json.loads(line)
        except json.JSONDecodeError as exc:
            raise TraceError(f"line {line_no}: {exc}") from exc
        require(isinstance(value, dict), f"line {line_no}: object required")
        result.append(value)
    return result


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("trace", type=Path)
    args = parser.parse_args()
    try:
        result = check_records(load_jsonl(args.trace))
    except (OSError, TraceError) as exc:
        print(f"G4 TRACE REJECTED: {exc}", file=sys.stderr)
        return 1
    print(json.dumps(result, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
