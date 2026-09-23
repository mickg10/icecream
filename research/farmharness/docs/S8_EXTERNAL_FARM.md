# S8 external farm adapter

`s8_external_farm_executor.py` owns only external role placement and bounded
SSH lifecycle.  Predictive `.ii` payloads, compile-database argv/output
bindings, warm environment readiness, cache rotation, action/RAW ledgers,
overlap metrics, and finalization remain the contracts of
`s8_real_c1f1_live_runner` and `p50compilee2e-run.sh`.

The authenticated placement keeps scheduler and submission C on q3 and puts
every default F relationship on q2 or research7. Research6 is captured in the
four-host authority but excluded from default placement. Using it as an F
requires `--include-research6` and fresh passing authority. Physical host digests
must be unique, q3 can never appear in the F
map, and fresh load is limited to 0.50.  Profiles are passed through as P29,
ZSTD_TU, ZSTD_ROUTE, GRZ_RESIDUAL, or RAW_II; RAW has no cache sidecar.

External execution requires a final-head product root, fresh authority,
batch manifest, predictive plan, topology assignment, and a mature
external-capable batch command.  The adapter retains product output,
authority, role placement, scheduler/C/F work trees, service map, and logs.
It does not admit local loopback elapsed time as timing evidence.

The canonical S8 accuracy contract remains the four corpora in
`s8_schema.CORPORA` (32 cells with calibration/held-out validation splits).
The campaign planner also inventories corpus4, corpus5, corpus6, corpus8,
corpus9, corpus10, and corpus11 through immutable manifest-ID mappings. Those
seven project labels are emitted only with the `expanded_descriptive` scope;
they cannot enter canonical calibration, held-out loss/accuracy, or the 32-cell
matrix. Missing RAW_II witness/engine inputs remain `NOT_READY`, and a
separate live observation remains `HOLD`.

For a multi-cell campaign, use the campaign driver's per-cell refresh command
so authority freshness is measured immediately before each external cell. The
command is an argv template (not a shell string) and must contain `{output}`;
the driver supplies a unique private attempt path. `{corpus}`, `{profile}`,
`{regime}`, `{topology}`, `{cell}`, and `{authority}` are also available. The
command's argv, exit status, stdout, stderr, and hashes are retained in the
cell attempt. A static `--external-farm-authority` may be supplied instead for
a single-cell/API run, but it cannot be combined with a refresh command or
provider.

For example, pass an authority capture command with its required remote-root
and descriptor arguments:

```sh
--mode external-farm \
--external-farm-authority-command "python3 \
/path/to/final-head/research/farmharness/s8_external_farm_authority.py \
  --execute --root /path/to/final-head --output {output} \
  --descriptor-dir {output}.descriptors \
  --idle-cooldown-timeout 30 --idle-cooldown-interval 2 ..."
```

The absolute generator path above must be from the same pinned final-head
checkout used by the campaign, and `{output}.descriptors` keeps descriptors
private to that attempt. `--idle-cooldown-timeout` is a bounded opt-in window
(seconds)
for the named transient `placement:host_not_idle:<host>` disposition; the
generator recaptures all four hosts at the requested interval when the
failing host is required by the selected placement (q3 is always required).
An excluded research6 HOLD is retained without blocking the default map.
Every wait diagnostic is emitted on stderr. The default timeout is `0`
(single-shot), and identity, binary, schema, capture, and other placement
errors never retry.
