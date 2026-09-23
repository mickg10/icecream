# Icecream docker-compose benchmark test

This directory contains a reproducible docker-compose setup for benchmarking and debugging Icecream scheduling/throughput.

It spins up **7 containers**:

- 1 scheduler (`icecc-scheduler`)
- 4 build servers (`iceccd -m 4`)
- 2 workers/submitters (`iceccd --no-remote -m 4`) that run a synthetic C++ build

The workers generate a project with many translation units and compile them with `make -j…` using Icecream wrappers, while periodic daemon state telemetry is emitted:

- `--state-jsonl` writes a JSONL file to the shared `/out` volume
- `--state-log` prints the same JSON lines to the daemon log output

## Run

From the repo root:

```bash
ICEFARM_TMPDIR=/path/to/existing/scratch ./tests/compose/run.sh
```

Prerequisites are uv 0.9.21, Docker Compose v2, a Linux host with enough scratch space
for seven containers, and the compose context from this checkout. This is a
local benchmark/debug workload, not the bounded `make qa` gate.
The image prepares its Python environment from the same `uv.lock` during
build; worker runs do not download Python packages.

Artifacts go under `tests/compose/out/<run-id>/` by default (override with `ICECC_OUT_DIR`).

The image build uses at most two make jobs by default (`BUILD_JOBS=2`). To
override that limit when building the compose images manually, run
`docker compose build --build-arg BUILD_JOBS=4` from `tests/compose` (choose a
value appropriate for the machine). This only bounds compilation while the
image is built; it does not bound the runtime resources used by the seven
containers or their worker builds.

## Tunables

Environment variables:

- `ICECC_OUT_DIR` – output directory (default: `tests/compose/out/<run-id>`)
- `ICECC_NETNAME` – Icecream netname (default: `composebench`)
- `ICECC_WORKER_NFILES` – number of `.cpp` files per worker (default: `200`)
- `ICECC_WORKER_JOBS` – `make -j` value per worker (default: `64`)
- `ICECC_STATE_INTERVAL` – daemon JSONL interval in seconds (default: `1`)

## Verify

`run.sh` runs `verify.py` which checks:

- both worker builds succeed
- workers compiled remotely (via `icecc` logs)
- build servers were actually used (via daemon JSONL `slots.used`)
