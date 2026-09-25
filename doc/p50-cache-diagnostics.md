# P50 retry and service diagnostics

Diagnostics are disabled unless `ICECC_P50_DIAGNOSTICS=1` is present in the
process environment. Records are bounded, single-line JSON and deliberately
omit job IDs, compiler arguments, source data, paths, and endpoint identities.

The client writes `P50_RETRY_DIAG {json}` once for each Error 106 retry
decision. Version 1 records contain `decision` (`retry_requested`,
`suppressed_one_shot`, or `rejected_missing_endpoint`), `reason`
(`source_transfer`, `worker_resource`, `worker_transport`, or `unknown`),
`stage` (`source_transfer`, `compile_result`, `remote_assignment`, or
`unknown`), `original_code`, and the `strict` 0/1 flag. `original_code` is the
failure code before normalization into the retry class. A retry event records
the decision before predecessor settlement or reconnection begins.
Interpret the code with its reason: source-transfer codes come from the local
transfer result, while worker resource/transport codes are client error codes.

The sidecar writes `P51_SERVICE_METRICS {json}` on its existing timer (at most
once per ten seconds) and once at stop. Timing populations cover P51/R2 source
operations only:

- `admission_wait`: accepted request to raw-byte/count credit grant;
- `read_queue`: credit grant to a preparation worker starting;
- `read`: source `pread` span;
- `delivery`: source-read finish through owner-executor queuing and transfer /
  reply settlement. This is not codec-only time. Operations cancelled before
  reply settlement are absent from this population.

Each timing series has `count`, `total_ns`, and `max_ns`. Periodic atomic
snapshots are best-effort and may observe counters between updates; the stop
snapshot is emitted after runtime joins and is final. Raw-byte current,
high-water, and limit describe the shared source budget and can include legacy
R1 users, even though the timing series are P51/R2-only. `pid` and
`elapsed_ms` identify a process instance and its monotonic runtime position.

`p50service-metrics-run.sh` checks that the opt-in exact-fit P51 transfer
emits a valid final record, releases all bytes/credits, stays within its raw
limit, and records each timing phase. It repeats with diagnostics unset and
asserts that no metrics record is emitted. The completion-flow runner checks
the client retry record schema and retry/suppression decisions when diagnostics
are enabled, and confirms silence when disabled.
