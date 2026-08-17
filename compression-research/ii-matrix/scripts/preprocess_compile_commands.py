#!/usr/bin/env python3
"""Replay a compile database as ordered preprocessing jobs."""

from __future__ import annotations

import argparse
from concurrent.futures import ThreadPoolExecutor
import json
import os
from pathlib import Path
import shlex
import subprocess


PAIR_FLAGS = {"-o", "-MF", "-MT", "-MQ", "--serialize-diagnostics"}
DROP_FLAGS = {"-c", "-MD", "-MMD", "-MP", "-MG", "-save-temps", "-save-temps=obj"}
DROP_PREFIXES = ("-MF", "-MT", "-MQ", "-Wp,-MD,", "-Wp,-MMD,")


def arguments(entry: dict[str, object]) -> list[str]:
    if "arguments" in entry:
        return [str(value) for value in entry["arguments"]]  # type: ignore[index]
    return shlex.split(str(entry["command"]), posix=True)


def preprocess_arguments(original: list[str], output: Path) -> list[str]:
    rewritten: list[str] = []
    skip = False
    for argument in original:
        if skip:
            skip = False
            continue
        if argument in PAIR_FLAGS:
            skip = True
            continue
        if argument in DROP_FLAGS or any(argument.startswith(prefix) for prefix in DROP_PREFIXES):
            continue
        if argument.startswith("-o") and len(argument) > 2:
            continue
        rewritten.append(argument)
    rewritten.extend(["-E", "-o", str(output)])
    return rewritten


def run_one(index: int, entry: dict[str, object], out: Path) -> dict[str, object]:
    output = (out / f"{index:08d}.ii").resolve()
    directory = Path(str(entry.get("directory", "."))).resolve()
    argv = preprocess_arguments(arguments(entry), output)
    output.parent.mkdir(parents=True, exist_ok=True)
    result = subprocess.run(argv, cwd=directory, stdout=subprocess.PIPE,
                            stderr=subprocess.PIPE, check=False)
    if result.returncode != 0:
        output.unlink(missing_ok=True)
    return {
        "ordinal": index,
        "directory": str(directory),
        "source": str(entry.get("file", "")),
        "output": str(output),
        "argv": argv,
        "returncode": result.returncode,
        "stderr": result.stderr.decode("utf-8", "replace"),
        "stdout": result.stdout.decode("utf-8", "replace"),
        "bytes": output.stat().st_size if output.is_file() else 0,
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--cc-json", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--jobs", type=int, default=max(1, os.cpu_count() or 1))
    parser.add_argument("--limit", type=int, default=0)
    parser.add_argument("--allow-failures", type=int, default=0)
    parser.add_argument("--raw-manifest", type=Path, required=True)
    parser.add_argument("--commands-out", type=Path, required=True)
    parser.add_argument("--log", type=Path, required=True)
    args = parser.parse_args()
    if args.jobs < 1 or args.limit < 0 or args.allow_failures < 0:
        raise SystemExit("invalid numeric option")

    entries = json.loads(args.cc_json.read_text(encoding="utf-8"))
    if not isinstance(entries, list) or not entries:
        raise SystemExit("compile database is empty or invalid")
    if args.limit:
        entries = entries[:args.limit]
    args.out.mkdir(parents=True, exist_ok=True)
    args.raw_manifest.parent.mkdir(parents=True, exist_ok=True)
    args.commands_out.parent.mkdir(parents=True, exist_ok=True)
    args.log.parent.mkdir(parents=True, exist_ok=True)

    with ThreadPoolExecutor(max_workers=args.jobs) as pool:
        futures = [pool.submit(run_one, index, entry, args.out)
                   for index, entry in enumerate(entries)]
        results = [future.result() for future in futures]

    successes = [row for row in results if row["returncode"] == 0]
    failures = [row for row in results if row["returncode"] != 0]
    manifest_root = args.raw_manifest.resolve().parent
    manifest_paths: list[str] = []
    for row in successes:
        output = Path(str(row["output"])).resolve(strict=True)
        try:
            relative = output.relative_to(manifest_root)
        except ValueError as error:
            raise SystemExit(f"preprocessed output is outside cell root: {output}") from error
        manifest_paths.append(relative.as_posix())
    args.raw_manifest.write_text("".join(f"{path}\n" for path in manifest_paths),
                                 encoding="utf-8")
    with args.commands_out.open("w", encoding="utf-8") as stream:
        for row in results:
            stream.write(json.dumps(row, sort_keys=True) + "\n")
    with args.log.open("w", encoding="utf-8") as stream:
        stream.write(f"total={len(results)} success={len(successes)} failure={len(failures)}\n")
        for row in failures:
            stream.write(f"\n[{row['ordinal']}] rc={row['returncode']} source={row['source']}\n")
            stream.write(str(row["stderr"]))

    print(f"preprocessed {len(successes)}/{len(results)} TUs; failures={len(failures)}")
    if not successes or len(failures) > args.allow_failures:
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
