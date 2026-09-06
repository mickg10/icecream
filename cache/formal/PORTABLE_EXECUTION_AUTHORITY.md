# Protocol-50 portable formal execution authority

This file authorizes execution of the packaged Protocol-50 formal models from
an Autotools source release that has no Git object database.  It is an
execution authority only.  It does not replace or weaken the historical S6
lineage review in `run_zstd_route_finput_composition_tlc.sh`, and it does not
make a product, profile, deployment, or live-farm acceptance claim.

`formal_aggregate_manifest.json` is the selected-lane authority.  It names
every runner, model, configuration, expected clean row, and expected mutant
invariant used by `make protocol50-formal`.  The aggregate runner authenticates
one pinned `tla2tools.jar` before starting any lane, hashes every selected
input, allocates a unique retained state directory, records exact commands and
bounded TLC state counts, and refuses missing inputs, missing row markers,
timeouts, unexpected exits, and any output containing `SKIP`.

The portable ZSTD_ROUTE/FInput lane remains bound to
`s6_mutation_manifest.jsonl`.  That manifest authenticates all 87 declared
rows and their model/configuration bytes.  The aggregate target selects the
nine review-focused rows named in `formal_aggregate_manifest.json`: core-05,
core-18, core-19, composition-general-safety, and the five composition core
safety-deletion controls.  Unselected rows are reported as not started and
cannot be presented as a complete 87-row matrix.  Every selected row must be
`pass`; a partial, held, timed-out, missing, or skipped selected row fails the
aggregate target.

Historical S6 review still uses the original default mode, pinned correction
specification digest, Git ancestry, single-parent, and clean-worktree checks.
Portable execution is enabled only by the aggregate runner through
`S6_PORTABLE_EXECUTION=1`; that mode binds provenance to packaged input hashes
instead of claiming unavailable Git lineage.
