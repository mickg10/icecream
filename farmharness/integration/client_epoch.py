"""Precise wrapper timing for authenticated, drained client route restarts."""

from collections.abc import Mapping
from typing import Any

CLIENT_ROUTE_EPOCH_CONTRACT = "icefarm-client-route-wrapper-epoch-v1"


def client_route_epoch(
    event: Mapping[str, Any], client: str, started: int, finished: int,
    dispatch: int,
) -> int:
    """Resolve a job around an already authenticated pause/restart/resume.

    Store GUIDs and cold-transfer results are deliberately not inputs. Scheduler
    timestamps represent whole-second bins, not precise admission instants.
    The caller must authenticate the full restart receipt and wrapper record.
    """
    try:
        coordination = event["receipt"]["coordination"]
        pause = coordination["clients"][client]
        resume = coordination["resume"][client]
        drained = pause["finished_ms"]
        signal = coordination["signal"]["sent_ms"]
        ready = coordination["ready_ms"]
        opened = resume["started_ms"]
        released = resume["finished_ms"]
        values = (started, finished, dispatch, drained, signal, ready, opened, released)
        if any(type(value) is not int or value < 0 for value in values):
            raise ValueError("invalid client epoch timestamp")
        if (
            event["action"] != "restart" or event["instance"] != client
            or type(event["event_epoch"]) is not int or event["event_epoch"] != 1
            or event["fired_ms"] != ready
            or pause["status"] != "PAUSED" or pause["active_after"] != 0
            or resume["status"] != "OPEN"
            or not drained <= signal <= ready <= opened <= released
            or started > finished or dispatch % 1000 != 0
            or dispatch + 999 < started or dispatch > finished
        ):
            raise ValueError("inconsistent client restart timing")
        if finished <= drained:
            return 0
        if started >= opened:
            return 1
        raise ValueError("job overlaps the drained client restart interval")
    except (KeyError, TypeError) as exc:
        raise ValueError("missing client restart timing evidence") from exc
