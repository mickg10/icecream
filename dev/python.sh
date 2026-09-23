#!/bin/sh
# One locked Python environment for tools, tests and child processes.
set -eu

source_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd -P)
sh "$source_root/farmharness/integration/check_scratch.sh"
export TMPDIR="$ICEFARM_TMPDIR" TMP="$ICEFARM_TMPDIR"
export TEMP="$ICEFARM_TMPDIR" TEMPDIR="$ICEFARM_TMPDIR"
export PYTHONDONTWRITEBYTECODE=1
export PYTHONNOUSERSITE=1
unset PYTHONHOME

if [ "${1-}" = --exec ] && [ "$#" -lt 2 ]; then
    echo 'usage: sh dev/python.sh --exec COMMAND [ARG ...]' >&2
    exit 2
fi

if ! command -v uv >/dev/null 2>&1; then
    echo 'uv 0.9.21 is required; install it before running make python-sync' >&2
    exit 2
fi

# Cache/downloads are shared within the selected scratch root; environments
# are checkout-specific so syncing another worktree cannot change this one.
checkout_id=$(printf '%s' "$source_root" | cksum | awk '{print $1}')
uv_root="$ICEFARM_TMPDIR/icecream-uv-$(id -u)"
export UV_CACHE_DIR="${UV_CACHE_DIR:-$uv_root/cache}"
export UV_PYTHON_INSTALL_DIR="${UV_PYTHON_INSTALL_DIR:-$uv_root/python}"
export UV_PROJECT_ENVIRONMENT="${UV_PROJECT_ENVIRONMENT:-$uv_root/env-$checkout_id}"
for path in "$UV_CACHE_DIR" "$UV_PYTHON_INSTALL_DIR" "$UV_PROJECT_ENVIRONMENT"; do
    case "$path" in
        */../*|*/..) echo "uv storage paths must not contain '..': $path" >&2; exit 2 ;;
        /*) ;;
        *) echo "uv storage paths must be absolute: $path" >&2; exit 2 ;;
    esac
    if [ "$path" = / ]; then
        echo 'uv storage paths must not be the filesystem root' >&2
        exit 2
    fi
    if [ -d "$path" ] && [ "$(CDPATH= cd -- "$path" && pwd -P)" = / ]; then
        echo 'uv storage paths must not resolve to the filesystem root' >&2
        exit 2
    fi
done

# --locked rejects stale metadata rather than silently updating uv.lock.
# --managed-python prevents accidentally selecting a host/Conda interpreter.
python_version=$(cat "$source_root/.python-version")
uv sync --locked --managed-python --python "$python_version" --project "$source_root"
export VIRTUAL_ENV="$UV_PROJECT_ENVIRONMENT"
export PATH="$VIRTUAL_ENV/bin:$PATH"
export PYTHONPATH="$source_root"

case "${1-}" in
    --sync) shift; test "$#" -eq 0; exit 0 ;;
    --exec) shift; exec "$@" ;;
    *) exec "$VIRTUAL_ENV/bin/python" "$@" ;;
esac
