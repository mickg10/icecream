from __future__ import annotations

import json
import subprocess
from pathlib import Path

import pytest

import s8_heldout_preflight as preflight


def _write(path: Path, value: str | bytes) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(value.encode() if isinstance(value, str) else value)


def _fixture(root: Path) -> None:
    for corpus, config in preflight.CORPORA.items():
        source_root = root / config["source_root"]
        corpus_root = root / config["corpus_root"]
        source_relative = "tools/utils/test_platform.cpp" if corpus == "DuckDB" else "llvm/lib/Analysis/AliasAnalysis.cpp"
        source = source_root / source_relative
        _write(source, "int heldout_fixture() { return 0; }\n")
        representative = corpus_root / config["representative_name"]
        _write(representative, b"# 1 \"fixture\"\nint heldout_fixture();\n")
        _write(corpus_root / "manifest.txt", str(representative) + "\n")
        matrix_manifest = root / config["matrix_manifest"]
        _write(matrix_manifest, (str(representative) + "\n") * 4)
        metadata = {"corpus_id": "corpus3" if corpus == "DuckDB" else "corpus",
                    "project": "DuckDB" if corpus == "DuckDB" else "LLVM",
                    "git_commit": "a" * 9, "TU": 689 if corpus == "DuckDB" else 1238,
                    "source_checkouts": [str(source_root)]}
        _write(corpus_root / "METADATA.json", json.dumps(metadata) + "\n")
        compile_db = root / config["compile_db"]
        _write(compile_db, json.dumps([{
            "directory": str(source.parent), "file": str(source),
            "command": f"/usr/bin/c++ -std=c++17 -c {source} -o unit.o",
        }]) + "\n")


def _product_build(root: Path) -> Path:
    build = root / "product-build"
    for relative in preflight.PRODUCT_BINARIES:
        path = build / relative
        _write(path, (str(relative) + "\n").encode())
        path.chmod(0o755)
    _write(build / preflight.PRODUCT_CONFIG,
           "#define ICECC_P50_WITH_LIBBSC 1\n")
    _write(build / preflight.PRODUCT_CACHE_MAKEFILE,
           "LIBBSC_CFLAGS = -DICECC_P50_WITH_LIBBSC\n"
           "LIBBSC_LIBS = /opt/libbsc.a\n")
    return build


def _git_fixture(root: Path, content: str = "source\n") -> Path:
    root.mkdir(parents=True, exist_ok=True)
    subprocess.run(["git", "init", "-q", str(root)], check=True)
    _write(root / "tracked.txt", content)
    subprocess.run(["git", "-C", str(root), "add", "tracked.txt"], check=True)
    subprocess.run([
        "git", "-C", str(root), "-c", "user.name=S8 test",
        "-c", "user.email=s8-test@example.invalid", "commit", "-qm", "fixture",
    ], check=True)
    return root


def test_preflight_rejects_matrix_with_wrong_repeat_factor(tmp_path: Path) -> None:
    root = tmp_path / "corpus"
    item = root / "one.ii"
    _write(item, b"fixture")
    manifest = root / "matrix.txt"
    _write(manifest, (str(item) + "\n") * 3)
    with pytest.raises(preflight.PreflightError, match="repeat_factor_invalid"):
        preflight._paths_from_manifest(manifest, root, "matrix", repeat_factor=4)


def test_git_identity_allows_untracked_outputs_but_rejects_tracked_edits(
    tmp_path: Path,
) -> None:
    root = _git_fixture(tmp_path / "source")
    clean = preflight._git_identity(root)
    assert clean["status"] == "tracked_clean"
    assert clean["untracked"] == "ignored"
    assert "reject_tracked_or_index_changes" in clean["status_policy"]

    _write(root / "generated-product-output", "generated\n")
    assert preflight._git_identity(root) == clean

    _write(root / "tracked.txt", "mutated\n")
    with pytest.raises(preflight.PreflightError, match="source_git_tracked_changes"):
        preflight._git_identity(root)


def test_product_build_git_binding_cannot_use_source_repository(
    tmp_path: Path,
) -> None:
    source_repo = _git_fixture(tmp_path / "source-repo")
    product_build = _git_fixture(_product_build(tmp_path / "product-repo"), "product\n")
    runner = {
        "sha256": "c" * 64,
        "source_contract": {"sha256": "d" * 64},
        "profiles": list(preflight.PROFILES), "warm_values": [0, 1],
        "contract_check": "PASS",
    }
    source_identity = preflight._git_identity(source_repo)
    product_identity = preflight._git_identity(product_build)
    assert source_identity["commit"] != product_identity["commit"]
    assert source_identity["tree"] != product_identity["tree"]
    binding = preflight._product_build_contract(product_build, source_repo, runner)
    assert binding["git"] == product_identity
    assert binding["git"] != source_identity


def test_product_build_binding_rejects_incompatible_runner(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch,
) -> None:
    _fixture(tmp_path)
    build = _product_build(tmp_path)
    monkeypatch.setattr(preflight, "_git_identity",
                        lambda root: {"commit": "a" * 40, "tree": "b" * 40})
    runner = {
        "path": str(Path(__file__).resolve().parents[2] / preflight.RUNNER),
        "sha256": "c" * 64,
        "source_contract": {"sha256": "d" * 64},
        "profiles": ["ZSTD_TU"], "warm_values": [0, 1],
        "contract_check": "PASS",
    }
    monkeypatch.setattr(preflight, "_runner_contract", lambda repo: runner)
    with pytest.raises(preflight.PreflightError,
                       match="product_build.runner_compatibility_invalid"):
        preflight.preflight(tmp_path, Path(__file__).resolve().parents[2], build)
