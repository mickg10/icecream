import copy

import pytest

from farmharness.integration.client_epoch import client_route_epoch


def event():
    return {
        "action": "restart", "instance": "C1", "event_epoch": 1,
        "fired_ms": 1789846618134,
        "receipt": {"coordination": {
            "clients": {"C1": {"finished_ms": 1789846615949,
                                "status": "PAUSED", "active_after": 0}},
            "signal": {"sent_ms": 1789846616896},
            "ready_ms": 1789846618134,
            "resume": {"C1": {"started_ms": 1789846618745,
                               "finished_ms": 1789846618746, "status": "OPEN"}},
        }},
    }


def test_retained_first_post_restart_job_uses_precise_wrapper_time():
    witness = event()
    dispatch = 1789846618000
    assert dispatch < witness["fired_ms"]  # Historical timestamp law is wrong.
    assert client_route_epoch(witness, "C1", 1789846618801,
                              1789846619071, dispatch) == 1
    assert client_route_epoch(witness, "C1", 1789846614100,
                              1789846615500, 1789846614000) == 0


@pytest.mark.parametrize("started,finished,dispatch", [
    (1789846615800, 1789846618900, 1789846618000),  # crosses drain
    (1789846618000, 1789846618200, 1789846618000),  # before resume
    (1789846618801, 1789846619071, 1789846617000),  # stale dispatch
    (1789846618801, 1789846619071, 1789846620000),  # future dispatch
    (1789846618801, 1789846619071, 1789846618801),  # not second-resolution
    (True, 1789846619071, 1789846618000),
    (1789846619071, 1789846618801, 1789846618000),
])
def test_invalid_job_timing_fails_closed(started, finished, dispatch):
    with pytest.raises(ValueError):
        client_route_epoch(event(), "C1", started, finished, dispatch)


@pytest.mark.parametrize("mutation", ["resume_early", "not_drained", "missing", "wrong_client"])
def test_invalid_gate_timing_fails_closed(mutation):
    witness = copy.deepcopy(event())
    coordination = witness["receipt"]["coordination"]
    if mutation == "resume_early":
        coordination["resume"]["C1"]["started_ms"] = 1789846617000
    elif mutation == "not_drained":
        coordination["clients"]["C1"]["active_after"] = 1
    elif mutation == "missing":
        del coordination["signal"]
    else:
        witness["instance"] = "C2"
    with pytest.raises(ValueError):
        client_route_epoch(witness, "C1", 1789846618801,
                           1789846619071, 1789846618000)
