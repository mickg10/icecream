# S8 direct-reference witness reuse

The external executor accepts an optional `reference_witness` path in
`execute_and_finalize_external_cell`.  The path names a retained
`manifest.jsonl` produced from a prior PASS cell with all artifacts retained:

```sh
python3 research/farmharness/s8_reference_witness.py create \
  --cell-output /abs/path/to/pass-cell \
  --authority /abs/path/to/pass-cell/product-evidence/external-farm-authority.json \
  --package-dir /abs/path/to/witness-package
```

The caller passes the resulting
`/abs/path/to/witness-package/manifest.jsonl` as `reference_witness`.
The executor re-snapshots the current batch, predictive plan, source manifest,
compile database/command, authority, and pinned compiler image before staging
the package.  The runner checks the newly created toolchain archive too.  At
the batch barrier every returned remote object is compared with its retained
object; `S8_REFERENCE_REUSE` lines in the retained product log are the explicit
per-occurrence evidence.  A failed check aborts the cell and never starts a
direct local reference compiler.  Omitting `reference_witness` retains the
existing fresh-direct path.

The package is immutable and self-contained (`manifest.jsonl` plus
`objects/<ordinal>.o`).  Package creation requires direct and remote objects
to have equal digest and bytes and requires terminal PASS evidence.
