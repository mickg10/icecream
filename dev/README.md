# Local build and QA SDK

From a normal Git checkout on a Linux Docker host, with Git, Make and
Python 3.10+ installed:

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
exceed Docker's reported total. Ubuntu 22.04 is separately selectable. This
small developer spec does not replace the existing multi-host qualification
spec or its Make targets.

### Repository and offline inputs

Set `image_repository` to a prepared SDK repository, or override it with
`make qa IMAGE_REPO=registry.example:5000/team/icecream`. It pulls the tag
`sdk-ubuntu24.04` (or the selected profile) and checks the SDK recipe label.
The image must have been built from this checkout's Dockerfile and runner.
It never silently falls back to another repository or a local build. With an
empty repository, `base_image` selects a mirrored Ubuntu base image and
`http_proxy` specifies a separate HTTP(S) package-download proxy. Registry
authentication and Docker daemon proxy settings remain normal Docker setup.

For a prepared host, use `docker image save -o /data/icecream/sdk.tar IMAGE`
with the SDK tag from the build log. Transfer that archive and the checkout;
set `"image_bundle": "/data/icecream/sdk.tar", "offline": true` in `farm.json`.
Loading restores image tags; the selected SDK tag must have the matching
recipe. Offline mode will not pull or install packages. An explicitly selected
repository still determines the required tag in offline mode. Build containers
have no network. A normal clone must include the pinned P43 commit
`cd74801e0fa4e83e3ae254ca1d7fe98642f36b89`; shallow clones may need that history
fetched before going offline.
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

This developer gate does not certify all S* restart scenarios, all compiler
environments, multi-host performance, or historical qualified image identities.
Rebuilt legacy binaries are new artifacts, not the old qualified P43 image.

Retained compression goldens compare frame order, kind and decoded contents
across zstd library versions. The historical vectors are unchanged;
same-build repeatability and cross-provider checks still compare exact bytes.

`dev/Dockerfile` builds a source-free Ubuntu 24.04 development toolchain image.
It is separate from the Compose worker/product image and farm compiler
environments. The container does not start a farm, contact workers, or need a
Docker socket. It installs native C++ dependencies, Autotools, Git, shell
utilities, Python 3/pytest, and tools used by local tests.

The host wrapper owns image selection and scratch provisioning. Its profile is
`ubuntu24.04` by default. With an empty `image_repository`, it builds this SDK
from the configured `BASE_IMAGE` (default `ubuntu:24.04`). With a non-empty
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
The Ubuntu 24.04 gate passed on q4 (containerd/overlayfs), using the SDK built
on nas642 (ZFS) and transferred with Docker save/load: 169 native checks plus
two root-only checks, 1,486 Python checks, and all five mixed cases. Six native
and seven Python checks skipped. With four jobs and a 16 GiB limit, the complete
command took about 11 minutes 37 seconds; Python accounted for 54 seconds.
This timing excludes preparing/transferring the SDK. Ubuntu 22.04 remains an
untested selection. Local
namespace-dependent cases can skip when the container/host disallows those
namespaces; optional retained-corpus probes can skip when their external corpus
is absent. This is local build/test evidence, not farm qualification.
The native suite also compiles many separate checking binaries; its time is
additional to the Python suite. Inspect per-stage logs rather than treating
the Python-only timing as a budget for all of `make qa`.
