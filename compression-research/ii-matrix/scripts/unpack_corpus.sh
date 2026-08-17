#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 2 ]]; then
    echo "usage: $0 ARCHIVE OUTPUT_DIRECTORY" >&2
    exit 2
fi
archive=$(realpath "$1")
output=$2
here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)

[[ -f $archive ]] || { echo "missing archive: $archive" >&2; exit 2; }
if [[ -e $output ]]; then
    [[ -d $output && -z $(find "$output" -mindepth 1 -maxdepth 1 -print -quit) ]] || {
        echo "refusing nonempty output: $output" >&2; exit 2;
    }
else
    mkdir -p "$output"
fi
output=$(realpath "$output")

if [[ -f $archive.sha256 ]]; then
    (cd "$(dirname "$archive")" && sha256sum -c "$(basename "$archive").sha256")
fi
zstd -q -d --long=31 -c "$archive" | tar -xf - -C "$output"
python3 "$here/verify_corpus.py" "$output"
