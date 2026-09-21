from __future__ import annotations

import datetime as dt
import hashlib
import io
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


def _with_idle(capture: dict[str, object], idle: float) -> dict[str, object]:
    value = dict(capture)
    sample = dict(capture["cpu_sample"])
    sample["idle_percent"] = idle
    value["cpu_sample"] = sample
    value["cpu_sample_digest"] = authority.digest(authority.canonical(sample))
    return value


class _Clock:
    def __init__(self) -> None:
        self.now = 0.0
        self.sleeps: list[float] = []

    def monotonic(self) -> float:
        return self.now

    def sleep(self, seconds: float) -> None:
        self.sleeps.append(seconds)
        self.now += seconds


def _remote_roots() -> dict[str, str]:
    return {host: "/roles" for host in authority.HOSTS}


def test_idle_cooldown_recaptures_q3_until_authority_can_publish(tmp_path: Path) -> None:
    root = _root(tmp_path)
    now = dt.datetime.now(dt.timezone.utc).replace(microsecond=0)
    captures = _captures(now, root)
    busy_q3 = _with_idle(captures["q3"], 80.0)
    clock = _Clock()
    calls: list[str] = []
    stderr = io.StringIO()

    def fake_capture(host: str, _remote_root: str, *, timeout: float) -> dict[str, object]:
        calls.append(host)
        return busy_q3 if host == "q3" and calls.count("q3") == 1 else captures[host]

    value = authority.capture_and_build_authority(
        root=root, remote_roots=_remote_roots(), descriptor_dir=tmp_path / "d",
        output=tmp_path / "a.json", idle_cooldown_timeout=2.0,
        idle_cooldown_interval=1.0, capture_fn=fake_capture,
        sleep_fn=clock.sleep, monotonic_fn=clock.monotonic, stderr=stderr)
    assert value["schema"] == authority.SCHEMA
    assert calls == ["q3", "q2", "research6", "research7",
                     "q3", "q2", "research6", "research7"]
    assert clock.sleeps == [1.0]
    assert "placement:host_not_idle:q3" in stderr.getvalue()
    assert (tmp_path / "a.json").is_file()
    assert (tmp_path / "d" / "q3-descriptor.json").is_file()


def test_idle_cooldown_times_out_without_publishing_partial_outputs(tmp_path: Path) -> None:
    root = _root(tmp_path)
    now = dt.datetime.now(dt.timezone.utc).replace(microsecond=0)
    captures = _captures(now, root)
    busy_q3 = _with_idle(captures["q3"], 80.0)
    clock = _Clock()
    calls: list[str] = []
    stderr = io.StringIO()

    def fake_capture(host: str, _remote_root: str, *, timeout: float) -> dict[str, object]:
        calls.append(host)
        return busy_q3 if host == "q3" else captures[host]

    with pytest.raises(authority.AuthorityError, match="host_not_idle:q3"):
        authority.capture_and_build_authority(
            root=root, remote_roots=_remote_roots(), descriptor_dir=tmp_path / "d",
            output=tmp_path / "a.json", idle_cooldown_timeout=2.0,
            idle_cooldown_interval=1.0, capture_fn=fake_capture,
            sleep_fn=clock.sleep, monotonic_fn=clock.monotonic, stderr=stderr)
    assert calls == ["q3", "q2", "research6", "research7"] * 3
    assert clock.sleeps == [1.0, 1.0]
    assert "cooldown timeout" in stderr.getvalue()
    assert not (tmp_path / "a.json").exists()
    assert not (tmp_path / "d").exists()


def test_idle_cooldown_does_not_retry_other_placement_errors(tmp_path: Path) -> None:
    root = _root(tmp_path)
    now = dt.datetime.now(dt.timezone.utc).replace(microsecond=0)
    captures = _captures(now, root)
    clock = _Clock()
    calls: list[str] = []
    stderr = io.StringIO()

    def fake_capture(host: str, _remote_root: str, *, timeout: float) -> dict[str, object]:
        calls.append(host)
        return captures[host]

    with pytest.raises(authority.AuthorityError, match="C1F20/40:mapping_invalid"):
        authority.capture_and_build_authority(
            root=root, remote_roots=_remote_roots(), descriptor_dir=tmp_path / "d",
            output=tmp_path / "a.json", idle_cooldown_timeout=10.0,
            idle_cooldown_interval=1.0, capture_fn=fake_capture,
            sleep_fn=clock.sleep, monotonic_fn=clock.monotonic, stderr=stderr,
            mappings={"C1F1/100000": ["q2"], "C1F20/40": ["q2"]})
    assert calls == list(authority.HOSTS)
    assert clock.sleeps == []
    assert stderr.getvalue() == ""
    assert not (tmp_path / "a.json").exists()


def test_idle_cooldown_recaptures_required_q2(tmp_path: Path) -> None:
    root = _root(tmp_path)
    now = dt.datetime.now(dt.timezone.utc).replace(microsecond=0)
    captures = _captures(now, root)
    busy_q2 = _with_idle(captures["q2"], 80.0)
    clock = _Clock()
    calls: list[str] = []

    def fake_capture(host: str, _remote_root: str, *, timeout: float) -> dict[str, object]:
        calls.append(host)
        return busy_q2 if host == "q2" and calls.count("q2") == 1 else captures[host]

    value = authority.capture_and_build_authority(
        root=root, remote_roots=_remote_roots(), descriptor_dir=tmp_path / "d",
        output=tmp_path / "a.json", idle_cooldown_timeout=2.0,
        idle_cooldown_interval=1.0, capture_fn=fake_capture,
        sleep_fn=clock.sleep, monotonic_fn=clock.monotonic, stderr=io.StringIO())
    assert value["schema"] == authority.SCHEMA
    assert calls == ["q3", "q2", "research6", "research7"] * 2
    assert clock.sleeps == [1.0]


def test_idle_cooldown_restarts_with_full_snapshot_after_host_changes(
    tmp_path: Path,
) -> None:
    root = _root(tmp_path)
    now = dt.datetime.now(dt.timezone.utc).replace(microsecond=0)
    captures = _captures(now, root)
    busy_q3 = _with_idle(captures["q3"], 80.0)
    busy_q2 = _with_idle(captures["q2"], 80.0)
    clock = _Clock()
    calls: list[str] = []

    def fake_capture(host: str, _remote_root: str, *, timeout: float) -> dict[str, object]:
        round_index = len(calls) // len(authority.HOSTS)
        calls.append(host)
        if round_index == 0 and host == "q3":
            return busy_q3
        if round_index == 1 and host == "q2":
            return busy_q2
        return captures[host]

    value = authority.capture_and_build_authority(
        root=root, remote_roots=_remote_roots(), descriptor_dir=tmp_path / "d",
        output=tmp_path / "a.json", idle_cooldown_timeout=3.0,
        idle_cooldown_interval=1.0, capture_fn=fake_capture,
        sleep_fn=clock.sleep, monotonic_fn=clock.monotonic,
        stderr=io.StringIO())
    assert value["schema"] == authority.SCHEMA
    assert calls == list(authority.HOSTS) * 3
    assert clock.sleeps == [1.0, 1.0]
    assert (tmp_path / "a.json").is_file()


def test_idle_cooldown_times_out_for_required_research7(tmp_path: Path) -> None:
    root = _root(tmp_path)
    now = dt.datetime.now(dt.timezone.utc).replace(microsecond=0)
    captures = _captures(now, root)
    captures["research7"] = _with_idle(captures["research7"], 80.0)
    clock = _Clock()
    calls: list[str] = []
    stderr = io.StringIO()

    def fake_capture(host: str, _remote_root: str, *, timeout: float) -> dict[str, object]:
        calls.append(host)
        return captures[host]

    with pytest.raises(authority.AuthorityError, match="host_not_idle:research7"):
        authority.capture_and_build_authority(
            root=root, remote_roots=_remote_roots(), descriptor_dir=tmp_path / "d",
            output=tmp_path / "a.json", idle_cooldown_timeout=2.0,
            idle_cooldown_interval=1.0, capture_fn=fake_capture,
            sleep_fn=clock.sleep, monotonic_fn=clock.monotonic, stderr=stderr)
    assert calls == ["q3", "q2", "research6", "research7"] * 3
    assert clock.sleeps == [1.0, 1.0]
    assert "host=research7" in stderr.getvalue()
    assert not (tmp_path / "a.json").exists()
    assert not (tmp_path / "d").exists()


def test_excluded_research6_hold_does_not_trigger_cooldown(tmp_path: Path) -> None:
    root = _root(tmp_path)
    now = dt.datetime.now(dt.timezone.utc).replace(microsecond=0)
    captures = _captures(now, root)
    captures["research6"] = _with_idle(captures["research6"], 80.0)
    clock = _Clock()
    calls: list[str] = []

    def fake_capture(host: str, _remote_root: str, *, timeout: float) -> dict[str, object]:
        calls.append(host)
        return captures[host]

    value = authority.capture_and_build_authority(
        root=root, remote_roots=_remote_roots(), descriptor_dir=tmp_path / "d",
        output=tmp_path / "a.json", idle_cooldown_timeout=10.0,
        idle_cooldown_interval=1.0, capture_fn=fake_capture,
        sleep_fn=clock.sleep, monotonic_fn=clock.monotonic, stderr=io.StringIO())
    assert value["hosts"]["research6"]["idle"]["status"] == "HOLD"
    assert calls == list(authority.HOSTS)
    assert clock.sleeps == []


@pytest.mark.parametrize(
    ("timeout", "interval", "error"),
    [(-1.0, 1.0, "idle_cooldown_timeout:invalid"),
     (0.0, 0.0, "idle_cooldown_interval:invalid"),
     (authority.MAX_IDLE_COOLDOWN_TIMEOUT_SECONDS + 1, 1.0,
      "idle_cooldown_timeout:invalid")],
)
def test_idle_cooldown_bounds_fail_before_capture(
    tmp_path: Path, timeout: float, interval: float, error: str) -> None:
    root = _root(tmp_path)
    called = False

    def unexpected_capture(*_args: object, **_kwargs: object) -> dict[str, object]:
        nonlocal called
        called = True
        raise AssertionError("capture must not run")

    with pytest.raises(authority.AuthorityError, match=error):
        authority.capture_and_build_authority(
            root=root, remote_roots=_remote_roots(), descriptor_dir=tmp_path / "d",
            output=tmp_path / "a.json", idle_cooldown_timeout=timeout,
            idle_cooldown_interval=interval, capture_fn=unexpected_capture)
    assert called is False
