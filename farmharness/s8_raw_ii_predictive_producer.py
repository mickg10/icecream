#!/usr/bin/env python3
"""Produce the trace-free RAW_II control curve from explicit wire inputs.

RAW_II is the whole-legacy control arm.  This producer does not call the
compressed predictive engine and does not alias P29.  The legacy-wire witness
supplies the exact C-to-F frame accounting (CompileFile + FileChunk + End); a
separately scoped control engine supplies its F-to-C and elapsed estimates.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import stat
import subprocess
import sys
from collections import deque
from pathlib import Path
from typing import Any, Iterator

try:
    from . import s8_depth_runner as depth_runner
    from . import s8_predictive_live_normalizer as normalizer
    from .s8_schema import ALL_CORPORA, ALL_SPLITS, CONTROL_PROFILES, CURRENT_SEMANTICS, REGIMES
except ImportError:  # pragma: no cover
    import s8_depth_runner as depth_runner
    import s8_predictive_live_normalizer as normalizer
    from s8_schema import ALL_CORPORA, ALL_SPLITS, CONTROL_PROFILES, CURRENT_SEMANTICS, REGIMES

DepthPlanError = depth_runner.DepthPlanError
build_schedule = depth_runner.build_schedule


SCHEMA = "icecream-s8-raw-ii-predictive-producer-v2"
ENGINE_SCHEMA = "icecream-s8-raw-ii-control-engine-v2"
WITNESS_SCHEMA = "icecream-s8-raw-ii-legacy-wire-witness-v1"
FORMULA = {
    "name": "legacy-filechunk-wire-v1",
    "c_to_f": "compile_file_bytes+file_chunk_bytes+end_bytes",
}
ENGINE_SCOPE = "raw_ii_control_engine"
MAX_BYTES = 64 * 1024 * 1024
MAX_UNTRACKED_ENTRIES = 4096
MAX_UNTRACKED_BYTES = 256 * 1024 * 1024
MAX_TRACKED_ENTRIES = 1_000_000
MAX_PRODUCT_WALK_ENTRIES = 1_000_000
MAX_PRODUCT_WALK_DEPTH = 64
MAX_PRODUCT_WALK_PATH_BYTES = 256 * 1024 * 1024
MAX_GIT_STATUS_ENTRIES = 1_000_000
MAX_GIT_STATUS_BYTES = 256 * 1024 * 1024
HEX64 = re.compile(r"^[0-9a-fA-F]{64}$")
HEX40 = re.compile(r"^[0-9a-fA-F]{40}$")


class RawIIError(ValueError):
    """A RAW_II control input is absent, ambiguous, or malformed."""


def _canonical(value: object) -> bytes:
    return (json.dumps(value, sort_keys=True, separators=(",", ":"),
                       ensure_ascii=True, allow_nan=False) + "\n").encode("ascii")


def _read(path: Path, label: str) -> tuple[dict[str, Any], dict[str, object]]:
    fd: int | None = None
    try:
        flags = (os.O_RDONLY | getattr(os, "O_CLOEXEC", 0) |
                 getattr(os, "O_NOFOLLOW", 0))
        fd = os.open(path, flags)
        info = os.fstat(fd)
        if (not stat.S_ISREG(info.st_mode) or info.st_nlink != 1):
            os.close(fd)
            fd = None
            raise RawIIError(f"{label}:not_private_regular_file")
    except OSError as exc:
        if fd is not None:
            try:
                os.close(fd)
            except OSError:
                pass
        raise RawIIError(f"{label}:unavailable:{path}") from exc
    try:
        if info.st_size > MAX_BYTES:
            raise RawIIError(f"{label}:too_large")
        digest = hashlib.sha256()
        chunks: list[bytes] = []
        size = 0
        while True:
            block = os.read(fd, 1 << 20)
            if not block:
                break
            size += len(block)
            if size > MAX_BYTES:
                raise RawIIError(f"{label}:too_large")
            chunks.append(block)
            digest.update(block)
        after = os.fstat(fd)
        if any(getattr(info, field) != getattr(after, field)
               for field in ("st_dev", "st_ino", "st_size", "st_mtime_ns", "st_ctime_ns")):
            raise RawIIError(f"{label}:changed_while_reading")
        raw = b"".join(chunks)
    except RawIIError:
        raise
    except OSError as exc:
        raise RawIIError(f"{label}:unavailable:{path}") from exc
    finally:
        if fd is not None:
            os.close(fd)
    try:
        value = json.loads(
            raw.decode("utf-8"), object_pairs_hook=_unique_keys,
            parse_constant=lambda token: (_ for _ in ()).throw(
                RawIIError(f"{label}:non_finite_json")))
    except (UnicodeDecodeError, json.JSONDecodeError) as exc:
        raise RawIIError(f"{label}:invalid_json") from exc
    if not isinstance(value, dict):
        raise RawIIError(f"{label}:object_required")
    return value, {"path": str(path.resolve()), "bytes": len(raw),
                   "sha256": digest.hexdigest()}


def _unique_keys(pairs: list[tuple[str, object]]) -> dict[str, object]:
    result: dict[str, object] = {}
    for key, value in pairs:
        if key in result:
            raise RawIIError(f"duplicate_json_key:{key}")
        result[key] = value
    return result


def _hex(value: object, label: str, pattern: re.Pattern[str] = HEX64) -> str:
    if (not isinstance(value, str) or pattern.fullmatch(value) is None or
            int(value, 16) == 0):
        raise RawIIError(f"{label}:invalid_digest")
    return value.lower()


def _relative(value: object, label: str) -> str:
    if (not isinstance(value, str) or not value or os.path.isabs(value) or
            any(part in ("", ".", "..") for part in Path(value).parts)):
        raise RawIIError(f"{label}:relative_path_invalid")
    return value


def _occurrence_key(ordinal: object, source_relative: object,
                    digest: object, source_bytes: object, label: str
                    ) -> tuple[int, str, str, int]:
    if type(ordinal) is not int or ordinal < 0:
        raise RawIIError(f"{label}.ordinal:invalid")
    relative = _relative(source_relative, f"{label}.source_relative")
    normalized_digest = _hex(digest, f"{label}.source_sha256")
    if type(source_bytes) is not int or source_bytes < 0:
        raise RawIIError(f"{label}.source_bytes:invalid")
    return ordinal, relative, normalized_digest, source_bytes


def _positive(value: object, label: str) -> int:
    if type(value) is not int or value <= 0:
        raise RawIIError(f"{label}:positive_integer_required")
    return value


def _cell(value: object, label: str) -> dict[str, str]:
    if (not isinstance(value, dict) or set(value) != {"corpus", "profile", "regime"} or
            value.get("corpus") not in ALL_CORPORA or
            value.get("profile") not in CONTROL_PROFILES or
            value.get("regime") not in REGIMES):
        raise RawIIError(f"{label}:cell_invalid")
    return {key: str(value[key]) for key in ("corpus", "profile", "regime")}


def _wire_total(value: object, label: str) -> int:
    if (not isinstance(value, dict) or
            set(value) != {"compile_file_bytes", "file_chunk_bytes", "end_bytes", "total_bytes"}):
        raise RawIIError(f"{label}:fields_invalid")
    parts = [_positive(value[key], f"{label}.{key}")
             for key in ("compile_file_bytes", "file_chunk_bytes", "end_bytes")]
    total = _positive(value["total_bytes"], f"{label}.total_bytes")
    if sum(parts) != total:
        raise RawIIError(f"{label}:formula_mismatch")
    return total


def _load_witness(path: Path, cell: dict[str, str]) -> tuple[dict[tuple[int, str, str, int], dict[str, object]], dict[str, object]]:
    value, facts = _read(path, "raw_ii_witness")
    if (value.get("schema") != WITNESS_SCHEMA or value.get("semantics") != CURRENT_SEMANTICS or
            value.get("cell") != cell or value.get("split") != ALL_SPLITS[cell["corpus"]] or
            value.get("formula") != FORMULA):
        raise RawIIError("raw_ii_witness:scope_or_formula_invalid")
    rows = value.get("rows")
    if not isinstance(rows, list) or not rows:
        raise RawIIError("raw_ii_witness:rows_invalid")
    result: dict[tuple[int, str, str, int], dict[str, object]] = {}
    for index, row in enumerate(rows):
        if (not isinstance(row, dict) or set(row) != {
                "ordinal", "source_relative", "source_sha256", "source_bytes", "c_to_f"}):
            raise RawIIError(f"raw_ii_witness:row_invalid:{index}")
        key = _occurrence_key(row["ordinal"], row["source_relative"],
                              row["source_sha256"], row["source_bytes"],
                              f"raw_ii_witness.row[{index}]")
        if key in result:
            raise RawIIError("raw_ii_witness:duplicate_occurrence")
        result[key] = {"source_bytes": key[3],
                       "c_to_f_bytes": _wire_total(
                           row["c_to_f"], f"raw_ii_witness.row[{index}].c_to_f")}
    return result, facts


def _load_engine(path: Path, cell: dict[str, str]) -> tuple[str, dict[tuple[int, str, str, int], tuple[int, int]], dict[str, object]]:
    value, facts = _read(path, "raw_ii_engine_manifest")
    if (value.get("schema") != ENGINE_SCHEMA or value.get("semantics") != CURRENT_SEMANTICS or
            value.get("cell") != cell or value.get("split") != ALL_SPLITS[cell["corpus"]] or
            value.get("control_baseline") != normalizer.CONTROL_BASELINE or
            value.get("engine_scope") != ENGINE_SCOPE):
        raise RawIIError("raw_ii_engine_manifest:scope_invalid")
    model_id = value.get("model_id")
    if not isinstance(model_id, str) or not re.fullmatch(r"[A-Za-z0-9_.-]+", model_id):
        raise RawIIError("raw_ii_engine_manifest:model_id_invalid")
    rows = value.get("rows")
    if not isinstance(rows, list) or not rows:
        raise RawIIError("raw_ii_engine_manifest:rows_invalid")
    result: dict[tuple[int, str, str, int], tuple[int, int, int, int]] = {}
    for index, row in enumerate(rows):
        if (not isinstance(row, dict) or set(row) != {
                "ordinal", "source_relative", "source_sha256", "source_bytes",
                "f_to_c_bytes", "elapsed_ns"}):
            raise RawIIError(f"raw_ii_engine_manifest:row_invalid:{index}")
        key = _occurrence_key(row["ordinal"], row["source_relative"],
                              row["source_sha256"], row["source_bytes"],
                              f"raw_ii_engine_manifest.row[{index}]")
        if key in result:
            raise RawIIError("raw_ii_engine_manifest:duplicate_occurrence")
        f_to_c = _positive(row["f_to_c_bytes"], f"raw_ii_engine_manifest.row[{index}].f_to_c_bytes")
        elapsed = _positive(row["elapsed_ns"],
                            f"raw_ii_engine_manifest.row[{index}].elapsed_ns")
        result[key] = (f_to_c, elapsed)
    return model_id, result, facts


def _load_plan(path: Path, cell: dict[str, str], depth: str) -> tuple[dict[str, Any], list[dict[str, Any]], dict[str, object]]:
    value, plan_facts = _read(path, "plan")
    if value.get("schema") != "icecream-s8-depth-run-plan-v1" or value.get("semantics") != CURRENT_SEMANTICS:
        raise RawIIError("plan:schema_invalid")
    if value.get("cell") != cell or value.get("split") != ALL_SPLITS[cell["corpus"]]:
        raise RawIIError("plan:cell_mismatch")
    expected_scope = ("canonical_s8" if cell["corpus"] in depth_runner.CORPORA
                      else "expanded_descriptive")
    if value.get("evaluation_scope") != expected_scope:
        raise RawIIError("plan:evaluation_scope_invalid")
    request = value.get("request")
    expected_depth: object = int(depth) if depth.isdigit() else depth
    if not isinstance(request, dict) or request.get("depth") != expected_depth:
        raise RawIIError("plan:depth_mismatch")
    contract = value.get("execution_contract")
    if (not isinstance(contract, dict) or contract.get("status") != "READY_RAW_II_CONTROL" or
            contract.get("producer") != "farmharness.s8_raw_ii_predictive_producer" or
            contract.get("control_baseline") != normalizer.CONTROL_BASELINE):
        raise RawIIError("plan:raw_ii_control_contract_invalid")
    inputs = value.get("inputs")
    scheduling = value.get("scheduling")
    if not isinstance(inputs, list) or not inputs or not isinstance(scheduling, dict):
        raise RawIIError("plan:inputs_or_scheduling_invalid")
    # Re-read the source authority and every selected occurrence immediately
    # before prediction.  A plan is a digest-bearing handoff, not permission
    # to use a source file that changed after planning.
    source_manifest = value.get("source_manifest")
    source_root = value.get("source_root")
    if (not isinstance(source_manifest, dict) or
            set(source_manifest) != {"path", "sha256", "bytes", "entries"} or
            not isinstance(source_root, str) or not source_root):
        raise RawIIError("plan:source_manifest_invalid")
    source_path = Path(str(source_manifest["path"]))
    try:
        actual_inputs, actual_manifest = depth_runner._manifest_inputs(
            source_path, Path(source_root), "source_manifest")
    except (DepthPlanError, OSError) as exc:
        raise RawIIError("plan:source_manifest_unavailable") from exc
    if (actual_manifest["sha256"] != source_manifest.get("sha256") or
            actual_manifest["bytes"] != source_manifest.get("bytes") or
            len(actual_inputs) != source_manifest.get("entries")):
        raise RawIIError("plan:source_manifest_changed")
    request = value.get("request")
    if not isinstance(request, dict) or request.get("depth") not in (100, 200, "full", "repeat-full"):
        raise RawIIError("plan:request_invalid")
    requested_depth = request["depth"]
    try:
        expected_inputs, expected_selection = depth_runner.select_inputs(
            actual_inputs, requested_depth)
    except DepthPlanError as exc:
        raise RawIIError("plan:input_selection_invalid") from exc
    if inputs != expected_inputs:
        raise RawIIError("plan:input_sequence_not_declared_selection")
    if request.get("source_selection") != expected_selection:
        raise RawIIError("plan:source_selection_invalid")
    for index, item in enumerate(inputs):
        if (not isinstance(item, dict) or set(item) != {
                "ordinal", "path", "source_relative", "sha256", "bytes"} or
                item["ordinal"] != index):
            raise RawIIError(f"plan:input_descriptor_invalid:{index}")
        key = _occurrence_key(item["ordinal"], item["source_relative"],
                              item["sha256"], item["bytes"],
                              f"plan.inputs[{index}]")
        expected = expected_inputs[index]
        if key != _occurrence_key(expected["ordinal"], expected["source_relative"],
                                   expected["sha256"], expected["bytes"],
                                   f"plan.expected[{index}]") or item["path"] != expected["path"]:
            raise RawIIError(f"plan:input_descriptor_mismatch:{index}")
        try:
            input_facts = depth_runner._digest(Path(item["path"]), f"plan.input[{index}]")
        except DepthPlanError as exc:
            raise RawIIError(f"plan:input_unavailable:{index}") from exc
        if input_facts["sha256"] != key[2] or input_facts["bytes"] != key[3]:
            raise RawIIError(f"plan:input_changed:{index}")
    try:
        expected = build_schedule(inputs, str(scheduling["topology"]), str(scheduling["depth_class"]))
    except (KeyError, DepthPlanError) as exc:
        raise RawIIError("plan:scheduling_invalid") from exc
    if scheduling != expected:
        raise RawIIError("plan:scheduling_authentication_failed")
    return value, inputs, plan_facts


def _git_untracked_paths(root: Path, *, ignored: bool) -> list[str]:
    """Read NUL-delimited paths with caps applied before buffering them."""
    command = ["git", "-C", str(root), "ls-files", "--others"]
    if ignored:
        command.append("--ignored")
    command.extend(("--exclude-standard", "-z"))
    process: subprocess.Popen[bytes] | None = None
    pending = bytearray()
    paths: list[str] = []
    enumerated_bytes = 0

    def reap_after_failure() -> None:
        if process is None:
            return
        try:
            if process.poll() is None:
                process.terminate()
                process.wait(timeout=1)
        except (OSError, subprocess.SubprocessError):
            try:
                process.kill()
                process.wait(timeout=1)
            except (OSError, subprocess.SubprocessError):
                pass
        finally:
            if process.stdout is not None:
                try:
                    process.stdout.close()
                except OSError:
                    pass

    try:
        process = subprocess.Popen(command, stdout=subprocess.PIPE,
                                   stderr=subprocess.DEVNULL)
        assert process.stdout is not None
        while True:
            chunk = process.stdout.read(1 << 16)
            if not chunk:
                break
            if enumerated_bytes + len(pending) + len(chunk) > MAX_UNTRACKED_BYTES:
                raise RawIIError("product_root:untracked_inventory_too_large")
            pending.extend(chunk)
            while True:
                separator = pending.find(0)
                if separator < 0:
                    break
                raw = bytes(pending[:separator])
                del pending[:separator + 1]
                if not raw:
                    continue
                enumerated_bytes += len(raw)
                if len(paths) >= MAX_UNTRACKED_ENTRIES:
                    raise RawIIError("product_root:untracked_inventory_too_many")
                if enumerated_bytes > MAX_UNTRACKED_BYTES:
                    raise RawIIError("product_root:untracked_inventory_too_large")
                paths.append(os.fsdecode(raw))
        if pending:
            raise RawIIError("product_root:untracked_inventory_malformed")
        if process.wait(timeout=15) != 0:
            raise RawIIError("product_root:untracked_inventory_unavailable")
    except RawIIError:
        reap_after_failure()
        raise
    except (OSError, subprocess.SubprocessError) as exc:
        reap_after_failure()
        raise RawIIError("product_root:untracked_inventory_unavailable") from exc
    return paths


def _git_tracked_paths(root: Path) -> set[str]:
    """Stream the tracked path set with the same bounded NUL parser."""
    command = ["git", "-C", str(root), "ls-files", "-v", "-z"]
    process: subprocess.Popen[bytes] | None = None
    pending = bytearray()
    paths: set[str] = set()
    enumerated_bytes = 0

    def reap_after_failure() -> None:
        if process is None:
            return
        try:
            if process.poll() is None:
                process.terminate()
                process.wait(timeout=1)
        except (OSError, subprocess.SubprocessError):
            try:
                process.kill()
                process.wait(timeout=1)
            except (OSError, subprocess.SubprocessError):
                pass
        finally:
            if process.stdout is not None:
                try:
                    process.stdout.close()
                except OSError:
                    pass

    try:
        process = subprocess.Popen(command, stdout=subprocess.PIPE,
                                   stderr=subprocess.DEVNULL)
        assert process.stdout is not None
        while True:
            chunk = process.stdout.read(1 << 16)
            if not chunk:
                break
            if (enumerated_bytes + len(pending) + len(chunk) >
                    MAX_PRODUCT_WALK_PATH_BYTES):
                raise RawIIError("product_root:tracked_inventory_too_large")
            pending.extend(chunk)
            while True:
                separator = pending.find(0)
                if separator < 0:
                    break
                raw = bytes(pending[:separator])
                del pending[:separator + 1]
                if not raw:
                    continue
                enumerated_bytes += len(raw)
                if len(paths) >= MAX_TRACKED_ENTRIES:
                    raise RawIIError("product_root:tracked_inventory_too_many")
                if enumerated_bytes > MAX_PRODUCT_WALK_PATH_BYTES:
                    raise RawIIError("product_root:tracked_inventory_too_large")
                decoded = os.fsdecode(raw)
                if len(decoded) < 3 or decoded[1] != " ":
                    raise RawIIError("product_root:tracked_inventory_malformed")
                if decoded[0] in {"h", "s", "S"}:
                    raise RawIIError("product_root:tracked_index_flags_set")
                paths.add(decoded[2:])
        if pending:
            raise RawIIError("product_root:tracked_inventory_malformed")
        if process.wait(timeout=15) != 0:
            raise RawIIError("product_root:tracked_inventory_unavailable")
    except RawIIError:
        reap_after_failure()
        raise
    except (OSError, subprocess.SubprocessError) as exc:
        reap_after_failure()
        raise RawIIError("product_root:tracked_inventory_unavailable") from exc
    return paths


def _git_status_snapshot(root: Path) -> str:
    """Capture bounded NUL-delimited status, including ignored outputs."""
    command = ["git", "-C", str(root), "status", "--porcelain=v1",
               "--untracked-files=all", "--ignored", "-z"]
    process: subprocess.Popen[bytes] | None = None
    chunks: list[bytes] = []
    total = 0
    records = 0

    def reap_after_failure() -> None:
        if process is None:
            return
        try:
            if process.poll() is None:
                process.terminate()
                process.wait(timeout=1)
        except (OSError, subprocess.SubprocessError):
            try:
                process.kill()
                process.wait(timeout=1)
            except (OSError, subprocess.SubprocessError):
                pass
        finally:
            if process.stdout is not None:
                try:
                    process.stdout.close()
                except OSError:
                    pass

    try:
        process = subprocess.Popen(command, stdout=subprocess.PIPE,
                                   stderr=subprocess.DEVNULL)
        assert process.stdout is not None
        while True:
            chunk = process.stdout.read(1 << 16)
            if not chunk:
                break
            total += len(chunk)
            if total > MAX_GIT_STATUS_BYTES:
                raise RawIIError("product_root:git_status_too_large")
            records += chunk.count(b"\0")
            if records > MAX_GIT_STATUS_ENTRIES:
                raise RawIIError("product_root:git_status_too_many")
            chunks.append(chunk)
        if process.wait(timeout=15) != 0:
            raise RawIIError("product_root:git_status_unavailable")
    except RawIIError:
        reap_after_failure()
        raise
    except (OSError, subprocess.SubprocessError) as exc:
        reap_after_failure()
        raise RawIIError("product_root:git_status_unavailable") from exc
    raw = b"".join(chunks)
    if raw and not raw.endswith(b"\0"):
        raise RawIIError("product_root:git_status_malformed")
    return os.fsdecode(raw)


def _status_is_tracked_dirty(status: str) -> bool:
    for record in status.split("\0"):
        if not record:
            continue
        if len(record) < 3 or record[2] != " ":
            raise RawIIError("product_root:git_status_malformed")
        if record[:3] not in {"?? ", "!! "}:
            return True
    return False


def _walk_product_entries(root: Path) -> Iterator[tuple[str, stat.stat_result]]:
    """Walk product entries without following links and with explicit caps."""
    # Keep an open directory FD for every pending descent.  Child opens are
    # relative to that FD with O_NOFOLLOW|O_DIRECTORY, so a replacement of a
    # path component cannot redirect the walk.
    directory_flags = (os.O_RDONLY | getattr(os, "O_CLOEXEC", 0) |
                       getattr(os, "O_NOFOLLOW", 0) |
                       getattr(os, "O_DIRECTORY", 0))
    try:
        root_fd = os.open(root, directory_flags)
    except OSError as exc:
        raise RawIIError("product_root:walk_unavailable") from exc
    pending: deque[tuple[int, str, int]] = deque([(root_fd, "", 0)])
    entry_count = 0
    path_bytes = 0
    try:
        while pending:
            directory_fd, relative_directory, depth = pending.pop()
            if depth >= MAX_PRODUCT_WALK_DEPTH:
                os.close(directory_fd)
                raise RawIIError("product_root:walk_depth_exceeded")
            try:
                iterator = os.scandir(directory_fd)
            except OSError as exc:
                os.close(directory_fd)
                raise RawIIError("product_root:walk_unavailable") from exc
            try:
                for entry in iterator:
                    if not relative_directory and entry.name == ".git":
                        continue
                    name = os.fsdecode(entry.name)
                    relative = (name if not relative_directory else
                                f"{relative_directory}/{name}")
                    candidate = Path(relative)
                    if (candidate.is_absolute() or
                            any(part in ("", ".", "..") for part in candidate.parts)):
                        raise RawIIError("product_root:walk_path_invalid")
                    encoded = os.fsencode(relative)
                    path_bytes += len(encoded)
                    if path_bytes > MAX_PRODUCT_WALK_PATH_BYTES:
                        raise RawIIError("product_root:walk_inventory_too_large")
                    if entry_count >= MAX_PRODUCT_WALK_ENTRIES:
                        raise RawIIError("product_root:walk_inventory_too_many")
                    try:
                        info = entry.stat(follow_symlinks=False)
                    except OSError as exc:
                        raise RawIIError("product_root:walk_unavailable") from exc
                    entry_count += 1
                    yield relative, info
                    if stat.S_ISDIR(info.st_mode):
                        if not _generated_owned(info):
                            raise RawIIError("product_root:untracked_artifact_invalid")
                        if depth + 1 >= MAX_PRODUCT_WALK_DEPTH:
                            raise RawIIError("product_root:walk_depth_exceeded")
                        try:
                            child_fd = os.open(entry.name, directory_flags,
                                               dir_fd=directory_fd)
                        except OSError as exc:
                            raise RawIIError("product_root:walk_unavailable") from exc
                        pending.append((child_fd, relative, depth + 1))
            finally:
                iterator.close()
                os.close(directory_fd)
    finally:
        while pending:
            try:
                os.close(pending.pop()[0])
            except OSError:
                pass


def _generated_owned(info: stat.stat_result) -> bool:
    return (info.st_uid == os.geteuid() and info.st_gid == os.getegid())


def _untracked_inventory(root: Path) -> dict[str, object]:
    """Bind bounded content identity for generated files outside HEAD."""
    git_relatives = sorted(set(_git_untracked_paths(root, ignored=False) +
                               _git_untracked_paths(root, ignored=True)))
    tracked = _git_tracked_paths(root)
    walked = _walk_product_entries(root)
    walk_relatives: set[str] = set()
    for relative, info in walked:
        if relative in tracked:
            if not stat.S_ISREG(info.st_mode) or info.st_nlink != 1:
                raise RawIIError("product_root:tracked_artifact_invalid")
            continue
        if stat.S_ISDIR(info.st_mode):
            continue
        walk_relatives.add(relative)
        if not _generated_owned(info):
            raise RawIIError("product_root:untracked_artifact_invalid")
        if not stat.S_ISREG(info.st_mode) or info.st_nlink != 1:
            raise RawIIError("product_root:untracked_artifact_invalid")
    if walk_relatives != set(git_relatives):
        raise RawIIError("product_root:untracked_inventory_changed")
    relatives = git_relatives
    if len(relatives) > MAX_UNTRACKED_ENTRIES:
        raise RawIIError("product_root:untracked_inventory_too_many")
    if sum(len(os.fsencode(relative)) for relative in relatives) > MAX_UNTRACKED_BYTES:
        raise RawIIError("product_root:untracked_inventory_too_large")
    entries: list[dict[str, object]] = []
    total_bytes = 0
    for relative in relatives:
        candidate = Path(relative)
        if candidate.is_absolute() or any(part in ("", ".", "..")
                                          for part in candidate.parts):
            raise RawIIError("product_root:untracked_path_invalid")
        path = root.joinpath(candidate)
        try:
            info = path.lstat()
        except OSError as exc:
            raise RawIIError("product_root:untracked_artifact_unavailable") from exc
        if not _generated_owned(info):
            raise RawIIError("product_root:untracked_artifact_invalid")
        if stat.S_ISLNK(info.st_mode):
            raise RawIIError("product_root:untracked_artifact_invalid")
        elif stat.S_ISREG(info.st_mode) and info.st_nlink == 1:
            try:
                facts = depth_runner._digest(path, "product_root.untracked",
                                             MAX_UNTRACKED_BYTES)
            except DepthPlanError as exc:
                raise RawIIError("product_root:untracked_artifact_invalid") from exc
            total_bytes += int(facts["bytes"])
            item = {"path": relative, "kind": "file", "bytes": facts["bytes"],
                    "sha256": facts["sha256"]}
        else:
            raise RawIIError("product_root:untracked_artifact_invalid")
        if total_bytes > MAX_UNTRACKED_BYTES:
            raise RawIIError("product_root:untracked_inventory_too_large")
        entries.append(item)
    raw = _canonical(entries)
    return {"entries": entries, "count": len(entries), "bytes": total_bytes,
            "sha256": hashlib.sha256(raw).hexdigest()}


def _git_identity_snapshot(root: Path) -> dict[str, str]:
    try:
        head = subprocess.run(["git", "-C", str(root), "rev-parse", "HEAD"],
                              check=True, capture_output=True, text=True,
                              timeout=15).stdout.strip()
        tree = subprocess.run(["git", "-C", str(root), "rev-parse", "HEAD^{tree}"],
                              check=True, capture_output=True, text=True,
                              timeout=15).stdout.strip()
    except (OSError, subprocess.SubprocessError) as exc:
        raise RawIIError("product_root:git_identity_unavailable") from exc
    if (not HEX40.fullmatch(head) or not HEX40.fullmatch(tree)):
        raise RawIIError("product_root:git_identity_invalid")
    status = _git_status_snapshot(root)
    return {"head": head.lower(), "tree": tree.lower(), "status": status}


def _product_identity(root: Path) -> dict[str, object]:
    """Read the explicit product checkout's real HEAD/tree and file inventory."""
    if not root.is_absolute() or root.is_symlink() or not root.is_dir():
        raise RawIIError("product_root:unavailable")
    try:
        top = subprocess.run(["git", "-C", str(root), "rev-parse", "--show-toplevel"],
                             check=True, capture_output=True, text=True, timeout=15).stdout.strip()
    except (OSError, subprocess.SubprocessError) as exc:
        raise RawIIError("product_root:git_identity_unavailable") from exc
    if Path(top).resolve() != root.resolve():
        raise RawIIError("product_root:git_identity_invalid")
    before = _git_identity_snapshot(root)
    if _status_is_tracked_dirty(before["status"]):
        raise RawIIError("product_root:tracked_worktree_dirty")
    untracked = _untracked_inventory(root)
    after = _git_identity_snapshot(root)
    second_untracked = _untracked_inventory(root)
    final = _git_identity_snapshot(root)
    if (before != after or after != final or
            untracked != second_untracked):
        raise RawIIError("product_root:changed_during_inventory")
    return {"root": str(root.resolve()), "head": before["head"], "tree": before["tree"],
            "tracked_status": before["status"], "untracked": untracked}


def produce(plan_path: Path, witness_path: Path, engine_path: Path, output_dir: Path,
            depth: str, product_root: Path) -> dict[str, object]:
    cell_hint = None
    try:
        plan_raw, _facts = _read(plan_path, "plan")
        cell_hint = _cell(plan_raw.get("cell"), "plan")
    except RawIIError:
        raise
    plan, inputs, plan_facts = _load_plan(plan_path, cell_hint, depth)
    witness, witness_facts = _load_witness(witness_path, cell_hint)
    model_id, engine_rows, engine_facts = _load_engine(engine_path, cell_hint)
    result = plan.get("result")
    if (not isinstance(result, dict) or
            not isinstance(result.get("directory"), str) or
            not Path(result["directory"]).is_absolute() or
            Path(result["directory"]).resolve() != output_dir.resolve()):
        raise RawIIError("plan:result_directory_mismatch")
    product = _product_identity(product_root)
    seen: set[tuple[int, str, str, int]] = set()
    for index, item in enumerate(inputs):
        key = _occurrence_key(item.get("ordinal"), item.get("source_relative"),
                              item.get("sha256"), item.get("bytes"),
                              f"plan.inputs[{index}]")
        if key not in witness or key not in engine_rows:
            raise RawIIError(f"raw_ii:control_input_missing:{index}")
        if item.get("bytes") != witness[key]["source_bytes"]:  # type: ignore[index]
            raise RawIIError(f"raw_ii:witness_source_bytes_mismatch:{index}")
        seen.add(key)
    if set(witness) != seen or set(engine_rows) != seen:
        raise RawIIError("raw_ii:control_input_set_mismatch")
    if output_dir.exists() or output_dir.is_symlink() or not output_dir.is_absolute():
        raise RawIIError("output_dir:must_be_new_absolute_directory")
    scheduling = plan["scheduling"]
    cumulative_c = cumulative_f = cumulative_channel = 0
    cumulative_elapsed = 0
    serial_service = 0
    slots = scheduling.get("global_slots")
    slots_per_f = scheduling.get("slots_per_f")
    assignments = scheduling.get("assignments")
    if (type(slots) is not int or slots <= 0 or type(slots_per_f) is not int or
            slots_per_f <= 0 or not isinstance(assignments, list) or
            len(assignments) != len(inputs)):
        raise RawIIError("plan:scheduling_runtime_invalid")
    slot_available = [0] * slots
    rows: list[dict[str, object]] = []
    for ordinal, item in enumerate(inputs):
        key = _occurrence_key(item["ordinal"], item["source_relative"],
                              item["sha256"], item["bytes"],
                              f"plan.inputs[{ordinal}]")
        c_to_f = int(witness[key]["c_to_f_bytes"])
        f_to_c, elapsed = engine_rows[key]
        assignment = assignments[ordinal]
        if (not isinstance(assignment, dict) or assignment.get("ordinal") != ordinal or
                type(assignment.get("global_slot")) is not int or
                not 0 <= assignment["global_slot"] < slots or
                type(assignment.get("f_relationship")) is not int or
                assignment["f_relationship"] != assignment["global_slot"] // slots_per_f or
                assignment.get("per_f_slot") != assignment["global_slot"] % slots_per_f):
            raise RawIIError(f"plan:scheduling_assignment_invalid:{ordinal}")
        global_slot = assignment["global_slot"]
        relationship = assignment["f_relationship"]
        slot_admission = slot_available[global_slot]
        source_start = slot_admission
        finish = source_start + elapsed
        slot_available[global_slot] = finish
        cumulative_c += c_to_f
        cumulative_f += f_to_c
        cumulative_channel += c_to_f + f_to_c
        cumulative_elapsed = max(cumulative_elapsed, finish)
        serial_service += elapsed
        rows.append({
            "step": ordinal,
            "tu_id": f"{cell_hint['corpus']}-{cell_hint['regime']}-tu-{ordinal:06d}-{key[2][:12]}",
            "cell": cell_hint,
            "model_id": model_id,
            "occurrence": {"ordinal": key[0], "source_relative": key[1],
                           "sha256": key[2], "bytes": key[3]},
            "cumulative": {"C_TO_F_bytes": cumulative_c, "F_TO_C_bytes": cumulative_f,
                            "channel_bytes": cumulative_channel, "elapsed_ns": cumulative_elapsed,
                            "throughput_bytes_per_s": cumulative_channel * 1_000_000_000 / cumulative_elapsed},
            "scheduling": {"global_slot": global_slot, "f_relationship": relationship,
                           "per_f_slot": assignment["per_f_slot"],
                           "start_ns": source_start, "finish_ns": finish,
                           "service_ns": elapsed},
        })
    curve_raw = b"".join(_canonical(row) for row in rows)
    # Match the live runner's plan/input contract byte-for-byte.  Curve JSON
    # records retain their newline-delimited producer encoding, but identity
    # digests use the shared no-newline canonical form.
    input_digest = hashlib.sha256(normalizer.canonical_bytes(
        {"source_manifest_sha256": plan["source_manifest"]["sha256"],
         "inputs": inputs})).hexdigest()
    topology_digest = hashlib.sha256(_canonical(scheduling)).hexdigest()
    identity = {"corpus": cell_hint["corpus"], "profile": "RAW_II", "regime": cell_hint["regime"],
                "split": ALL_SPLITS[cell_hint["corpus"]], "run_id": f"{output_dir.name}-{plan_facts['sha256'][:12]}",
                "source_commit": product["head"], "source_tree": product["tree"],
                "input_digest": input_digest, "topology_digest": topology_digest, "model_id": model_id}
    comparison = normalizer.comparison_descriptor(plan_facts["sha256"], scheduling)
    manifest = {"schema": normalizer.MANIFEST_SCHEMA, "identity": identity, "comparison": comparison,
                "control_baseline": normalizer.CONTROL_BASELINE,
                "units": {"point": "step", "channel_bytes": "bytes", "elapsed_ns": "ns",
                          "C_TO_F_bytes": "bytes", "F_TO_C_bytes": "bytes",
                          "throughput_bytes_per_s": "bytes_per_s"},
                "curve": {"path": "predictive_sim.jsonl", "sha256": hashlib.sha256(curve_raw).hexdigest(),
                          "bytes": len(curve_raw)},
                "provenance": {"mode": "predictive_sim", "producer": SCHEMA, "trace_free": True}}
    producer = {"schema": SCHEMA, "semantics": CURRENT_SEMANTICS, "cell": cell_hint,
                "control_baseline": normalizer.CONTROL_BASELINE, "engine_scope": ENGINE_SCOPE,
                "plan": plan_facts, "witness": witness_facts, "engine": engine_facts,
                "formula": FORMULA, "identity": identity, "product_git": product,
                "scheduling": {"topology": scheduling["topology"],
                                "global_slots": scheduling["global_slots"],
                                "f_relationships": scheduling["f_relationships"],
                                "slots_per_f": scheduling["slots_per_f"],
                                "makespan_ns": cumulative_elapsed,
                                "serial_service_ns": serial_service}}
    output_dir.mkdir(parents=True)
    (output_dir / "predictive_sim.jsonl").write_bytes(curve_raw)
    (output_dir / "predictive_curve_manifest.json").write_bytes(_canonical(manifest))
    (output_dir / "producer_manifest.json").write_bytes(_canonical(producer))
    return producer


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--plan", type=Path, required=True)
    parser.add_argument("--raw-ii-witness", type=Path, required=True)
    parser.add_argument("--engine-manifest", type=Path, required=True)
    parser.add_argument("--product-root", type=Path, required=True)
    parser.add_argument("--output-dir", type=Path)
    parser.add_argument("--repeat-plan", type=Path)
    parser.add_argument("--depth", choices=("100", "200", "full"), required=True)
    args = parser.parse_args(argv)
    try:
        plan_path = args.plan.absolute()
        witness_path = args.raw_ii_witness.absolute()
        engine_path = args.engine_manifest.absolute()
        product_root = args.product_root.absolute()
        if args.repeat_plan is not None:
            # Paired full runs are plan-bound: each plan owns its output
            # directory.  Supplying the first output directory alongside a
            # repeat plan would re-run full-1 into an existing result and
            # fail closed at the output boundary.
            if args.output_dir is not None:
                raise RawIIError("repeat_full:output_dir_is_plan_bound")
            first_value, _first_facts = _read(plan_path, "plan")
            first_result = first_value.get("result")
            if (not isinstance(first_result, dict) or
                    not isinstance(first_result.get("directory"), str)):
                raise RawIIError("plan:result_directory_invalid")
            repeat_path = args.repeat_plan.absolute()
            repeat_value, _repeat_facts = _read(repeat_path, "repeat_plan")
            repeat_result = repeat_value.get("result")
            if (not isinstance(repeat_result, dict) or
                    not isinstance(repeat_result.get("directory"), str)):
                raise RawIIError("repeat_plan:result_directory_invalid")
            produce(plan_path, witness_path, engine_path, Path(first_result["directory"]),
                    args.depth, product_root)
            produce(repeat_path, witness_path, engine_path, Path(repeat_result["directory"]),
                    "repeat-full", product_root)
        else:
            if args.output_dir is None:
                raise RawIIError("output_dir:required_without_repeat_plan")
            produce(plan_path, witness_path, engine_path, args.output_dir.absolute(),
                    args.depth, product_root)
    except RawIIError as exc:
        print(f"s8_raw_ii_predictive_producer: {exc}", file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
