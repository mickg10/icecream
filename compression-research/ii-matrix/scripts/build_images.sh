#!/usr/bin/env bash
set -euo pipefail

here=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
lock=${1:-$here/profiles.lock.tsv}
tmp="$lock.tmp"

printf 'profile\timage\timage_id\tbuilt_utc\n' > "$tmp"
while IFS=$'\t' read -r profile image _source_prefix _compiler _dependency_mode _run_user; do
    [[ $profile == profile ]] && continue
    docker build --pull -f "$here/images/$profile.Dockerfile" -t "$image" "$here"
    image_id=$(docker image inspect --format '{{.Id}}' "$image")
    built_utc=$(date -u +%Y-%m-%dT%H:%M:%SZ)
    printf '%s\t%s\t%s\t%s\n' "$profile" "$image" "$image_id" "$built_utc" >> "$tmp"
done < "$here/profiles.tsv"

mv "$tmp" "$lock"
printf 'wrote %s\n' "$lock"
