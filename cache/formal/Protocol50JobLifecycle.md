# Protocol-50 job-lifecycle model

`Protocol50.tla` verifies the cache transaction, immutable-object publication, session fencing, replay, and the durable-F/lost-C-acknowledgement window. `Protocol50JobLifecycle.tla` is a deliberately separate and smaller model for the compiler-job layer that consumes an exact committed input.

It models:

- two independent F-store incarnations;
- two compile attempts for the same logical TU;
- P50 and established/legacy input as mutually exclusive attempt modes;
- attachment before or after exact input commit;
- compiler environment readiness as an independent join;
- compiler-job cancellation and restart;
- a late result from a cancelled attempt;
- exactly one accepted result;
- committed-input eviction only when no eligible waiting/running P50 consumer exists;
- F restart invalidating only that F's input and waiting/running attempts.

The model intentionally does **not** duplicate DICT/Need/Fill or lost-commit mechanics; those remain in `Protocol50.tla`. The product trace should eventually contain both cache-transaction actions and the job actions needed to refine this model:

```text
ATTEMPT_STARTED
ENVIRONMENT_READY
LEGACY_INPUT_READY
COMPILER_STARTED
ATTEMPT_CANCELLED
COMPILER_FINISHED
LATE_RESULT
RESULT_ACCEPTED
COMMITTED_INPUT_EVICTED
F_STORE_RESTARTED
```

The key product requirement exposed by this model is that `INPUT_COMMITTED` must create a retained, attachable input record keyed by `(C_STORE_GUID, TU_SEQ)`. A compiler-job restart is a new attempt consuming that record; it is not a new cache transaction merely because the job child changed.

Run:

```sh
java -cp /path/to/tla2tools.jar tlc2.TLC \
  -metadir /tmp/icecream-p50-job-tlc-state \
  -config cache/formal/Protocol50JobLifecycle.cfg \
  cache/formal/Protocol50JobLifecycle.tla
```

This branch adds only the model and its configuration. It has not been independently TLC-run by BigOracle's connector environment; the coordinator should run the command above before merging it into the implementation branch.
