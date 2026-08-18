# Corrected P29 fixed-16 evidence

This directory is the small, reviewable evidence package from the corrected
stable-Root P29 prefix-identity sweep.  The complete retained run, including
the raw planes and literal wires named by each identity record, remains on
`tt-quietbox2` at:

```text
/home/ttuser/issue16-p29-prefix-state/fixed16-corrected-v1
```

The sweep used the first `min(112, complete TUs)` as the causal probe.  All 16
complete replays were exact and all 16 separately executed probes were
byte-identical to the corresponding complete-program prefix.  The corrected
complete P29 total is 78,541,028 bytes.

The process timings are diagnostic two-process research measurements.  They
are not the isolated common-input throughput gate.

Key retained SHA-256 values:

```text
source codec50.cpp       89ed3b299332f90898b0bd86338052d67d686e93797d2e332080f7ed33c7c753
measured codec50 binary  6cb0d9dbbf04f2e90459a49a715e55e06308cc05b382343a460c6dd62e5b526a
fixed-16 source ledger   cf40a7ab8d286150108b94299ae8c2d762dab89d2c5c127edd83e07b5784f4f4
measurement table        e3242606b8f72c6b289a102cde4560068dbfecab36b13db402445c1af219c246
summary JSON             a5dfea70c568d6213d36084c8ece19a30ed120eb2267f4c8a873e249540b8b57
report                    8cf03feb00ee4dae47f50fe4d144304efaaebc32b14eac8b07c2951ef378746e
```

`run_p29_fixed16_identity.sh` regenerates the ledger before and after the run,
refuses source drift, and delegates every cell to
`run_p29_prefix_identity.sh`.  `summarize_p29_fixed16_identity.py` rechecks
every curve, component row, replay result, identity digest, and tooling digest
before writing the aggregate files here.
