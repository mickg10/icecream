# Corrected P29 native-nine holdout evidence

This is the small review package for the nine native projects that were not
used to choose or tune `causal-selector-policy-v1`.  The complete retained run,
including all raw planes, literal wires, component curves, logs, and timings,
is on `tt-quietbox2` at:

```text
/home/ttuser/issue16-p29-prefix-state/native9-corrected-v1
```

The sweep covers 30,194 TUs and 221,650,774,134 raw bytes.  All nine complete
P29 replays were exact, and every separately executed TU112 probe was
byte-identical to the corresponding prefix of its complete run.  Complete P29
wire is 132,291,209 bytes; total probe wire is 13,835,928 bytes.

The policy thresholds and development-input hashes were already pushed in
selector commit `66e0e02` before this run began.  This evidence must not be
used to retune that policy before its frozen prediction is scored against the
independent GRZ/zstd replay.

Key SHA-256 values:

```text
source codec50.cpp       89ed3b299332f90898b0bd86338052d67d686e93797d2e332080f7ed33c7c753
measured codec50 binary  6cb0d9dbbf04f2e90459a49a715e55e06308cc05b382343a460c6dd62e5b526a
native-nine ledger       36399fa6e9545c2a4888f9518e3a69de29923cdf25d1eb997d53975bfd9b0937
measurement table        cde0f70f0287517b8a18ae5399d3a4b12c90db4a2995b731770a27bd67d8cf31
summary JSON             3b0e178cc3edca82d11b4f294c13796dc4d2815cdaa91e0a9bf86fe0fa969cf9
report                    77b54336b4ff8d6bde2d26fe6422febc548b1fb2d5da1c0fba48ecd17aafe85d
```

Process timings are diagnostic two-process research measurements.  They do
not establish the one-pass, overlapped rate gate.
