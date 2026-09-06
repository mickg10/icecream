from __future__ import annotations

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
    return set(result.stdout.splitlines())


def test_farmharness_distribution_manifest_is_complete_and_private_free() -> None:
    manifest = ROOT / "farmharness" / "DISTFILES"
    paths = _manifest(manifest)
    assert all(path.startswith("farmharness/") for path in paths)
    _assert_safe_regular_files(paths)
    source_files = _source_files("farmharness")
    if source_files is not None:
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


def test_hermetic_test_compiler_is_executable() -> None:
    compiler = ROOT / "farmharness/integration/tests/fixtures/test-compiler.sh"
    assert compiler.stat().st_mode & 0o111
