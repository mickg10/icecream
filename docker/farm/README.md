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
Current launch blockers are represented as preflight failures rather than guesses:

- `quietbox3` answers on the LAN, but its SSH account/interface/Docker facts are not available.
- `research7` is reachable, but the inventoried account cannot use Docker.
- `research6` accepts Docker metadata operations, but the live gate could not start even a bounded
  `true` container from the unpacked runtime image; the daemon reported a fatal session-health error.
- the staged Icecream runtime prefix and daemon-runtime image exist on `nas642`, `research6`, and
  `quietbox2`; research6 remains launch-disabled because its engine could not start a bounded
  container.
- submitter-specific source, corpus, build, and result roots are not yet present on every submitter.
- all four build-environment images exist on `quietbox2`; the Debian image is also staged on
  `nas642` for the retained smoke attempt.

`preflight` is read-only and reports all selected-host failures. `up` performs that same check before
creating any directory or container.

## Capacity accounting

`local_80_nas_submitter` is the named owner-proposed profile:

| worker identity | declared F containers/slots | resource group |
|---|---:|---|
| research6 | 16 | `nas-r6-r7-shared` |
| research7 | 16 | `nas-r6-r7-shared` |
| quietbox2 | 24 | `quietbox2-physical` |
| quietbox3 | 24 | `quietbox3-physical` |
| total | 80 | three independent resource groups |

The research6 and research7 allocations are additive, but they and `nas642` share the same
owner-described physical machine. Their current guest views are 20, 12, and 16 CPUs respectively;
the launcher records those views and group load without summing them as independent physical
capacity. The shared group has a 32-worker aggregate cap. Scheduler and primary-submitter load on
`nas642` remains charged to that group.

`local_56_q2_submitter` makes `quietbox2` the submitter and assigns it zero workers. This prevents
submitter preprocessing from being hidden behind 24 worker slots. Its F allocation is research6=16,
research7=16, quietbox3=24.

`local_16_r6_smoke` is a targeted research6 recovery gate: scheduler and C on `nas642`, with F
containers only on `research6`. It is intentionally launch-disabled in the checked manifest until
the recorded Docker session-health failure is corrected and re-inventoried. The proven small path
uses `local_80_nas_submitter` with C1F1, whose deterministic first allocation is quietbox2. Neither
path changes or waits for quietbox3 access or research7 Docker permission, and neither replaces the
named 80-slot capacity profile.

Each F in C1F1, C1F2, and C1F20 is a distinct container running `iceccd -m 1`. Allocation is
deterministic round-robin over the profile's host order. The plan records container count, daemon
host identities, and independent resource-group count separately.

## Image classes

There are two separate concepts:

1. `Dockerfile.node` is the scheduler/F daemon runtime class: Ubuntu 22.04, GCC 11.4, and Boost
   1.74. It is also a controlled build environment for staging Icecream itself. GCC 11 uses
   `ICE_CXX_STANDARD_FLAG=-std=c++2b` for the repository's C++23 configure check.
2. The C submitter/build environments are exactly the four retained `.ii` generation classes. The
   daemon runtime image is not a fifth corpus environment.

| environment | exact tag | compiler / standard library | verified corpus cells |
|---|---|---|---:|
| `debian-gcc` | `ice-ii/debian-gcc:v2` | GCC 12.2 / libstdc++ 12 | 11 |
| `fedora-clang-libcxx` | `ice-ii/fedora-clang-libcxx:v2` | Clang 20.1.8 / libc++ | 11 |
| `linuxbrew` | `ice-ii/linuxbrew:v2` | Clang 22.1.8 / libstdc++ 12 | 11 |
| `conan-gcc` | `ice-ii/conan-gcc:v2` | GCC 14.2 / libstdc++ 14 | 11 |

The manifest retains the inventoried engine IDs and local content-addressed RepoDigests and pins a
portable fingerprint over OS, architecture, runtime config, and uncompressed layer
hashes. Docker engines using legacy and containerd stores can report different `.Id` values for the
same saved image; the portable fingerprint is the cross-engine launch gate, while both native IDs
remain run provenance. The 44-cell evidence is 11 projects by all four profiles. The former harness location was
`~/icecream-ii-matrix/compression-research/ii-matrix/produce_cell.sh`; its Dockerfiles and manifests
were not present under the inventoried quietbox2 roots, so this work does not reconstruct or replace
them. It reuses the retained images.

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
  --scenario c1f20 \
  --profile local_80_nas_submitter \
  --environment debian-gcc \
  --run-label baseline-01

docker/farm/farm.py preflight \
  --scenario c1f1 \
  --profile local_80_nas_submitter \
  --environment debian-gcc \
  --run-label baseline-01 \
  --output /tanksmall/scratch/ictmp/farm-preflight-baseline-01.json
```

If the default scheduler/control pair is occupied, select a reviewed free pair explicitly. The
control port is always the following port, and both values become part of the deterministic run ID:

```sh
docker/farm/farm.py preflight \
  --scenario c1f1 \
  --profile local_16_r6_smoke \
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

The foreground entrypoints launch, run acceptance, collect, and always tear down exact run-labelled
containers:

```sh
docker/farm/run-c1f1 \
  --run-label baseline-c1f1 \
  --controller-output /tanksmall/scratch/ictmp/icecream-farm/controller-results

docker/farm/run-c1f2 \
  --run-label baseline-c1f2 \
  --controller-output /tanksmall/scratch/ictmp/icecream-farm/controller-results

docker/farm/run-c1f20 \
  --run-label baseline-c1f20 \
  --controller-output /tanksmall/scratch/ictmp/icecream-farm/controller-results
```

Use `--profile local_56_q2_submitter` for quietbox2 submission. The profile selects quietbox2 as C
and removes its F allocation; an attempt to override C onto a host that still has worker slots is
rejected.

For a manually inspected run:

```sh
docker/farm/farm.py up \
  --scenario c1f1 --run-label inspect-01 \
  --controller-output /tanksmall/scratch/ictmp/icecream-farm/controller-results

docker/farm/farm.py accept --run-dir /absolute/controller/results/p50-c1f1-RUNHASH
docker/farm/farm.py collect --run-dir /absolute/controller/results/p50-c1f1-RUNHASH
docker/farm/farm.py down --run-dir /absolute/controller/results/p50-c1f1-RUNHASH
```

`down` reads the retained run manifest, checks each container's exact
`org.icecream.farm.run_id` label, and removes only those named containers. It does not remove images,
source, corpus, state, build, or result directories.

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

Controller evidence is retained under `CONTROLLER_OUTPUT/RUN_ID/`: plan/manifest snapshot, preflight,
per-host Compose JSON, readiness `listcs`, acceptance stdout/stderr/provenance, Docker inspect/stats/logs,
per-node service logs, and collection hashes. Each host also retains its bound results and state under
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
