# S8 method-matrix reporter

`s8_method_matrix_report.py` is a read-only aggregation boundary for verified
simulator experiments.  It calls `s8_method_matrix_simulator.verify_experiment`
for every input before consuming `manifest.json`, `summary.json`, and
`occurrences.jsonl`; it never starts a codec, native binary, farm, or live
endpoint.

The accepted dimensions are exactly `C1F1/100000` and `C1F20/40`, with depths
`100`, `200`, `full-1`, and `state-carrying-full-2`.  A key is the tuple
`(topology, depth, pass)` and duplicate keys are rejected.  C1F20 is reported
as 20 relationships with 40 capacity slots.  Full-2 requires the verified
predecessor-continuity marker and relationship state in the source experiment.

Usage:

```text
PYTHONPATH=research/farmharness python research/farmharness/s8_method_matrix_report.py \
  --output-root experiments/matrix \
  experiments/run-a experiments/run-b
```

The command creates a collision-safe UTC timestamped directory containing
`results.jsonl`, `summary.json`, `matrix.csv`, and `table.md`.  Each result has
one of the seven method names (`RAW_II`, `ZSTD_TU`, `P29`, `GRZ_RESIDUAL`,
`ZSTD_ROUTE`, `ZSTD_COHORT`, `ZSTD_GLOBAL`) and explicit status/reason fields.
RAW_II retains only authenticated source byte totals: encoded bytes, wire
bytes, timing, and compression ratios are null.  Product wire and timing are
reported only when every row has a verified `product_transaction`; codec-only
rows are not upgraded to wire witnesses.  COHORT and GLOBAL remain optional
and unavailable statuses do not fail core completion.

`summary.json` reports `COMPLETE` only when all eight topology/depth
dimensions are covered and every source experiment has all five core methods
ready.  Otherwise it reports `INCOMPLETE_REQUESTED_MATRIX` and lists missing
dimensions.  The loss-curve field is an unfitted, transparent tabular point
list by depth; no interpolation or historical/S5/S7 data is combined.
