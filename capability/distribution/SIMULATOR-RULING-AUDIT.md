# Issue 16 simulator ruling audit

This audit follows BigOracle comment
[`5365968429`](https://github.com/mickg10/icecream/issues/16#issuecomment-5365968429).
The common distribution simulator is Python. The physical P29 and GRZ codec programs it
measures are C++ programs. The browser dashboard is a separate workstream and is not part of
this change.

The primary simulation assumption is deliberately narrow:

```text
all selected TUs are available
compiler environments are already resident
physical codec bytes are exact
each F has independent object and route state
compile durations come from the selected trace
```

Producer timing is not part of this model.

## Ordered ruling audit

| Order | Required addition | State at `c697107` | State in this branch |
|---:|---|---|---|
| 1 | static stable/dense routing with exact P29/GRZ bytes | Physical ledgers were exact, but their route map came only from a compile-only dynamic assignment. The schema named `rendezvous` without implementing it. | Complete. A stable compile identity is bound to one F inside a configurable dense home frontier. Both physical builders consume that route map, and replay refuses drift. The real-codec smoke and the full corrected Firefox round-robin/`k1`/`k2`/`k3`/`k4`/`k8`/`k20` sweep pass. See `FIREFOX-STATIC-ROUTING-SWEEP-RESULTS.md`. |
| 2 | F decode/install/materialize/verify and compiler-pipe stages | Missing. P29 close delivery and GRZ frame delivery immediately meant input ready. | Still missing; this is the next simulator implementation slice after the exact width sweep. |
| 3 | compiled-result return traffic | Missing. | Still missing. Add measured per-TU result bytes only after the F input stages are explicit. |
| 4 | optional `ENV_ENSURE` | Missing, and the primary rows correctly assume resident environments. | Complete in the additive R4 minimum experiment core. Primary rows remain resident. The explicit absent-environment stress path is single-flight per C/F route, overlaps input traffic, gates compile on environment plus input readiness, and keeps environment bytes outside the source score. |
| 5 | restart, eviction, and reroute | Missing. | Still missing. Add only after stable routing and explicit F stages close. |

The static-routing branch originally stopped at the earliest missing slice. The later R4 minimum
experiment-core work freezes the mode-neutral manifest, execution header, identity/accounting
fields, replay closure, and explicit environment stress path without claiming that ordered F
stage item 2 or result-return item 3 is complete. It does not introduce an online receiver-state
selector, a spill heuristic, or restart behavior.

The corrected R4 closure gate binds scenario workload identities to actual per-TU content,
requires stable codec/session/nonce route identity, includes environment bytes in exact-route
network totals while retaining the source score separately, and independently replays the
retained event/resource/queue summaries. These are experiment-contract corrections; they do not
advance the ordered simulator stages above.

## Static routing contract

A scenario selects the policy with:

```json
{
  "scheduler": {
    "ready_job_policy": "fifo-release",
    "placement_policy": "rendezvous",
    "dense_frontier_workers": 4
  }
}
```

`dense_frontier_workers` must be between one and `workers.f_count`, and is valid only with
`rendezvous` placement.

The mapping is deterministic and has two levels:

```text
affinity family = (environment, workload, compiler profile)
home frontier   = top k Fs by stable rendezvous score of the affinity family

stable TU identity = (environment, workload, source_job_id)
destination F      = top rendezvous score inside the home frontier
```

The build number and chronological logical index are excluded from the TU identity. Therefore an
unchanged compile identity returns to the same F on later builds. Different workload families may
select different home frontiers, distributing independent projects without spraying each project
over the entire farm.

This row is static: if the selected F has no input-staging capacity, the TU waits. It does not
silently spill to another F. That makes it an exact density/affinity control rather than an
unlabelled adaptive policy.

Identity and order are now separated at the simulator boundary:

```text
TU_SEQ  allocated when a prepared input is released by its C
REL_SEQ allocated when that TU is dispatched on one C/F relationship
```

Every C receives a complete contiguous TU sequence. Every relationship receives its own complete
contiguous REL sequence. A relationship is no longer required to be a sorted projection of
`TU_SEQ`; route-local admission order is authoritative.

## Exact physical binding

Both physical builders first run the common compile-only scheduler under the selected scenario.
The resulting static destinations are then materialized by the real codec:

```text
scenario + static route map
    -> exact P29 typed C-to-F/F-to-C streams per populated route
    -> or one exact GRZ current-TU frame stream per populated route
    -> physical ledger bound to scenario SHA-256, worker, TU_SEQ, and REL_SEQ
    -> common simulator replay
```

The physical adapter still rejects a different worker, TU identity, route sequence, payload
digest, reconstruction status, or directional byte total. `summary.json` and every JSONL snapshot
now include the routing algorithm, frontier, selected home set, stable identity definition, and
assignment counts. A structured `route-bound` event is emitted for every static TU before its
dispatch.

## Real-codec acceptance cell

The checked-in fixture has one C, four Fs, a two-F dense frontier, four stable TUs, and one cold
plus one unchanged warm build. Run both real codecs with:

```bash
python3 capability/distribution/run_static_routing_smoke.py \
  --p29-codec /path/to/codec50-sink \
  --grz-codec /path/to/grz2g \
  --out /path/to/empty/result-directory
```

The retained run is:

```text
/tanksmall/scratch/ictmp/issue16-results/static-routing-smoke-simulator-research-20260821-v3/
```

| codec | jobs | selected Fs | C to F | F to C | simulated generation sum | checks |
|---|---:|---|---:|---:|---:|---|
| P29 | 8 | F2, F3 | 1,289 B | 500 B | 7,408,960 ns | exact reconstruction, typed replay, byte/time closure, stable rebuild mapping, eight JSONL route events |
| GRZ | 8 | F2, F3 | 2,576 B | 0 B | 6,420,608 ns | full decode, deterministic retry, prefix decode, byte/time closure, stable rebuild mapping, eight JSONL route events |

The retained `smoke-summary.json` SHA-256 is
`78ebddea2b872b1792a177e1cef51dd232f0856b593c283a21a24c3c3c59d9c5`.
The exact codec executables are retained under
`/tanksmall/scratch/ictmp/issue16-results/static-routing-codecs-simulator-research-20260821/`;
their SHA-256 values are `827241179368a454616d7adfc76472fd99fa827703458cea3bd0217bf08d61be`
for P29 and `018bef04367819aa34712241cae380e8ac07371610c410a4be2b7451b15b3c72`
for GRZ.
These tiny sizes validate the machinery; they are not a codec ranking.

## Completed width experiment and next slice

The corrected cold-plus-four-warm Firefox width sweep is complete. Every routing/codec cell has
its own scenario-bound real-codec ledger and corrected common-simulator replay. The exact byte,
phase, build, time, resource, closure, and artifact results are in
`FIREFOX-STATIC-ROUTING-SWEEP-RESULTS.md`. P29 `k8` is the selected primary row: it is both smaller
and faster than the P29 round-robin and `k20` rows in the current model.

The next coherent simulator slice is now ruling item 2: explicit F
decode/install/materialize/verify/compiler-pipe stages. Preserve the exact source ledgers and route
assignments, add measured per-TU stage work and JSONL state, and rerun P29 `k8`, P29 round-robin,
and GRZ `k8`. Do not introduce preprocessing. Compiled-result return traffic remains the following
separate slice.
