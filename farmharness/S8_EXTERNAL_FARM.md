# S8 external farm adapter

`s8_external_farm_executor.py` owns only external role placement and bounded
SSH lifecycle.  Predictive `.ii` payloads, compile-database argv/output
bindings, warm environment readiness, cache rotation, action/RAW ledgers,
overlap metrics, and finalization remain the contracts of
`s8_real_c1f1_live_runner` and `p50compilee2e-run.sh`.

The authenticated placement keeps scheduler and submission C on q3 and puts
every currently executable F relationship on q2 or research7.  Research6 is
captured in the four-host authority but remains HOLD and is rejected as an F
target.  Physical host digests must be unique, q3 can never appear in the F
map, and fresh load is limited to 0.50.  Profiles are passed through as P29,
ZSTD_TU, ZSTD_ROUTE, GRZ_RESIDUAL, or RAW_II; RAW has no cache sidecar.

External execution requires a final-head product root, fresh authority,
batch manifest, predictive plan, topology assignment, and a mature
external-capable batch command.  The adapter retains product output,
authority, role placement, scheduler/C/F work trees, service map, and logs.
It does not admit local loopback elapsed time as timing evidence.

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
/path/to/final-head/farmharness/s8_external_farm_authority.py \
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
