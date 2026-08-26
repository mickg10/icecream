# Protocol-50 S2 restart/replay exit precursor

`p50s2restartreplay` is a bounded test seam, not production CompileFile
wiring.  It combines the existing exact supervisor lifecycle with the existing
phase-open and sealed-FD reducers and prints one explicit counter row per
kill/replay case:

* sidecar death before READY;
* sidecar death after READY and before CACHE_SESSION handoff;
* restart-budget exhaustion;
* lease withdrawal and identity/path rotation.  The fake service inherits the
  supervisor-prebound listener, proves its exact pathname association and
  `SO_ACCEPTCONN` state, and publishes both nonzero incarnation-derived C and F
  GUIDs in strict READY v2; it never binds a second listener;
* exact phase-open replay plus stale/conflicting rejection;
* descriptor adoption followed by a modeled lost-ACK reconnect, exact replay,
  conflicting replay, stale replay, and the reducer's single transition/fork
  and compile-cursor witnesses.

The harness deliberately does not invent missing product identity.  These are
explicit HOLDs until the real lifecycle is available:

* no product C/F/CompileFile wiring;
* no real daemon-to-service process kill injected between sealed-FD adoption
  and its transport ACK.

Absolute deadlines remain local monotonic test values.  No steady-clock time
point is serialized across a host boundary.
