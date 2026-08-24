#!/usr/bin/env python3
"""Validate the exact per-artifact S1b installed-identity fact roster."""

from __future__ import annotations

import collections
import re
import sys
from pathlib import Path


EXECUTABLE_LABELS = {
    "installed_icecc",
    "installed_icecc_create_env",
    "installed_iceccd",
    "installed_scheduler",
}

ARTIFACT_LABELS = EXECUTABLE_LABELS | {
    "installed_libicecc_a",
    "installed_icecc_pc",
    "package_inventory",
    "destdir_listing",
    "configure_log",
    "services_log",
    "cache_log",
    "daemon_log",
    "scheduler_log",
    "client_log",
    "install_log",
}

EXPECTED_KEYS = {
    f"{label}_{suffix}"
    for label in ARTIFACT_LABELS
    for suffix in ("mode", "size", "sha256")
}


def validate(path: Path) -> None:
    rows: list[tuple[str, str]] = []
    with path.open(encoding="utf-8") as stream:
        for number, raw in enumerate(stream, 1):
            line = raw.rstrip("\n")
            if "\t" not in line:
                raise ValueError(f"facts row {number} is not tab-separated: {line!r}")
            key, value = line.split("\t", 1)
            rows.append((key, value))

    counts = collections.Counter(key for key, _ in rows)
    missing_or_duplicate = sorted(key for key in EXPECTED_KEYS if counts[key] != 1)
    artifact_shaped = {
        key
        for key in counts
        if re.search(r"_(?:mode|size|sha256)$", key)
        and key != "installed_manifest_sha256"
    }
    unexpected = sorted(artifact_shaped - EXPECTED_KEYS)
    if missing_or_duplicate or unexpected:
        raise ValueError(
            "installed artifact facts roster mismatch: "
            f"missing_or_duplicate={missing_or_duplicate!r} unexpected={unexpected!r}"
        )

    values = dict(rows)
    for label in ARTIFACT_LABELS:
        expected_mode = "755" if label in EXECUTABLE_LABELS else "644"
        actual_mode = values[f"{label}_mode"]
        if actual_mode != expected_mode:
            raise ValueError(
                f"invalid mode fact for {label}: expected {expected_mode!r}, "
                f"got {actual_mode!r}"
            )
        size = values[f"{label}_size"]
        if not size.isdigit() or int(size) <= 0:
            raise ValueError(f"invalid size fact for {label}: {size!r}")
        digest = values[f"{label}_sha256"]
        if not re.fullmatch(r"[0-9a-f]{64}", digest):
            raise ValueError(f"invalid sha256 fact for {label}: {digest!r}")


def main(argv: list[str]) -> int:
    if len(argv) != 2:
        print(f"usage: {argv[0]} FACTS", file=sys.stderr)
        return 2
    try:
        validate(Path(argv[1]))
    except (OSError, ValueError) as error:
        print(f"INSTALLED-ARTIFACT-FACTS-SCHEMA=FAIL: {error}", file=sys.stderr)
        return 1
    print("INSTALLED-ARTIFACT-FACTS-SCHEMA=PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
