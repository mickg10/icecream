#!/usr/bin/env python3
"""Final corrections layered on the pinned formal acceptance runner.

Version 2 owns tool hashes, TLC capability preflight, backend PATH pinning, and
the differential matrix. Version 3 owns real TLC trace normalization and the
expected-counterexample queue semantics. This overlay closes the remaining
preflight and artifact-isolation holes exposed by real canonical runs:

* backend version evidence must come from a successful backend-specific probe;
  nonzero usage/error output is never accepted as a version;
* LS4, which has no ordinary version flag, is identified by its pinned hash,
  embedded ``ls4-1.0`` package marker, and GNU ELF build ID parsed directly
  from the binary; and
* TLAPS runs from a byte-identical copy of every local TLA+ module under the
  external artifact tree, so `.tlacache` and generated `*_TTrace_*` modules can
  never dirty the exact source checkout.

The immutable artifact directory is rejected if it is inside the checkout.
"""

from __future__ import annotations

import os
import re
import shutil
import struct
import sys
from pathlib import Path
from typing import Any, Mapping, Sequence

sys.dont_write_bytecode = True

import run_formal_checks_v3 as v3
import run_formal_checks_v2 as v2


_VERSION_PROBES: dict[str, tuple[str, ...]] = {
    "isabelle": ("version",),
    "zenon": ("-v",),
    "z3": ("--version",),
}


def _elf_build_id(path: Path) -> str:
    """Return the GNU ELF build ID without invoking an unpinned utility."""
    try:
        data = path.read_bytes()
    except OSError as exc:
        raise v2.FormalRunError(f"cannot read ELF backend {path}: {exc}") from exc
    if len(data) < 16 or data[:4] != b"\x7fELF":
        raise v2.FormalRunError(f"{path}: LS4 backend is not an ELF binary")

    elf_class = data[4]
    encoding = data[5]
    if encoding == 1:
        endian = "<"
    elif encoding == 2:
        endian = ">"
    else:
        raise v2.FormalRunError(f"{path}: unsupported ELF data encoding {encoding}")

    if elf_class == 1:
        header_format = endian + "HHIIIIIHHHHHH"
        program_format = endian + "IIIIIIII"
        program_offset_index = 1
        program_filesz_index = 4
    elif elf_class == 2:
        header_format = endian + "HHIQQQIHHHHHH"
        program_format = endian + "IIQQQQQQ"
        program_offset_index = 2
        program_filesz_index = 5
    else:
        raise v2.FormalRunError(f"{path}: unsupported ELF class {elf_class}")

    header_size = struct.calcsize(header_format)
    if len(data) < 16 + header_size:
        raise v2.FormalRunError(f"{path}: truncated ELF header")
    header = struct.unpack_from(header_format, data, 16)
    program_table_offset = int(header[4])
    program_entry_size = int(header[8])
    program_count = int(header[9])
    minimum_program_size = struct.calcsize(program_format)
    if program_entry_size < minimum_program_size:
        raise v2.FormalRunError(
            f"{path}: ELF program-header entry is too small: {program_entry_size}"
        )
    if program_count <= 0 or program_count > 65535:
        raise v2.FormalRunError(
            f"{path}: invalid ELF program-header count {program_count}"
        )
    table_end = program_table_offset + program_entry_size * program_count
    if program_table_offset < 0 or table_end > len(data):
        raise v2.FormalRunError(f"{path}: truncated ELF program-header table")

    for index in range(program_count):
        offset = program_table_offset + index * program_entry_size
        program = struct.unpack_from(program_format, data, offset)
        if int(program[0]) != 4:  # PT_NOTE
            continue
        note_offset = int(program[program_offset_index])
        note_size = int(program[program_filesz_index])
        note_end = note_offset + note_size
        if note_offset < 0 or note_end > len(data):
            raise v2.FormalRunError(f"{path}: truncated ELF PT_NOTE segment")
        cursor = note_offset
        while cursor + 12 <= note_end:
            namesz, descsz, note_type = struct.unpack_from(
                endian + "III", data, cursor
            )
            cursor += 12
            name_end = cursor + namesz
            if name_end > note_end:
                raise v2.FormalRunError(f"{path}: truncated ELF note name")
            name = data[cursor:name_end]
            cursor = (name_end + 3) & ~3
            desc_end = cursor + descsz
            if desc_end > note_end:
                raise v2.FormalRunError(f"{path}: truncated ELF note descriptor")
            descriptor = data[cursor:desc_end]
            cursor = (desc_end + 3) & ~3
            if note_type == 3 and name.rstrip(b"\0") == b"GNU" and descriptor:
                return descriptor.hex()
    raise v2.FormalRunError(f"{path}: GNU ELF build ID was not found")


def _ls4_identity(path: Path) -> str:
    try:
        data = path.read_bytes()
    except OSError as exc:
        raise v2.FormalRunError(f"cannot read LS4 backend {path}: {exc}") from exc
    if re.search(rb"ls4-1\.0", data, re.IGNORECASE) is None:
        raise v2.FormalRunError(
            f"{path}: embedded ls4-1.0 package/build marker was not found"
        )
    return f"package=ls4-1.0; elf_build_id={_elf_build_id(path)}"


def backend_version(
    name: str, path: Path, repo: Path
) -> tuple[tuple[str, ...] | None, int | None, str]:
    """Return only successful version/build identity evidence."""
    if name == "ls4":
        return None, None, _ls4_identity(path)
    probe = _VERSION_PROBES.get(name)
    if probe is None:
        raise v2.FormalRunError(f"no version probe defined for backend {name!r}")
    result = v2.run_capture([str(path), *probe], repo, timeout=30)
    output = result["output"].strip()
    if result["returncode"] != 0:
        excerpt = output[:500] if output else "<no output>"
        raise v2.FormalRunError(
            f"backend {name}: version probe exited {result['returncode']}: {excerpt}"
        )
    if not output:
        raise v2.FormalRunError(
            f"backend {name}: successful version probe produced no output"
        )
    return tuple(result["command"]), result["returncode"], result["output"]


def pin_backends(
    raw_specs: Sequence[str], repo: Path
) -> tuple[dict[str, v2.Backend], dict[str, str]]:
    backends: dict[str, v2.Backend] = {}
    for raw in raw_specs:
        name, path, expected = v2.parse_backend_spec(raw)
        if name in backends:
            raise v2.FormalRunError(f"duplicate backend name: {name}")
        actual = v2.exact_sha(path, expected, f"TLAPS backend {name}")
        command, returncode, output = backend_version(name, path, repo)
        backends[name] = v2.Backend(
            name=name,
            path=path,
            sha256=actual,
            version_command=command,
            version_returncode=returncode,
            version_output=output,
        )

    missing = v2.REQUIRED_BACKENDS - set(backends)
    extra = set(backends) - v2.REQUIRED_BACKENDS
    if missing or extra:
        raise v2.FormalRunError(
            f"backend set must be exactly {sorted(v2.REQUIRED_BACKENDS)}; "
            f"missing={sorted(missing)}, extra={sorted(extra)}"
        )

    parents: list[str] = []
    for name in sorted(backends):
        parent = str(backends[name].path.parent)
        if parent not in parents:
            parents.append(parent)
    environment = dict(os.environ)
    environment["PATH"] = os.pathsep.join(
        [*parents, environment.get("PATH", "")]
    )
    for backend in backends.values():
        resolved = shutil.which(backend.path.name, path=environment["PATH"])
        if resolved is None or Path(resolved).resolve() != backend.path:
            raise v2.FormalRunError(
                f"backend {backend.name}: PATH resolves {backend.path.name!r} "
                f"to {resolved!r}, expected {backend.path}"
            )
    return backends, environment


def _require_clean_checkout(repo: Path, context: str) -> None:
    result = v2.run_capture(
        ["git", "status", "--porcelain", "--untracked-files=all"], repo
    )
    if result["returncode"] != 0:
        raise v2.FormalRunError(
            f"{context}: git status failed: {result['output']}"
        )
    if result["output"].strip():
        raise v2.FormalRunError(
            f"{context}: source checkout was modified:\n{result['output']}"
        )


def run_tlaps_proof(
    *,
    proof: Mapping[str, Any],
    tlapm: Path,
    formal_dir: Path,
    artifacts: Path,
    time_bin: Path,
    proof_env: Mapping[str, str],
) -> dict[str, Any]:
    """Run TLAPS from an artifact-tree copy, never from the source tree."""
    source_proof = formal_dir / proof["file"]
    if not source_proof.is_file():
        raise v2.FormalRunError(
            f"{proof['id']}: missing proof file {source_proof}"
        )

    run_dir = artifacts / proof["id"] / "tlaps"
    work_dir = run_dir / "work"
    if work_dir.exists():
        raise v2.FormalRunError(
            f"{proof['id']}: isolated TLAPS work directory already exists: {work_dir}"
        )
    work_dir.mkdir(parents=True)

    copied_inputs: dict[str, str] = {}
    for source in sorted(formal_dir.glob("*.tla")):
        destination = work_dir / source.name
        shutil.copy2(source, destination)
        copied_inputs[source.name] = v2.sha256_file(destination)
    work_proof = work_dir / source_proof.name
    if not work_proof.is_file():
        raise v2.FormalRunError(
            f"{proof['id']}: proof was not copied into isolated work directory"
        )
    if v2.sha256_file(source_proof) != v2.sha256_file(work_proof):
        raise v2.FormalRunError(
            f"{proof['id']}: isolated proof copy differs from source"
        )

    log_path = run_dir / "tlapm.log"
    metrics_path = run_dir / "time.txt"
    result = v2.run_logged(
        [str(tlapm), str(work_proof)],
        cwd=work_dir,
        log_path=log_path,
        metrics_path=metrics_path,
        time_bin=time_bin,
        timeout_seconds=int(proof.get("timeout_seconds", 1800)),
        env=proof_env,
    )

    # This checks the entire preceding TLC + TLAPS execution, not merely the
    # proof copy operation. Any unexpected repository artifact is red.
    _require_clean_checkout(formal_dir.parent, proof["id"])

    log = v2.read_text(log_path)
    if result.timed_out or result.returncode != 0:
        raise v2.FormalRunError(
            f"{proof['id']}: tlapm failed/timeout, exit {result.returncode}"
        )
    if result.peak_rss_kib is None or result.peak_rss_kib <= 0:
        raise v2.FormalRunError(
            f"{proof['id']}: positive peak RSS evidence is missing"
        )
    if re.search(r"\b(?:unproved|omitted|sorry)\b", log, re.IGNORECASE):
        raise v2.FormalRunError(
            f"{proof['id']}: proof log contains an unproved marker"
        )
    if not re.search(
        r"(?:All obligations proved|obligations? proved)",
        log,
        re.IGNORECASE,
    ):
        raise v2.FormalRunError(
            f"{proof['id']}: tlapm did not report all obligations proved"
        )

    generated_files = sorted(
        str(path.relative_to(work_dir))
        for path in work_dir.rglob("*")
        if path.is_file() and path.name not in copied_inputs
    )
    record = {
        "id": proof["id"],
        "file": proof["file"],
        "source_file_sha256": v2.sha256_file(source_proof),
        "isolated_work_dir": str(work_dir),
        "isolated_tla_inputs": copied_inputs,
        "isolated_generated_files": generated_files,
        "command": result.command,
        "returncode": result.returncode,
        "elapsed_seconds": result.elapsed_seconds,
        "peak_rss_kib": result.peak_rss_kib,
        "log_sha256": v2.sha256_file(log_path),
        "metrics_sha256": v2.sha256_file(metrics_path),
    }
    v2.write_json(run_dir / "result.json", record)
    return record


_base_self_tests = v3.run_python_self_tests


def run_python_self_tests(
    formal_dir: Path, artifacts: Path
) -> list[dict[str, Any]]:
    results = _base_self_tests(formal_dir, artifacts)
    environment = dict(os.environ)
    environment["PYTHONDONTWRITEBYTECODE"] = "1"
    script = formal_dir / "run_formal_checks_v4_test.py"
    if not script.is_file():
        raise v2.FormalRunError(f"missing Python self-test: {script}")
    result = v2.run_capture(
        [sys.executable, str(script)],
        formal_dir,
        timeout=120,
        env=environment,
    )
    log_path = artifacts / "self-tests" / f"{script.name}.log"
    log_path.parent.mkdir(parents=True, exist_ok=True)
    log_path.write_text(result["output"], encoding="utf-8")
    if result["returncode"] != 0:
        raise v2.FormalRunError(
            f"Python self-test failed: {script.name}; see {log_path}"
        )
    results.append(
        {
            "script": script.name,
            "sha256": v2.sha256_file(script),
            "command": result["command"],
            "returncode": result["returncode"],
            "elapsed_seconds": result["elapsed_seconds"],
            "log_sha256": v2.sha256_file(log_path),
        }
    )
    return results


# v2.main resolves these names from its module globals at runtime.
v2.pin_backends = pin_backends
v2.run_tlaps_proof = run_tlaps_proof
v2.run_python_self_tests = run_python_self_tests


def _is_inside(path: Path, directory: Path) -> bool:
    try:
        path.relative_to(directory)
        return True
    except ValueError:
        return False


def main(argv: Sequence[str] | None = None) -> int:
    arguments = list(argv) if argv is not None else sys.argv[1:]
    try:
        parsed = v2.build_parser().parse_args(arguments)
        repo = parsed.repo.resolve()
        artifacts = parsed.artifacts.resolve()
        # Check identity and cleanliness before v2 creates any artifact path.
        v2.git_identity(repo, parsed.expected_git_sha)
        if _is_inside(artifacts, repo):
            raise v2.FormalRunError(
                f"artifact directory must be outside the checkout: {artifacts}"
            )
    except v2.FormalRunError as exc:
        print(f"formal acceptance failed: {exc}", file=sys.stderr)
        return 1
    return v2.main(arguments)


if __name__ == "__main__":
    raise SystemExit(main())
