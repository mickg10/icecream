# compression-research

Corpus-generation infrastructure + per-corpus z19-long metrics + the icecream #16 study.

- `corpus-infra/`  — `regenerate_corpuses.sh` (clone→configure→`-E`→manifest, per-corpus `recipes/`),
  `snapshot_corpuses.sh`, `preprocess_corpus.py`, `corpus-metrics.tsv`. One-command, idempotent,
  cannot clobber live corpora. Snapshots need `zstd -d --long=31` to decompress.
- `data/`     — `corpus-metrics.tsv`, `study15.tsv`, `loo-*.tsv`.
- `reports/`  — `corpus-metrics.md`, `study15-report.html`, design/analysis writeups.

Corpora (`.ii`, ~27 GiB) and snapshots are large data, gitignored; regenerate from `corpus-infra/`.
