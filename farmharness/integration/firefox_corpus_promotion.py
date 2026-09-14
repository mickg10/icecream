"""Fail-closed validation of a generated Firefox corpus authority promotion.

This module validates metadata and file identities only.  In particular, it
does not hash the (potentially multi-gigabyte) translation-unit inputs; archive
creation is the operation responsible for hashing those bodies.
"""

from __future__ import annotations

import hashlib
import re
from dataclasses import dataclass
from pathlib import Path, PurePosixPath
from typing import Any, Mapping

from .firefox_corpus_authority import (
    AUTHORITY_SCHEMA,
    MUTATION_AFTER,
    MUTATION_BEFORE,
    PAIR_INDEX_SCHEMA,
)
from .firefox_root_header_authority import (
    AUTHORITY_SCHEMA as ROOT_HEADER_AUTHORITY_SCHEMA,
    COMPILE_BIND_FIELDS as ROOT_HEADER_COMPILE_BIND_FIELDS,
    COMPILE_DOC_FIELDS as ROOT_HEADER_COMPILE_DOC_FIELDS,
    COMPILE_ROW_FIELDS as ROOT_HEADER_COMPILE_ROW_FIELDS,
    FILE_BIND_FIELDS as ROOT_HEADER_FILE_BIND_FIELDS,
    NAMESPACE_ARGV as ROOT_HEADER_NAMESPACE_ARGV,
    PAIR_FIELDS as ROOT_HEADER_PAIR_FIELDS,
    PAIR_ROW_FIELDS as ROOT_HEADER_PAIR_ROW_FIELDS,
    PAIR_SCHEMA as ROOT_HEADER_PAIR_SCHEMA,
    ROOT_EDIT_FIELDS as ROOT_HEADER_ROOT_EDIT_FIELDS,
    RootHeaderAuthorityError,
    SELECTION_FIELDS as ROOT_HEADER_SELECTION_FIELDS,
    SELECTION_ROW_FIELDS as ROOT_HEADER_SELECTION_ROW_FIELDS,
    TRACE_SCHEMA as ROOT_HEADER_TRACE_SCHEMA,
    _compile_commands as _root_compile_commands,
    _header_edit as _root_header_edit,
    _manifest_root as _root_manifest_root,
    _reconstruct_argv as _root_reconstruct_argv,
    _same_or_single_insert as _root_same_or_single_insert,
    _trace as _root_trace,
)
from .schema_validation import ValidationError, load_json


SHA256_RE = re.compile(r"^[0-9a-f]{64}$")


class FirefoxCorpusPromotionError(ValueError):
    """The authority cannot be promoted without weakening an invariant."""


@dataclass(frozen=True)
class PromotionResult:
    authority_path: Path
    authority_sha256: str
    pair_rows: tuple[dict[str, Any], ...]
    physical_paths: tuple[tuple[Path, Path], ...]
    mutation_kind: str = "marker-replacement"
    inserted_byte: int | None = None


class _TransformedStream:
    """Stream A while applying the one permitted mutation, without buffering it."""

    def __init__(self, path: Path) -> None:
        self._handle = path.open("rb")
        self._pending = bytearray()
        self._eof = False
        self.digest = hashlib.sha256()
        self.occurrences = 0

    def __iter__(self) -> _TransformedStream:
        return self

    def __next__(self) -> bytes:
        while True:
            marker = self._pending.find(MUTATION_BEFORE)
            if marker >= 0:
                prefix = bytes(self._pending[:marker])
                del self._pending[: marker + len(MUTATION_BEFORE)]
                self.occurrences += 1
                return prefix + MUTATION_AFTER
            keep = len(MUTATION_BEFORE) - 1
            if not self._eof and len(self._pending) > keep:
                output = bytes(self._pending[:-keep])
                del self._pending[:-keep]
                return output
            if self._eof:
                if not self._pending:
                    self._handle.close()
                    raise StopIteration
                output = bytes(self._pending)
                self._pending.clear()
                return output
            chunk = self._handle.read(1024 * 1024)
            if chunk:
                self.digest.update(chunk)
                self._pending.extend(chunk)
                continue
            self._eof = True
            if len(self._pending) > keep:
                output = bytes(self._pending[:-keep])
                del self._pending[:-keep]
                return output


def _verify_body_pair(
    source_a: Path,
    source_b: Path,
    expected_a: str,
    expected_b: str,
    affected: bool,
    subject: str,
) -> tuple[str, str]:
    stream = _TransformedStream(source_a)
    digest_b = hashlib.sha256()
    pending_expected = b""
    pending_b = b""
    exhausted = False
    with source_b.open("rb") as handle_b:
        while True:
            if not pending_expected and not exhausted:
                try:
                    pending_expected = next(stream)
                except StopIteration:
                    exhausted = True
            if not pending_b:
                pending_b = handle_b.read(1024 * 1024)
                if pending_b:
                    digest_b.update(pending_b)
            if not pending_expected and not pending_b:
                if exhausted:
                    break
                continue
            if not pending_expected or not pending_b:
                _fail(subject, "Turn-B bytes are not the exact permitted mutation")
            amount = min(len(pending_expected), len(pending_b))
            if pending_expected[:amount] != pending_b[:amount]:
                _fail(subject, "Turn-B bytes are not the exact permitted mutation")
            pending_expected = pending_expected[amount:]
            pending_b = pending_b[amount:]
    if stream.occurrences != (1 if affected else 0):
        _fail(subject, "mutation occurrence count disagrees with pair affected flag")
    observed_a = stream.digest.hexdigest()
    observed_b = digest_b.hexdigest()
    if observed_a != expected_a or observed_b != expected_b:
        _fail(subject, "body SHA-256 does not match pair metadata")
    return observed_a, observed_b


def _fail(subject: str, message: str) -> None:
    raise FirefoxCorpusPromotionError(f"{subject}: {message}")


def _digest(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for block in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def _sha(value: Any, subject: str) -> str:
    if not isinstance(value, str) or SHA256_RE.fullmatch(value) is None:
        _fail(subject, "must be a lowercase SHA-256 digest")
    return value


def _path(value: Any, subject: str) -> Path:
    if not isinstance(value, str):
        _fail(subject, "must be an absolute path")
    parsed = PurePosixPath(value)
    if (
        not parsed.is_absolute()
        or value in ("/", "")
        or ".." in parsed.parts
        or any(ord(char) < 32 or ord(char) == 127 for char in value)
    ):
        _fail(subject, "must be a non-root absolute safe path")
    result = Path(value)
    if not result.is_file() or result.is_symlink():
        _fail(subject, "must name a regular non-symlink file")
    return result


def _executable(value: Any, subject: str) -> Path:
    """Validate an executable path while permitting a symlink alias."""
    if not isinstance(value, str):
        _fail(subject, "must be an absolute path")
    parsed = PurePosixPath(value)
    if (
        not parsed.is_absolute()
        or value in ("/", "")
        or ".." in parsed.parts
        or any(ord(char) < 32 or ord(char) == 127 for char in value)
    ):
        _fail(subject, "must be a non-root absolute safe path")
    result = Path(value)
    resolved = result.resolve(strict=False)
    if not resolved.is_file():
        _fail(subject, "must resolve to a regular file")
    return result


def _json(path: Path, subject: str) -> Any:
    try:
        return load_json(path)
    except ValidationError as exc:
        _fail(subject, f"cannot read JSON: {exc}")


def _directory(value: Any, subject: str) -> Path:
    if not isinstance(value, str):
        _fail(subject, "must be an absolute directory path")
    parsed = PurePosixPath(value)
    if (
        not parsed.is_absolute()
        or value in ("/", "")
        or ".." in parsed.parts
        or any(ord(char) < 32 or ord(char) == 127 for char in value)
    ):
        _fail(subject, "must be a non-root absolute safe path")
    result = Path(value)
    if not result.is_dir() or result.is_symlink():
        _fail(subject, "must name a regular non-symlink directory")
    return result


def _bound_file(value: Any, declared: Any, subject: str) -> Path:
    path = _path(value, f"{subject}.path")
    expected = _sha(declared, f"{subject}.sha256")
    observed = _digest(path)
    if observed != expected:
        _fail(subject, f"SHA-256 mismatch (expected {expected}, got {observed})")
    return path


def _manifest(path: Path, count: int, subject: str) -> tuple[Path, ...]:
    try:
        lines = path.read_text(encoding="utf-8").splitlines()
    except (OSError, UnicodeError) as exc:
        _fail(subject, f"cannot read manifest: {exc}")
    if len(lines) != count:
        _fail(subject, f"expected {count} lines, got {len(lines)}")
    result: list[Path] = []
    seen: set[Path] = set()
    for index, line in enumerate(lines):
        candidate = _path(line, f"{subject}[{index}]")
        if candidate in seen:
            _fail(subject, f"duplicate physical path at index {index}")
        seen.add(candidate)
        result.append(candidate)
    return tuple(result)


def _logical(value: Any, subject: str) -> PurePosixPath:
    if not isinstance(value, str):
        _fail(subject, "must be a safe canonical logical path")
    path = PurePosixPath(value)
    if (
        path.is_absolute()
        or str(path) in ("", ".")
        or ".." in path.parts
        or any(ord(char) < 32 or ord(char) == 127 for char in value)
        or str(path) != value
    ):
        _fail(subject, "must be a normalized safe relative path")
    return path


def _selection_logical(value: Any, root: Path, subject: str) -> PurePosixPath:
    """Normalize a trace-relative path to the corpus-root suffix."""
    candidate = _logical(value, subject)
    root_parts = PurePosixPath(root.as_posix().lstrip("/")).parts
    parts = candidate.parts
    for index in range(len(parts) - len(root_parts) + 1):
        if parts[index : index + len(root_parts)] == root_parts:
            suffix = PurePosixPath(*parts[index + len(root_parts) :])
            if str(suffix) in ("", "."):
                _fail(subject, "has no suffix below corpus root")
            return suffix
    return candidate


def _require_mapping(value: Any, subject: str) -> Mapping[str, Any]:
    if not isinstance(value, dict):
        _fail(subject, "must be an object")
    return value


def _validate_root_header_corpus_promotion(
    corpus: Mapping[str, Any],
    authority_path: Path,
    authority: Mapping[str, Any],
) -> PromotionResult:
    """Promote the distinct literal-root-header authority into farm inputs.

    Capture performs the expensive source, namespace, compile, and body
    revalidation once.  Farm planning rechecks the immutable receipt graph and
    every row-to-manifest binding without rehashing the multi-gigabyte TU
    bodies; archive construction performs that streamed body check.
    """

    required_authority = {
        "body_law", "compile_commands", "compile_validation", "generation",
        "pair_index", "provenance", "root_edit", "schema", "selection",
        "source", "trace", "turn_a_manifest", "turn_b_manifest", "tus",
    }
    if set(authority) != required_authority:
        _fail("authority", "literal root-header authority fields are not exact")
    tus = corpus.get("tus")
    if tus != 1000 or authority.get("tus") != tus:
        _fail("authority.tus", "literal root-header promotion requires exactly 1000 TUs")
    body_law = _require_mapping(authority.get("body_law"), "authority.body_law")
    declared_body_law = _require_mapping(
        corpus.get("root_header_body_law"), "corpus.root_header_body_law"
    )
    if set(declared_body_law) != {"affected", "unaffected"}:
        _fail("corpus.root_header_body_law", "fields are not exact")
    affected_expected = declared_body_law.get("affected")
    unaffected_expected = declared_body_law.get("unaffected")
    if (
        not isinstance(affected_expected, int)
        or isinstance(affected_expected, bool)
        or not isinstance(unaffected_expected, int)
        or isinstance(unaffected_expected, bool)
        or affected_expected < 1
        or unaffected_expected < 1
        or affected_expected + unaffected_expected != tus
        or dict(body_law) != dict(declared_body_law)
    ):
        _fail(
            "authority.body_law",
            "does not match the corpus-bound 1000-TU affected/unaffected law",
        )

    root_edit = _require_mapping(authority.get("root_edit"), "authority.root_edit")
    if set(root_edit) != ROOT_HEADER_ROOT_EDIT_FIELDS:
        _fail("authority.root_edit", "fields are not exact")
    inserted_hex = root_edit.get("inserted_byte_hex")
    if not isinstance(inserted_hex, str) or re.fullmatch(r"[0-9a-f]{2}", inserted_hex) is None:
        _fail("authority.root_edit.inserted_byte_hex", "must identify one inserted byte")
    inserted_byte = int(inserted_hex, 16)

    corpus_root = _directory(corpus.get("root"), "corpus.root")
    source = _require_mapping(authority.get("source"), "authority.source")
    if set(source) != {"commit", "git", "header", "root"} or source.get("root") != str(corpus_root):
        _fail("authority.source", "fields/root do not match corpus root")
    source_commit = source.get("commit")
    if not isinstance(source_commit, str) or re.fullmatch(r"[0-9a-f]{40}", source_commit) is None:
        _fail("authority.source.commit", "must be a lowercase Git commit")
    source_git = _require_mapping(source.get("git"), "authority.source.git")
    source_header = _require_mapping(source.get("header"), "authority.source.header")
    if set(source_git) != {"head", "status", "status_sha256"}:
        _fail("authority.source.git", "fields are not exact")
    if (
        source_git.get("head") != source_commit
        or source_git.get("status") != ""
        or source_git.get("status_sha256") != hashlib.sha256(b"").hexdigest()
    ):
        _fail("authority.source.git", "does not record the exact clean source commit")
    if set(source_header) != {"committed_sha256", "current_sha256", "path", "relative", "tracked"}:
        _fail("authority.source.header", "fields are not exact")

    generation = _require_mapping(authority.get("generation"), "authority.generation")
    if set(generation) != {"compile_commands_sha256", "generator", "source_commit", "trace_sha256"}:
        _fail("authority.generation", "fields are not exact")
    if (
        generation.get("generator") != "farmharness.integration.firefox_root_header_authority"
        or generation.get("source_commit") != source_commit
    ):
        _fail("authority.generation", "does not bind the literal root-header generator/source")

    provenance = _require_mapping(authority.get("provenance"), "authority.provenance")
    if (
        set(provenance) != {"edit_diff", "edited_header", "execution_receipt", "mode", "true_path_bind"}
        or provenance.get("mode") != "retained-true-path-revalidation"
    ):
        _fail("authority.provenance", "retained true-path revalidation fields are not exact")
    true_path_bind = _require_mapping(
        provenance.get("true_path_bind"), "authority.provenance.true_path_bind"
    )
    if set(true_path_bind) != {"mount_argv", "namespace_argv", "target"}:
        _fail("authority.provenance.true_path_bind", "fields are not exact")

    before_path = _bound_file(
        root_edit.get("path"), root_edit.get("before_sha256"), "authority.root_edit.before"
    )
    after_path = _bound_file(
        provenance.get("edited_header"), root_edit.get("after_sha256"), "authority.root_edit.after"
    )
    diff_path = _bound_file(
        provenance.get("edit_diff"), root_edit.get("diff_sha256"), "authority.root_edit.diff"
    )
    try:
        observed_edit = _root_header_edit(
            before_path, after_path, diff_path, str(before_path)
        )
    except RootHeaderAuthorityError as exc:
        _fail("authority.root_edit", str(exc))
    if dict(root_edit) != observed_edit:
        _fail("authority.root_edit", "recomputed literal root edit differs")
    if (
        source_header.get("path") != str(before_path)
        or source_header.get("committed_sha256") != root_edit.get("before_sha256")
        or source_header.get("current_sha256") != root_edit.get("before_sha256")
        or source_header.get("tracked") is not True
        or not isinstance(source_header.get("relative"), str)
        or not source_header.get("relative")
    ):
        _fail("authority.source.header", "does not bind the clean root header")
    if (
        true_path_bind.get("target") != str(before_path)
        or true_path_bind.get("mount_argv")
        != ["mount", "--bind", str(after_path), str(before_path)]
        or true_path_bind.get("namespace_argv") != list(ROOT_HEADER_NAMESPACE_ARGV)
    ):
        _fail("authority.provenance.true_path_bind", "does not bind the exact root edit")
    execution_receipt = _require_mapping(
        provenance.get("execution_receipt"), "authority.provenance.execution_receipt"
    )
    if set(execution_receipt) != ROOT_HEADER_FILE_BIND_FIELDS:
        _fail("authority.provenance.execution_receipt", "fields are not exact")
    _bound_file(
        execution_receipt.get("path"),
        execution_receipt.get("sha256"),
        "authority.provenance.execution_receipt",
    )

    def bound_section(
        name: str, expected_fields: frozenset[str] = ROOT_HEADER_FILE_BIND_FIELDS
    ) -> tuple[Path, Mapping[str, Any]]:
        section = _require_mapping(authority.get(name), f"authority.{name}")
        if set(section) != expected_fields:
            _fail(f"authority.{name}", "binding fields are not exact")
        path = _bound_file(section.get("path"), section.get("sha256"), f"authority.{name}")
        return path, section

    compile_commands_path, compile_commands_meta = bound_section("compile_commands")
    trace_path, trace_meta = bound_section("trace")
    a_manifest_path, a_manifest_meta = bound_section("turn_a_manifest")
    b_manifest_path, b_manifest_meta = bound_section("turn_b_manifest")
    pair_path, pair_meta = bound_section("pair_index")
    selection_path, _selection_meta = bound_section("selection")
    validation_path, validation_meta = bound_section(
        "compile_validation", ROOT_HEADER_COMPILE_BIND_FIELDS
    )
    if (
        generation.get("compile_commands_sha256") != compile_commands_meta.get("sha256")
        or generation.get("trace_sha256") != trace_meta.get("sha256")
    ):
        _fail("authority.generation", "does not bind compile commands and trace")
    a_paths = _manifest(a_manifest_path, tus, "turn_a_manifest")
    b_paths = _manifest(b_manifest_path, tus, "turn_b_manifest")
    for name, observed in (("turn_a_manifest", a_manifest_path), ("turn_b_manifest", b_manifest_path)):
        if corpus.get(name) != str(observed):
            _fail(f"corpus.{name}", "does not match literal root-header authority")
    if a_manifest_meta.get("path") == b_manifest_meta.get("path"):
        _fail("turn manifests", "A and B must be distinct files")
    try:
        trace_rows = _root_trace(trace_path, tus)
        turn_a_root = _root_manifest_root(a_paths, trace_rows, "turn_a_manifest")
        turn_b_root = _root_manifest_root(b_paths, trace_rows, "turn_b_manifest")
        command_specs = _root_compile_commands(
            compile_commands_path, trace_rows, a_paths, True
        )
    except RootHeaderAuthorityError as exc:
        _fail("authority.selection", str(exc))

    pair_doc = _require_mapping(_json(pair_path, "pair_index"), "pair_index")
    if set(pair_doc) != ROOT_HEADER_PAIR_FIELDS or pair_doc.get("schema") != ROOT_HEADER_PAIR_SCHEMA:
        _fail("pair_index", "literal root-header pair schema/fields are not exact")
    pairs = pair_doc.get("pairs")
    if not isinstance(pairs, list) or len(pairs) != tus:
        _fail("pair_index.pairs", f"expected {tus} rows")
    pair_rows: list[dict[str, Any]] = []
    logical_seen: set[PurePosixPath] = set()
    affected = 0
    for expected, raw in enumerate(pairs):
        row = _require_mapping(raw, f"pair_index.pairs[{expected}]")
        if set(row) != ROOT_HEADER_PAIR_ROW_FIELDS or row.get("index") != expected:
            _fail(f"pair_index.pairs[{expected}]", "fields/index are not exact")
        logical = _logical(row.get("path"), f"pair_index.pairs[{expected}].path")
        if logical in logical_seen:
            _fail("pair_index.pairs", f"duplicate logical path {logical}")
        logical_seen.add(logical)
        _sha(row.get("turn_a_sha256"), f"pair_index.pairs[{expected}].turn_a_sha256")
        _sha(row.get("turn_b_sha256"), f"pair_index.pairs[{expected}].turn_b_sha256")
        if not isinstance(row.get("affected"), bool):
            _fail(f"pair_index.pairs[{expected}].affected", "must be boolean")
        affected += int(row["affected"])
        pair_rows.append(dict(row))
    if affected != affected_expected:
        _fail(
            "pair_index.pairs",
            "affected count does not reproduce the corpus-bound body law",
        )
    normalized = hashlib.sha256(
        ("\n".join(str(row["path"]) for row in pair_rows) + "\n").encode()
    ).hexdigest()
    if corpus.get("pair_index_sha256") != pair_meta.get("sha256"):
        _fail("corpus.pair_index_sha256", "does not match literal root-header pair index")
    if corpus.get("normalized_manifest_sha256") != normalized:
        _fail("corpus.normalized_manifest_sha256", "does not match literal root-header paths")

    selection_doc = _require_mapping(_json(selection_path, "selection"), "selection")
    if (
        set(selection_doc) != ROOT_HEADER_SELECTION_FIELDS
        or selection_doc.get("schema") != ROOT_HEADER_TRACE_SCHEMA
        or selection_doc.get("count") != tus
        or selection_doc.get("compile_commands_sha256") != compile_commands_meta.get("sha256")
        or selection_doc.get("trace_sha256") != trace_meta.get("sha256")
        or selection_doc.get("turn_a_root") != str(turn_a_root)
        or selection_doc.get("turn_b_root") != str(turn_b_root)
    ):
        _fail("selection", "literal root-header selection schema/count/fields are not exact")
    selected = selection_doc.get("rows")
    if not isinstance(selected, list) or len(selected) != tus:
        _fail("selection.rows", f"expected {tus} rows")
    selected_by_index: dict[int, Mapping[str, Any]] = {}
    for expected, raw in enumerate(selected):
        row = _require_mapping(raw, f"selection.rows[{expected}]")
        if set(row) != ROOT_HEADER_SELECTION_ROW_FIELDS or row.get("index") != expected:
            _fail(f"selection.rows[{expected}]", "fields/index are not exact")
        selected_by_index[expected] = row
        trace_row = trace_rows[expected]
        command_spec = command_specs[trace_row["actual_input"]]
        if (
            row.get("turn_a_path") != str(a_paths[expected])
            or row.get("turn_b_path") != str(b_paths[expected])
        ):
            _fail(f"selection.rows[{expected}]", "physical manifest bijection failed")
        if (
            row.get("ii_relative") != pair_rows[expected]["path"]
            or row.get("ii_relative") != trace_row["ii_relative"]
            or row.get("actual_input") != trace_row["actual_input"]
        ):
            _fail(f"selection.rows[{expected}]", "trace/pair mapping mismatch")
        if (
            row.get("command_sha256") != command_spec["command_sha256"]
            or row.get("resolution_basis") != command_spec["resolution_basis"]
            or row.get("resolution_row_sha256")
            != command_spec["resolution_row_sha256"]
        ):
            _fail(
                f"selection.rows[{expected}]",
                "does not reproduce the compile-database selection",
            )

    validation_doc = _require_mapping(_json(validation_path, "compile_validation"), "compile_validation")
    if (
        set(validation_doc) != ROOT_HEADER_COMPILE_DOC_FIELDS
        or validation_doc.get("schema") != "icefarm-firefox-root-compile-validation-v1"
        or validation_doc.get("count") != tus
        or validation_doc.get("compile_commands_sha256") != compile_commands_meta.get("sha256")
        or validation_doc.get("source_error") != ""
    ):
        _fail("compile_validation", "literal root-header compile receipt is not an exact pass")
    if validation_meta.get("turn_a_pass") != tus or validation_meta.get("turn_b_pass") != tus:
        _fail("authority.compile_validation", "pass counts do not match corpus tus")
    rows = validation_doc.get("rows")
    if not isinstance(rows, list) or len(rows) != 2 * tus:
        _fail("compile_validation.rows", f"expected exactly {2 * tus} rows")
    seen_turns: set[tuple[int, str]] = set()
    compilers: set[tuple[str, str]] = set()
    for raw in rows:
        row = _require_mapping(raw, "compile_validation.rows[]")
        if set(row) != ROOT_HEADER_COMPILE_ROW_FIELDS:
            _fail("compile_validation.rows", "row fields are not exact")
        index, turn = row.get("index"), row.get("turn")
        if (
            not isinstance(index, int)
            or isinstance(index, bool)
            or turn not in ("A", "B")
            or not 0 <= index < tus
            or (index, turn) in seen_turns
        ):
            _fail("compile_validation.rows", "must contain each contiguous A/B index exactly once")
        seen_turns.add((index, turn))
        selected_row = selected_by_index[index]
        expected_path = a_paths[index] if turn == "A" else b_paths[index]
        expected_sha = pair_rows[index]["turn_a_sha256"] if turn == "A" else pair_rows[index]["turn_b_sha256"]
        if (
            row.get("actual_input") != selected_row["actual_input"]
            or row.get("input_sha256") != expected_sha
            or row.get("command_sha256") != selected_row["command_sha256"]
            or row.get("resolution_basis") != selected_row["resolution_basis"]
            or row.get("resolution_row_sha256") != selected_row["resolution_row_sha256"]
        ):
            _fail(f"compile_validation.rows[{index},{turn}]", "does not match selection/pair authority")
        if (
            not isinstance(row.get("elapsed_ms"), (int, float))
            or isinstance(row.get("elapsed_ms"), bool)
            or row["elapsed_ms"] < 0
            or row.get("error") != ""
            or not isinstance(row.get("working_directory"), str)
            or not row["working_directory"]
        ):
            _fail(f"compile_validation.rows[{index},{turn}]", "is not a valid successful compile row")
        argv = row.get("argv")
        compiler_path = row.get("compiler_path")
        compiler_sha = _sha(row.get("compiler_sha256"), f"compile_validation.rows[{index},{turn}].compiler_sha256")
        executable = _executable(compiler_path, f"compile_validation.rows[{index},{turn}].compiler_path")
        if not isinstance(argv, list) or not argv or argv[0] != str(executable):
            _fail(f"compile_validation.rows[{index},{turn}].argv", "does not start with bound compiler")
        if _digest(executable.resolve()) != compiler_sha:
            _fail(f"compile_validation.rows[{index},{turn}].compiler_sha256", "does not match compiler bytes")
        compilers.add((str(executable.resolve()), compiler_sha))
        if row.get("exit_code") != 0 or row.get("timed_out") is not False:
            _fail(f"compile_validation.rows[{index},{turn}]", "is not a definitive pass")
        for field in ("output_sha256", "stderr_sha256", "stdout_sha256"):
            _sha(row.get(field), f"compile_validation.rows[{index},{turn}].{field}")
        object_path = row.get("object_path")
        if not isinstance(object_path, str) or object_path not in argv or str(expected_path) not in argv:
            _fail(f"compile_validation.rows[{index},{turn}].argv", "does not bind input and output paths")
        command_spec = command_specs[selected_row["actual_input"]]
        try:
            expected_argv = _root_reconstruct_argv(
                command_spec, expected_path, Path(object_path)
            )
        except RootHeaderAuthorityError as exc:
            _fail(f"compile_validation.rows[{index},{turn}].argv", str(exc))
        if (
            argv != expected_argv
            or str(executable.resolve()) != command_spec["compiler_path"]
            or compiler_sha != command_spec["compiler_sha256"]
            or row.get("working_directory") != command_spec["directory"]
        ):
            _fail(
                f"compile_validation.rows[{index},{turn}]",
                "does not reproduce the selected compile command",
            )
    if len(seen_turns) != 2 * tus:
        _fail("compile_validation.rows", "missing A/B index")

    source_evidence = _require_mapping(
        validation_doc.get("source_evidence"), "compile_validation.source_evidence"
    )
    if (
        set(source_evidence)
        != {"commit", "git_end", "git_start", "header_end", "header_start", "root"}
        or source_evidence.get("commit") != source_commit
        or source_evidence.get("root") != str(corpus_root)
        or source_evidence.get("git_start") != source_git
        or source_evidence.get("git_end") != source_git
        or source_evidence.get("header_start") != source_header
        or source_evidence.get("header_end") != source_header
    ):
        _fail("compile_validation.source_evidence", "does not bind the authority source")
    executor = _require_mapping(validation_doc.get("executor"), "compile_validation.executor")
    if (
        set(executor) != {"jobs", "timeout_s"}
        or not isinstance(executor.get("jobs"), int)
        or isinstance(executor.get("jobs"), bool)
        or executor["jobs"] < 1
        or not isinstance(executor.get("timeout_s"), (int, float))
        or isinstance(executor.get("timeout_s"), bool)
        or executor["timeout_s"] <= 0
    ):
        _fail("compile_validation.executor", "is invalid")

    recipes = corpus.get("compiler_recipes")
    if not isinstance(recipes, dict):
        _fail("corpus.compiler_recipes", "must be an object")
    for compiler_path, compiler_sha in compilers:
        matching = []
        for name, recipe_raw in recipes.items():
            recipe = _require_mapping(recipe_raw, f"corpus.compiler_recipes.{name}")
            executable = _executable(recipe.get("executable"), f"corpus.compiler_recipes.{name}.executable")
            if str(executable.resolve()) == compiler_path:
                if recipe.get("binary_sha256") != compiler_sha:
                    _fail(f"corpus.compiler_recipes.{name}", "binary SHA does not match root-header compiler")
                matching.append(name)
        if len(matching) != 1:
            _fail("authority.compiler", "each root-header compiler must match exactly one corpus recipe")

    return PromotionResult(
        authority_path=authority_path,
        authority_sha256=_digest(authority_path),
        pair_rows=tuple(pair_rows),
        physical_paths=tuple(zip(a_paths, b_paths, strict=True)),
        mutation_kind="single-insert",
        inserted_byte=inserted_byte,
    )


def validate_corpus_promotion(corpus: Mapping[str, Any]) -> PromotionResult:
    """Validate a Firefox authority referenced by a corpus mapping.

    The return value contains canonical pair rows and their A/B physical
    paths, suitable for a later lifecycle/archive step.  No source body is
    read or hashed.
    """

    receipt = _require_mapping(corpus.get("authority_receipt"), "authority_receipt")
    authority_path = _bound_file(
        receipt.get("path"), receipt.get("sha256"), "authority_receipt"
    )
    authority = _require_mapping(_json(authority_path, "authority_receipt"), "authority")
    if authority.get("schema") == ROOT_HEADER_AUTHORITY_SCHEMA:
        return _validate_root_header_corpus_promotion(corpus, authority_path, authority)
    if authority.get("schema") != AUTHORITY_SCHEMA:
        _fail(
            "authority.schema",
            f"expected {AUTHORITY_SCHEMA!r} or {ROOT_HEADER_AUTHORITY_SCHEMA!r}",
        )
    tus = corpus.get("tus")
    if not isinstance(tus, int) or tus < 1 or authority.get("tus") != tus:
        _fail("authority.tus", "does not agree with corpus tus")
    corpus_root = _directory(corpus.get("root"), "corpus.root")

    def bound_section(name: str) -> tuple[Path, Mapping[str, Any]]:
        section = _require_mapping(authority.get(name), f"authority.{name}")
        path = _bound_file(section.get("path"), section.get("sha256"), f"authority.{name}")
        return path, section

    a_manifest_path, a_manifest_meta = bound_section("turn_a_manifest")
    b_manifest_path, b_manifest_meta = bound_section("turn_b_manifest")
    pair_path, pair_meta = bound_section("pair_index")
    selection_path, selection_meta = bound_section("selection")
    validation_path, validation_meta = bound_section("compile_validation")
    a_paths = _manifest(a_manifest_path, tus, "turn_a_manifest")
    b_paths = _manifest(b_manifest_path, tus, "turn_b_manifest")
    for field, observed in (("turn_a_manifest", a_manifest_path), ("turn_b_manifest", b_manifest_path)):
        if field in corpus and corpus[field] != str(observed):
            _fail(f"corpus.{field}", "does not match authority receipt")
    if a_manifest_meta.get("path") == b_manifest_meta.get("path"):
        _fail("turn manifests", "A and B must be distinct files")

    pair_doc = _require_mapping(_json(pair_path, "pair_index"), "pair_index")
    if pair_doc.get("schema") != PAIR_INDEX_SCHEMA:
        _fail("pair_index.schema", f"expected {PAIR_INDEX_SCHEMA!r}")
    pairs = pair_doc.get("pairs")
    if not isinstance(pairs, list) or len(pairs) != tus:
        _fail("pair_index.pairs", f"expected {tus} rows")
    pair_rows: list[dict[str, Any]] = []
    logical_seen: set[PurePosixPath] = set()
    for expected, raw in enumerate(pairs):
        row = _require_mapping(raw, f"pair_index.pairs[{expected}]")
        if row.get("index") != expected:
            _fail(f"pair_index.pairs[{expected}].index", "must be contiguous")
        logical = _logical(row.get("path"), f"pair_index.pairs[{expected}].path")
        if logical in logical_seen:
            _fail("pair_index.pairs", f"duplicate logical path {logical}")
        logical_seen.add(logical)
        _sha(row.get("turn_a_sha256"), f"pair_index.pairs[{expected}].turn_a_sha256")
        _sha(row.get("turn_b_sha256"), f"pair_index.pairs[{expected}].turn_b_sha256")
        if not isinstance(row.get("affected"), bool):
            _fail(f"pair_index.pairs[{expected}].affected", "must be boolean")
        pair_rows.append(dict(row))
    normalized = hashlib.sha256(
        ("\n".join(str(row["path"]) for row in pair_rows) + "\n").encode()
    ).hexdigest()
    if authority.get("normalized_manifest_sha256") != normalized:
        _fail("authority.normalized_manifest_sha256", "does not match pair paths")
    if pair_meta.get("sha256") != _digest(pair_path):  # bound_file already checks; explicit receipt binding
        _fail("authority.pair_index", "receipt hash changed during validation")
    if corpus.get("pair_index_sha256") != pair_meta.get("sha256"):
        _fail("corpus.pair_index_sha256", "does not match authority pair-index hash")
    if corpus.get("normalized_manifest_sha256") != normalized:
        _fail("corpus.normalized_manifest_sha256", "does not match pair paths")

    selection_doc = _require_mapping(_json(selection_path, "selection"), "selection")
    if selection_doc.get("schema") != "icefarm-firefox-selection-v1" or selection_doc.get("count") != tus:
        _fail("selection", "schema/count does not match corpus")
    selected = selection_doc.get("rows")
    if not isinstance(selected, list) or len(selected) != tus:
        _fail("selection.rows", f"expected {tus} rows")
    selected_by_index: dict[int, Mapping[str, Any]] = {}
    for expected, raw in enumerate(selected):
        row = _require_mapping(raw, f"selection.rows[{expected}]")
        if row.get("index") != expected or expected in selected_by_index:
            _fail("selection.rows", "indexes must be exactly contiguous")
        selected_by_index[expected] = row
        if row.get("turn_a_path") != str(a_paths[expected]) or row.get("turn_b_path") != str(b_paths[expected]):
            _fail(f"selection.rows[{expected}]", "physical manifest bijection failed")
        relative = _selection_logical(
            row.get("relative"), corpus_root, f"selection.rows[{expected}].relative"
        )
        if str(relative) != pair_rows[expected]["path"]:
            _fail(f"selection.rows[{expected}]", "logical pair path mismatch")
        if row.get("turn_a_sha256") != pair_rows[expected]["turn_a_sha256"] or row.get("turn_b_sha256") != pair_rows[expected]["turn_b_sha256"]:
            _fail(f"selection.rows[{expected}]", "pair SHA mismatch")
        _sha(row.get("turn_a_sha256"), f"selection.rows[{expected}].turn_a_sha256")
        _sha(row.get("turn_b_sha256"), f"selection.rows[{expected}].turn_b_sha256")

    validation_doc = _require_mapping(_json(validation_path, "compile_validation"), "compile_validation")
    if validation_doc.get("schema") != "icefarm-firefox-compile-validation-v1":
        _fail("compile_validation.schema", "unexpected schema")
    if validation_meta.get("turn_a_pass") != tus or validation_meta.get("turn_b_pass") != tus:
        _fail("authority.compile_validation", "pass counts do not match corpus tus")
    rows = validation_doc.get("rows")
    if not isinstance(rows, list) or len(rows) != 2 * tus:
        _fail("compile_validation.rows", f"expected exactly {2 * tus} rows")
    seen_turns: set[tuple[int, str]] = set()
    for raw in rows:
        row = _require_mapping(raw, "compile_validation.rows[]")
        index, turn = row.get("index"), row.get("turn")
        if not isinstance(index, int) or turn not in ("A", "B") or (index, turn) in seen_turns or not 0 <= index < tus:
            _fail("compile_validation.rows", "must contain each contiguous A/B index exactly once")
        seen_turns.add((index, turn))
        selected_row = selected_by_index[index]
        expected_path = selected_row["turn_a_path"] if turn == "A" else selected_row["turn_b_path"]
        expected_sha = selected_row["turn_a_sha256"] if turn == "A" else selected_row["turn_b_sha256"]
        if row.get("path") != expected_path or row.get("input_sha256") != expected_sha:
            _fail(f"compile_validation.rows[{index},{turn}]", "does not match selection")
        if row.get("exit_code") != 0 or row.get("timed_out") is not False:
            _fail(f"compile_validation.rows[{index},{turn}]", "is not a definitive pass")
        _sha(row.get("output_sha256"), f"compile_validation.rows[{index},{turn}].output_sha256")
    if len(seen_turns) != 2 * tus:
        _fail("compile_validation.rows", "missing A/B index")

    compiler = _require_mapping(authority.get("compiler"), "authority.compiler")
    compiler_path = _executable(compiler.get("path"), "authority.compiler.path")
    compiler_hash = _sha(compiler.get("sha256"), "authority.compiler.sha256")
    if _digest(compiler_path.resolve()) != compiler_hash:
        _fail("authority.compiler.sha256", "does not match compiler bytes")
    if validation_doc.get("compiler") != compiler.get("path"):
        _fail("compile_validation.compiler", "does not match authority compiler path")
    if validation_doc.get("compiler_arguments") != compiler.get("arguments"):
        _fail("compile_validation.compiler_arguments", "does not match authority compiler")
    if validation_doc.get("compiler_sha256") != compiler_hash:
        _fail("compile_validation.compiler_sha256", "does not match authority compiler")
    recipes = corpus.get("compiler_recipes")
    if not isinstance(recipes, dict):
        _fail("corpus.compiler_recipes", "must be an object")
    matching = []
    for name, recipe_raw in recipes.items():
        recipe = _require_mapping(recipe_raw, f"corpus.compiler_recipes.{name}")
        executable = _executable(recipe.get("executable"), f"corpus.compiler_recipes.{name}.executable")
        if executable.resolve() == compiler_path.resolve() and recipe.get("arguments") == compiler.get("arguments"):
            if recipe.get("binary_sha256") != compiler_hash:
                _fail(f"corpus.compiler_recipes.{name}", "binary SHA does not match authority compiler")
            matching.append(name)
    if len(matching) != 1:
        _fail("authority.compiler", "must match exactly one corpus compiler recipe")

    return PromotionResult(
        authority_path=authority_path,
        authority_sha256=_digest(authority_path),
        pair_rows=tuple(pair_rows),
        physical_paths=tuple(zip(a_paths, b_paths, strict=True)),
    )


def verify_corpus_bodies(result: PromotionResult) -> tuple[tuple[str, str], ...]:
    """Stream and verify every A/B body against pair hashes and mutation rules."""
    observed: list[tuple[str, str]] = []
    for index, (row, (source_a, source_b)) in enumerate(
        zip(result.pair_rows, result.physical_paths, strict=True)
    ):
        if result.mutation_kind == "marker-replacement":
            observed.append(
                _verify_body_pair(
                    source_a,
                    source_b,
                    row["turn_a_sha256"],
                    row["turn_b_sha256"],
                    row["affected"],
                    f"pair[{index}]",
                )
            )
            continue
        if result.mutation_kind != "single-insert" or result.inserted_byte is None:
            _fail(f"pair[{index}]", "unknown promoted mutation law")
        identical, changed = _root_same_or_single_insert(
            source_a, source_b, result.inserted_byte
        )
        if identical != (not row["affected"]) or changed != row["affected"]:
            _fail(f"pair[{index}]", "Turn-B bytes are not the exact permitted insertion")
        observed_a, observed_b = _digest(source_a), _digest(source_b)
        if observed_a != row["turn_a_sha256"] or observed_b != row["turn_b_sha256"]:
            _fail(f"pair[{index}]", "body SHA-256 does not match pair metadata")
        observed.append((observed_a, observed_b))
    return tuple(observed)


def validate_and_verify_bodies(corpus: Mapping[str, Any]) -> PromotionResult:
    """Run metadata validation followed by streamed body verification."""
    result = validate_corpus_promotion(corpus)
    verify_corpus_bodies(result)
    return result


validate_promotion = validate_corpus_promotion
