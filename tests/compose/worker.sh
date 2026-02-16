#!/usr/bin/env bash
set -euo pipefail

worker_name="${WORKER_NAME:-worker}"
netname="${ICECC_NETNAME:-composebench}"
nfiles="${ICECC_WORKER_NFILES:-200}"
jobs="${ICECC_WORKER_JOBS:-64}"
state_interval="${ICECC_STATE_INTERVAL:-1}"

out_dir="/out/${worker_name}"
mkdir -p "${out_dir}"
chmod a+rwx /out "${out_dir}" || true

echo "[${worker_name}] starting iceccd submitter (-m 4, --no-remote)"
/usr/local/sbin/iceccd -n "${netname}" -s scheduler -N "${worker_name}" --no-remote -m 4 -v -v \
  --state-jsonl "${out_dir}/iceccd.state.jsonl" --state-interval "${state_interval}" --state-log &
iceccd_pid=$!

cleanup() {
  if kill -0 "${iceccd_pid}" 2>/dev/null; then
    kill "${iceccd_pid}" 2>/dev/null || true
    wait "${iceccd_pid}" 2>/dev/null || true
  fi
}
trap cleanup EXIT

echo "[${worker_name}] waiting for scheduler control port and 4 build servers"
deadline=$((SECONDS + 120))
while (( SECONDS < deadline )); do
  if printf "listcs\nquit\n" | nc -w 1 scheduler 8766 >"${out_dir}/scheduler.listcs.txt" 2>/dev/null; then
    count=$(grep -cE '^ build[1-4] ' "${out_dir}/scheduler.listcs.txt" || true)
    if [[ "${count}" -eq 4 ]]; then
      break
    fi
  fi
  sleep 1
done
if (( SECONDS >= deadline )); then
  echo "[${worker_name}] ERROR: scheduler/build servers not ready in time" >&2
  cat "${out_dir}/scheduler.listcs.txt" 2>/dev/null || true
  exit 1
fi

echo "[${worker_name}] waiting for local iceccd to connect to scheduler"
deadline=$((SECONDS + 120))
while (( SECONDS < deadline )); do
  if /usr/local/bin/icecc --dump-daemon >"${out_dir}/daemon.internals.txt" 2>/dev/null; then
    if grep -q "Scheduler: connected" "${out_dir}/daemon.internals.txt"; then
      break
    fi
  fi
  sleep 1
done
if (( SECONDS >= deadline )); then
  echo "[${worker_name}] ERROR: local iceccd did not connect to scheduler in time" >&2
  cat "${out_dir}/daemon.internals.txt" 2>/dev/null || true
  exit 1
fi

echo "[${worker_name}] building native env"
mkdir -p /work/env
pushd /work/env >/dev/null
set +e
env_out="$(/usr/local/bin/icecc --build-native g++ 2>&1)"
rc=$?
set -e
echo "${env_out}" > "${out_dir}/build-native.txt"
if [[ $rc -ne 0 ]]; then
  echo "[${worker_name}] ERROR: icecc --build-native failed" >&2
  cat "${out_dir}/build-native.txt" >&2 || true
  exit 1
fi
env_file="$(echo "${env_out}" | awk '/^creating /{print $2}' | tail -n 1)"
if [[ -z "${env_file}" ]]; then
  env_file="$(ls -1 *.tar* 2>/dev/null | head -n 1 || true)"
fi
if [[ -z "${env_file}" || ! -f "${env_file}" ]]; then
  echo "[${worker_name}] ERROR: cannot find generated env tarball" >&2
  ls -la >&2 || true
  exit 1
fi
export ICECC_VERSION="/work/env/${env_file}"
cp -f "${env_file}" "${out_dir}/${env_file}"
popd >/dev/null

echo "[${worker_name}] generating benchmark project (${nfiles} files)"
proj="/work/bench"
rm -rf "${proj}"
mkdir -p "${proj}/src"
cat > "${proj}/src/heavy.hpp" <<'EOF'
#pragma once
#include <cstdint>
#include <utility>

template <std::size_t N>
struct SumSquares {
    static constexpr std::uint64_t value = N * N + SumSquares<N - 1>::value;
};
template <>
struct SumSquares<0> {
    static constexpr std::uint64_t value = 0;
};

template <std::size_t... Is>
constexpr std::uint64_t seq_sum(std::index_sequence<Is...>) {
    return ((Is * Is) + ... + 0ULL);
}

template <std::size_t N>
constexpr std::uint64_t heavy_value() {
    return SumSquares<N>::value + seq_sum(std::make_index_sequence<N>{});
}
EOF

for i in $(seq 1 "${nfiles}"); do
  cat > "${proj}/src/f${i}.cpp" <<EOF
#include "heavy.hpp"
extern "C" std::uint64_t f${i}() {
    return heavy_value<600>() + ${i};
}
EOF
done

cat > "${proj}/Makefile" <<'EOF'
CXX ?= g++
CXXFLAGS ?= -O2 -pipe -std=c++17

SRCS := $(wildcard src/*.cpp)
OBJS := $(patsubst %.cpp,%.o,$(SRCS))

all: $(OBJS)

%.o: %.cpp src/heavy.hpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

clean:
	rm -f $(OBJS)
EOF

echo "[${worker_name}] starting build (make -j${jobs})"
export ICECC_DEBUG=debug
export ICECC_LOGFILE="${out_dir}/icecc.log"
export PATH="/usr/local/libexec/icecc/bin:/usr/local/bin:/usr/bin:/bin"

pushd "${proj}" >/dev/null
set +e
/usr/bin/time -p make -j"${jobs}" all >"${out_dir}/build.stdout.txt" 2>"${out_dir}/build.stderr.txt"
build_rc=$?
set -e
popd >/dev/null

echo "${build_rc}" > "${out_dir}/build.exitcode.txt"
if [[ "${build_rc}" -ne 0 ]]; then
  echo "[${worker_name}] ERROR: build failed" >&2
  tail -n 200 "${out_dir}/build.stderr.txt" >&2 || true
  exit "${build_rc}"
fi

echo "[${worker_name}] build OK"
exit 0
