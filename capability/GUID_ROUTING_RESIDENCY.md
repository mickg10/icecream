# GUID-native routing and residency contract

This document is the local-oracle contract for the Issue #16 GUID-routing
replay.  It implements BigOracle's corrected identity ruling while tightening
two terms that would otherwise make the experiment ambiguous:

1. a matching generation is evidence of useful residency, not proof that F
   owns every object needed by the next TU;
2. `C_GUID` belongs to C, while `FCacheEpoch` belongs to F.  An F can retain
   state for many C GUIDs and therefore does not advertise one global C GUID.

The exact codec dialogue remains F-authoritative.  Scheduler state affects
placement and byte cost only; Root/Need/Fill still determines the actual
missing set.

## Identity and lifetime

| Name | Owner | Lifetime | Meaning |
|---|---|---|---|
| `C_GUID` | long-lived C cache service | ordinary builds and generation rotations; changes on C service restart/replacement | scheduler affinity family and owner of the object namespace |
| `SourceGeneration` | C cache authority | one immutable ordinal namespace; changes on rotation/compaction | exact namespace for typed object ordinals |
| `ObjectKey` | C and F codec stores | one SourceGeneration | `(SourceGeneration, ObjectKind, generation-local u32 ordinal)` |
| `TUKey` | C | stable across rebuild order and ordinary source edits | placement key inside one C GUID home set |
| `FCacheEpoch` | F cache service | one F cache incarnation; changes on F restart or whole-cache reset | invalidates every scheduler residency record from the old F incarnation |
| `ResidencySequence` | F | monotonic within one FCacheEpoch | orders cache updates so a delayed older update cannot recreate removed residency |

Repository, project, path, compiler profile, and configuration are not cache
identity.  Target/environment/profile remain eligibility inputs.  They may
partition the eligible subset of a C GUID home set, but they must neither join
different C GUIDs nor split the identity of one C GUID.

`TUKey` must not be the physical or chronological TU ordinal.  The replay must
derive it from stable C-side TU lineage and retain it across standard, reverse,
shuffle, unchanged rebuild, header edit, generated edit, and revert cases.
Collisions only change placement quality; they do not name codec objects.

## Scheduler residency record

The scheduler's compact placement record is:

```text
ResidencyKey = (F identity, FCacheEpoch, C_GUID, SourceGeneration)

ResidencyValue = {
    ResidencySequence,
    resident Region/public-Line/Block counts and bytes,
    last committed TUKey,
    last successful cache commit/touch,
    optional C-to-F Fill bytes paid by that commit
}
```

The F cache itself remains richer and exact: it stores the typed-ordinal
presence vectors.  The scheduler does not need object-ID lists.

Do not call the scheduler predicate `ExactWarm`.  The exact predicate in the
corrected ruling:

```text
C_GUID match && SourceGeneration match && FCacheEpoch match
```

proves only `GenerationResidentCandidate`.  After one TU, after partial
eviction, or after a sparse workload, many objects required by the next TU may
still be absent.  The replay can compute the exact missing-byte cost from its
full simulated F state; the product scheduler has only a residency estimate.

```text
GenerationResidentCandidate(job, F) :=
    job.C_GUID == residency.C_GUID
    && job.SourceGeneration == residency.SourceGeneration
    && residency.FCacheEpoch == F.current_cache_epoch

ActualMissing(job, F) :=
    dependencies(job) - exact_typed_objects_resident_at_F
```

Only `ActualMissing` determines Fill.  G2/G3/G4 may use the first predicate and
the compact summary; G5 uses the second as its offline cost input.

## Authoritative update and invalidation rules

1. **C relationship start.** C supplies `C_GUID` and latches one
   `SourceGeneration`.  A new C service instance creates a new `C_GUID`.
2. **F login.** F supplies its current `FCacheEpoch`.  Scheduler discards every
   record for older epochs of that F.
3. **TU prepare.** No scheduler credit is earned.  Prepared state can still be
   rolled back.
4. **F cache commit.** After F commits the Fill/Root transaction, it advances
   `ResidencySequence` and may publish the resulting residency summary.  This
   event, not compiler success, establishes cache residency.  A compiler can
   fail after the cache has legitimately retained the definitions.
5. **Ordinary touch.** F may coalesce updates, but a later update must carry a
   higher sequence and a state no older than the reported commit.
6. **Partial eviction.** F publishes the reduced summary with a higher
   sequence.  Generation match remains a useful hint but never implies full
   coverage.
7. **Whole-generation eviction.** F publishes removal of
   `(C_GUID, SourceGeneration)` with a higher sequence.  Scheduler removes that
   placement immediately.
8. **Per-C shard reset.** Remove every generation for that C GUID on that F.
   A per-shard epoch may be added later if measurement warrants it; the minimal
   protocol can use ordered removals.
9. **Whole F cache reset or F restart.** Change `FCacheEpoch`; all earlier
   records become ineligible in one operation.
10. **C generation rotation.** The old generation may stay resident and serve
    in-flight or explicitly old-generation work.  It gives no exact-object
    credit to the new generation because ordinals are generation-local.
11. **C restart/replacement.** New `C_GUID`; no old placement is inherited even
    if project/profile strings happen to match.
12. **Scheduler reconnect/restart.** Either rebuild records from F summaries or
    begin without residency credit.  Never infer residency from project names.

Updates must be ordered by `(F identity, FCacheEpoch, ResidencySequence)`.
An update from an old F epoch or a non-increasing sequence is ignored.  This
ordering is needed even though a stale placement can only waste transfer: it
keeps the replay metrics and the scheduler's cache-opening decisions coherent.

## Per-TU exact transition ledger

`cap_m5_stream --cache-curve-out FILE` exports one audit row per committed TU.
The option is intentionally separate from `--curve-out`: it scans receiver
mirrors and must remain disabled for rate gates.
`verify_m5_cache_curve.py` independently checks schema, chronological TU
coverage, per-F state chaining, Need/install equality, and every count equation.
When this mode is enabled, each F also sends its actual before-Fill,
after-commit, and after-eviction store digest over a dedicated local audit
pipe. C requires all three F digests to equal its receiver mirror. The audit
pipe is not a protocol/network lane and none of its bytes enter the C-to-F or
F-to-C ledger.

Each row contains:

```text
TU and assigned F
cache counts + typed-set fingerprints before Root/Fill
Need Region count
installed Region/public-Line/Block/path counts
cache counts + fingerprints after Fill
evicted Region/public-Line/Block counts
cache counts + fingerprints after eviction
per-F chain closure and count-accounting closure
C-mirror/F-store equality at all three boundaries
```

For every row:

```text
before(i,F) == after(previous committed TU on F)

filled.regions = before.regions + installed.regions
installed.regions = Need.regions
filled.public = before.public + installed.public
filled.blocks = before.blocks + installed.blocks
filled.paths = before.paths + installed.paths

after.regions + evicted.regions = filled.regions
after.public + evicted.public = filled.public
after.blocks + evicted.blocks = filled.blocks
after.paths = filled.paths
```

The existing direction curve independently requires:

```text
CtoF = Root + Fill + CControl
FtoC = Need + FControl
Wire = CtoF + FtoC
```

Together, the two ledgers bind each placement decision to its physical C-to-F
cost and to the exact evolution of that destination's cache mirror.

## G0-G5 replay contract

| Row | Placement information | Required interpretation |
|---|---|---|
| G0 | existing FASTEST and ROUND_ROBIN controls | no cache-residency preference |
| G1 | `C_GUID` family only | compact persistent home set, no generation credit |
| G2 | G1 plus matching `SourceGeneration` and current `FCacheEpoch` | generation-resident estimate, not guaranteed complete coverage |
| G3 | G2 plus minimum capacity-sufficient home set | open the fewest cache domains that satisfy requested slots and eligibility |
| G4 | G3 plus stable `TUKey` rendezvous placement | retain TU lineage when the home set is unchanged; minimally remap when it changes |
| G5 | offline state-aware assignment | minimize actual C-to-F missing bytes under the same capacity and makespan model |

G5 must be defined carefully.  For a small fixture, enumerate or solve the full
assignment space and call the result exact.  For full corpora, an unproven
heuristic must be labelled an upper bound, not an exact oracle.  A useful full
trace bound is the best of multiple deterministic searches plus a separately
reported relaxation/lower bound.

Every G row must replay actual cache state in scheduler order.  It is invalid
to score each TU against a fixed initial cache or to let a destination receive
credit before its prior transaction commits.

Required scenarios are the BigOracle list: cold generation, one warm F plus
cold alternatives, same GUID/new generation, new GUID/same project, one GUID
across multiple projects, F epoch change, generation eviction, heterogeneous
capacity at 30 slots, unchanged rebuild, header edit, generated edit, and
revert.  Report physical C-to-F and Fill bytes, cache domains opened,
generation-match assignments, actual missing bytes, stale-generation choices,
replication, TUKey retention, queue delay, compile makespan, and byte/time
regret against the correctly labelled G5 result.

## Initial decision rule

Start with the smallest mechanism that can be falsified by the replay:

1. filter by ordinary execution eligibility;
2. reuse current-generation members of the C GUID home set when they have
   capacity;
3. use the minimum capacity-sufficient subset;
4. choose within that subset with stable `TUKey` rendezvous placement;
5. open a new cache domain only when queue-delay/makespan improvement repays
   its measured Root/Fill price.

If G3 or G4 is within 10% of the correctly defined G5 byte and time objectives,
stop scheduler-policy expansion.  The next optimization target is then the
per-width Root/Fill lower bound, not a shared cross-F object store.
