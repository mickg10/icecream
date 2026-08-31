# S8 supervisor image

`docker/s8-supervisor.Dockerfile` is the production supervisor definition. It
derives from the already-qualified Ubuntu 22 farm-node image by immutable
digest and adds only the tools the host-visible supervisor needs: `python3`,
`git`, and the Ubuntu Docker CLI package (`docker.io`). The image does not
contain the checkout or campaign artifacts; `s8_protected_launcher.py` mounts
those paths and the host Docker socket at execution time.

The definition is intentionally declarative. Building, pulling, and running
Docker are operator actions. The exact build command is:

```sh
docker build --pull=false --platform=linux/amd64 \
  --file docker/s8-supervisor.Dockerfile \
  --tag icecream/s8-supervisor:ubuntu22-tools-1 .
```

Capture the resulting image inspect document without changing it, then create
the authority receipt exactly once:

```sh
docker image inspect icecream/s8-supervisor:ubuntu22-tools-1 \
  > /absolute/path/s8-supervisor.inspect.json
python3 farmharness/s8_supervisor_image.py \
  --image-ref icecream/s8-supervisor:ubuntu22-tools-1 \
  --inspect-json /absolute/path/s8-supervisor.inspect.json \
  --receipt /absolute/path/s8-supervisor-image-authority.json
```

The receipt records the Dockerfile path/size/hash, digest-pinned base, exact
resulting inspect `.Id`, `linux/amd64` platform, required tools, and hash of
the raw inspect capture. A second write to the same receipt is rejected; do
not replace an authority receipt after a build. Pass its recorded image ID to
the protected launcher as `--supervisor-image-id` and retain the receipt with
the campaign handoff.
