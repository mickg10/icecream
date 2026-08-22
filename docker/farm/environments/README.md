# P50-capable corpus environments

These Dockerfiles extend the four immutable `v2` environments that produced the retained 44-cell
`.ii` corpus. They preserve each environment's compiler and standard-library identity while adding
the development libraries needed to configure and build the C++23/P50 Icecream tree:

- Boost 1.74 or newer, including Boost.Asio coroutine headers
- xxHash 0.8 or newer
- Zstandard 1.4 or newer
- LZO, libarchive, and libcap-ng development files

The derived images use a separate `v2-p50` tag. The original `v2` tags are not replaced. The farm
manifest pins the derived engine IDs and portable content fingerprints, and the same exact images
are staged on `nas642`, `quietbox2`, and `quietbox3` before a matrix run.

Build each image on a host that already retains its corresponding `v2` base, for example:

```sh
docker build --pull=false \
  --build-arg BASE_IMAGE=ice-ii/debian-gcc:v2 \
  -f docker/farm/environments/Dockerfile.debian-gcc-p50 \
  -t ice-ii/debian-gcc:v2-p50 .
```

Each build performs minimum Boost, xxHash, and Zstandard version checks. The retained farm
provisioning gate additionally compiles, links, and runs one C++23 program that exercises
Boost.Asio awaitables, XXH3, reusable Zstandard contexts, LZO, libarchive, and libcap-ng.
