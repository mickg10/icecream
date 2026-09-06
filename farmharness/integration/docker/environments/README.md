# Client execution environments

These are the four retained heterogeneous C Docker environments. They are
orthogonal to the stable scheduler/worker image and to the selected Icecream
product version. The harness mounts the selected product's `/opt/icecream`
runtime read-only into one of these images.

Each `v2-p50` image extends the corresponding immutable corpus-producing `v2`
image with the libraries needed by the P50 product. The farm authority pins
both its hub image ID and its portable closure; a rebuild that drifts is
refused.

`farmtest images --foundations` verifies or builds the stable S/F image and
all four C images, then retains one closure-keyed Docker transport artifact per
image. Transport artifacts use `zstd -19 --long=31 --threads=8 --check`.
Supplying `--scenario` distributes only the stable runtime and C environments
selected by that scenario. Remote loading explicitly decodes with
`zstd -d --long=31`; the desired image tag is applied only after the loaded
transport tag's portable closure has been verified.
