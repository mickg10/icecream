#!/bin/sh
set -eu

srcdir=${srcdir:-$(dirname "$0")}
PYTHON=${PYTHON:-python3}
trace_file=${TMPDIR:-/tmp}/p50-trace-$$.jsonl
mutated_file=${TMPDIR:-/tmp}/p50-trace-mutated-$$.jsonl
cross_file=${TMPDIR:-/tmp}/p50-trace-cross-$$.jsonl
abort_file=${TMPDIR:-/tmp}/p50-trace-abort-$$.jsonl
relseq_file=${TMPDIR:-/tmp}/p50-trace-relseq-$$.jsonl
nonce_file=${TMPDIR:-/tmp}/p50-trace-nonce-$$.jsonl
trap 'rm -f "$trace_file" "$mutated_file" "$cross_file" "$abort_file" "$relseq_file" "$nonce_file"' EXIT HUP INT TERM

P50_TRACE_PATH=$trace_file "$srcdir/p50_slice0_test"
"$PYTHON" "$srcdir/../cache/formal/check_trace.py" "$trace_file"

"$PYTHON" - "$trace_file" "$mutated_file" <<'PY'
import json
import sys

source, target = sys.argv[1:]
rows = [json.loads(line) for line in open(source, encoding="utf-8")]
index = next(i for i, row in enumerate(rows) if row["action"] == "NEED_RECORDED")
rows[index]["remaining_need"] += 1
with open(target, "w", encoding="utf-8") as output:
    for row in rows:
        output.write(json.dumps(row, separators=(",", ":")) + "\n")
PY

if "$PYTHON" "$srcdir/../cache/formal/check_trace.py" "$mutated_file" \
        >"$mutated_file.out" 2>&1; then
    echo "p50_trace_check: mutated trace unexpectedly passed" >&2
    rm -f "$mutated_file.out"
    exit 1
fi
grep -F "Need precedes exact DICT" "$mutated_file.out" >/dev/null || {
    cat "$mutated_file.out" >&2
    rm -f "$mutated_file.out"
    exit 1
}
rm -f "$mutated_file.out"

"$PYTHON" - "$trace_file" "$cross_file" <<'PY'
import json
import sys

source, target = sys.argv[1:]
rows = [json.loads(line) for line in open(source, encoding="utf-8")]
relations = {}
for i, row in enumerate(rows):
    if row["action"] == "TX_BEGIN" and row["actor"] == "C":
        relations.setdefault((row["c_store_guid"], row["f_store_guid"]), []).append(i)
selected = next(indices for indices in relations.values() if len(indices) >= 2)
rows[selected[1]]["rel_seq"] = rows[selected[0]]["rel_seq"]
with open(target, "w", encoding="utf-8") as output:
    for row in rows:
        output.write(json.dumps(row, separators=(",", ":")) + "\n")
PY

if "$PYTHON" "$srcdir/../cache/formal/check_trace.py" "$cross_file" \
        >"$cross_file.out" 2>&1; then
    echo "p50_trace_check: cross-TU cursor mutation unexpectedly passed" >&2
    rm -f "$cross_file.out"
    exit 1
fi
grep -E "(C TX_BEGIN missed its cursor|second C active transaction)" \
    "$cross_file.out" >/dev/null || {
    cat "$cross_file.out" >&2
    rm -f "$cross_file.out"
    exit 1
}
rm -f "$cross_file.out"

cat >"$abort_file" <<'EOF'
{"actor":"F","action":"SESSION_OPENED","c_store_guid":"c","f_store_guid":"f","session_serial":1,"history_nonce":0,"rel_seq":0,"tu_seq":0,"transaction_digest":"","raw_digest":"","state_digest":""}
{"actor":"F","action":"HISTORY_RESET","c_store_guid":"c","f_store_guid":"f","session_serial":1,"history_nonce":1,"rel_seq":0,"tu_seq":0,"transaction_digest":"","raw_digest":"","state_digest":"s0"}
{"actor":"C","action":"TX_BEGIN","c_store_guid":"c","f_store_guid":"f","session_serial":1,"history_nonce":1,"rel_seq":0,"tu_seq":9,"transaction_digest":"tx","raw_digest":"raw","state_digest":"s0"}
{"actor":"F","action":"TX_BEGIN","c_store_guid":"c","f_store_guid":"f","session_serial":1,"history_nonce":1,"rel_seq":0,"tu_seq":9,"transaction_digest":"tx","raw_digest":"raw","state_digest":"s0"}
{"actor":"F","action":"DICT_COMPLETE","c_store_guid":"c","f_store_guid":"f","session_serial":1,"history_nonce":1,"rel_seq":0,"tu_seq":9,"transaction_digest":"tx","raw_digest":"raw","state_digest":"s0"}
{"actor":"F","action":"NEED_RECORDED","c_store_guid":"c","f_store_guid":"f","session_serial":1,"history_nonce":1,"rel_seq":0,"tu_seq":9,"transaction_digest":"tx","raw_digest":"raw","state_digest":"s0","need_keys":[],"remaining_need":0}
{"actor":"F","action":"BODY_COMPLETE","c_store_guid":"c","f_store_guid":"f","session_serial":1,"history_nonce":1,"rel_seq":0,"tu_seq":9,"transaction_digest":"tx","raw_digest":"raw","state_digest":"s0"}
{"actor":"F","action":"INPUT_MATERIALIZED","c_store_guid":"c","f_store_guid":"f","session_serial":1,"history_nonce":1,"rel_seq":0,"tu_seq":9,"transaction_digest":"tx","raw_digest":"raw","state_digest":"s0"}
{"actor":"F","action":"INPUT_COMMITTED","c_store_guid":"c","f_store_guid":"f","session_serial":1,"history_nonce":1,"rel_seq":0,"tu_seq":9,"transaction_digest":"tx","raw_digest":"raw","state_digest":"s1"}
{"actor":"C","action":"TX_ABORTED","c_store_guid":"c","f_store_guid":"f","session_serial":1,"history_nonce":1,"rel_seq":0,"tu_seq":9,"transaction_digest":"tx","raw_digest":"raw","state_digest":"s0"}
EOF

if "$PYTHON" "$srcdir/../cache/formal/check_trace.py" "$abort_file" \
        >"$abort_file.out" 2>&1; then
    echo "p50_trace_check: abort-after-commit fixture unexpectedly passed" >&2
    rm -f "$abort_file.out"
    exit 1
fi
grep -F "C abort after durable F commit before acceptance" \
    "$abort_file.out" >/dev/null || {
    cat "$abort_file.out" >&2
    rm -f "$abort_file.out"
    exit 1
}
rm -f "$abort_file.out"

"$PYTHON" - "$trace_file" "$relseq_file" <<'PY'
import json
import sys

source, target = sys.argv[1:]
rows = [json.loads(line) for line in open(source, encoding="utf-8")]
begins = [i for i, row in enumerate(rows)
          if row["action"] == "TX_BEGIN" and row["actor"] == "C"]
index = begins[-1]
rows[index]["rel_seq"] = (1 << 64) - 1
with open(target, "w", encoding="utf-8") as output:
    for row in rows:
        output.write(json.dumps(row, separators=(",", ":")) + "\n")
PY

if "$PYTHON" "$srcdir/../cache/formal/check_trace.py" "$relseq_file" \
        >"$relseq_file.out" 2>&1; then
    echo "p50_trace_check: terminal REL_SEQ fixture unexpectedly passed" >&2
    rm -f "$relseq_file.out"
    exit 1
fi
grep -E "(C TX_BEGIN missed its cursor|new C history did not start at zero|second C active transaction)" \
    "$relseq_file.out" >/dev/null || {
    cat "$relseq_file.out" >&2
    rm -f "$relseq_file.out"
    exit 1
}
rm -f "$relseq_file.out"

cat >"$nonce_file" <<'EOF'
{"actor":"F","action":"SESSION_OPENED","c_store_guid":"c","f_store_guid":"f","session_serial":1,"history_nonce":0,"rel_seq":0,"tu_seq":0,"transaction_digest":"","raw_digest":"","state_digest":""}
{"actor":"F","action":"HISTORY_RESET","c_store_guid":"c","f_store_guid":"f","session_serial":1,"history_nonce":9,"rel_seq":0,"tu_seq":0,"transaction_digest":"","raw_digest":"","state_digest":"s9"}
{"actor":"F","action":"HISTORY_RESET","c_store_guid":"c","f_store_guid":"f","session_serial":1,"history_nonce":8,"rel_seq":0,"tu_seq":0,"transaction_digest":"","raw_digest":"","state_digest":"s8"}
EOF

if "$PYTHON" "$srcdir/../cache/formal/check_trace.py" "$nonce_file" \
        >"$nonce_file.out" 2>&1; then
    echo "p50_trace_check: decreasing HISTORY_NONCE fixture unexpectedly passed" >&2
    rm -f "$nonce_file.out"
    exit 1
fi
grep -F "HISTORY_NONCE was reused or did not increase" \
    "$nonce_file.out" >/dev/null || {
    cat "$nonce_file.out" >&2
    rm -f "$nonce_file.out"
    exit 1
}
rm -f "$nonce_file.out"

echo "p50_trace_check: canonical trace and semantic mutations passed"
