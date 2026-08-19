#!/usr/bin/env bash
# Focused whole-TU transaction gate.  The codec admits the target TU globally once, rejects
# its first complete per-F attempt before close, proves every committed route-local plane was
# restored, retransmits the exact captured bytes, commits F, loses the Ack, and recovers the
# retained receipt without a second F application.
set -Eeuo pipefail

HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
BIN=${BIN:-$HERE/build/codec50-sink}
MAN=${1:?usage: selector_transaction_retry.sh MANIFEST}
WORK=${WORK:-$(mktemp -d /tmp/p29-txn-retry.XXXXXX)}
TX_TU=${TX_TU:-3}
EXPECT_BLOB_MODE=${EXPECT_BLOB_MODE:-}
EXPECT_MO_PENDING=${EXPECT_MO_PENDING:-}

fail(){
  echo "TRANSACTION RETRY FAIL: $*" >&2
  echo "  retained: $WORK" >&2
  exit 1
}
trap 'rc=$?; [ "$rc" -eq 0 ] || fail "aborted with status $rc at line $LINENO"' ERR

[ -x "$BIN" ] || fail "codec binary not executable: $BIN"
[ -s "$MAN" ] || fail "manifest is empty or missing: $MAN"
mkdir -p "$WORK"
head -n "$((TX_TU+1))" "$MAN" >"$WORK/manifest"
[ "$(wc -l <"$WORK/manifest")" -gt "$TX_TU" ] || fail "manifest has no zero-based TU $TX_TU"

BASE=(--z 3 --mixed-regions --byte-array-lines --direct-ordinals --compressed-blobs
      --blob-threads 8 --blob-lazy-fallback --mo-factor --s1-max-chain 1024
      --blob-z 9 --blob-zstd-workers 4 --blob-zstd-job-mib 5 --blob-zstd-overlap-log 3
      --stable-root-tags --literal-ondemand --literal-group-skip-zstd10 --route-s1 1
      --transactional-tu --selftest-transaction-reject-once "$TX_TU")

rc=0
"$BIN" --manifest "$WORK/manifest" "${BASE[@]}" \
  --cf-sink "$WORK/retry.cf" --fc-sink "$WORK/retry.fc" \
  >"$WORK/out" 2>"$WORK/err" || rc=$?
[ "$rc" -eq 0 ] || fail "codec exited $rc"
grep -qF "transaction reject/retry selftest: PASS TU=$TX_TU" "$WORK/out" || fail "PASS record is absent"
if [ -n "$EXPECT_BLOB_MODE" ] &&
   ! grep -qE "blob_mode=$EXPECT_BLOB_MODE([[:space:]]|$)" "$WORK/out"; then
  fail "target TU did not use required blob mode $EXPECT_BLOB_MODE"
fi
if [ -n "$EXPECT_MO_PENDING" ]; then
  pending=$(sed -nE 's/.*mo_pending=([0-9]+).*/\1/p' "$WORK/out" | tail -1)
  [ -n "$pending" ] || fail "target TU did not report its MO pending-definition count"
  [ "$pending" -ge "$EXPECT_MO_PENDING" ] ||
    fail "target TU reported $pending MO definitions, expected at least $EXPECT_MO_PENDING"
fi
[ -s "$WORK/retry.cf" ] || fail "C-to-F retry stream is empty"
[ -s "$WORK/retry.fc" ] || fail "F-to-C retry stream is empty"

echo "transaction reject/retry: PASS (TU=$TX_TU, retained=$WORK)"
