#!/bin/sh
# artifact_selection_test.sh -- fail-able gate for farm.py's per-role
# --binary-set-S/-C/-F selection mechanism (S4).
#
# WITHOUT starting a full cluster: resolves each role's selected runtime
# root via farm.py's OWN role_tree() function and issues remote commands
# through farm.py's OWN sh() transport (imported directly, never
# reimplemented) -- so this test exercises the exact code path up() and
# run_client() use. It launches that role's executable with a
# version-identity probe inside the pinned container on the target host
# and asserts the reported identity matches the selected set:
#   p43 (tag 1.4, cd74801e0fa4e83e3ae254ca1d7fe98642f36b89) -> "...1.4.0"
#   p50 (trunk,   43297d535232d8becb58866d5fb6cc73fa3b033d) -> "...1.4.92"
# Then, for every role, it flips the selection (p43 <-> p50) and asserts
# the probe output CHANGES. A selection mechanism that only changes a
# printed label while actually launching the same binary would make every
# "flip" assertion below fail -- that is the bug this test exists to catch
# (verified by deliberately introducing exactly that bug into role_tree()
# and confirming this script then reports FAIL; see the S4 report for that
# run's verbatim output).
set -eu

FARMDIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
HOST=${ARTIFACT_TEST_HOST:-q3}
IMG=icecream/farm-node:ubuntu22-gcc11-boost174

fail() { echo "FAIL: $*" >&2; exit 1; }

# probe SET ROLE -- prints the version-identity string reported by ROLE's
# executable when farm.py's role_tree(SET) resolves the runtime root.
# SET is "p43", "p50", or "default" (meaning no --binary-set given, i.e.
# role_tree(None) -- the pre-existing hardcoded TREE).
probe() {
    set_name=$1 role=$2
    python3 - "$set_name" "$role" "$HOST" "$IMG" "$FARMDIR" <<'PY'
import re
import sys

set_name, role, host, img, farmdir = sys.argv[1:6]
sys.path.insert(0, farmdir)
import farm

tree = farm.role_tree(None if set_name == "default" else set_name)
cmds = {
    "S": f"docker run --rm -v {tree}:/probe -u 0:0 {img} "
         f"/probe/obj/scheduler/icecc-scheduler --version",
    "F": f"docker run --rm -v {tree}:/probe -u 0:0 {img} bash -c "
         f"\"strings /probe/obj/daemon/iceccd | "
         f"grep -oE 'ICECREAM daemon [0-9]+\\.[0-9]+(\\.[0-9]+)?' | head -1\"",
    "C": f"docker run --rm -v {tree}:/probe -u 0:0 {img} /probe/obj/client/icecc --version",
}
r = farm.sh(host, cmds[role], timeout=60)
out = (r.stdout + r.stderr).strip()
if role == "S":
    m = re.search(r"ICECREAM scheduler [0-9]+\.[0-9]+(\.[0-9]+)?", out)
    out = m.group(0) if m else out
print(out)
PY
}

assert_contains() {
    haystack=$1 needle=$2 label=$3
    case "$haystack" in
        *"$needle"*) echo "ok - $label ($haystack)" ;;
        *) fail "$label: expected to contain '$needle', got '$haystack'" ;;
    esac
}

assert_differs() {
    a=$1 b=$2 label=$3
    if [ "$a" = "$b" ]; then
        fail "$label: selection flip did not change the probe output ('$a' both times) -- the mechanism only changed a label, not the launched binary"
    fi
    echo "ok - $label ('$a' != '$b')"
}

echo "== identity baseline: each (role, set) reports its own version =="
for role in S F C; do
    p43_out=$(probe p43 "$role")
    p50_out=$(probe p50 "$role")
    assert_contains "$p43_out" "1.4.0" "role=$role set=p43 reports P43 identity"
    assert_contains "$p50_out" "1.4.92" "role=$role set=p50 reports trunk identity"
done

echo
echo "== selection-flip mutation: same role, opposite set, output MUST change =="
for role in S F C; do
    p43_out=$(probe p43 "$role")
    p50_out=$(probe p50 "$role")
    assert_differs "$p43_out" "$p50_out" "role=$role flip p43<->p50 changes the launched binary's reported identity"
done

echo
echo "PASS: artifact_selection_test -- selection mechanism changes the launched binary, not merely its label"
