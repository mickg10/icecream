#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"

run_id="${ICECC_RUN_ID:-$(date -u +%Y%m%dT%H%M%SZ)-$$}"
out_root="${ICECC_OUT_DIR:-${script_dir}/out/${run_id}}"
mkdir -p "${out_root}"
chmod a+rwx "${out_root}" || true

export ICECC_OUT_DIR="${out_root}"
export ICECC_NETNAME="${ICECC_NETNAME:-composebench}"
export ICECC_WORKER_NFILES="${ICECC_WORKER_NFILES:-200}"
export ICECC_WORKER_JOBS="${ICECC_WORKER_JOBS:-64}"
export ICECC_STATE_INTERVAL="${ICECC_STATE_INTERVAL:-1}"

# Avoid collisions/leaked networks on repeated runs.
project_safe="$(printf '%s' "icecc_${run_id}" | tr -c 'a-zA-Z0-9' '_' | tr 'A-Z' 'a-z')"
export COMPOSE_PROJECT_NAME="${COMPOSE_PROJECT_NAME:-${project_safe}}"

cd "${script_dir}"

echo "== build image"
docker compose build

echo "== start scheduler + build servers"
docker compose up -d scheduler build1 build2 build3 build4

echo "== wait for build servers to register"
deadline=$((SECONDS + 120))
while (( SECONDS < deadline )); do
  if docker compose exec -T scheduler bash -lc 'printf "listcs\nquit\n" | nc -w 1 localhost 8766' >"${out_root}/scheduler.listcs.txt" 2>/dev/null; then
    count=$(grep -cE '^ build[1-4] ' "${out_root}/scheduler.listcs.txt" || true)
    if [[ "${count}" -eq 4 ]]; then
      break
    fi
  fi
  sleep 1
done
if (( SECONDS >= deadline )); then
  echo "ERROR: build servers not ready; scheduler.listcs.txt:" >&2
  cat "${out_root}/scheduler.listcs.txt" >&2 || true
  docker compose logs --no-color >"${out_root}/compose.log" 2>/dev/null || true
  docker compose down -v || true
  exit 1
fi

echo "== run workers (parallel)"
set +e
docker compose run --rm worker1 &
p1=$!
docker compose run --rm worker2 &
p2=$!
wait "${p1}"
rc1=$?
wait "${p2}"
rc2=$?
set -e

echo "== collect logs"
docker compose logs --no-color >"${out_root}/compose.log" 2>/dev/null || true

echo "== scheduler snapshot"
docker compose exec -T scheduler bash -lc 'printf "listcs\nlistrequests\nlistjobs verbose\nquit\n" | nc -w 1 localhost 8766' >"${out_root}/scheduler.snapshot.txt" 2>/dev/null || true

echo "== shutdown"
docker compose down -v --remove-orphans || true

echo "== verify"
python3 "${script_dir}/verify.py" "${out_root}"

if [[ "${rc1}" -ne 0 || "${rc2}" -ne 0 ]]; then
  echo "ERROR: worker exit codes: worker1=${rc1} worker2=${rc2}" >&2
  exit 1
fi

echo "OK: ${out_root}"
