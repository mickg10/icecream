from pathlib import Path


def test_node_runtime_installs_and_checks_shaping_tool() -> None:
    dockerfile = Path(__file__).parents[1] / "docker" / "Dockerfile.node"
    recipe = dockerfile.read_text(encoding="utf-8")
    assert "        iproute2 \\\n" in recipe
    assert "&& /usr/sbin/tc -V" in recipe
