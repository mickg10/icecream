# Protocol-50 authoritative public-session join

The F daemon registers a complete cache-session arm binding when it accepts
`P50_SOURCE_ARM` and creates the live `WAITP50INPUT` owner. A later public
`CACHE_SESSION` claim may reserve that exact registration; it cannot create an
owner from wire fields, arrival order, a singleton assumption, or an internal
F connection identifier.

Observation monotonicity is consumed at WAIT registration time. This matters:
two live jobs can arm as observations 100 and 101 and their public cache
connections can arrive 101 then 100. Both exact claims remain valid. Once a
WAIT registration is retired, its observation is never reused in that F daemon
incarnation, so a delayed claim becomes a lookup-only refusal. An unseen owner
retirement installs an exact `(daemon_generation, connection_sequence, client)`
fence in a bounded table; lower still-live connection sequences are unaffected,
and fence exhaustion fails closed rather than forgetting a stale owner.

The wire carries a proof scoped to the exact C control launch and arm
observation. A persistent C-role `P50CacheSessionAttemptAuthority` burns at
most two capabilities for that scope, and the reducer consumes the proof before
reserving an attempt. A nonzero ordinal, a forged capability, or a capability
from an independently reset authority is not sufficient.

The F-local `ConnectionLeaseId` and `client_id` never cross the C-to-F wire.
They are stored only in the registered row and revalidated by the daemon at
each private-connect and descriptor-release boundary.
`P50CacheSessionOwnerAuthority` is mandatory: production implements it with
the current `ConnectionLeaseRegistry`, exact Client/channel/peer identity and
cache-eligibility checks. The callback receives the table's complete current-F
incarnation and must compare it to the currently published listener. A merely
nonzero owner value cannot register or reserve a row. The table also binds one
local structured-READY lease observation, which rotates with F listener
replacement and never crosses the public wire. C and F StoreIdentity GUIDs
must have different 127-bit roots; their different role bits alone are not
evidence of independence.

`P50CacheSessionOwnerAuthorityAdapter` is the production-shaped bridge: its
owner resolver supplies the exact Client, MsgChannel, and peer credentials,
its READY provider supplies the current structured-READY incarnation, and it
calls `ConnectionLeaseRegistry::revalidate` before admitting the owner. The
active daemon call site remains HOLD until the live F lane supplies those
providers and consumes the reducer's decision.

The registration deadline is mechanically `now + source_budget_msec`, with a
checked overflow path; callers cannot select a longer absolute deadline.
Every admission pass first sweeps expired `Armed` and `AttemptReserved` rows,
so a silent pre-detach WAIT cannot hold capacity until an explicit
expire/retire callback. Detached, in-flight, and reconciliation rows are
never swept; post-detach uncertainty remains nonterminal reconciliation.

Pre-detach failure can reopen one attempt within the original deadline. After
descriptor detach, cancellation or expiry enters `ReconcileRequired` and
suppresses delivery. That state is deliberately nonterminal: only an exact
endpoint settlement can prove no commit or report canonical `committed_input`.
A late committed witness remains authoritative after the local deadline.

This reducer does not yet change the production `CACHE_SESSION` message or
detach a descriptor. Those call sites remain HOLD until the wire and live
F-arm successors converge on the same binding.
