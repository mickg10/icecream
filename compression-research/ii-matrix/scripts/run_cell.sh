#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 4 ]]; then
    echo "usage: $0 PROFILE PROJECT SOURCE_CHECKOUT ADAPTER_NAME" >&2
    exit 2
fi
profile=$1
project=$2
source_checkout=$(realpath "$3")
adapter_name=$4
here=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
matrix_root=${II_MATRIX_ROOT:-/home/ttuser/ictmp/ii-matrix}
jobs=${JOBS:-24}

profile_row=$(awk -F '\t' -v wanted="$profile" 'NR>1 && $1==wanted {print; found=1} END{if(!found)exit 1}' "$here/profiles.tsv") || {
    echo "unknown profile: $profile" >&2; exit 2;
}
IFS=$'\t' read -r _ image source_prefix _compiler _dependency_mode _run_user <<< "$profile_row"
adapter=$here/adapters/$adapter_name
[[ -f $adapter ]] || { echo "missing adapter: $adapter" >&2; exit 2; }
[[ -d $source_checkout ]] || { echo "missing source checkout: $source_checkout" >&2; exit 2; }

cell=$matrix_root/$project/$profile
archive=$cell/$project-$profile.ii.tar.zst
[[ ! -e $archive ]] || { echo "cell archive already complete: $archive" >&2; exit 2; }
mkdir -p "$cell/build" "$cell/loose" "$cell/logs"
chmod ugo+rwx "$cell" "$cell/build" "$cell/loose" "$cell/logs"
image_id=$(docker image inspect --format '{{.Id}}' "$image")

docker run --rm \
    --mount "type=bind,src=$source_checkout,dst=$source_prefix" \
    --mount "type=bind,src=$cell,dst=/cell" \
    --mount "type=bind,src=$here,dst=/harness,readonly" \
    --env "SOURCE_ROOT=$source_prefix" --env BUILD_ROOT=/cell/build \
    --env LOOSE_ROOT=/cell/loose --env CELL_ROOT=/cell --env "JOBS=$jobs" \
    --env "MATRIX_PROJECT=$project" --env "MATRIX_PROFILE=$profile" \
    "$image" bash -lc \
      "bash /harness/adapters/$adapter_name && python3 /harness/scripts/capture_environment.py --output /cell/environment.json --project '$project' --profile '$profile' --source-root '$source_prefix' --build-root /cell/build --image-id '$image_id'" \
    2>&1 | tee "$cell/logs/container.log"

"$here/scripts/package_corpus.sh" "$cell" "$project" "$profile"
