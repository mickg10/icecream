#!/usr/bin/env python3
"""Validate one retained icecream-experiment-v2 JSONL stream."""

from __future__ import annotations

import argparse
import json
from pathlib import Path

from run_scenario import validate_experiment_jsonl


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("experiment", type=Path)
    args = parser.parse_args()
    print(json.dumps(validate_experiment_jsonl(args.experiment), sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
