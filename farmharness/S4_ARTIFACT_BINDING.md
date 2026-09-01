# S4 artifact binding

`s4_artifact_binder.py` is the provenance boundary for the later S4 matrix
runner. It consumes completed build receipts and hashes the already-produced
files. It does not run configure, make, Docker, SSH, or a farm process.

The manifest always contains rows for P43, P44, and P50. A missing or invalid
receipt is represented as `NOT_BOUND`, and the overall result remains
`NOT_READY`. P44 deliberately has no retained binary digest set: it cannot be
promoted from source authority alone. P43 is checked against the retained
role digests in `s4_version_transition_planner.py`. P50 must name the final
product runtime authority and source commit `04006b9d94161a047154121f47785aec747ffd87`;
the planner metadata commit `0aa537f746d41386aed49a88905406a2514b76f5` is never
accepted as a P50 runtime build.

## Exact P44 receipt recipe (non-executing)

Build P44 from commit `16b48c2bf2715a8eff329ee124bd52bd4731db29` in a private
checkout, record the checkout tree, build options and output root, and record
the byte count and SHA-256 of each regular file. Copy those facts into
`P44_BUILD_RECEIPT_TEMPLATE.json`; replace every `RECORD_*` value. Confirm
that `services/comm.h` contains exactly `#define PROTOCOL_VERSION 44`, then
submit the receipt to the binder. The binder itself is the only required audit
step after the build:

```text
python farmharness/s4_artifact_binder.py \
  --receipt P44=/private/receipt/p44.json \
  --receipt P43=/private/receipt/p43.json \
  --receipt P50=/private/receipt/p50.json \
  --output /private/manifest/s4-artifacts.json
```

The output path is create-once and mode `0444`; an existing manifest is never
overwritten. A later runner must re-run `--audit` immediately before using
the manifest so deleted, replaced, changed, or aliased role files cannot be
silently used.
