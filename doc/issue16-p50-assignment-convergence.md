# Protocol-50 assignment convergence mapping

This convergence preserves three independently reviewed heads before the
Protocol-50 assignment-identity product change:

| Authority | Exact head | Owned contribution |
|---|---|---|
| Protocol-49 product | `e05f6d0a02340a65f110a28b63e9a339cb8983bc` | Ordered scheduler/worker assignment controls, four operator modes, and the claimed-assignment retention correction. |
| Closed InputRecord | `0f6736ee4a23e7b8e1675a4cd80784a39e16188e` | Bounded closed-record behavior and its duplicate/reopen tests. |
| Assignment formal models | `67917f45ead6ce577063c9c5606b2aa3b35edb84` | Assignment, actor-ordering, delivery, mutant, and witness models including the delivery-time `Revoked` versus `ClaimedOrLater` correction. |

The first explicit merge is
`58efbb8079999641841cde88d8838ede1677e5cb`, with parents
`e05f6d0a02340a65f110a28b63e9a339cb8983bc` and
`0f6736ee4a23e7b8e1675a4cd80784a39e16188e`. Its only overlap resolution is
the additive union in `unittests/Makefile.am`: all Protocol-49 product tests
remain registered and the closed-record `p50inputrecordreopen` test is added.

The second explicit merge has first parent
`58efbb8079999641841cde88d8838ede1677e5cb` and second parent
`67917f45ead6ce577063c9c5606b2aa3b35edb84`. The formal files are additive.
`cache/Makefile.am` retains the product/InputRecord sources and appends the
formal distribution list; `cache/formal/run_tlc.sh` appends the independently
approved assignment rows to the existing matrix. This mapping document is
added to the distribution list by the same convergence merge.

No product source is reconciled by choosing one head over another. The
Protocol-50 wire/product implementation is a separate successor commit so its
review can distinguish imported authority from new behavior.
