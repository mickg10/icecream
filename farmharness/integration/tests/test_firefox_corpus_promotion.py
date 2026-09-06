from __future__ import annotations

import hashlib
import json
from pathlib import Path

import pytest

from farmharness.integration.firefox_corpus_promotion import (
    FirefoxCorpusPromotionError,
    validate_and_verify_bodies,
    validate_corpus_promotion,
    verify_corpus_bodies,
)
from farmharness.integration.schema_validation import canonical_bytes


def _sha(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def _json(path: Path, value: object) -> str:
    path.write_bytes(canonical_bytes(value))
    return _sha(path)


def _fixture(tmp_path: Path) -> dict:
    root = tmp_path / "corpus"
    root.mkdir()
    a = tmp_path / "a.ii"
    b = tmp_path / "b.ii"
    a.write_bytes(b'mozalloc_abort("alloc overflow")')
    b.write_bytes(b'mozalloc_abort("Xalloc overflow")')
    am = tmp_path / "A.manifest"
    bm = tmp_path / "B.manifest"
    am.write_text(f"{a}\n", encoding="utf-8")
    bm.write_text(f"{b}\n", encoding="utf-8")
    pair = tmp_path / "pair.json"
    pair_value = {
        "pairs": [{"affected": True, "index": 0, "path": "source/a.ii", "turn_a_sha256": _sha(a), "turn_b_sha256": _sha(b)}],
        "schema": "icefarm-firefox-pair-index-v1",
    }
    pair_sha = _json(pair, pair_value)
    selection = tmp_path / "selection.json"
    full_relative = f"{root.as_posix().lstrip('/')}/source/a.ii"
    selection_sha = _json(selection, {"count": 1, "rows": [{"audit": "audit.json", "corrected_trace_logical": 0, "index": 0, "relative": full_relative, "turn_a_path": str(a), "turn_a_sha256": _sha(a), "turn_b_path": str(b), "turn_b_sha256": _sha(b)}], "schema": "icefarm-firefox-selection-v1"})
    compiler = "/usr/bin/cc" if Path("/usr/bin/cc").is_file() else "/bin/cc"
    compiler_hash = _sha(Path(compiler).resolve())
    validation = tmp_path / "validation.json"
    validation_sha = _json(validation, {"compiler": compiler, "compiler_arguments": ["-O2"], "compiler_sha256": compiler_hash, "jobs": 1, "rows": [{"exit_code": 0, "index": 0, "input_sha256": _sha(a), "output_sha256": "d" * 64, "path": str(a), "timed_out": False, "turn": "A"}, {"exit_code": 0, "index": 0, "input_sha256": _sha(b), "output_sha256": "e" * 64, "path": str(b), "timed_out": False, "turn": "B"}], "schema": "icefarm-firefox-compile-validation-v1", "timeout_s": 1})
    authority = tmp_path / "authority.json"
    normalized = hashlib.sha256(b"source/a.ii\n").hexdigest()
    authority_value = {"compile_validation": {"path": str(validation), "sha256": validation_sha, "turn_a_pass": 1, "turn_b_pass": 1}, "compiler": {"arguments": ["-O2"], "path": compiler, "sha256": compiler_hash}, "normalized_manifest_sha256": normalized, "pair_index": {"path": str(pair), "sha256": pair_sha}, "schema": "icefarm-firefox-corpus-authority-v1", "selection": {"path": str(selection), "sha256": selection_sha}, "turn_a_manifest": {"path": str(am), "sha256": _sha(am)}, "turn_b_manifest": {"path": str(bm), "sha256": _sha(bm)}, "tus": 1}
    authority_sha = _json(authority, authority_value)
    return {"root": str(root), "tus": 1, "pair_index_sha256": pair_sha, "normalized_manifest_sha256": normalized, "compiler_recipes": {"cc": {"arguments": ["-O2"], "binary_sha256": compiler_hash, "executable": "/bin/cc"}}, "authority_receipt": {"path": str(authority), "sha256": authority_sha}}


def test_validates_metadata_without_source_body_hashing(tmp_path: Path) -> None:
    result = validate_corpus_promotion(_fixture(tmp_path))
    assert result.pair_rows[0]["path"] == "source/a.ii"
    assert result.physical_paths[0][0].name == "a.ii"


@pytest.mark.parametrize("field", ["authority_receipt", "pair_index_sha256", "normalized_manifest_sha256"])
def test_rejects_tampered_binding(tmp_path: Path, field: str) -> None:
    corpus = _fixture(tmp_path)
    if field == "authority_receipt":
        corpus[field] = {**corpus[field], "sha256": "0" * 64}
    else:
        corpus[field] = "0" * 64
    with pytest.raises(FirefoxCorpusPromotionError):
        validate_corpus_promotion(corpus)


def test_rejects_pair_index_order_or_selection_mismatch(tmp_path: Path) -> None:
    corpus = _fixture(tmp_path)
    authority = Path(corpus["authority_receipt"]["path"])
    value = json.loads(authority.read_text(encoding="utf-8"))
    pair = Path(value["pair_index"]["path"])
    pair_value = json.loads(pair.read_text(encoding="utf-8"))
    pair_value["pairs"][0]["index"] = 1
    pair.write_bytes(canonical_bytes(pair_value))
    with pytest.raises(FirefoxCorpusPromotionError):
        validate_corpus_promotion(corpus)


def test_strict_loader_rejects_duplicate_json_keys(tmp_path: Path) -> None:
    corpus = _fixture(tmp_path)
    authority = Path(corpus["authority_receipt"]["path"])
    authority.write_text('{"schema":"icefarm-firefox-corpus-authority-v1", "schema":"bad"}', encoding="utf-8")
    corpus["authority_receipt"]["sha256"] = _sha(authority)
    with pytest.raises(FirefoxCorpusPromotionError, match="duplicate key"):
        validate_corpus_promotion(corpus)


def test_streamed_body_verification_rejects_tamper(tmp_path: Path) -> None:
    corpus = _fixture(tmp_path)
    result = validate_corpus_promotion(corpus)
    assert validate_and_verify_bodies(corpus) == result
    result.physical_paths[0][1].write_bytes(b"tampered")
    with pytest.raises(FirefoxCorpusPromotionError, match="body SHA|exact permitted"):
        verify_corpus_bodies(result)
