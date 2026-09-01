# S6 V6 formal transplant (review pending)

This is a provisional exact-file transplant onto b702a35cf4060a560135d488e51b2d39d2fd3526.
It is formal-model/test evidence only. It does not merge the divergent S6
history, alter production ZSTD_ROUTE behavior, or authorize an S6 acceptance
claim.

The source authority for every file below is commit
c06a7cfe8ae651ec16855b1f2214f31e7b1988ab (parent
dd649d40593b1b685c92ea62965072c63172af7b, tree
7eff39291dc26b5823f46ddd23b3e196166e5a87) in the existing local S6
worktree. c06a7cfe contains the final-epoch/deletion-witness correction;
dd649d40 contains the V5 checker, reachability-driver, and bounded-shape
corrections. The V5 evidence package was built from
0faffd239a360d29ad405ec257a1b35a1d991138, so these files intentionally
remain review-pending.

| Transplanted path | Source commit/blob SHA-256 |
| --- | --- |
| Protocol50ZstdRoute.tla | c06a7cfe / 2206f7157a18184fed02285934a1aafa4d014404 |
| Protocol50ZstdRouteFInputInterface.tla | c06a7cfe / 14c141bade15dead5e4c8f79395dfe5886d85d70 |
| Protocol50ZstdRouteFInputComposition.tla | c06a7cfe / 4872c4b4ea9f8c4911088403f1dfbb6c5305002d |
| run_zstd_route_finput_composition_tlc.sh | c06a7cfe / 8794fd1e2f294e8e46384460fa7fab1ddc0c3795 |
| s6_mutation_manifest.jsonl | c06a7cfe / 72df333e7cb246c869dfc75291e149b2267ab96c |

The 86 configs are the exact V6 blobs from the same source commit, preserving
the full manifest's one-config-per-row mapping. The initial focused execution
uses only core-05, core-18, core-19, the general composition row, and the five
composition safety deletion rows.

The runner has one local, review-only delta from its c06a7cfe blob: when
S6_TRANSPLANT_REVIEW=1 it requires b702a35cf4060a560135d488e51b2d39d2fd3526
to be the merge-base ancestor and requires this authority map. The normal
exact-V6-parent gate is unchanged.
The review runner also classifies TLC's initial-invariant diagnostic form
Error: The invariant of NAME is equal to FALSE as initial-invariant; this is
diagnostic normalization only and does not alter expected exits or properties.

The transplant is intentionally not a production implementation. It supplies
the missing formal assets needed to reproduce and review the named rows on
this exact b702 branch. Accepted ZSTD_ROUTE semantics remain one
long-distance state stream per C-F relationship, with tentative per-TU state
promoted only by its matching commit and exact retry/reset behavior.

## Review-only model correction

This branch adds a narrow composition-model correction after the correspondence
audit: `CoreIndependent` is disabled for an owner-cancelled final-epoch route
in `reset-required`; `FinalCancelledResetQuiescence` keeps that cancellation
outcome quiescent. Non-final cancellation still uses the existing explicit
`ClassifyFault -> ResetOffer -> ResetAck -> ResetCommitAfterOwnerCancel` path.
The invariant `TouchedCancelRequiresReset` and all production sources remain
unchanged. `Protocol50ZstdRouteFInputCompositionFinalCancel.cfg` is a focused
epoch-one regression for this correction. This is review-pending
formal-model evidence, not a production acceptance claim.
