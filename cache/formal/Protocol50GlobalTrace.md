# S3 global trace binding

`Protocol50Global.tla` is the bounded cross-relationship resource model. Its
Level-1 trace vocabulary is intentionally separate from the per-route
`check_trace.py` vocabulary; a row cannot be silently accepted as an unknown
global action.

| Trace action | TLA transition | Required safety edge |
| --- | --- | --- |
| `NAMESPACE_ADMITTED` | `ADMIT_NAMESPACE` | admission is refused after wrap-stop |
| `NAMESPACE_TOUCHED` | `TOUCH_NAMESPACE` | monotonic global LRU clock |
| `TU_STARTED` / `TU_FINISHED` | `START_TU` / `FINISH_TU` | pins are released only at TU close |
| `ARENA_INSTALLING` | `BEGIN_INSTALL` | one free staging slot owns the key |
| `ARENA_RETRY_INSTALLING` | `RETRY_INSTALL` | crash retry starts from clean `ABSENT` |
| `ARENA_PRESENT` | `PUBLISH_INSTALL` | canonical immutable content and both caps |
| `ARENA_PINNED` / `ARENA_UNPINNED` | `PIN_OBJECT` / `UNPIN_OBJECT` | `PRESENT ↔ PINNED` is lease-scoped |
| `INSTALL_CRASHED` | `CRASH_MID_INSTALL` | partial bytes and slot are reclaimed |
| `CONTENT_CONFLICT_FATAL` | `CONFLICTING_CONTENT` | same key/different content is terminal |
| `NAMESPACE_EVICTED` | `EVICT_NAMESPACE` | whole namespace, oldest eligible LRU only |
| `GENERATION_ADVANCED` | `ADVANCE_GENERATION` | no generation wrap |
| `GENERATION_WRAP_STOPPED` | `WRAP_GENERATION` | admission stop at terminal generation |
| `C_GUID_FLIPPED` | `GUID_FLIP` | fresh GUID before admission resumes |

`check_global_trace.py` is the independent Level-1 checker. The static trace
gate deletes or mutates one action at a time and requires rejection through
the named edge. TLC's bounded Level-2 rows use the same action names as the
TLA transitions; the model itself remains model/test only and does not add a
product trace emitter or persistence implementation.
