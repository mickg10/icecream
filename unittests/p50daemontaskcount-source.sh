#!/bin/sh
set -eu

src=${ICECC_TEST_TOP_SRCDIR:-$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)}
serve=$src/daemon/serve.cpp
counter=$src/daemon/p50_task_count.cpp

check_fork_guard()
{
    awk '
        /flush_debug\(\);/ { flushed = NR }
        /const auto task_count = iceccd_task_count\(\);/ { counted = NR }
        /if \(!task_count.has_value\(\) \|\| \*task_count != 1\)/ { refused = NR }
        /iceccd compile-worker fork failed/ { fork_failure = NR }
        /pid_t pid = fork\(\);/ { forked = NR }
        END {
            if (!flushed || !counted || !refused || !forked || !fork_failure ||
                    !(flushed < counted && counted < refused && refused < forked &&
                      forked < fork_failure))
                exit 1
        }
    ' "$1"
}

grep -F '#include "p50_task_count.h"' "$serve" >/dev/null
grep -F 'const auto task_count = iceccd_task_count();' "$serve" >/dev/null
grep -F 'iceccd compile fork refused: process task count' "$serve" >/dev/null
grep -F 'flush_debug();' "$serve" >/dev/null
grep -F 'errno = 0;' "$counter" >/dev/null
grep -F 'scan_failed = errno != 0;' "$counter" >/dev/null

check_fork_guard "$serve"

# Prove that the source gate turns red when the exact refusal predicate is
# neutralized while all of the old count/log/fork anchors remain present.
mutant=$(mktemp "${TMPDIR:-/tmp}/p50daemontaskcount.XXXXXX")
trap 'rm -f "$mutant"' EXIT HUP INT TERM
sed 's/if (!task_count.has_value() || \*task_count != 1)/if (false)/' \
    "$serve" >"$mutant"
if check_fork_guard "$mutant"; then
    echo 'FAIL: deletion mutant neutralized the fork refusal without detection' >&2
    exit 1
fi

thread_source_found=false
for source in "$src"/daemon/*; do
    case $source in
        *.cpp|*.cc|*.c|*.h|*.hpp) ;;
        *) continue ;;
    esac
    if grep -nE \
            'std::thread|std::jthread|std::async|std::future|packaged_task|pthread_create|pthread_atfork' \
            "$source"; then
        thread_source_found=true
    fi
done
if "$thread_source_found"; then
    echo 'FAIL: iceccd daemon source creates or repairs around a thread' >&2
    exit 1
fi

grep -F 'hidden daemon thread mutant was not observable at fork audit' \
    "$src/unittests/p50_daemon_task_count_test.cpp" >/dev/null

echo 'PASS: every compile-worker fork is guarded by the one-task iceccd audit'
