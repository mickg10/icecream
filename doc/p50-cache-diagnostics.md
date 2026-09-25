# P50 retry and service diagnostics

Diagnostics are disabled unless `ICECC_P50_DIAGNOSTICS=1` is present in the
process environment (the exact value `1`; other values leave them disabled).
Records are bounded, single-line JSON and deliberately omit compiler
arguments, source data, paths, and endpoint identities. Retry and service
records omit job IDs; remote-phase records include numeric assignment metadata
for joining observations on the client machine.

## Remote invocation phases

For an R2 assignment, the client writes one `P50_REMOTE_PHASE {json}` record
to stderr when `build_remote_int` returns or unwinds. Emission does not depend
on the normal log verbosity. A record describes one invocation, not every
retry combined. It ends after the remote compiler result and terminal
disposition, not immediately after source transfer.

The fixed schema contains `schema_version` (1), `job_id`, 64-bit
`assignment_nonce`, `profile_mask`, `stage`, `outcome`, `error_code`,
`start_ms`, `end_ms`, and these duration fields:

| Field | Measured interval |
| --- | --- |
| `compiler_connect_ms` | Connection attempts to the selected compiler endpoint |
| `environment_ready_ms` | Environment setup and verification after connection, including any environment-transfer local-slot wait |
| `local_slot_wait_ms` | Local preprocessing slot acquisition |
| `local_cpp_prepare_ms` | Complete source preparation |
| `lease_send_to_fd_ms` | C lease request through control-FD reply |
| `arm_send_to_armed_ms` | F ARM request through validated ARMED reply |
| `lease_send_to_armed_ms` | Combined lease/ARM interval, including intervening setup |
| `armed_to_control_begin_ms` | ARMED receipt through local control-operation start |
| `control_wait_ms` | Control-operation start through transfer-result settlement |
| `compiler_result_wait_ms` | Ordinary compiler result/output and terminal-disposition handling |

Unreached durations and unavailable metadata are `null`, distinct from a
measured zero milliseconds. A phase that exits unsuccessfully still records
its elapsed time. `stage` names the last entered phase (or `complete`);
`outcome` distinguishes success, failure, and exception. Interpret the
numeric error with that stage rather than treating all codes as one domain.

`start_ms` and `end_ms` use the local monotonic clock. They support local
time-bin aggregation; do not compare different machines' clocks without
independent alignment. The combined lease/ARM duration overlaps its component
durations and must not be added to them. ARMED-to-control-start is **not** an
observation of actual service enqueue. These intervals also do not measure
scheduler assignment-to-client-entry delay, so they cannot alone account for
all scheduler WAIT time.

## Retry and service records

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
