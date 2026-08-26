#!/bin/sh
# Deletion-sensitive gate for the bounded S2 restart/replay exit precursor.
set -eu

src=${ICECC_TEST_TOP_SRCDIR:-$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)}
test_file="$src/unittests/p50_s2_restart_replay_test.cpp"
doc="$src/cache/P50_S2_RESTART_REPLAY.md"

gate() {
    candidate=$1
    missing=0
    for gate_pattern in \
        'void case_before_ready()' \
        'void case_after_ready_before_handoff()' \
        'void case_restart_budget_exhaustion()' \
        'void case_lease_withdrawal_rotation()' \
        'void case_phase_replay()' \
        'void case_adoption_lost_ack_and_reverse_replay()' \
        'pre_ready_deaths' \
        'post_ready_deaths' \
        'lease_withdrawals' \
        'phase_replays' \
        'phase_conflicts' \
        'reverse_replays' \
        'reverse_conflicts' \
        'F_DUPFD_CLOEXEC' \
        'CHECK(ledger.transition_count() == 1' \
        'ledger.fork_count() == 1' \
        'compile_cursor_takes' \
        'HOLD real_C/F_CompileFile_wiring' \
        'HOLD wire_lost_ACK_process_kill'; do
        if ! grep -F "$gate_pattern" "$candidate" >/dev/null; then
            missing=1
        fi
    done
    return "$missing"
}

test -f "$test_file" && test -f "$doc"
gate "$test_file"
grep -F 'p50s2restartreplay' "$src/unittests/Makefile.am" >/dev/null
grep -F 'p50_s2_restart_replay_sanitize.sh' "$src/unittests/Makefile.am" >/dev/null
grep -F 'P50_S2_RESTART_REPLAY.md' "$src/cache/Makefile.am" >/dev/null
grep -F 'CompileFile wiring' "$doc" >/dev/null
grep -F 'product C/F' "$doc" >/dev/null

mutant_dir=$(mktemp -d "${TMPDIR:-/tmp}/p50s2restartreplay-mutants.XXXXXX")
trap 'rm -rf "$mutant_dir"' EXIT HUP INT TERM
for pattern in \
    'void case_before_ready()' \
    'void case_after_ready_before_handoff()' \
    'void case_restart_budget_exhaustion()' \
    'void case_lease_withdrawal_rotation()' \
    'void case_phase_replay()' \
    'void case_adoption_lost_ack_and_reverse_replay()' \
    'CHECK(ledger.transition_count() == 1' \
    'CHECK(ledger.transition_count() == 1'; do
    mutant="$mutant_dir/${pattern##*/}.cpp"
    grep -vF "$pattern" "$test_file" >"$mutant"
    if gate "$mutant"; then
        echo "FAIL: S2 deletion mutant survived: $pattern" >&2
        exit 1
    fi
done
echo 'ok - bounded S2 restart/replay source mutants are rejected'
