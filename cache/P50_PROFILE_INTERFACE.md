# Protocol-50 transaction profile interface

`ProfileDialogue` is the transaction-engine boundary for profile-specific
transaction state. `P50ServerEndpoint::Impl::Pending` owns the move-only
type-erased object and obtains it from `make_profile_dialogue(ProfileId, ...)`.
The endpoint therefore retains route identity, publication ordering, and
aggregate byte reservations without naming a concrete codec dialogue.

The vtable forwards profile-owned `TX_BEGIN`, component admission,
materialization, visible commit, disconnect, and state observations. The
current `ZSTD_TU` adapter is the only registered implementation. A future
profile adds an adapter and factory entry without changing pending ownership
or the endpoint reducer.

This is an internal production seam: it introduces no wire fields, changes no
digest or identity law, and does not alter the existing ZSTD_TU exception or
publication behavior. Unsupported or unnegotiated profiles fail closed at the
factory.
