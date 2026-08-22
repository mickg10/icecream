# Issue 16 provisional foundations convergence

This local convergence is provisional pending the final BigOracle verdict for
the zero-wire correction. It is a testable integration head, not release or
merge authority by itself.

## Authority map

- Product, Protocol 50, package dependency, and C++23 portability authority:
  `d03c750613a6f1e914f6879ad3ec8e8f486885ab`. It contains product head
  `c9488d74d0bfbd8dd6ff8d4d56aaad74c2fe5201` and package head
  `0d68b5fa352a14b384921849b1094e63a93a1283`.
- Remote zero-wire assignment correction authority:
  `b5b7676113e321c298379f6c3ad70ceb988a3157`. It and `d03c7506` are exact
  siblings on `c9488d74`.
- Protocol 50 assignment-identity formal authority:
  `1295be1880509419b1ec65cad9093dd5c1564c80`.
- Accepted R6 input-seam and terminal-status authority:
  `7832dcd2a8d16be5f170e5407ae17fc22d02b9e0`, retaining accepted seam ancestor
  `b247bf7b4db5f83097403430f6a6fb61d4f6e5b4`.

The merge chain first joins the two product siblings, then merges the formal
authority, then merges the accepted R6 authority. No authority is squashed or
copied without ancestry.

## Merge boundary

The formal authority remains under `cache/formal/`. The R6 authority adds the
legacy client input pump, daemon compiler-input source, their focused tests,
and terminal `STATUS_TEXT` ownership. The only textual conflicts were
additive unions in `doc/Makefile.am` and `unittests/Makefile.am`.

Protocol 50 assignment identity, corrected remote zero-wire refusal, scheduler
eligibility, InputRecord/cache ownership, and package behavior remain selected
from their authorities above. This merge does not add cache advertisement,
M3 behavior, or a `1.5.90` release-version change.
