#!/bin/sh
set -eu

if [ "$#" -ne 7 ]; then
    echo "usage: acceptance.sh SOURCE BUILD_DIR RESULT_DIR SCHEDULER ENVIRONMENT RUN_ID WORKER_ENDPOINTS" >&2
    exit 64
fi

source_file=$1
build_dir=$2
result_dir=$3
scheduler=$4
environment=$5
run_id=$6
worker_endpoints=$7

test -r "$source_file"
mkdir -p "$build_dir" "$result_dir"

object_file="$build_dir/tiny.o"
executable_file="$build_dir/tiny"
compile_log="$result_dir/acceptance-icecc.log"
compiler_version_file="$result_dir/compiler-version.txt"
program_output_file="$result_dir/program-output.txt"
provenance_file="$result_dir/acceptance-provenance.tsv"

: >"$compile_log"
"${CXX:-c++}" --version >"$compiler_version_file"

export ICECC_DEBUG=debug
export ICECC_LOGFILE="$compile_log"
export ICECC_SCHEDULER="$scheduler"
export ICECC_TEST_REMOTEBUILD=1

/opt/icecream/bin/icecc "${CXX:-c++}" -std=c++17 -O2 -Wall -Wextra -Werror \
    -c "$source_file" -o "$object_file"
"${CXX:-c++}" "$object_file" -o "$executable_file"
"$executable_file" >"$program_output_file"

expected='icecream-farm-ok 42'
actual=$(tr -d '\r\n' <"$program_output_file")
test "$actual" = "$expected"
test -s "$object_file"
test -x "$executable_file"

# A successful local fallback is not farm acceptance. The client trace must contain
# the remote assignment and its scheduler job identity.
grep -Eq 'Have to use host .* - Job ID: [0-9]+' "$compile_log"
job_line=$(grep -E 'Have to use host .* - Job ID: [0-9]+' "$compile_log" | tail -n 1)
matched_endpoint=
old_ifs=$IFS
IFS=,
for endpoint in $worker_endpoints; do
    case "$job_line" in
        *"Have to use host $endpoint - Job ID: "*)
            matched_endpoint=$endpoint
            break
            ;;
    esac
done
IFS=$old_ifs
test -n "$matched_endpoint"

{
    printf 'run_id\t%s\n' "$run_id"
    printf 'environment_class\t%s\n' "$environment"
    printf 'scheduler\t%s\n' "$scheduler"
    printf 'allowed_worker_endpoints\t%s\n' "$worker_endpoints"
    printf 'selected_worker_endpoint\t%s\n' "$matched_endpoint"
    printf 'compiler\t%s\n' "${CXX:-c++}"
    printf 'compiler_version\t%s\n' "$(head -n 1 "$compiler_version_file")"
    printf 'source_sha256\t%s\n' "$(sha256sum "$source_file" | cut -d' ' -f1)"
    printf 'object_path\t%s\n' "$object_file"
    printf 'object_sha256\t%s\n' "$(sha256sum "$object_file" | cut -d' ' -f1)"
    printf 'executable_path\t%s\n' "$executable_file"
    printf 'executable_sha256\t%s\n' "$(sha256sum "$executable_file" | cut -d' ' -f1)"
    printf 'object_description\t%s\n' "$(file -b "$object_file")"
    printf 'completion_identity\t%s\n' "$job_line"
    printf 'program_output\t%s\n' "$actual"
} >"$provenance_file"

printf '%s\n' "$provenance_file"
