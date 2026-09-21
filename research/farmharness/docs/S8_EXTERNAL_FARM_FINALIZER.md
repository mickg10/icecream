# S8 external-farm finalizer adapter

`s8_real_c1f1_live_runner.finalize` accepts an optional
`ExternalFarmFinalization` value.  This is a finalization boundary, not a
transport: the caller supplies an already-retained workdir, a private
path/SHA/bytes descriptor for product stdout, an authenticated
`external-farm-v1` placement manifest plus SHA-256, and a separate canonical
`external-farm-authority-v1` JSON plus SHA-256.  The authority binds all four
captured hosts, physical and boot identities, ordered worker placement,
per-host binaries, idle witness, descriptors, and pinned image closure.
An authenticated `external-farm-receipt-v1` descriptor with `mode` set to
`external_farm` binds the manifest, authority, stdout, suite, and workdir.
Its exact keys are `schema`, `mode`, `suite`, `manifest_sha256`,
`authority_sha256`, `stdout` (`path`, `sha256`, `bytes`), `workdir`, and
`execution`.  The execution block contains an execution ID, q3 scheduler and
client identities, an ordered F identity for every relationship, bounded PID
and container IDs, UTC `started_at`/`finished_at`, exact stdout start/end
markers, and `artifacts` set to `{"collection":"complete","cleanup":"complete"}`.
The stdout markers are
`S8_EXTERNAL_FARM_EXECUTION execution_id=... manifest_sha256=... authority_sha256=... phase=start|end timestamp=...`.
They are required evidence of the external lifecycle, and every used host's
idle capture must be no more than 300 seconds before `started_at`.

Use `execution_environment="external_farm_product_build"` when calling the
adapter.  The adapter then runs the mature product evidence path unchanged,
including timing, action-trace, legacy-wire, returned-object, and scheduler
markers.  It emits `external_farm_timing` and the authenticated placement in
the evidence, curve, and experiment manifests, and copies both source
authorities into `product-evidence/`.  A local launch identity, local workdir
mapping, local scheduler endpoint, overlapping C/S/F host digest, suite
mismatch, disconnected worker digest, changed authority, or changed source
bytes is rejected.

The ordinary `finalize` call without `external_farm` remains loopback-only.
No SSH, Docker, remote execution, or heldout data is performed by this
adapter.
