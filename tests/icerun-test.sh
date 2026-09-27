#! /bin/bash

dir="$1"
num="$2"
gate_dir="$3"
test -z "$dir" -o -z "$num" && exit 1

touch "$dir"/running$num
if test -n "$gate_dir"; then
    touch "$gate_dir"/ready$num
    waited=0
    while test ! -e "$gate_dir"/release$num && test ! -e "$gate_dir"/release-all; do
        sleep 0.1
        waited=$((waited + 1))
        if test "$waited" -ge 300; then
            echo "timed out waiting for local slot release ($num)" >&2
            rm -f "$dir"/running$num
            exit 98
        fi
    done
elif test -z "$ICERUN_TEST_VALGRIND"; then
    sleep 0.2
else
    sleep 1
fi
rm "$dir"/running$num
touch "$dir"/done$num
exit 0
