from __future__ import annotations

import ast
import re
import subprocess
from pathlib import Path


ROOT = Path(__file__).resolve().parents[3]


def _manifest(path: Path) -> list[str]:
    lines = path.read_text(encoding="utf-8").splitlines()
    assert lines
    assert lines == sorted(set(lines))
    return lines


def _assert_safe_regular_files(paths: list[str]) -> None:
    for item in paths:
        relative = Path(item)
        assert not relative.is_absolute()
        assert ".." not in relative.parts
        assert "__pycache__" not in relative.parts
        assert relative.suffix not in {".pyc", ".pyo"}
        assert relative.name != "farm.local.json"
        assert "results" not in relative.parts
        target = ROOT / relative
        assert target.is_file(), item
        assert not target.is_symlink(), item


def _source_files(prefix: str) -> set[str] | None:
    if not (ROOT / ".git").exists():
        return None
    result = subprocess.run(
        ["git", "ls-files", "--cached", "--others", "--exclude-standard", prefix],
        cwd=ROOT,
        check=True,
        capture_output=True,
        text=True,
        shell=False,
    )
    # Moves may be reviewed before staging. Ignore deleted index entries;
    # the manifest's regular-file check still rejects a missing listed file.
    return {path for path in result.stdout.splitlines() if (ROOT / path).exists()}


def test_farmharness_distribution_manifest_is_complete_and_private_free() -> None:
    manifest = ROOT / "farmharness" / "DISTFILES"
    paths = _manifest(manifest)
    assert all(path.startswith(("farmharness/", "research/farmharness/")) for path in paths)
    _assert_safe_regular_files(paths)
    source_files = _source_files("farmharness")
    if source_files is not None:
        source_files |= _source_files("research/farmharness") or set()
        assert source_files == set(paths) | {"farmharness/DISTFILES"}


def test_formal_distribution_manifest_is_complete() -> None:
    manifest = ROOT / "cache" / "formal" / "DISTFILES"
    relative = _manifest(manifest)
    assert all(path.startswith("formal/") for path in relative)
    paths = [f"cache/{path}" for path in relative]
    _assert_safe_regular_files(paths)
    source_files = _source_files("cache/formal")
    if source_files is not None:
        assert source_files == set(paths) | {"cache/formal/DISTFILES"}


def test_automake_distribution_uses_reviewed_manifests() -> None:
    top = (ROOT / "Makefile.am").read_text(encoding="utf-8")
    cache = (ROOT / "cache" / "Makefile.am").read_text(encoding="utf-8")
    assert "EXTRA_DIST += farmharness/DISTFILES $(ICEFARM_DIST_FILES)" in top
    assert "EXTRA_DIST += formal/DISTFILES $(P50_FORMAL_DIST_FILES)" in cache


def test_product_distribution_excludes_retired_grz_implementation() -> None:
    cache = (ROOT / "cache/Makefile.am").read_text(encoding="utf-8")
    assert "../vendor/grouprlz/" not in cache
    assert "../vendor/libbsc/" not in cache
    # These are still part of the supported codec API and regression surface.
    assert "codec/tuples.h" in cache
    assert "codec/p29_wire.h" in cache
    tests = (ROOT / "unittests/Makefile.am").read_text(encoding="utf-8")
    assert "codec_provider_test.cpp" in tests
    top = (ROOT / "Makefile.am").read_text(encoding="utf-8")
    assert "research/vendor/" not in top + cache
    assert "research/codecs/" not in top + cache


def test_production_headers_do_not_depend_on_research_tree() -> None:
    assert (ROOT / "cache/codec/p29_online_s1.h").is_file()
    assert "codec/p29_online_s1.h" in (ROOT / "cache/Makefile.am").read_text()
    forbidden = re.compile(r'^\s*#\s*include\s*[<"](?:\.\./)*(?:research|capability)/', re.MULTILINE)
    for directory in ("cache", "client", "daemon", "scheduler", "services"):
        for path in (ROOT / directory).rglob("*"):
            if path.suffix in {".cpp", ".h"} and path.is_file():
                assert not forbidden.search(path.read_text(encoding="utf-8")), path


def test_historical_reports_are_nested_and_packaged_by_explicit_path() -> None:
    top = (ROOT / "Makefile.am").read_text(encoding="utf-8")
    for name in (
        "AUDIT-ISSUE-1.md", "BENCH", "DEPLOYMENT-MATRIX.md", "PERF-REPORT.md",
        "WEBGUI-REVIEW.md", "P50_RESULT_DISPOSITION_WIRE_AUDIT.md",
    ):
        assert not (ROOT / name).exists()
        assert (ROOT / "research/reports" / name).is_file()
        assert f"research/reports/{name}" in top


def test_live_harness_does_not_import_archived_research_tools() -> None:
    active = ROOT / "farmharness"
    assert not list(active.glob("s[4-8]*"))
    resolver = active / "newgen_farm_env.py"
    assert resolver.is_file()
    for path in [resolver, *sorted((active / "integration").glob("*.py"))]:
        tree = ast.parse(path.read_text(encoding="utf-8"))
        for node in ast.walk(tree):
            names = (
                [alias.name for alias in node.names] if isinstance(node, ast.Import)
                else [node.module or ""] if isinstance(node, ast.ImportFrom)
                else []
            )
            assert not any(name.split(".")[0] in {"research", "capability"}
                           for name in names), path


def test_hermetic_test_compiler_is_executable() -> None:
    compiler = ROOT / "farmharness/integration/tests/fixtures/test-compiler.sh"
    assert compiler.stat().st_mode & 0o111
