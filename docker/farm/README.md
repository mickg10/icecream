# Multi-host Docker farm launcher

This directory prepares a real local-LAN Icecream farm. It does not emulate several hosts on one
Docker engine: the controller invokes the Docker engine on each selected host over an explicit SSH
transport, and every service uses that host's network and host-local bind mounts.

The default topology is deliberately narrow:

- scheduler: `nas642` at `10.0.27.127`
- submitter profiles: `nas642`, or `quietbox2` with quietbox2 worker capacity excluded
- worker hosts: `research6`, `research7`, `quietbox2`, and `quietbox3`
- LAN: `10.0.27.0/24`
- WAN hosts: none (`wan_hosts=[]`)

The launcher rejects addresses outside that LAN and rejects the Tailscale `100.64.0.0/10` range.
SSH aliases are used only for account/key selection; `HostName` is overridden with the inventoried
LAN address on every connection. A later WAN experiment needs a separate, visibly WAN-labelled
manifest and is not enabled by this launcher.

## Current inventory and launch status

The retained read-only snapshot is
[`inventory/2026-08-21-local-lan.json`](inventory/2026-08-21-local-lan.json). From `nas642`,
`research6`, `research7`, and `quietbox2`, every explicit LAN target answered ICMP and TCP/22.
The dependency and quietbox3 follow-up is retained separately in
[`inventory/2026-08-22-p50-dependencies.json`](inventory/2026-08-22-p50-dependencies.json).
The later F/slot and client-capacity clarification is retained in
[`inventory/2026-08-22-topology-correction.json`](inventory/2026-08-22-topology-correction.json);
it supersedes the older snapshot's interpretation of one container per slot without rewriting the
historical inventory.
Current launch blockers are represented as preflight failures rather than guesses:

- `quietbox3` is inventoried at `10.0.27.101` as `mickg10`, with interface
  `enp10s0f1np1`, 32 visible CPUs, passwordless administrative access, and a working Docker engine.
  Its exact runtime/compiler images are staged; launch remains disabled only until its host-local
  runtime, source, and corpus mount roots are populated.
- `research7` is reachable, but the inventoried account cannot use Docker.
- `research6` accepts Docker metadata operations, but the live gate could not start even a bounded
  `true` container from the unpacked runtime image; the daemon reported a fatal session-health error.
- the staged Icecream runtime prefix and daemon-runtime image exist on `nas642`, `research6`, and
  `quietbox2`; research6 remains launch-disabled because its engine could not start a bounded
  container.
- submitter-specific source, corpus, build, and result roots are not yet present on every submitter.
- P50-capable derivatives of all four build-environment images are staged identically on
  `quietbox2`, `quietbox3`, and the default submitter `nas642`. The original
  corpus-producing `v2` images remain unchanged.

`preflight` is read-only and reports all selected-host failures. `up` performs that same check before
creating any directory or container.

## Capacity accounting

An F is a physical daemon endpoint, never a slot. The conservative nas642 profile is:

| F host | daemon containers | compile slots (`iceccd -m`) | resource group |
|---|---:|---:|---|
| research6 | 1 | 16 | `nas-r6-r7-shared` |
| research7 | 1 | 16 | `nas-r6-r7-shared` |
| quietbox2 | 1 | 24 | `quietbox2-physical` |
| quietbox3 | 1 | 24 | `quietbox3-physical` |
| total | 4 | 80 | three resource groups |

The research6 and research7 allocations are additive, but they and `nas642` share the same
owner-described physical machine. Their current guest views are 20, 12, and 16 CPUs respectively;
the launcher records those views and group load without summing them as independent physical
capacity. The shared group has a 64-slot ceiling: research6 and research7 are independent F daemons
that may each advertise up to 32 slots. Scheduler and primary-submitter load on
`nas642` remains charged to that group.

`conservative_nas_submitter` is C1F4 with 80 slots. `max32_nas_submitter` keeps those same four F
identities and advertises 32 slots per F, for 128 slots. `conservative_q2_submitter` makes
`quietbox2` C and excludes its F daemon, producing C1F3 with 56 slots; `max32_q2_submitter` keeps the
same three F identities at 32 slots each, for 96 slots.

`r6_recovery_nas_submitter` is a targeted research6 recovery gate: scheduler and C on `nas642`, with
one F daemon on `research6` advertising 16 slots. It is launch-disabled in the checked manifest until
the recorded Docker session-health failure is corrected and re-inventoried. The proven small path
uses `conservative_nas_submitter` with C1F1, whose deterministic F is quietbox2.

C1F1, C1F2, C1F3, and C1F4 select the first one through four physical hosts in the profile's declared
order. Each selected host receives exactly one container, one daemon node name, one LAN route
identity, and one cache-identity owner. The plan records those counts separately from compile slots;
readiness verifies the exact endpoint and the `jobs=*/SLOTS` value reported by `listcs`.

`nas642` is intentionally retained as the naturally bandwidth-limited C experiment point.
`quietbox2` is the faster-C comparison. A live acceptance captures before/after byte counters on each
explicit LAN interface and compile elapsed time in `network-ledger.json`, producing C→F and F→C
directional provenance. These are host-interface aggregate observations, not per-flow attribution.

## Image classes

There are two separate concepts:

1. `Dockerfile.node` is the scheduler/F daemon runtime class: Ubuntu 22.04, GCC 11.4, and Boost
   1.74. It is also a controlled build environment for staging Icecream itself. GCC 11 uses
   `ICE_CXX_STANDARD_FLAG=-std=c++2b` for the repository's C++23 configure check.
2. The C submitter/build environments are exactly the four retained `.ii` generation classes. The
   daemon runtime image is not a fifth corpus environment.

| environment | exact tag | compiler / standard library | verified corpus cells |
|---|---|---|---:|
| `debian-gcc` | `ice-ii/debian-gcc:v2-p50` | GCC 12.2 / libstdc++ 12 | 11 |
| `fedora-clang-libcxx` | `ice-ii/fedora-clang-libcxx:v2-p50` | Clang 20.1.8 / libc++ | 11 |
| `linuxbrew` | `ice-ii/linuxbrew:v2-p50` | Clang 22.1.8 / libstdc++ 12 | 11 |
| `conan-gcc` | `ice-ii/conan-gcc:v2-p50` | GCC 14.2 / libstdc++ 14 | 11 |

The original `v2` tags are the corpus provenance; the separate `v2-p50` derivatives add only the
libraries needed to build the current tree. Their Dockerfiles are retained under
`docker/farm/environments/`. The manifest retains the inventoried engine IDs and pins a
portable fingerprint over OS, architecture, runtime config, and uncompressed layer
hashes. Docker engines using legacy and containerd stores can report different `.Id` values for the
same saved image; the portable fingerprint is the cross-engine launch gate, while both native IDs
remain run provenance. The 44-cell evidence is 11 projects by all four profiles. The former harness location was
`~/icecream-ii-matrix/compression-research/ii-matrix/produce_cell.sh`; its Dockerfiles and manifests
were not present under the inventoried quietbox2 roots, so this work does not reconstruct or replace
them. It reuses the retained images.

Scheduler, C, and F binary-set selection is a separate matrix dimension. The checked manifest has
one available set, `accepted-current`, whose paths and image content are verified by preflight. It
also declares `p43` and `p50` as unavailable selection seams with no artifact path or image. Passing
`--scheduler-version p43`, `--submitter-version p50`, or the corresponding worker option therefore
fails during plan construction until an exact reviewed artifact is added; the launcher never labels
the current binary as a missing protocol version.

## Host-local mount contract

The manifest keeps environment class separate from physical host. Each host declares these paths:

- `runtime_prefix`: installed `/opt/icecream` tree, mounted read-only at `/opt/icecream`
- `source_root`: selected corrected source tree, mounted read-only at `/workspace` for C
- `corpus_root`: `.ii` corpus/package root, mounted read-only at `/corpus` for C
- `build_root`: writable external C build tree
- `results_root`: writable logs, acceptance metadata, objects, and executables
- `state_root`: writable daemon environment/cache state, separated per run and container

Workers mount only runtime, state, and results. Submitters additionally mount source, corpus, and
build roots. No container-local source or result is treated as durable evidence.

Each service starts as container root only long enough to assign its exact run-scoped writable bind
directories to UID/GID 65534, then replaces that shell with the retained daemon, which drops to
`nobody`. The launcher never changes ownership of the shared source, corpus, runtime, or parent
directories.

To select corrected M2/M3 binaries or a different host checkout without editing the farm manifest,
pass a narrow overlay:

```json
{
  "hosts": {
    "nas642": {
      "runtime_prefix": "/absolute/stage/opt/icecream",
      "source_root": "/absolute/corrected/source"
    }
  }
}
```

Only the six mount keys are accepted in `--mounts`; network and host identity cannot be overridden
through that file.

## Preparing the daemon runtime

Build the distinct node image on `nas642`:

```sh
docker build --pull=false \
  -f docker/farm/Dockerfile.node \
  -t icecream/farm-node:ubuntu22-gcc11-boost174 \
  docker/farm
docker image inspect --format '{{.Id}}' icecream/farm-node:ubuntu22-gcc11-boost174
```

The checked manifest pins the reviewed build's image ID. If the Dockerfile is intentionally rebuilt,
review the package/version change and update both retained identity fields before distribution. A CLI
override must provide both `--node-image-ref` and the exact `--node-image-fingerprint`; mixed runtime
content is rejected across selected hosts.

Build and stage an installed prefix from the selected source. The source is read-only; build and
stage trees stay external:

```sh
mkdir -p /tanksmall/scratch/ictmp/icecream-farm/build-runtime
mkdir -p /tanksmall/scratch/ictmp/icecream-farm/runtime
docker run --rm \
  --user "$(id -u):$(id -g)" \
  --mount type=bind,src=/absolute/corrected/source,dst=/src,readonly \
  --mount type=bind,src=/tanksmall/scratch/ictmp/icecream-farm/build-runtime,dst=/build \
  --mount type=bind,src=/tanksmall/scratch/ictmp/icecream-farm/runtime,dst=/stage \
  icecream/farm-node:ubuntu22-gcc11-boost174 \
  bash -lc 'test ! -e /build/source-copy && cp -a /src /build/source-copy && cd /build/source-copy && autoreconf -fi && mkdir obj && cd obj && ../configure --prefix=/opt/icecream --without-man ICE_CXX_STANDARD_FLAG=-std=c++2b && make -j16 && make DESTDIR=/stage install-exec'
```

Use a new empty `build-runtime` path for each source revision. The command copies into that external
path because Autotools bootstrap files cannot be generated in the read-only source mount. The farm
stage uses `install-exec`: scheduler, daemon, client, environment helper, and wrapper links are needed;
manual pages and development headers are not part of the mounted runtime prefix.

Provision that staged tree at each selected host's `runtime_prefix`. Do not point multiple physical
hosts at a path that only exists on `nas642`; bind paths are host-local.

After building the runtime image on `nas642`, stream its exact image to selected Docker engines:

```sh
docker/farm/farm.py sync-image --runtime \
  --source nas642 \
  --destination research6 \
  --destination research7 \
  --destination quietbox2
```

The command verifies the source and destination portable content fingerprints and retains each
engine's native image ID as provenance. It does not run containers or remove images. `quietbox3`
cannot be selected until its SSH transport is configured and verified.

For a `nas642` submitter, copy an existing build environment from `quietbox2` through the
controller without creating an archive on disk:

```sh
docker/farm/farm.py sync-image --environment debian-gcc \
  --source quietbox2 --destination nas642
```

Repeat for the other three environments before the four-profile matrix.

## Planning and read-only gates

Commands are run from the repository root. A run label is explicit input; the run ID is a stable
hash of label, manifest, profile, scenario, and environment. Reusing all inputs produces the same ID,
and `up` refuses to overwrite an existing controller run directory.

```sh
docker/farm/farm.py validate

docker/farm/farm.py plan \
  --scenario c1f4 \
  --profile conservative_nas_submitter \
  --environment debian-gcc \
  --run-label baseline-01

docker/farm/farm.py preflight \
  --scenario c1f1 \
  --profile conservative_nas_submitter \
  --environment debian-gcc \
  --run-label baseline-01 \
  --output /tanksmall/scratch/ictmp/farm-preflight-baseline-01.json
```

If the default scheduler/control pair is occupied, select a reviewed free pair explicitly. The
control port is always the following port, and both values become part of the deterministic run ID:

```sh
docker/farm/farm.py preflight \
  --scenario c1f1 \
  --profile r6_recovery_nas_submitter \
  --scheduler-port 18765 \
  --environment debian-gcc \
  --run-label lan-smoke-01
```

Preflight verifies, for every selected host:

- exact LAN interface/address and non-overlay scheduler route
- SSH reachability and expected hostname
- Docker server permission and Compose availability
- current CPU view, cpuset/cgroup quota, load, memory, and CPU pressure
- required bind roots and writable parents for run-created roots
- an exact readable copy of `acceptance/tiny.cpp` under the selected submitter source mount
- installed scheduler/daemon/client/environment-helper binaries, their SHA-256 values, and identical
  daemon hashes across selected hosts
- pinned portable content fingerprints for the node runtime and build-environment images, retaining
  each engine's native image ID as provenance
- absence of every exact deterministic container name before launch
- absence of listeners only on ports opened by the plan: scheduler/control and F worker ports;
  submit-only C daemons do not reserve a listening port

No readiness check is a fixed sleep. Container health and controller readiness both issue `listcs`
and `quit` over the scheduler's line-oriented control port; they never make an unframed connection
to the binary daemon port. Launch then requires every C/F node name together with its exact planned
LAN address and registration port to appear in `listcs`. Each `--no-remote` C daemon must register
with port `0`; each F must register its exact planned listening port. A C daemon reported at an
unused nominal port such as `14000` does not satisfy readiness.

## Scenario entrypoints

Every named foreground entrypoint performs read-only preflight, exact per-host Compose bring-up,
scheduler/`listcs` readiness including the advertised slot check, compile/link/run acceptance,
evidence collection, and exact labelled teardown:

```sh
docker/farm/run-nas-c1f1 \
  --run-label baseline-c1f1 \
  --controller-output /tanksmall/scratch/ictmp/icecream-farm/controller-results

docker/farm/run-nas-c1f2 \
  --run-label baseline-c1f2 \
  --controller-output /tanksmall/scratch/ictmp/icecream-farm/controller-results

docker/farm/run-nas-c1f4 \
  --run-label baseline-nas-c1f4 \
  --controller-output /tanksmall/scratch/ictmp/icecream-farm/controller-results

docker/farm/run-q2-c1f1 \
  --run-label baseline-q2-c1f1 \
  --controller-output /tanksmall/scratch/ictmp/icecream-farm/controller-results

docker/farm/run-q2-c1f2 \
  --run-label baseline-q2-c1f2 \
  --controller-output /tanksmall/scratch/ictmp/icecream-farm/controller-results

docker/farm/run-q2-c1f3 \
  --run-label baseline-q2-c1f3 \
  --controller-output /tanksmall/scratch/ictmp/icecream-farm/controller-results
```

The explicit upper-bound commands are `run-nas-c1f4-max32` and `run-q2-c1f3-max32`. They change only
slot budgets; F container, route-identity, and cache-identity-owner counts remain four and three.

For a manually inspected run:

```sh
docker/farm/farm.py up \
  --scenario c1f1 --run-label inspect-01 \
  --controller-output /tanksmall/scratch/ictmp/icecream-farm/controller-results

docker/farm/farm.py accept --run-dir /absolute/controller/results/p50-c1f1-RUNHASH
docker/farm/farm.py collect --run-dir /absolute/controller/results/p50-c1f1-RUNHASH
docker/farm/farm.py down --run-dir /absolute/controller/results/p50-c1f1-RUNHASH
docker/farm/farm.py status --run-dir /absolute/controller/results/p50-c1f1-RUNHASH
docker/farm/farm.py reconcile --desired down \
  --run-dir /absolute/controller/results/p50-c1f1-RUNHASH
```

`down` reads the retained run manifest, checks each container's exact
`org.icecream.farm.run_id` label, and removes only those named containers. It does not remove images,
source, corpus, state, build, result directories, the host Docker engine, or unrelated containers.
Repeated `up` reuses an already-ready exact run; repeated `run` reuses an accepted-and-down run;
repeated `down` reports the exact containers absent. A partial run must be brought down before a new
label is launched, preserving the prior evidence rather than overwriting it.

Run all four accepted build environments from one submitter:

```sh
docker/farm/run-matrix matrix01 nas642 \
  --controller-output /tanksmall/scratch/ictmp/icecream-farm/controller-results

docker/farm/run-matrix matrix02 quietbox2 \
  --controller-output /tanksmall/scratch/ictmp/icecream-farm/controller-results
```

## Acceptance evidence

Acceptance compiles `acceptance/tiny.cpp` through the selected C container and its local submit
daemon, links the returned object, runs the executable, and requires exactly `icecream-farm-ok 42`.
It also refuses a local compile fallback: the Icecream client log must contain a numeric scheduler
Job ID and an exact endpoint from the planned F set. Provenance records:

- run ID, environment class, scheduler endpoint, compiler and first version line
- source, object, and executable SHA-256 values
- object file description
- allowed and selected F endpoint plus remote completion identity from the client trace
- exact program output
- selected F host identities and advertised compile slots from `listcs`
- nas642-limited or quietbox2-faster client-capacity provenance and live C→F/F→C LAN-interface
  counter deltas with elapsed time

Controller evidence is retained under `CONTROLLER_OUTPUT/RUN_ID/`: plan/manifest snapshot, preflight,
per-host Compose JSON, readiness `listcs`, acceptance stdout/stderr/provenance, Docker inspect/stats/logs,
per-node service logs, `network-ledger.json`, and `evidence-hashes.sha256`. Each host also retains its bound results and state under
its manifest path. Teardown is recorded in `run.json`.

This launcher measures orchestration and real compilation behavior. It does not claim protocol
properties from the formal models, and it does not embed unfinished networking or later cache work.

The retained C1F1 proof is run `p50-c1f1-15d4bf7c3fbf`, scheduler and C on nas642 and F on
quietbox2 (`10.0.27.212:12000`). It records scheduler Job ID 1, successful compile/link/run output
`icecream-farm-ok 42`, object and executable digests, per-node logs, and removal of all three exact
run-labelled containers. Its compact summary and full evidence hash ledger are retained outside the
source tree at:

```text
/tanksmall/MICKG2/mickg/src/mickg10/icecream-artifacts/
  icecream-docker-farm-logs/20260821/acceptance-summary-q2-final.log
  icecream-docker-farm-logs/20260821/retained-evidence-q2.sha256
```

## Local checks

```sh
python3 docker/farm/test_farm.py
python3 -m py_compile docker/farm/farm.py
make farm-check
```
