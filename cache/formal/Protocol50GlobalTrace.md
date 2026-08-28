# S3 global trace binding

`Protocol50Global.tla` is the bounded cross-relationship resource model. Its
Level-1 trace vocabulary is intentionally separate from the per-route
`check_trace.py` vocabulary; a row cannot be silently accepted as an unknown
global action.

| Trace action | TLA transition | Required safety edge |
| --- | --- | --- |
| `NAMESPACE_ADMITTED` | `ADMIT_NAMESPACE` | admission is refused after wrap-stop and binds the current C GUID |
| `NAMESPACE_TOUCHED` | `TOUCH_NAMESPACE` | repeated touches assign a globally increasing LRU timestamp |
| `TU_STARTED` / `TU_FINISHED` | `START_TU` / `FINISH_TU` | pins are released only at TU close |
| `ARENA_INSTALLING` | `BEGIN_INSTALL` | one free staging slot owns the key |
| `ARENA_RETRY_INSTALLING` | `RETRY_INSTALL` | crash retry starts from clean `ABSENT` |
| `ARENA_PRESENT` | `PUBLISH_INSTALL` | staged content must match canonical immutable content and both caps |
| `ARENA_PINNED` / `ARENA_UNPINNED` | `PIN_OBJECT` / `UNPIN_OBJECT` | `PRESENT ↔ PINNED` is lease-scoped |
| `INSTALL_CRASHED` | `CRASH_MID_INSTALL` | partial bytes and slot are reclaimed |
| `CONTENT_CONFLICT_FATAL` | `CONFLICTING_CONTENT` | same key/different content is terminal |
| `NAMESPACE_EVICTED` | `EVICT_NAMESPACE` | whole namespace, oldest eligible LRU only |
| `GENERATION_ADVANCED` | `ADVANCE_GENERATION` | no generation wrap |
| `GENERATION_WRAP_STOPPED` | `WRAP_GENERATION` | admission stop at terminal generation |
| `C_GUID_FLIPPED` | `GUID_FLIP` | fresh, non-aliased GUID before admission resumes |

The reverse slot edge is checked in both directions: a nonempty slot owner
must identify exactly one `INSTALLING` arena entry, and every
`INSTALLING` entry must own exactly one slot. The crash-keeps-slot mutant is
expected to fail the reverse direction. The bounded model also includes a
watchdog only for an explicitly stalled, enabled writer; it is a finite safety
check, not a claim that a fair scheduler eventually publishes every object.

`check_global_trace.py` is the independent Level-1 fixture checker. The
production `GlobalResourceModel` in `p50_slice0.*` is the executable owner
for the same cross-relationship state and emits `GlobalActionRecord` rows
through `GlobalResourceTrace`/`write_global_trace`. It carries the staged
digest from `ARENA_INSTALLING`/`ARENA_RETRY_INSTALLING` through
`ARENA_PRESENT`, binds each arena to its namespace's `(C_GUID,generation)`,
and accounts resident, staging, and total simultaneous bytes. The static
trace gate deletes or mutates one action at a time and requires rejection
through the named edge; the source census rejects a model-only change that
omits the production owner or emitter. TLC's bounded Level-2 rows use the
same action names as the TLA transitions.
Admission and every arena row also carries the current GUID and generation;
the checker requires those fields rather than silently accepting an unbound
identity. The fresh fixture repeats a touch (`n0` gets timestamp 3 after
`n1` gets timestamp 2) and then evicts `n1`, making the global-LRU ordering
observable before the generation cycle.
The two namespaces start with distinct C GUIDs and use namespace-specific
canonical content, so the same key can carry independently owned content
without aliasing. A GUID flip rejects a GUID currently owned by the other
namespace.

`global-admit-at-terminal-generation.jsonl` fixture is intentionally red: it
reaches `generation = MaxGeneration` and attempts admission before the
wrap-stop/GUID-flip sequence. `global-fresh-cycle.jsonl` is the positive
bounded witness for eviction, generation advance, wrap stop, GUID flip,
re-admission, a fresh TU, and a new installation. `ClearNamespace` resets all
lifecycle-local state needed by that cycle while retaining generation/GUID
history and audit flags.

