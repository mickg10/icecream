#!/usr/bin/env bash
set -uo pipefail

usage() {
    echo "usage: run-qa.sh bootstrap|qa [jobs (default: 2)]" >&2
}

MODE=${1:-}
JOBS=${2:-${JOBS:-2}}
if [[ $# -gt 2 || ( "$MODE" != bootstrap && "$MODE" != qa ) ]]; then
    usage
    exit 2
fi
case "$JOBS" in
    ''|*[!0-9]*) echo "jobs must be an integer from 1 to 8" >&2; exit 2 ;;
esac
if (( JOBS < 1 || JOBS > 8 )); then
    echo "jobs must be an integer from 1 to 8" >&2
    exit 2
fi

SOURCE_MOUNT=/source
WORK_ROOT=/work
if [[ ! -d "$SOURCE_MOUNT" || ! -r "$SOURCE_MOUNT" ]]; then
    echo "required read-only source mount /source is missing or unreadable" >&2
    exit 2
fi
source_mount_options=$(findmnt -n -o OPTIONS --target "$SOURCE_MOUNT" 2>/dev/null || true)
case ",$source_mount_options," in
    *,ro,*) ;;
    *) echo "source snapshot at /source must be mounted read-only" >&2; exit 2 ;;
esac
if [[ ! -d "$WORK_ROOT" || ! -w "$WORK_ROOT" ]]; then
    echo "required writable scratch mount /work is missing or unwritable; no fallback is used" >&2
    exit 2
fi

# Native checks may need root inside Docker, but their retained output must
# remain removable by the host user. Never follow symlinks outside this mount.
if [[ -n "${ICEFARM_OUTPUT_UID:-}" || -n "${ICEFARM_OUTPUT_GID:-}" ]]; then
    if [[ ! "${ICEFARM_OUTPUT_UID:-}" =~ ^[0-9]+$ || ! "${ICEFARM_OUTPUT_GID:-}" =~ ^[0-9]+$ ]]; then
        echo "output ownership requires numeric UID and GID" >&2
        exit 2
    fi
    trap 'result=$?; chown -R --no-dereference "$ICEFARM_OUTPUT_UID:$ICEFARM_OUTPUT_GID" "$WORK_ROOT" || result=1; exit "$result"' EXIT
fi

SRC="$WORK_ROOT/source"
BUILD="$WORK_ROOT/build"
INSTALL="$WORK_ROOT/install"
TMP="$WORK_ROOT/tmp"
ARTIFACTS="$WORK_ROOT/artifacts"
for path in "$SRC" "$BUILD" "$INSTALL" "$TMP" "$ARTIFACTS" \
    "$WORK_ROOT/uv-cache" "$WORK_ROOT/python-env"; do
    if [[ -L "$path" ]]; then
        echo "refusing symlinked work path: $path" >&2
        exit 2
    fi
done
mkdir -p "$TMP" "$ARTIFACTS" || exit 2
if [[ ! -w "$TMP" || ! -w "$ARTIFACTS" ]]; then
    echo "/work/tmp and /work/artifacts must be writable" >&2
    exit 2
fi

# Unix socket paths have a small fixed length limit. /tmp is a short alias,
# not fallback storage: the wrapper must bind the very same owned scratch
# directory at both /work/tmp and /tmp.
if [[ $(stat -c '%d:%i' "$TMP") != $(stat -c '%d:%i' /tmp) ]]; then
    echo "bind /work/tmp at /tmp too; temporary objects must stay in the selected scratch directory" >&2
    exit 2
fi
TMP=/tmp

export TMPDIR="$TMP" TMP="$TMP" TEMP="$TMP" TEMPDIR="$TMP"
export ICEFARM_TMPDIR="$TMP"
export PYTHONDONTWRITEBYTECODE=1
export PYTHONPYCACHEPREFIX="$TMP/python-cache"
export UV_PYTHON_INSTALL_DIR=/opt/uv-python
export UV_CACHE_DIR="$WORK_ROOT/uv-cache"
export UV_PROJECT_ENVIRONMENT="$WORK_ROOT/python-env"
export VIRTUAL_ENV="$UV_PROJECT_ENVIRONMENT"
export PATH="$VIRTUAL_ENV/bin:$PATH"
export UV_OFFLINE=1
export UV_PYTHON_DOWNLOADS=never

if [[ ! -e "$SRC" ]]; then
    stage_dir=$(mktemp -d "$WORK_ROOT/.source.staging.XXXXXX") || exit 2
    if ! cp -a "$SOURCE_MOUNT"/. "$stage_dir"/; then
        echo "failed to stage source snapshot from /source; partial copy: $stage_dir" >&2
        exit 2
    fi
    if ! mv "$stage_dir" "$SRC"; then
        echo "failed to publish staged source snapshot at $SRC; copy remains at $stage_dir" >&2
        exit 2
    fi
elif [[ ! -d "$SRC" ]]; then
    echo "/work/source already exists and is not a directory" >&2
    exit 2
else
    echo "Using existing staged snapshot at $SRC (use a fresh /work for another snapshot)."
fi

# The legacy product source (the mixed-version QA baseline) predates the
# Python tooling and is only built natively.
native_only=0
if [[ "$MODE" == bootstrap && ! -e "$SRC/pyproject.toml" ]]; then
    native_only=1
fi

# The managed Python environment was resolved from this exact metadata during
# SDK image construction. Refuse to run a source snapshot with a different
# lockfile or interpreter pin; rebuilding the SDK is required instead.
SDK_METADATA_ROOT=/opt/icecream-python-src
for metadata in pyproject.toml uv.lock .python-version; do
    (( native_only )) && break
    if [[ ! -f "$SRC/$metadata" || ! -f "$SDK_METADATA_ROOT/$metadata" ]] ||
       ! cmp -s "$SRC/$metadata" "$SDK_METADATA_ROOT/$metadata"; then
        echo "SDK Python metadata mismatch for $metadata; rebuild the SDK from this source snapshot" >&2
        exit 2
    fi
done
mkdir -p "$BUILD" "$INSTALL" || exit 2
mkdir -p "$WORK_ROOT/uv-cache" "$WORK_ROOT/python-env" || exit 2
if [[ -d /opt/uv-cache ]]; then
    cp -a /opt/uv-cache/. "$WORK_ROOT/uv-cache/" || exit 2
fi

# Match a normal developer checkout. Several P50 tests intentionally require
# a non-root service identity. Root remains only the container orchestrator.
run_as_user=
if [[ $(id -u) == 0 ]]; then
    run_as_user=nobody
    chown -R --no-dereference "$(id -u nobody):$(id -g nobody)" \
        "$SRC" "$BUILD" "$INSTALL" "$TMP" "$ARTIFACTS" \
        "$WORK_ROOT/uv-cache" "$WORK_ROOT/python-env" || exit 2
fi

autogen_status=null
configure_status=null
build_status=null
install_status=null
python_sync_status=null
native_check_status=null
native_root_check_status=null
pytest_status=null

run_stage() {
    local name=$1; shift
    local logfile="$ARTIFACTS/$name.log"
    local status
    local -a command=("$@")
    if [[ -n "$run_as_user" && "$name" != native-root-check ]]; then
        command=(runuser -u "$run_as_user" -- "${command[@]}")
    fi
    printf '\n== %s ==\n' "$name"
    if "${command[@]}" >"$logfile" 2>&1; then
        status=0
    else
        status=$?
    fi
    cat "$logfile"
    printf '\n[%s exit=%s; log=%s]\n' "$name" "$status" "$logfile"
    case "$name" in
        autogen) autogen_status=$status ;;
        configure) configure_status=$status ;;
        build) build_status=$status ;;
        install) install_status=$status ;;
        python-sync) python_sync_status=$status ;;
        native-check) native_check_status=$status ;;
        native-root-check) native_root_check_status=$status ;;
        python-pytest) pytest_status=$status ;;
    esac
    return 0
}

# Resolve the locked Python environment as the same unprivileged identity used
# for source and native QA. This is required in both bootstrap and QA modes
# (except for the native-only legacy baseline); no prebuilt root-owned
# environment is used as a fallback.
(( native_only )) ||
    run_stage python-sync bash -c 'cd "$1" && uv sync --locked --offline --managed-python --python "$(cat .python-version)"' _ "$SRC"

prepare_build() {
    run_stage autogen bash -c 'cd "$1" && ./autogen.sh' _ "$SRC"
    (( autogen_status == 0 )) || return 1
    run_stage configure bash -c 'cd "$1" && "$2/configure" --prefix="$3" --sbindir="$3/sbin" --without-man' _ "$BUILD" "$SRC" "$INSTALL"
    (( configure_status == 0 )) || return 1
    run_stage build make -C "$BUILD" -j"$JOBS" all
    (( build_status == 0 )) || return 1
    return 0
}

build_ok=0
if prepare_build; then
    build_ok=1
fi

if [[ "$MODE" == bootstrap ]]; then
    if (( build_ok )); then
        run_stage install make -C "$BUILD" install
    fi
else
    if (( build_ok && python_sync_status == 0 )); then
        run_stage install make -C "$BUILD" install
        run_stage native-check make -C "$BUILD" -j"$JOBS" check
        if [[ -n "$run_as_user" && "$native_check_status" == 0 ]]; then
            # Also exercise the service's explicit root-to-user transition,
            # including leak detection, rather than omitting that branch.
            run_stage native-root-check make -C "$BUILD/unittests" -j"$JOBS" \
                check TESTS="p50cacheservice p50cacheservice-sanitize.sh"
        fi
    else
        echo "Native make check not run because build or offline uv sync failed; see stage logs." >&2
    fi

    # This suite is independent of native compilation and still runs on failure.
    if [[ "$python_sync_status" == 0 ]]; then
        run_stage python-pytest bash -c 'cd "$1" && command -v python3 && python3 -B -m pytest -q -p no:cacheprovider --junitxml="$2/pytest.xml" farmharness/integration/tests' _ "$SRC" "$ARTIFACTS"
    else
        echo "Python QA not run because offline uv sync failed; see python-sync stage log." >&2
    fi
fi

overall=0
for status in "$autogen_status" "$configure_status" "$build_status"; do
    [[ "$status" == 0 ]] || overall=1
done
if [[ "$MODE" == bootstrap ]]; then
    [[ "$install_status" == 0 ]] || overall=1
    (( native_only )) || [[ "$python_sync_status" == 0 ]] || overall=1
else
    [[ "$install_status" == 0 ]] || overall=1
    [[ "$native_check_status" == 0 && "$build_ok" == 1 ]] || overall=1
    [[ -z "$run_as_user" || "$native_root_check_status" == 0 ]] || overall=1
    [[ "$pytest_status" == 0 ]] || overall=1
fi

summary_tmp="$ARTIFACTS/summary.json.tmp"
printf '{"mode":"%s","jobs":%s,"autogen_exit":%s,"configure_exit":%s,"build_exit":%s,"install_exit":%s,"python_sync_exit":%s,"native_check_exit":%s,"native_root_check_exit":%s,"pytest_exit":%s,"overall_exit":%s}\n' \
    "$MODE" "$JOBS" "$autogen_status" "$configure_status" "$build_status" \
    "$install_status" "$python_sync_status" "$native_check_status" "$native_root_check_status" "$pytest_status" "$overall" > "$summary_tmp"
mv -f "$summary_tmp" "$ARTIFACTS/summary.json"
printf '\nSummary: %s/summary.json\nArtifacts: %s\n' "$ARTIFACTS" "$ARTIFACTS"
exit "$overall"
