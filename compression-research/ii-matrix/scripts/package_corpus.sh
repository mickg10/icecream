#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 3 ]]; then
    echo "usage: $0 CELL_DIRECTORY PROJECT PROFILE" >&2
    exit 2
fi
cell=$(realpath "$1")
project=$2
profile=$3
here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)

[[ $project =~ ^[A-Za-z0-9._+-]+$ && $profile =~ ^[A-Za-z0-9._+-]+$ ]] || {
    echo "project/profile contains unsupported characters" >&2; exit 2;
}
[[ $cell != / && $cell != "$HOME" && -f $cell/raw-manifest.txt ]] || {
    echo "invalid cell or missing raw-manifest.txt: $cell" >&2; exit 2;
}
for required in environment.json commands.jsonl; do
    [[ -f $cell/$required ]] || { echo "missing $cell/$required" >&2; exit 2; }
done

stage=$cell/stage
archive=$cell/$project-$profile.ii.tar.zst
temporary=$archive.tmp
[[ ! -e $archive && ! -e $temporary ]] || { echo "archive already exists: $archive" >&2; exit 2; }
[[ ! -e $stage ]] || { echo "stage already exists: $stage" >&2; exit 2; }

python3 "$here/canonicalize_corpus.py" --raw-manifest "$cell/raw-manifest.txt" --stage "$stage" --move
cp "$cell/environment.json" "$cell/commands.jsonl" "$stage/"
[[ ! -d $cell/environment ]] || cp -a "$cell/environment" "$stage/"
[[ ! -d $cell/logs ]] || cp -a "$cell/logs" "$stage/"

tar --sort=name --mtime=@0 --owner=0 --group=0 --numeric-owner --format=posix \
    --pax-option=delete=atime,delete=ctime -C "$stage" -cf - . \
  | zstd -q -6 --long=31 -T0 -o "$temporary"
zstd -q -t --long=31 "$temporary"

verify_root=$(mktemp -d /tmp/ice-ii-verify.XXXXXXXX)
cleanup() { rm -rf -- "$verify_root"; }
trap cleanup EXIT
zstd -q -d --long=31 -c "$temporary" | tar -xf - -C "$verify_root"
python3 "$here/verify_corpus.py" "$verify_root" --manifest-out "$verify_root/manifest.txt"

mv "$temporary" "$archive"
(cd "$cell" && sha256sum "$(basename "$archive")" > "$(basename "$archive").sha256")

# These exact cell-local trees are disposable only after the archive replay above passed.
rm -rf -- "$stage"
[[ ! -d $cell/loose ]] || rm -rf -- "$cell/loose"
printf 'archive=%s\n' "$archive"
printf 'bytes=%s\n' "$(stat -c%s "$archive")"
cat "$archive.sha256"
