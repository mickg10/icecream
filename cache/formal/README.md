# Protocol-50 formal lane

`Protocol50.tla` deliberately abstracts compression, timing, compiler environments, scheduler
choice, and P29 parsing. The bounded model has one C relationship, two F stores, two TUs, two
immutable keys/content values, and two session tokens per F. It covers token fencing including
an explicit stale-object callback, route reset, immediate canonical object retention, distinct
C-active/F-pending overlays, disconnect/replay, a durable F commit whose C receipt is lost, and
the exact Need/BODY/materialization prerequisites for commit.

The JSONL records both `actor` and action. TLA+ `TX_BEGIN(actor)` therefore represents the
separate C-open and F-accept transitions. `INPUT_COMMITTED` is F's durable transition;
`COMMIT_ACCEPTED` is the normal C receipt, while `LOST_COMMIT_ACCEPTED` closes the explicit
disconnect/reconnect window. `check_trace.py` keeps independent C/F state keyed by
`(C_STORE_GUID, F_STORE_GUID)`, including the original Need key set and the installed
Key64-to-content-digest mapping. Session handles bind the F-store incarnation as well as the
non-reused session serial.

Pin/evict transitions and cache-versus-existing-path result arbitration are intentionally not
claimed by M0/M1: neither has a product action in this slice. They enter the model and trace
only with the M5 eviction/legacy-retry implementation. Until then, immutable M1 objects do not
evict and compiler-result choice remains outside this cache transaction.

Run TLC with an external `tla2tools.jar`:

```sh
java -cp /path/to/tla2tools.jar tlc2.TLC \
  -metadir /tmp/icecream-p50-tlc-state \
  -config cache/formal/Protocol50.cfg cache/formal/Protocol50.tla
```
