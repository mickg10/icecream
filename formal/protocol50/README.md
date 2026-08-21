# Withdrawn Protocol-50 exploratory model

This directory is **not** the canonical Protocol-50 formal target.

A subsequent line-by-line review found that this exploratory 697-line model still retained several states that its accompanying review comment claimed had been removed:

- `ComputeNeed` and `PinClosure` remained separate, preserving an eviction TOCTOU race;
- `AckCommit` and `Disconnect` did not carry the current session token;
- `ResetHistory` did not guard C's retained active transaction;
- running compiler attempts were not represented by a monotonic authorization fact across an F-cache restart.

No TLC result is claimed for the corrected semantics merely because this older model once explored successfully.

The canonical product-adjacent formal suite is under:

```text
cache/formal/Protocol50.tla
cache/formal/Protocol50JobLifecycle.tla
```

on the `implementer/issue16-p50-vertical` lineage. The focused correction and review branch is:

```text
bigoracle/issue16-p50-job-lifecycle-formal
```

Do not use the files in this directory for implementation trace refinement, merge acceptance, or protocol claims. They are retained only as review history until the coordinator decides whether to delete them.
