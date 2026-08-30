#!/bin/sh
# Regression for the compile-db eval seam: assignments must reach the
# external client, not merely the shell function that invokes it.
set -eu
work=$(mktemp -d "${TMPDIR:-/tmp}/p50argv-env.XXXXXX")
trap 'rm -rf "$work"' EXIT HUP INT TERM
mock=$work/client
envout=$work/env
cat >"$mock" <<'EOF'
#!/bin/sh
env | grep -E '^(ICECC_TEST_SOCKET|ICECC_TEST_REMOTEBUILD|ICECC_VERSION|ICECC_P50_C1F1_REQUIRED|ICECC_P50_C1F1_TIMEOUT|ICECC_PREFERRED_HOST|ICECC_DEBUG|ICECC_LOGFILE)=' | sort >"$P50_ENV_OUT"
EOF
chmod 755 "$mock"
export P50_ENV_OUT="$envout"
run_client_with_timeout() { "$@"; }
remote_compile_args=$(python3 -c 'import shlex,sys; print(shlex.join([sys.argv[1], "-c", "source.cc"]))' "$mock")
ICECC_TEST_SOCKET=socket
ICECC_TEST_REMOTEBUILD=1
ICECC_VERSION=env.tar.gz
ICECC_P50_C1F1_REQUIRED=1
ICECC_P50_C1F1_TIMEOUT=3600
ICECC_PREFERRED_HOST=p50-f
ICECC_DEBUG=debug
ICECC_LOGFILE=client.log
export ICECC_TEST_SOCKET ICECC_TEST_REMOTEBUILD ICECC_VERSION \
    ICECC_P50_C1F1_REQUIRED ICECC_P50_C1F1_TIMEOUT ICECC_PREFERRED_HOST ICECC_DEBUG ICECC_LOGFILE
eval "run_client_with_timeout $remote_compile_args"
test "$(wc -l <"$envout")" -eq 8
grep -Fx 'ICECC_TEST_SOCKET=socket' "$envout"
grep -Fx 'ICECC_TEST_REMOTEBUILD=1' "$envout"
grep -Fx 'ICECC_VERSION=env.tar.gz' "$envout"
grep -Fx 'ICECC_P50_C1F1_REQUIRED=1' "$envout"
grep -Fx 'ICECC_P50_C1F1_TIMEOUT=3600' "$envout"
grep -Fx 'ICECC_PREFERRED_HOST=p50-f' "$envout"
grep -Fx 'ICECC_DEBUG=debug' "$envout"
grep -Fx 'ICECC_LOGFILE=client.log' "$envout"
echo 'ok - compile-db argv eval exports client environment'

python3 - "$(dirname "$0")/p50compilee2e-run.sh" <<'PY'
import pathlib
import sys

text = pathlib.Path(sys.argv[1]).read_text(encoding="utf-8")
assert "tokens.append('-gno-record-gcc-switches')" in text
assert "'-grecord-gcc-switches', '-gno-record-gcc-switches'" in text
PY
echo 'ok - compile-db debug argv preserves DWARF with deterministic producer notes'
