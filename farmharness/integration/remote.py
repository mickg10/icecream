"""Bounded argv-only command transport with a deterministic fake recorder."""

from __future__ import annotations

import base64
import json
import os
import shlex
import signal
import subprocess
from dataclasses import dataclass
from typing import Any, Iterable, Protocol

try:
    from .farm_spec import FarmSpec
except ImportError:  # Direct execution from this directory.
    from farm_spec import FarmSpec


class RemoteError(RuntimeError):
    """A bounded remote command failed."""


REMOTE_ARGV_EXEC = (
    "import base64,json,os,sys;"
    "a=json.loads(base64.urlsafe_b64decode(sys.argv[1].encode()));"
    "os.execvp(a[0],a)"
)


@dataclass(frozen=True)
class PlannedCommand:
    sequence: int
    phase: str
    host: str
    instance: str | None
    transport: str
    timeout_s: int
    argv: tuple[str, ...]

    def __post_init__(self) -> None:
        if not self.argv or any(not isinstance(item, str) or "\0" in item for item in self.argv):
            raise ValueError("command argv must contain non-NUL strings")
        if self.timeout_s <= 0:
            raise ValueError("command timeout must be positive")

    def as_dict(self) -> dict[str, Any]:
        return {
            "argv": list(self.argv),
            "host": self.host,
            "instance": self.instance,
            "phase": self.phase,
            "sequence": self.sequence,
            "timeout_s": self.timeout_s,
            "transport": self.transport,
        }


@dataclass(frozen=True)
class CommandResult:
    returncode: int
    stdout: str
    stderr: str


class Recorder(Protocol):
    def invoke(self, command: PlannedCommand) -> CommandResult:
        ...


class FakeRecorder:
    """Record exact commands without executing or writing anything."""

    def __init__(self) -> None:
        self.commands: list[PlannedCommand] = []

    def invoke(self, command: PlannedCommand) -> CommandResult:
        self.commands.append(command)
        return CommandResult(returncode=0, stdout="", stderr="")


class SubprocessTransport:
    """Execute a fully resolved argv with no shell and a hard timeout."""

    def invoke(self, command: PlannedCommand) -> CommandResult:
        process: subprocess.Popen[str] | None = None
        try:
            process = subprocess.Popen(
                list(command.argv),
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                encoding="utf-8",
                errors="replace",
                text=True,
                shell=False,
                start_new_session=True,
            )
            stdout, stderr = process.communicate(timeout=command.timeout_s)
        except subprocess.TimeoutExpired as exc:
            if process is not None:
                try:
                    os.killpg(process.pid, signal.SIGTERM)
                except ProcessLookupError:
                    pass
                try:
                    process.communicate(timeout=5)
                except subprocess.TimeoutExpired:
                    try:
                        os.killpg(process.pid, signal.SIGKILL)
                    except ProcessLookupError:
                        pass
                    process.communicate()
            raise RemoteError(
                f"command {command.sequence} timed out after {command.timeout_s}s on {command.host}"
            ) from exc
        except OSError as exc:
            raise RemoteError(f"command {command.sequence} could not start: {exc}") from exc
        assert process is not None
        result = CommandResult(process.returncode, stdout, stderr)
        if result.returncode != 0:
            raise RemoteError(
                f"command {command.sequence} failed rc={result.returncode} on {command.host}: "
                f"{(result.stderr or result.stdout)[-4000:].strip()}"
            )
        return result


def ssh_argv(farm: FarmSpec, host_name: str, remote_argv: Iterable[str]) -> tuple[str, ...]:
    host = farm.hosts[host_name]
    resolved = tuple(remote_argv)
    if not resolved or any(not isinstance(item, str) or "\0" in item for item in resolved):
        raise ValueError("remote argv must contain non-NUL strings")
    # OpenSSH concatenates its command operands for a remote login shell.  No
    # spec value is placed in that shell text: the sole dynamic operand is a
    # URL-safe base64 JSON payload decoded by this fixed, quoted Python shim.
    payload = base64.urlsafe_b64encode(
        json.dumps(resolved, separators=(",", ":"), ensure_ascii=False).encode()
    ).decode("ascii")
    return (
        "ssh",
        "-o",
        "BatchMode=yes",
        "-o",
        "ConnectTimeout=10",
        host["ssh"],
        "python3",
        "-c",
        shlex.quote(REMOTE_ARGV_EXEC),
        payload,
    )


def decode_ssh_payload(argv: Iterable[str]) -> tuple[str, ...]:
    """Decode a planned SSH payload for tests, review, and plan inspection."""

    values = tuple(argv)
    if len(values) < 2:
        raise ValueError("SSH wrapper argv is truncated")
    try:
        decoded = json.loads(base64.urlsafe_b64decode(values[-1].encode("ascii")))
    except (ValueError, UnicodeError, json.JSONDecodeError) as exc:
        raise ValueError("SSH wrapper payload is invalid") from exc
    if not isinstance(decoded, list) or not decoded or not all(isinstance(item, str) for item in decoded):
        raise ValueError("SSH wrapper payload is not an argv")
    return tuple(decoded)


def docker_argv(farm: FarmSpec, host_name: str, args: Iterable[str]) -> tuple[str, ...]:
    host = farm.hosts[host_name]
    context = host.get("docker_context")
    docker = ("docker", "--context", context, *tuple(args)) if context else ("docker", *tuple(args))
    return docker if context else ssh_argv(farm, host_name, docker)


def execute(commands: Iterable[PlannedCommand], recorder: Recorder) -> list[CommandResult]:
    return [recorder.invoke(command) for command in commands]
