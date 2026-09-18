"""Pure validation of the narrow same-endpoint P50 retry exception."""

from collections.abc import Mapping
from typing import Any


def same_endpoint_decision_valid(
    failure: Mapping[str, Any], binding: Mapping[str, Any] | None = None
) -> bool:
    """Require positive scheduler evidence, never infer permission from silence.

    The collector authenticates the source log; the verdict repeats these
    identity and dispatch checks on its retained observation. This does not
    replace the caller's failure-class, one-retry, or exact-output checks.
    """
    proof = failure.get("same_endpoint_decision")
    fields = {
        "generation",
        "job",
        "epoch",
        "nonce",
        "failed_host",
        "failed_port",
        "selected_host",
        "selected_port",
        "profile",
        "compatible_alternative",
        "line",
        "timestamp_ms",
        "new_line",
        "put_line",
        "put_ms",
        "worker",
        "source_path",
    }
    if not isinstance(proof, Mapping) or set(proof) != fields:
        return False
    for field in (
        "generation",
        "job",
        "epoch",
        "nonce",
        "failed_port",
        "selected_port",
        "line",
        "timestamp_ms",
        "new_line",
        "put_line",
        "put_ms",
    ):
        if type(proof[field]) is not int or proof[field] < 1:
            return False
    if (
        proof["job"] >= 2**32
        or max(proof["epoch"], proof["nonce"]) >= 2**64
        or max(proof["failed_port"], proof["selected_port"]) > 65535
        or type(proof["profile"]) is not int
        or proof["profile"] != 1
        or type(proof["compatible_alternative"]) is not int
        or proof["compatible_alternative"] != 0
        or not proof["new_line"] < proof["line"] < proof["put_line"]
        or proof["timestamp_ms"] > proof["put_ms"]
    ):
        return False
    if any(
        not isinstance(proof[field], str) or not proof[field]
        for field in ("failed_host", "selected_host", "worker", "source_path")
    ):
        return False
    failed_endpoint = f"{proof['failed_host']}:{proof['failed_port']}"
    selected_endpoint = f"{proof['selected_host']}:{proof['selected_port']}"
    if not (
        failed_endpoint
        == selected_endpoint
        == failure.get("failed_endpoint")
        == failure.get("retry_endpoint")
        and proof["job"] == failure.get("retry_scheduler_job")
        and proof["epoch"] == failure.get("retry_assignment_epoch")
        and proof["nonce"] == failure.get("retry_assignment_nonce")
    ):
        return False
    if binding is not None and not (
        proof["generation"] == binding.get("final_generation")
        and proof["job"] == binding.get("final_scheduler_job")
        and proof["put_ms"] == binding.get("final_dispatch_ms")
        and proof["worker"] == binding.get("final_worker")
        and binding.get("first_worker") == binding.get("final_worker")
    ):
        return False
    return True
