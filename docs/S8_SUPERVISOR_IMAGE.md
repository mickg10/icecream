# S8 supervisor image

`docker/s8-supervisor.Dockerfile` is a small production supervisor image. It
adds only `python3`, `git`, and the Ubuntu Docker CLI package (`docker.io`);
the checkout and campaign artifacts are mounted by
`s8_protected_launcher.py` at execution time.

The farm-node tag is a local image with no registry digest. Its verified local
config ID is
`sha256:fe001a6138f017608b8846b43bf268a76a9d7a5b66c3364ba3f881da2ff0c54b`.
Materialize the content-addressed build base before building. The first check
fails closed if the local tag has drifted; `docker tag` does not copy or alter
image content:

```sh
set -eu
BASE_SOURCE=icecream/farm-node:ubuntu22-gcc11-boost174
BASE=icecream/s8-supervisor-base:ubuntu22-config-fe001a6138f017608b8846b43bf268a76a9d7a5b66c3364ba3f881da2ff0c54b
BASE_ID=sha256:fe001a6138f017608b8846b43bf268a76a9d7a5b66c3364ba3f881da2ff0c54b
test "$(docker image inspect "$BASE_SOURCE" --format '{{.Id}}')" = "$BASE_ID"
docker tag "$BASE_SOURCE" "$BASE"
test "$(docker image inspect "$BASE" --format '{{.Id}}')" = "$BASE_ID"
docker image inspect "$BASE" > /absolute/path/s8-supervisor-base.inspect.json
```

The exact offline build command is then:

```sh
docker build --pull=false --platform=linux/amd64 \
  --file docker/s8-supervisor.Dockerfile \
  --tag icecream/s8-supervisor:ubuntu22-tools-1 .
```

Capture the resulting image inspect document without changing it, then create
the authority receipt exactly once. The receipt command requires the captured
base inspect, so a drifted or decoy base cannot be recorded:

```sh
docker image inspect icecream/s8-supervisor:ubuntu22-tools-1 \
  > /absolute/path/s8-supervisor.inspect.json
python3 farmharness/s8_supervisor_image.py \
  --image-ref icecream/s8-supervisor:ubuntu22-tools-1 \
  --base-inspect-json /absolute/path/s8-supervisor-base.inspect.json \
  --inspect-json /absolute/path/s8-supervisor.inspect.json \
  --receipt /absolute/path/s8-supervisor-image-authority.json
```

The receipt binds the Dockerfile hash, local base source/reference/config ID,
raw base and resulting-image inspect hashes, exact resulting `.Id`,
`linux/amd64`, and required tools. If source provenance is needed, pass the
final tracked-clean commit explicitly with `--source-commit`; do not reuse a
stale commit from an earlier build. A second write is rejected; pass the
recorded resulting image ID to the protected launcher as
`--supervisor-image-id`.
