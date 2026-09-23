from __future__ import annotations

import ast
import re
import subprocess
import sys
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


def _markdown_sources() -> list[Path]:
    if (ROOT / ".git").exists():
        result = subprocess.run(
            ["git", "ls-files", "--cached", "--others", "--exclude-standard", "*.md"],
            cwd=ROOT, check=True, capture_output=True, text=True, shell=False,
        )
        return [ROOT / name for name in result.stdout.splitlines()
                if (ROOT / name).is_file()]
    # A source snapshot has no index. Keep the fallback deliberately bounded
    # to shipped/documented trees rather than walking private build artifacts.
    roots = [ROOT / name for name in
             ("cache", "client", "daemon", "dev", "doc", "farmharness",
              "package_builder", "research", "tests")]
    paths = set(ROOT.glob("*.md"))
    paths.update(path for base in roots for path in base.rglob("*.md"))
    return sorted(path for path in paths if path.is_file())


def _markdown_anchors(path: Path) -> set[str]:
    anchors: set[str] = set()
    in_fence = False
    for line in path.read_text(encoding="utf-8").splitlines():
        if line.lstrip().startswith("```"):
            in_fence = not in_fence
            continue
        if in_fence:
            continue
        match = re.match(r"^#{1,6}\s+(.+?)\s*#*\s*$", line)
        if not match:
            continue
        heading = re.sub(r"[`*_]", "", match.group(1)).lower()
        slug = re.sub(r"[^a-z0-9 -]", "", heading)
        slug = re.sub(r"\s+", "-", slug).strip("-")
        if slug:
            anchors.add(slug)
    return anchors


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


def test_protocol_docs_are_the_explicit_cache_distribution_surface() -> None:
    cache = (ROOT / "cache" / "Makefile.am").read_text(encoding="utf-8")
    canonical = {
        "cache/P50_PROTOCOL.md",
        "cache/codec/P29V1_FORMAT.md",
        "cache/codec/ZSTD_FORMATS.md",
    }
    for path in canonical:
        assert (ROOT / path).is_file(), path
        assert path.removeprefix("cache/") in cache, path
    retired = re.compile(r"(?:^|/)P50_[^/]+\.md$")
    listed = [line.strip().rstrip("\\") for line in cache.splitlines()]
    assert not any(retired.search(item) and item != "P50_PROTOCOL.md" for item in listed)


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


def test_p50_reference_components_are_not_product_dependencies() -> None:
    references = (
        "p50_role_owner", "p50_cache_session_join", "p50_input_attachment",
        "p50_source_ingress", "p50_reverse_fd_retry",
        "p50_adopted_socket_lease", "p50_sidecar_supervisor",
    )
    tests = (ROOT / "unittests/Makefile.am").read_text()
    assert "check_LIBRARIES = libp50reference.a" in tests
    for stem in references:
        for suffix in (".h", ".cpp"):
            name = stem + suffix
            assert not (ROOT / "cache" / name).exists()
            assert (ROOT / "unittests/support" / name).is_file()
            assert f"support/{name}" in tests
    for directory in ("cache", "client", "daemon", "services", "scheduler"):
        makefile = (ROOT / directory / "Makefile.am").read_text()
        assert "libp50reference" not in makefile
        assert "unittests/support" not in makefile
        for path in (ROOT / directory).glob("*"):
            if path.suffix not in {".h", ".cpp"}:
                continue
            for include in re.findall(r'^\s*#\s*include\s*[<"]([^">]+)',
                                      path.read_text(), re.MULTILINE):
                assert "unittests/" not in include, path
                assert Path(include).stem not in references, path


def test_historical_reports_are_nested_and_packaged_by_explicit_path() -> None:
    top = (ROOT / "Makefile.am").read_text(encoding="utf-8")
    for name in ("BENCH", "P50_RESULT_DISPOSITION_WIRE_AUDIT.md"):
        assert not (ROOT / name).exists()
        assert (ROOT / "research/reports" / name).is_file()
        assert f"research/reports/{name}" in top


def test_ordinary_relative_markdown_links_and_anchors_resolve() -> None:
    link = re.compile(r"(?<!!)\[[^\]]+\]\(([^)]+)\)")
    for source in _markdown_sources():
        in_fence = False
        for line_number, line in enumerate(source.read_text(encoding="utf-8").splitlines(), 1):
            if line.lstrip().startswith("```"):
                in_fence = not in_fence
                continue
            if in_fence:
                continue
            for target in link.findall(line):
                target = target.strip().split(" ", 1)[0].strip("<>")
                if (not target or target.startswith("#") or
                        re.match(r"^[A-Za-z][A-Za-z0-9+.-]*:", target)):
                    continue
                parts = target.split("#", 1)
                relative = parts[0]
                fragment = parts[1] if len(parts) == 2 else ""
                assert relative, f"empty relative link in {source}:{line_number}"
                resolved = (source.parent / relative).resolve()
                assert resolved.is_file(), (
                    f"broken Markdown link {relative!r} in {source}:{line_number}"
                )
                if fragment:
                    assert fragment in _markdown_anchors(resolved), (
                        f"missing Markdown anchor #{fragment} in {resolved}:{line_number}"
                    )


def test_gitless_markdown_source_fallback_and_anchor_scan(tmp_path: Path, monkeypatch) -> None:
    (tmp_path / "research").mkdir()
    target = tmp_path / "research" / "target.md"
    target.write_text("# Canonical Heading\n\n```text\n# not-an-anchor\n```\n")
    (tmp_path / "README.md").write_text("[target](research/target.md#canonical-heading)\n")
    monkeypatch.setattr(sys.modules[__name__], "ROOT", tmp_path)
    assert target in _markdown_sources()
    assert "canonical-heading" in _markdown_anchors(target)
    assert "not-an-anchor" not in _markdown_anchors(target)


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
