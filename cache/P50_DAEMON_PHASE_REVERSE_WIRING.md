# Protocol-50 daemon phase/reverse wiring slice

This slice adds the smallest production-safe bridge after the bounded
`CACHE_SESSION` handoff:

* the daemon revalidates the immutable wrapper `ConnectionLeaseId`, exact
  `Client*`, exact `MsgChannel*`, and accept-time peer credentials before it
  enters cache dispatch;
* `CacheDispatchOutcome` records both facts needed by a later phase-open
  sender: the descriptor handoff was ACKed after adoption, and the ordinary
  stream's trailing-byte barrier was proven;
* `emit_attachment_phase_open()` accepts a complete later `P50SourceArm` only
  with both facts, uses the service-global `P50HandoffAuthority`, and returns
  the exact replay-stable `ATTACHMENT_PHASE_OPEN` envelope;
* the daemon build now compiles the existing phase-open and reverse sealed-FD
  reducers, so their sealed-master/fresh-`CLOEXEC`-dup and exact-token replay
  laws are present in the production link.

## Deliberate HOLDs

The current `CACHE_SESSION` wire is an empty discriminator. It does not carry
`C_STORE_GUID`, logical job/attempt, source mode, or a source arm, and the
generic fd-handoff ACK is not an attachment-phase ACK. Therefore this slice
does **not** synthesize an arm, emit a phase-open frame on the already-closed
control connection, or move the detached daemon `Client` into
`WAITP50INPUT`. Doing any of those from the empty discriminator would invent
identity and violate the trailing-byte/ownership boundary.

The remaining production steps are intentionally held until a reviewed
source-arm/attachment transport supplies the complete arm and a live F-side
receiver owns the authenticated wrapper connection:

1. send the returned phase-open envelope on that receiver's authenticated
   connection after the exact handoff ACK;
2. enter bounded `WAITP50INPUT` with one absolute local deadline;
3. stage committed input as one sealed master and deliver a fresh
   `FD_CLOEXEC` duplicate per retry through `ReverseFdOwner`;
4. let `ReverseFdReceiverLedger` accept before ACK, preserve the same delivery
   token/deadline, and pass the one accepted descriptor to the compiler
   worker; and
5. connect that descriptor to the compiler input path without consuming legacy
   TCP/ordinary `UseCS` fields or allowing a no-input ready/fork path.

The focused runtime test proves ACK+barrier gating, exact arm projection, and
idempotent phase-open replay. Existing reverse-reducer tests remain the
authority for sealed-FD retry and exact-token replay behavior.
