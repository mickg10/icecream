#!/bin/sh
set -eu

build_dir=${ICECC_TEST_BUILDDIR:?ICECC_TEST_BUILDDIR is required}
if [ "$(id -u)" -ne 0 ] || { [ ! -e /.dockerenv ] && [ ! -e /run/.containerenv ]; }; then
    echo "FAIL: receipt terminal-retirement check requires an isolated root container" >&2
    exit 2
fi
if [ -z "${ICEFARM_TMPDIR:-}" ] || [ ! -d "$ICEFARM_TMPDIR" ] ||
   [ ! -w "$ICEFARM_TMPDIR" ]; then
    echo "FAIL: receipt terminal-retirement check requires writable ICEFARM_TMPDIR" >&2
    exit 2
fi
work=$(mktemp -d "$ICEFARM_TMPDIR/p51-terminal-retirement.XXXXXX")
log=$work/run.log
set +e
timeout --foreground --signal=TERM --kill-after=2s 15s env \
    ICECC_TEST_POSITIVE_DAEMON=1 \
    "$build_dir/p50daemonpositive" \
    --p51-receipt-terminal-retirement-selftest "$work/fixture" >"$log" 2>&1
status=$?
set -e
cat "$log"
if [ "$status" -ne 0 ]; then
    echo "FAIL: terminal-retirement selftest exited $status; log=$log" >&2
    exit "$status"
fi
grep -q '^PASS: P51 one-shot clean idle EOF retires redirect, closes queued peers, and permits direct reconnect$' "$log"
echo "P51_RECEIPT_TERMINAL_RETIREMENT_PASS clean_idle=1 exact_rule_removed=1 queued_peer_closed=1 direct_reconnect=1"

# Verify the kernel's OUTPUT/owner redirect lifecycle independently of the
# C++ gate's fake-iptables argument-vector check above. This is intentionally
# a separate smoke, not a claim that the production gate itself ran under NAT.
python3 - "$work/nat" <<'PY'
import os
import pwd
import select
import shutil
import socket
import struct
import subprocess
import sys
import threading

work = sys.argv[1]
os.makedirs(work, exist_ok=True)
uid = pwd.getpwnam("nobody").pw_uid
gid = pwd.getpwnam("nobody").pw_gid

def listener():
    sock = socket.socket()
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    sock.bind(("127.0.0.1", 0))
    sock.listen(8)
    return sock

upstream = listener()
proxy = listener()
upstream_port = upstream.getsockname()[1]
proxy_port = proxy.getsockname()[1]
rule = ["-t", "nat", "-A", "OUTPUT", "-p", "tcp", "--dport",
        str(upstream_port), "-m", "owner", "--uid-owner", str(uid),
        "-j", "REDIRECT", "--to-ports", str(proxy_port)]
delete_rule = rule.copy()
delete_rule[2] = "-D"
installed = False

def iptables(args, expected):
    result = subprocess.run(["iptables", "-w", *args], check=False,
                            stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                            text=True, timeout=3)
    if (result.returncode == 0) != expected:
        raise RuntimeError("iptables command status mismatch: " +
                           " ".join(args) + " rc=" + str(result.returncode) +
                           " stderr=" + result.stderr.strip())

def client(port, mode):
    code = f"""import socket,sys,time
s=socket.socket(); s.settimeout(3)
s.connect(('127.0.0.1',{port})); print('CONNECTED local=%r peer=%r' % (s.getsockname(),s.getpeername()),flush=True)
started=time.monotonic(); data=b''; outcome='eof'
try:
    while True:
        chunk=s.recv(64)
        if not chunk: break
        data+=chunk
except Exception as exc:
    outcome=type(exc).__name__+':errno='+str(getattr(exc,'errno',None))
print('RESULT='+data.decode()+';OUTCOME='+outcome+';ELAPSED='+
      str(round(time.monotonic()-started,3)),flush=True)
mode={mode!r}
ok=((mode=='closed' and outcome.startswith(('eof','ConnectionResetError'))) or
    (mode=='reset' and outcome.startswith('ConnectionResetError')) or
    (mode not in ('closed','reset') and data.decode()==mode))
sys.exit(0 if ok else 1)
"""
    return ["setpriv", "--reuid", str(uid), "--regid", str(gid),
            "--clear-groups", "python3", "-c", code]

upstream_thread_error = []
def serve_upstream_once():
    try:
        conn, _ = upstream.accept()
        with conn:
            conn.sendall(b"direct\n")
    except Exception as exc:
        upstream_thread_error.append(repr(exc))

upstream_thread = threading.Thread(target=serve_upstream_once, daemon=True)
proxy.settimeout(3)

def wait_connected(proc):
    ready, _, _ = select.select([proc.stdout], [], [], 3)
    line = proc.stdout.readline().strip() if ready else ""
    if not line.startswith("CONNECTED "):
        raise RuntimeError("sidecar client did not connect through expected path")
    return line

try:
    # Verify the client-side close detector independently with an ordinary
    # TCP connection and an abortive server close.
    reset_listener = listener()
    reset_port = reset_listener.getsockname()[1]
    reset_client = subprocess.Popen(client(reset_port, "reset"),
                                    stdout=subprocess.PIPE,
                                    stderr=subprocess.PIPE, text=True)
    reset_connected = wait_connected(reset_client)
    reset_peer, _ = reset_listener.accept()
    reset_peer.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER,
                          struct.pack("ii", 1, 0))
    reset_peer.close()
    reset_out, reset_err = reset_client.communicate(timeout=4)
    reset_listener.close()
    print("P51_NAT_PLAIN_RST_CONTROL rc=%d out=%r err=%r" %
          (reset_client.returncode, reset_connected + "\\n" + reset_out, reset_err))
    if reset_client.returncode != 0 or "OUTCOME=ConnectionResetError" not in reset_out:
        raise RuntimeError("plain TCP abortive-close control failed: " + reset_err + reset_out)

    subprocess.run(["iptables", "-w", *rule], check=True, timeout=3)
    installed = True
    iptables(["-t", "nat", "-C", *rule[3:]], True)

    first_client = subprocess.Popen(client(upstream_port, "proxied\n"), stdout=subprocess.PIPE,
                                    stderr=subprocess.PIPE, text=True)
    first_connected = wait_connected(first_client)
    first, _ = proxy.accept()
    print("P51_NAT_PROXY_ACCEPT_FIRST local=%r peer=%r" %
          (first.getsockname(), first.getpeername()))
    with first:
        first.sendall(b"proxied\n")
    first_out, first_err = first_client.communicate(timeout=4)
    print("P51_NAT_REDIRECTED_FIRST rc=%d out=%r err=%r" %
          (first_client.returncode, first_connected + "\\n" + first_out, first_err))
    if first_client.returncode != 0 or "RESULT=proxied\n" not in first_out:
        raise RuntimeError("redirected first client failed: " + first_err + first_out)

    queued_clients = [subprocess.Popen(client(upstream_port, "closed"), stdout=subprocess.PIPE,
                                       stderr=subprocess.PIPE, text=True)
                      for _ in range(2)]
    queued_connected = [wait_connected(queued_client)
                        for queued_client in queued_clients]
    conntrack = shutil.which("conntrack")
    if conntrack:
        state = subprocess.run([conntrack, "-L", "-p", "tcp", "-o", "extended"],
                               check=False, stdout=subprocess.PIPE,
                               stderr=subprocess.PIPE, text=True, timeout=3)
        selected = [line for line in state.stdout.splitlines()
                    if str(upstream_port) in line or str(proxy_port) in line]
        print("P51_NAT_CONNTRACK rc=%d rows=%r stderr=%r" %
              (state.returncode, selected, state.stderr.strip()))
    else:
        print("P51_NAT_CONNTRACK unavailable=1")
    proxy.setblocking(False)
    drained = 0
    while True:
        try:
            queued, _ = proxy.accept()
        except BlockingIOError:
            break
        queued.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER,
                          struct.pack("ii", 1, 0))
        print("P51_NAT_PROXY_ACCEPT_QUEUED local=%r peer=%r" %
              (queued.getsockname(), queued.getpeername()))
        queued.shutdown(socket.SHUT_RDWR)
        queued.close()
        drained += 1
    if drained != len(queued_clients):
        raise RuntimeError(f"expected two queued redirected peers, drained {drained}")
    subprocess.run(["iptables", "-w", *delete_rule], check=True, timeout=3)
    installed = False
    iptables(["-t", "nat", "-C", *rule[3:]], False)
    proxy.close()
    queued_failures = []
    for queued_client, connected in zip(queued_clients, queued_connected):
        queued_out, queued_err = queued_client.communicate(timeout=4)
        if (queued_client.returncode != 0 or "RESULT=;OUTCOME=" not in queued_out or
                "TimeoutError" in queued_out):
            ss = shutil.which("ss")
            state = (subprocess.run([ss, "-tnpo"], check=False,
                                    stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                    text=True, timeout=3).stdout if ss else "ss unavailable")
            print("P51_NAT_TCP_STATE_AFTER_DRAIN=%r" % state)
            queued_failures.append((connected, queued_client.returncode,
                                    queued_err, queued_out))
    print("P51_NAT_QUEUED_RESULTS=%r" % queued_failures)

    upstream_thread.start()
    direct_client = subprocess.run(client(upstream_port, "direct\n"), check=False,
                                   stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                   text=True, timeout=4)
    upstream_thread.join(timeout=4)
    if (direct_client.returncode != 0 or "RESULT=direct\n" not in direct_client.stdout or
            upstream_thread.is_alive() or upstream_thread_error):
        raise RuntimeError("fresh sidecar connection did not reach upstream: " +
                           direct_client.stderr + direct_client.stdout +
                           repr(upstream_thread_error))
    print("P51_NAT_DIRECT_RECONNECT rc=%d out=%r err=%r" %
          (direct_client.returncode, direct_client.stdout, direct_client.stderr))
    if queued_failures:
        raise RuntimeError("queued redirected peers did not promptly terminate: " +
                           repr(queued_failures))
    print("P51_RECEIPT_TERMINAL_RETIREMENT_REAL_NAT_PASS redirect=1 "
          "queued_peer_closed=1 rule_removed=1 direct_reconnect=1")
finally:
    if installed:
        subprocess.run(["iptables", "-w", *delete_rule], check=False,
                       stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=3)
    proxy.close()
    upstream.close()
PY
rm -rf "$work"
