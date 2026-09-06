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
    if authority.get("schema") != AUTHORITY_SCHEMA:
        _fail("authority.schema", f"expected {AUTHORITY_SCHEMA!r}")
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
    return tuple(observed)


def validate_and_verify_bodies(corpus: Mapping[str, Any]) -> PromotionResult:
    """Run metadata validation followed by streamed body verification."""
    result = validate_corpus_promotion(corpus)
    verify_corpus_bodies(result)
    return result


validate_promotion = validate_corpus_promotion
