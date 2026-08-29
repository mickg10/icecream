from __future__ import annotations

import hashlib
import json
from pathlib import Path

import pytest

import s8_input_manifest as inventory
from s8_schema import CORPORA, CURRENT_SEMANTICS, DECLARED_CELLS, SPLITS


def _request(tmp_path: Path, *, omit: str | None = None) -> Path:
    rows = []
    for index, corpus in enumerate(CORPORA):
        if corpus == omit:
            continue
        root = tmp_path / corpus
        source = root / "src" / "unit.cc"
        source.parent.mkdir(parents=True)
        source.write_text(f"int unit_{index}() {{ return {index}; }}\n", encoding="utf-8")
        database = root / "build" / "compile_commands.json"
        database.parent.mkdir()
        command = f"/usr/bin/c++ -I{root / 'include'} -std=c++20 -c {source} -o unit.o"
        database.write_text(json.dumps([{
            "directory": str(database.parent),
            "file": str(source),
            "command": command,
        }]) + "\n", encoding="utf-8")
        rows.append({
            "corpus": corpus,
            "root": str(root),
            "source_relative": "src/unit.cc",
            "compile_db": str(database),
        })
    request = tmp_path / "request.json"
    request.write_text(json.dumps({
        "schema": inventory.REQUEST_SCHEMA,
        "semantics": CURRENT_SEMANTICS,
        "corpora": rows,
    }) + "\n", encoding="utf-8")
    return request


@pytest.fixture
def git_identities(monkeypatch: pytest.MonkeyPatch) -> None:
    monkeypatch.setattr(
        inventory,
        "_git_identity",
        lambda root, corpus: ("a" * 40, ("b" * 39) + str(CORPORA.index(corpus) + 1)),
    )


def test_build_binds_authenticated_inputs_and_exactly_32_unique_cells(
    tmp_path: Path, git_identities: None,
) -> None:
    output = tmp_path / "manifest.json"
    manifest = inventory.build(_request(tmp_path), output)
    assert output.exists()
    assert manifest["schema"] == inventory.MANIFEST_SCHEMA
    assert len(manifest["corpora"]) == 4
    cells = manifest["cells"]
    assert len(cells) == 32
    assert len({cell["cell_id"] for cell in cells}) == 32
    assert {(cell["corpus"], cell["profile"], cell["regime"]) for cell in cells} == {
        (cell["corpus"], cell["profile"], cell["regime"]) for cell in DECLARED_CELLS
    }
    for cell in cells:
        assert cell["split"] == SPLITS[cell["corpus"]]
        source = Path(cell["source_root"]) / cell["source_relative"]
        assert cell["source_bytes"] == source.stat().st_size
        assert cell["source_sha256"] == hashlib.sha256(source.read_bytes()).hexdigest()
        argv_digest = hashlib.sha256(
            inventory.canonical_bytes(cell["compile_argv"])
        ).hexdigest()
        assert cell["compile_argv_sha256"] == argv_digest
        assert cell["compile_database_bytes"] > 0


def test_missing_corpus_is_an_exact_inventory_failure_without_output(tmp_path: Path) -> None:
    request = _request(tmp_path, omit="LLVM-1238")
    output = tmp_path / "manifest.json"
    with pytest.raises(inventory.InventoryError, match=r"^inventory failure: reason=missing_corpora=LLVM-1238$"):
        inventory.build(request, output)
    assert not output.exists()


def test_cli_emits_exact_inventory_failure(tmp_path: Path, capsys: pytest.CaptureFixture[str]) -> None:
    request = _request(tmp_path, omit="DuckDB")
    output = tmp_path / "manifest.json"
    assert inventory.main(["--request", str(request), "--out", str(output)]) == 2
    assert capsys.readouterr().err == "inventory failure: reason=missing_corpora=DuckDB\n"
    assert not output.exists()


def test_missing_source_is_an_exact_corpus_failure_without_output(
    tmp_path: Path, git_identities: None,
) -> None:
    request = _request(tmp_path)
    source = tmp_path / "fmt" / "src" / "unit.cc"
    source.unlink()
    output = tmp_path / "manifest.json"
    with pytest.raises(inventory.InventoryError, match=r"inventory failure: corpus=fmt reason=source_.*unavailable"):
        inventory.build(request, output)
    assert not output.exists()


def test_duplicate_compile_entries_fail_closed(tmp_path: Path, git_identities: None) -> None:
    request = _request(tmp_path)
    database = tmp_path / "fmt" / "build" / "compile_commands.json"
    value = json.loads(database.read_text(encoding="utf-8"))
    database.write_text(json.dumps(value + value) + "\n", encoding="utf-8")
    with pytest.raises(inventory.InventoryError, match=r"corpus=fmt reason=compile_entry_match_count=2"):
        inventory.build(request, tmp_path / "manifest.json")


def test_compile_argv_requires_source_token(tmp_path: Path, git_identities: None) -> None:
    request = _request(tmp_path)
    database = tmp_path / "fmt" / "build" / "compile_commands.json"
    value = json.loads(database.read_text(encoding="utf-8"))
    value[0]["command"] = "/usr/bin/c++ -std=c++20 -c other.cc -o unit.o"
    database.write_text(json.dumps(value) + "\n", encoding="utf-8")
    with pytest.raises(inventory.InventoryError, match=r"corpus=fmt reason=compile_entry_source_token_ambiguous"):
        inventory.build(request, tmp_path / "manifest.json")


def test_existing_output_is_not_overwritten(tmp_path: Path, git_identities: None) -> None:
    request = _request(tmp_path)
    output = tmp_path / "manifest.json"
    output.write_bytes(b"sentinel\n")
    with pytest.raises(inventory.InventoryError, match=r"^inventory failure: reason=output_already_exists"):
        inventory.build(request, output)
    assert output.read_bytes() == b"sentinel\n"
