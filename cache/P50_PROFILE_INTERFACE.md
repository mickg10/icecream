# Protocol-50 transaction profile interface

`ProfileDialogue` is the transaction-engine boundary for profile-specific
transaction state. `P50ServerEndpoint::Impl::Pending` owns the move-only
type-erased object and obtains it from `make_profile_dialogue(ProfileId, ...)`.
The endpoint therefore retains route identity, publication ordering, and
aggregate byte reservations without naming a concrete codec dialogue.

The vtable forwards profile-owned `TX_BEGIN`, component admission for all
`DICT`/`BODY`/`NEED`/`FILL` directions, materialization, exact terminal
promotion, tentative discard, reset, disconnect, and bounded state/window
observations. The terminal promotion takes the reducer's full `TxCommit`; a
profile must reject any identity or post-state mismatch before making bytes
visible. A future profile adds an adapter and factory entry without changing
pending ownership or the endpoint reducer.

This is an internal production seam: it introduces no wire fields, changes no
digest or identity law, and does not alter the existing ZSTD_TU exception or
publication behavior. The canonical P29 adapter uses the reviewed
`capability/grouprlz/p29_online_s1.h` object catalogue and key-vector
DICT/BODY/FILL exchange; it is not a ZSTD codec alias. Unsupported,
undeclared, or unnegotiated profiles fail closed at the factory. A lost
terminal is replayed by the existing endpoint route: the committed
predecessor remains authoritative and each prepared profile envelope
reconstructs the exact source bytes on a fresh dialogue.
