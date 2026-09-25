# Local build and QA SDK

From a normal Git checkout on a Linux Docker host, with Git, Make and
uv 0.9.21 installed (a host Python installation is not required):

```sh
ICEFARM_TMPDIR=/data/icecream make qa
```

The scratch directory must already exist and be writable. Both scratch and
Docker's data directory need at least 8 GiB free; this is a starting floor,
not an upper bound on retained results. No host Boost, compiler or pytest is
needed. Docker's data directory is reported, never relocated automatically.
The default is two CPU jobs and an 8 GiB build-container limit. Every invocation
retains its unique run directory; remove selected old results when no longer
needed. `make dev-bootstrap` builds/installs without running QA. `make qa`
performs the build itself; no preliminary bootstrap command is required.
On normal exit, build output ownership returns to the invoking host user.
The container's `/tmp` also maps into the selected run directory, covering
older tests that ignore `TMPDIR`.

Override [farm.json](../farm.json) with `FARM=/path/to/farm.json`:

```json
{
  "version": 1,
  "profile": "ubuntu24.04",
  "image_repository": "",
  "jobs": 2,
  "memory_gb": 8
}
```

`jobs` accepts 1–8 and cannot exceed Docker's reported CPU count; memory cannot
exceed Docker's reported total. Memory limits are per container, not a reserved
budget for the whole host; the mixed gate runs several role containers.
Ubuntu 22.04 is separately selectable. This
small developer spec does not replace the existing multi-host qualification
spec or its Make targets.

### Opt-in pipeline concurrency and restart gates

Inside an already built, disposable Linux test container, run:

```sh
make -C "$BUILD/unittests" p50daemonpositive-p51-multilink-check
make -C "$BUILD/unittests" p50daemonpositive-p51-restart-check
make -C "$BUILD/unittests" p50daemonpositive-p51-restart-w30-check
ICECC_TEST_P51_PRIVATE_NETNS=1 ICECC_TEST_DAEMON_UID=icecc \
  make -C "$BUILD/unittests" p51schedulerrestart-w30-check
```

`BUILD` is the absolute configured build directory. These gates require root
inside the container, an unprivileged `icecc` account, usable `iptables` with
Docker `NET_ADMIN`, and writable scratch-backed `ICEFARM_TMPDIR`. The daemon
account must be able to traverse the build path and temporary directory.
Mount scratch at a short container path such as `/work/tmp`: these fixtures
create Unix-domain sockets with a limited path length.
Use a private Docker bridge network, never `--network=host`; do not run these
network-redirection fixtures directly on the host. The scheduler-restart gate
requires the explicit private-namespace opt-in shown above because its receipt
helper installs a temporary namespace-local OUTPUT redirection rule.

The multi-link gate covers C1F2/3/4 and C2/3/4F1 for all three profiles, with
30 outstanding jobs per link (at most 120 total). It checks exact input
attachments and progress on healthy links while one link's receipts are held.
The restart gate currently covers ZSTD_TU C1F2/F-cache and C2F1/C-cache
replacement, one affected transfer plus a healthy sibling. The separate
`restart-w30` gate covers both replacements for all three profiles: 30 held
old commits, all original callers settling without accepting the discarded
receipts, healthy-sibling progress, and 30 fresh commits and exact input
attachments on the replacement identity. It checks bounded settlement; direct
per-caller completion-time comparison with the original deadline remains
pending. These targets are not automatically included in the default
`make qa` workflow and are not throughput benchmarks or cross-host farm tests.

The `p51schedulerrestart-w30-check` target replaces the actual scheduler
process while C/F daemons and cache processes remain alive, for all three
profiles. It holds 30 old receipts, verifies old callers settle fail-closed
under remote-only policy, then verifies 30 fresh-epoch receipts and remote
objects. It does not promise transparent retry of every interrupted caller.
The fixture uses the existing three-second test reconnect setting, not the
production reconnect cadence. It retains evidence and uses orderly timeout
cleanup; an interrupted case is a failure, never a passing skip.

The opt-in lost-receipt gate validates already-published R2 commits after the
receipt connection is discarded, for 1, 2, and 30 jobs in each profile:

```sh
set -eu
for profile in P29V1 ZSTD_TU ZSTD_ROUTE; do
  for count in 1 2 30; do
    ICECC_TEST_POSITIVE_DAEMON=1 ICECC_P51_MODE=on \
      ICECC_TEST_P51_PROFILE="$profile" \
      ICECC_TEST_P51_LOST_RECEIPTS="$count" \
      TMPDIR="$ICEFARM_TMPDIR" \
      "$BUILD/unittests/p50daemonpositive" \
      "$BUILD/daemon/iceccd" "$BUILD/cache/icecc-cache-service"
  done
done
```

Run this only as root inside a disposable Docker/Podman container on a private
bridge, with `NET_ADMIN`, `iptables`, an `icecc` account, and writable
scratch-backed `ICEFARM_TMPDIR`; never use host networking. Every row requires
the same F sidecar process to remain live across at least two physical cache
links and checks one exact retained-input attachment per recovered result.
It covers receipts lost after full commit publication, not an uncommitted
suffix, and verifies attachment rather than real compiler execution. Keep the
build and scratch paths short enough for Unix-domain socket limits.

The Linux blocked-writer check runs in the default `p50zstdsender` suite and
can also be selected independently from an already built checkout:

```sh
timeout 120s "$BUILD/unittests/p50zstdsender" --writer-backpressure-ack-shutdown
```

It uses loopback sockets, needs no farm or network-administration capability,
and checks all three profiles. A small send buffer and paused F reader create
a non-writable socket with queued bytes; C must validate the first receipt
while the second bundle remains blocked, then settle after peer shutdown and
sender retirement. The first committed result must survive. This is a
component-level kernel-backpressure check, not a full service-process shutdown
or an observed `send()` EAGAIN trace. Non-Linux focused runs exit 77 because
the queue witness requires Linux `SIOCOUTQ`; Linux failures are not skips.

### Repository and offline inputs

Set `image_repository` to a prepared SDK repository, or override it with
`make qa IMAGE_REPO=registry.example:5000/team/icecream`. It pulls the tag
`sdk-ubuntu24.04` (or the selected profile) and checks the SDK recipe label.
The image must have been built from this checkout's Dockerfile, runner and
Python metadata (`pyproject.toml`, `uv.lock`, `.python-version`).
It never silently falls back to another repository or a local build. With an
empty repository, `base_image` selects a mirrored Ubuntu base image and
`http_proxy` specifies a separate HTTP(S) package-download proxy. Registry
authentication and Docker daemon proxy settings remain normal Docker setup.

For a prepared host, use `docker image save -o /data/icecream/sdk.tar IMAGE`
with the SDK tag from the build log. Transfer that archive and the checkout;
set `"image_bundle": "/data/icecream/sdk.tar", "offline": true` in `farm.json`.
Loading restores image tags; the selected SDK tag must have the matching
recipe. Offline mode will not pull images or download packages. The SDK
creates each run's Python environment from its baked package cache, offline.
An explicitly selected
repository still determines the required tag in offline mode. Build containers
have no network. A normal clone must include the pinned P43 commit
`cd74801e0fa4e83e3ae254ca1d7fe98642f36b89`; shallow clones may need that history
fetched before going offline.
The host also needs uv and a prepared locked Python environment/cache:
run `ICEFARM_TMPDIR=/data/icecream make python-sync` while online, then set
`UV_OFFLINE=1` for offline commands. A Docker image bundle does not include
the host's uv installation or cache.
Run the command on the machine hosting Docker, including when reached over
SSH; a remote Docker context is not a substitute for transferring the checkout
and scratch files. Docker Desktop's separate VM is not validated by this gate.

### What a result means

The wrapper prints a unique run directory and writes `result.json`, stage
logs, native test logs and pytest JUnit XML. After local tests pass, QA builds
P43 from the pinned real 1.4.0 source and runs five isolated Docker cases:
three strict P50 runs (P29V1, ZSTD_TU, ZSTD_ROUTE), a legacy worker, and a
legacy client, each with a current scheduler.
Each requires a working compiled program and evidence of remote work; local
fallback is not accepted. The P50 rows also require cache-sidecar registration,
the requested profile's source commit and exact worker input attachment.
Roles share product images rather than needing
separate scheduler/client/worker builds.

For the pipeline candidate, run the mixed-role gate explicitly with R2 enabled:

```sh
sh dev/python.sh dev/mixed.py \
  --current-image YOUR_CURRENT_PRODUCT_IMAGE \
  --legacy-image YOUR_P43_PRODUCT_IMAGE \
  --output "$ICEFARM_TMPDIR/mixed-r2-new-run" --p51-r2
```

Set `ICEFARM_TMPDIR` to prepared scratch storage first; the output directory
must not already exist. These are built product images, not SDK images.
The default five cases remain unchanged. `--p51-r2` adds three R2 cases,
one per compression profile; `--only-p51-r2` skips the original five.
Each R2 case requires a remote compiled program, an R2 source-control lease,
and worker-side cache-link adoption. A negotiated window of 30 does not
prove 30 simultaneous transfers or persistent-link reuse: those require the
separate pipeline tests.

To check simultaneous P43, R1 and R2 compilation on a single Docker host:

```sh
sh dev/python.sh dev/mixed.py \
  --current-image YOUR_CURRENT_PRODUCT_IMAGE \
  --legacy-image YOUR_P43_PRODUCT_IMAGE \
  --output "$ICEFARM_TMPDIR/mixed-concurrent-new-run" \
  --concurrent-mixed --jobs 3 --memory-gb 8
```

This standalone selector cannot be combined with the R2 row selectors. It
defaults to P29V1; select `--concurrent-profile ZSTD_TU` or
`--concurrent-profile ZSTD_ROUTE` to exercise those profiles. Run each profile
sequentially with a distinct output directory to keep the same resource cap.
The profile selector requires `--concurrent-mixed`.
The gate requires at least three CPU quota units and 4 GiB aggregate container memory;
the example allows 8 GiB. The output must be beneath `ICEFARM_TMPDIR` and
must not already exist. One current scheduler uses `enforcing-compat` policy,
with separate current R1 and R2 workers and three client containers. P43 and
R1 use the R1 worker; R2 uses the R2 worker. Toolchain warmup precedes a common
start barrier, so this is a warm-environment compatibility test, not a cold
build benchmark.

The gate checks overlapping compiler process identities, measured-job remote
execution, exact program output, and the selected profile's source/attachment
evidence. It retains logs and image provenance, and removes its own containers
and network afterward. This is not a W30 occupancy test, a multi-host farm
test, or evidence for profiles not selected in that run. Available selectors
are not qualification results; see `PROJECT_STATE.md` for recorded runs.

The old-scheduler fallback case additionally requires
`--ordinary50-scheduler-binary`, `--ordinary50-scheduler-sha256`, and
`--ordinary50-scheduler-source-commit`. The gate accepts only source commit
`94e9b44025887412c70c1c46c35fc588d6dec776` and checks the executable's
supplied SHA256 before mounting it into the scheduler container. Build that
binary with a compatible runtime; it is not supplied by the current image.
Add `--only-old50-scheduler-fallback` to isolate this case. Success requires
remote compilation without R2 selection; supplying an old binary alone does
not establish compatibility. Each run retains `summary.json` and role logs.

This developer gate does not certify all S* restart scenarios, all compiler
environments, multi-host performance, or historical qualified image identities.
Rebuilt legacy binaries are new artifacts, not the old qualified P43 image.

Retained compression goldens compare frame order, kind and decoded contents
across zstd library versions. The historical vectors are unchanged;
same-build repeatability and cross-provider checks still compare exact bytes.

`dev/Dockerfile` builds a source-free Ubuntu 24.04 development toolchain image.
It is separate from the Compose worker/product image and farm compiler
environments. Build/test containers do not contact an external farm or need a
Docker socket; the host wrapper launches the later mixed-role containers.
The SDK installs native C++ dependencies, Autotools, Git, shell
utilities, uv and tools used by local tests. Python 3.12.12 and pytest are
prepared by `uv sync --locked`, not installed as distro pytest packages.

The host wrapper owns image selection and scratch provisioning. Its profile is
`ubuntu24.04` by default. With an empty `image_repository`, it builds this SDK
from the JSON `base_image` (default `ubuntu:24.04`). With a non-empty
`image_repository`, it must pull that prepared SDK and fail if unavailable;
there is no local-build fallback. Docker build args `BASE_IMAGE`, `DEV_PROFILE`,
and `RECIPE_REVISION` select an approved base mirror and stamp the image. The
labels are provenance markers, not a reproducibility guarantee: base digests
and package versions should be pinned by the selected profile when repeatable
builds are required.

Runtime contract: bind a clean source snapshot at `/source:ro` and an existing,
writable, unique scratch directory at `/work`, with that directory's `tmp`
subdirectory also bound at `/tmp`. The runner verifies both temporary paths
name the same directory. It sets `TMPDIR` and `ICEFARM_TMPDIR` to this short
`/tmp` alias to leave room for Unix socket names, never to fallback storage. It
copies the snapshot into `/work/source`, configures/builds out of tree in
`/work/build`, installs bootstrap products under `/work/install`, and writes
logs, JUnit XML, and `summary.json` under `/work/artifacts`. Reuse the same
`/work` for `bootstrap` followed by `qa`; use fresh scratch for another source
snapshot.
Python uses `/work/python-env` and `/work/uv-cache`, with its readable managed
interpreter under `/opt/uv-python`. The runner checks source Python metadata
against the SDK's baked copies before offline sync; a changed lock or Python
pin requires rebuilding the SDK rather than silently reusing old dependencies.

Examples (the host wrapper should add the mounts and selected image):

```sh
docker run --rm -v "$SOURCE_SNAPSHOT:/source:ro" -v "$RUN_ROOT:/work" \
  -v "$RUN_ROOT/tmp:/tmp" \
  "$SDK_IMAGE" bootstrap 2
docker run --rm --cap-add SYS_PTRACE -v "$SOURCE_SNAPSHOT:/source:ro" -v "$RUN_ROOT:/work" \
  -v "$RUN_ROOT/tmp:/tmp" \
  "$SDK_IMAGE" qa 2
```

`bootstrap` runs `autogen.sh`, out-of-tree configure, bounded `make -j2 all`,
and `make install`. `qa` rebuilds, installs into `/work/install`, runs native `make check`, and runs the full
`farmharness/integration/tests` pytest suite including tests marked thorough.
Build/test stages run as the existing `nobody` account inside the container,
matching ordinary non-root development. A separate required root-run pass
checks the cache service's root-to-user startup and its leak-checking test.
QA containers add `SYS_PTRACE` so Linux's `kcmp` descriptor-identity checks
can run; they keep a private PID namespace and do not mount host processes.
Bootstrap-only containers do not need this addition. Direct `docker run`
invocations of the QA runner must include `--cap-add SYS_PTRACE` too.
Python tests still run if native compilation/check fails; the runner records
stage exit statuses and logs and returns nonzero if any required stage fails.
See [recorded validation](../PROJECT_STATE.md#developer-qa) for exact tested
source identities, counts, resource settings and timings. Ubuntu 22.04 remains
an untested selection. Local
namespace-dependent cases can skip when the container/host disallows those
namespaces; optional retained-corpus probes can skip when their external corpus
is absent. This is local build/test evidence, not farm qualification.
The native suite also compiles many separate checking binaries; its time is
additional to the Python suite. Inspect per-stage logs rather than treating
the Python-only timing as a budget for all of `make qa`.

## Python tools

All local tools share `pyproject.toml` and `uv.lock`; this is a non-packaged
tooling project, not a Python build of Icecream. `make python-sync` performs
exact locked setup. `sh dev/python.sh SCRIPT.py ...` runs a Python script;
`sh dev/python.sh --exec COMMAND ...` also sets PATH for shell/native helpers
and their `#!/usr/bin/env python3` children. For example, from the repository:

```sh
ICEFARM_TMPDIR=/data/icecream sh dev/python.sh -m pytest cache/sim/test_p50sim.py
ICEFARM_TMPDIR=/data/icecream sh dev/python.sh --exec bash
```

Storage defaults to `$ICEFARM_TMPDIR/icecream-uv-<uid>/`: a shared download
cache and managed Python installation, plus one environment per checkout.
Explicit `UV_CACHE_DIR`, `UV_PYTHON_INSTALL_DIR` and `UV_PROJECT_ENVIRONMENT`
overrides must be absolute paths. Do not share one project environment between
simultaneously running checkouts with different locks. The uv version is
required by `pyproject.toml`; the interpreter patch version is selected by
`.python-version`. Updating either is a deliberate change that also rebuilds
the SDK. For package updates, use `uv lock` with those same storage variables,
review the lock diff, and rerun QA; normal commands use `--locked`.

Remote OS probes still execute the remote machine/image's `python3`; a
controller virtualenv path is not valid over SSH. Those probes use only the
standard library. Frozen farm images and historical execution records are not
rewritten by this environment conversion. Run historical local tools from the
managed shell above rather than adding per-script dependency installers.
