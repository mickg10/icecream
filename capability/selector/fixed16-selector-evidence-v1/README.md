# Fixed-16 causal selector evidence

This package joins two independently replayed candidates at the frozen first
`min(112, complete TUs)` decision boundary:

- corrected stable-Root P29, from the retained run at
  `/home/ttuser/issue16-p29-prefix-state/fixed16-corrected-v1`;
- current GRZ, from the retained run at
  `/home/ttuser/issue16-selector-v1/grz-fixed16-probe-v1`.

Both probe sets reconstructed all 16 inputs exactly.  Every P29 probe is
byte-identical to the corresponding prefix of a separately executed complete
run.  The GRZ summarizer additionally rechecks the current binary and source,
manifest prefix, TU offset map, stream and curve digests, group accounting,
complete replay report, and fixed-16 source ledger.

The complete GRZ sizes used for selection remain in
`capability/grouprlz/g2-fixed112-fixed16.tsv`; that wire is unchanged by the
later demand-populated decoder correction.  Process rates in the measurement
tables are diagnostic and are not selector inputs.

`policy-b-selector-features-v1.tsv` is the similarly audited 44-cell
development matrix, enriched with the same causal P29 component/trajectory and
GRZ group-counter features.  All project generations must remain in the same
validation fold.

Key SHA-256 values:

```text
44-cell causal features       b0ab8ed4da76bf0222db136f74ed3f919bc27779d27e9b76c40864ddfe527c90
fixed-16 P29 measurements     e3242606b8f72c6b289a102cde4560068dbfecab36b13db402445c1af219c246
fixed-16 GRZ measurements     599bd8ae51147422d2103e49a99b28c0aa7ef8acad6de5db281002321a7fc4cf
fixed-16 P29 summary          a5dfea70c568d6213d36084c8ece19a30ed120eb2267f4c8a873e249540b8b57
fixed-16 GRZ summary          c0da2c49615fb14401fd1d6949b7746e002dea80ea2d52361f357397d6f6a345
fixed-16 source ledger        cf40a7ab8d286150108b94299ae8c2d762dab89d2c5c127edd83e07b5784f4f4
current GRZ source            e997b612c3445c951fa8bfc4abd2942fbad532fac85ebae5aa5fe753fe64a6b9
current GRZ measured binary   647883b7a346ae48d76ea2ac542240e5c78b4a56049372ee6067f5ec94276af9
```
