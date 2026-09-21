# S8 external-farm authority

`s8_external_farm_authority.py` creates the private authority consumed by an
external S8 transport. Its normal invocation is a local dry-run and does not
contact a host:

```sh
python3 research/farmharness/s8_external_farm_authority.py --root /path/to/tree
```

The output is a plan only. Remote capture requires all four explicit roots and
the opt-in switch:

```sh
python3 research/farmharness/s8_external_farm_authority.py --execute \
  --root /path/to/final-head \
  --remote-root q3=/path/to/roles \
  --remote-root q2=/path/to/roles \
  --remote-root research6=/path/to/roles \
  --remote-root research7=/path/to/roles \
  --descriptor-dir /private/authority/descriptors \
  --output /private/authority/external-farm-authority.json
```

`--execute` only runs a bounded, read-only SSH probe. It reads machine-id,
boot-id, non-virtual NIC identity, CPU facts, a one-second aggregate
`/proc/stat` sample, load, a process baseline, role hashes, and
`docker image inspect`. It does not run Docker or Icecream lifecycle commands,
write a remote file, stage a tree, or run a workload.

The physical host digest is derived from the hashes of machine-id and the
stable non-virtual NIC inventory. The boot-id hash is a separate field. Each
descriptor and the authority are created once with mode `0600`; descriptor
directories are `0700`. Missing/changed role binaries, image closure, CPU
count, stale capture, physical overlap, unsafe paths, malformed identity, and
invalid mappings fail closed.

The default declarations are:

* `C1F1/100000`: one F relationship on q2.
* `C1F20/40`: 15 relationships on q2 and 5 on research7, with two slots per
  relationship.

Research6 is not used by default. `--include-research6` explicitly selects the
reviewed 10 q2 / 6 research6 / 4 research7 mapping, and research6 must have a
fresh `PASS` capture. A host whose one-minute load exceeds `0.50` or whose
one-second aggregate idle sample is below `95%` is `HOLD`; an unselected
research6 HOLD can be retained in the authority, but it cannot be mapped.
q3 is always C+S and is rejected in every F mapping.
