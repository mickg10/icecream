from __future__ import annotations

import datetime as dt
import hashlib
from pathlib import Path

import pytest

import s8_external_farm_authority as authority


def _root(tmp_path: Path) -> Path:
    root = tmp_path / "product"
    for rel in authority.ROLE_PATHS:
        path = root / rel
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes((rel + "\n").encode())
        path.chmod(0o755)
    return root


def _captures(now: dt.datetime, root: Path) -> dict[str, dict[str, object]]:
    roles = authority.final_head_role_hashes(root)
    captures = {}
    for index, host in enumerate(authority.HOSTS):
        machine = hashlib.sha256(f"machine-{host}".encode()).hexdigest()
        nic = hashlib.sha256(f"nic-{host}".encode()).hexdigest()
        sample = {"before": "cpu  1 2 3 4", "after": "cpu  2 3 4 5",
                  "duration_seconds": 1.0, "idle_percent": 100.0}
        captures[host] = {
            "hostname": host, "machine_id_sha256": machine,
            "boot_id_sha256": hashlib.sha256(f"boot-{host}".encode()).hexdigest(),
            "nic_identity_sha256": nic,
            "cpu_vendor_sha256": "a" * 64, "cpu_model_sha256": "b" * 64,
            "cpu_count": authority.EXPECTED_CPU_COUNTS[host],
            "image": {"reference": authority.PINNED_IMAGE,
                       "image_id": authority.IMAGE_CONFIG if host == "research7" else authority.IMAGE_INDEX,
                       "architecture": "amd64", "os": "linux", "created": "2026-08-21T17:11:43Z"},
            "binaries": dict(roles), "load_1m": 0.1,
            "captured_at": now.strftime("%Y-%m-%dT%H:%M:%SZ"),
            "baseline_digest": hashlib.sha256(f"baseline-{host}".encode()).hexdigest(),
            "cpu_sample": sample, "cpu_sample_digest": authority.digest(authority.canonical(sample)),
        }
    return captures


def test_default_plan_is_q2_and_research7_only() -> None:
    value = authority.relationship_mappings()
    assert value["C1F1/100000"] == ["q2"]
    assert value["C1F20/40"] == ["q2"] * 15 + ["research7"] * 5
    assert "research6" not in value["C1F20/40"]


def test_authority_writes_private_descriptors_and_mapping(tmp_path: Path) -> None:
    root = _root(tmp_path)
    now = dt.datetime.now(dt.timezone.utc).replace(microsecond=0)
    output = tmp_path / "private" / "authority.json"
    value = authority.build_authority(root=root, captures=_captures(now, root),
                                      descriptor_dir=tmp_path / "private" / "descriptors",
                                      output=output, now=now)
    assert value["placements"]["C1F20/40"]["relationship_hosts"] == ["q2"] * 15 + ["research7"] * 5
    assert output.stat().st_mode & 0o077 == 0
    assert all((tmp_path / "private" / "descriptors" / f"{h}-descriptor.json").stat().st_mode & 0o077 == 0
               for h in authority.HOSTS)
    assert value["hosts"]["q3"]["boot_id_digest"] != value["hosts"]["q3"]["physical_host_digest"]


def test_busy_research6_is_hold_but_default_mapping_remains_valid(tmp_path: Path) -> None:
    root = _root(tmp_path)
    now = dt.datetime.now(dt.timezone.utc).replace(microsecond=0)
    captures = _captures(now, root)
    captures["research6"]["cpu_sample"] = {"before": "cpu  1 2 3 4", "after": "cpu  5 6 7 8",
                                             "duration_seconds": 1.0, "idle_percent": 85.0}
    captures["research6"]["cpu_sample_digest"] = authority.digest(authority.canonical(captures["research6"]["cpu_sample"]))
    value = authority.build_authority(root=root, captures=captures,
                                      descriptor_dir=tmp_path / "d", output=tmp_path / "a.json", now=now)
    assert value["hosts"]["research6"]["idle"]["status"] == "HOLD"
    with pytest.raises(authority.AuthorityError, match="host_not_idle:research6"):
        authority.build_authority(root=root, captures=captures, descriptor_dir=tmp_path / "d2",
                                  output=tmp_path / "a2.json", include_research6=True, now=now)


@pytest.mark.parametrize("field", ["cpu_count", "image", "physical_overlap"])
def test_identity_mismatch_fails_closed(tmp_path: Path, field: str) -> None:
    root = _root(tmp_path)
    now = dt.datetime.now(dt.timezone.utc).replace(microsecond=0)
    captures = _captures(now, root)
    if field == "cpu_count":
        captures["q2"][field] = 1
    elif field == "image":
        captures["q2"]["image"]["image_id"] = "sha256:" + "c" * 64
    else:
        captures["q2"]["machine_id_sha256"] = captures["q3"]["machine_id_sha256"]
        captures["q2"]["nic_identity_sha256"] = captures["q3"]["nic_identity_sha256"]
    with pytest.raises(authority.AuthorityError):
        authority.build_authority(root=root, captures=captures, descriptor_dir=tmp_path / "d",
                                  output=tmp_path / "a.json", now=now)


def test_remote_capture_parser_retains_raw_cpu_sample() -> None:
    raw = "\n".join(["S8_SCHEMA=" + authority.CAPTURE_SCHEMA, "S8_HOSTNAME=q2",
                      "S8_MACHINE=" + "a" * 64, "S8_BOOT=" + "b" * 64,
                      "S8_NIC=" + "c" * 64, "S8_VENDOR=" + "d" * 64,
                      "S8_MODEL=" + "e" * 64, "S8_CPU=32", "S8_IMAGE_ID=" + authority.IMAGE_INDEX,
                      "S8_IMAGE_OS=linux", "S8_IMAGE_ARCH=amd64", "S8_IMAGE_CREATED=now",
                      "S8_LOAD=0.1", "S8_CAPTURED_AT=2026-08-31T00:00:00Z",
                      "S8_BASELINE=" + "f" * 64, "S8_SAMPLE_BEFORE=cpu  1 2 3 4",
                      "S8_SAMPLE_AFTER=cpu  2 3 4 5", "S8_SAMPLE_DURATION=1",
                      "S8_SAMPLE_IDLE=100"] + ["S8_BIN_" + rel + "=" + "1" * 64 for rel in authority.ROLE_PATHS])
    parsed = authority._parse_remote_capture(raw, "q2")
    assert parsed["cpu_sample"]["before"] == "cpu  1 2 3 4"
    assert parsed["cpu_sample"]["idle_percent"] == 100.0


def test_remote_capture_trims_cpuinfo_key_whitespace() -> None:
    assert authority.REMOTE_CAPTURE_SCRIPT.count(
        'key=$1; gsub(/^[[:space:]]+|[[:space:]]+$/, "", key)') == 2
    assert 'tolower(key)=="vendor_id"' in authority.REMOTE_CAPTURE_SCRIPT
    assert 'tolower(key)=="model name"' in authority.REMOTE_CAPTURE_SCRIPT


def test_remote_capture_counts_cpu_fields_and_queries_image_fields_separately() -> None:
    assert 'n=split(a,y," ")' in authority.REMOTE_CAPTURE_SCRIPT
    assert 'for(i=2;i<=n;i++)' in authority.REMOTE_CAPTURE_SCRIPT
    assert 'if(total<=0) exit 1' in authority.REMOTE_CAPTURE_SCRIPT
    assert "image_json=" not in authority.REMOTE_CAPTURE_SCRIPT
    for field in ("Id", "Os", "Architecture", "Created"):
        assert f"--format '{{{{.{field}}}}}'" in authority.REMOTE_CAPTURE_SCRIPT


def test_dry_run_does_not_capture(monkeypatch: pytest.MonkeyPatch, tmp_path: Path) -> None:
    root = _root(tmp_path)
    monkeypatch.setattr(authority, "final_head_source_identity", lambda _root: {"commit": "a" * 40, "tree": "b" * 40})
    value = authority.dry_run(root=root)
    assert value["status"] == "DRY_RUN" and value["execute_required"] is True
