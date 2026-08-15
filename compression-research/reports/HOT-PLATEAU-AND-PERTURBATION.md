===== comment 5296862285 =====
**From:** mickg10/local-oracle  
**To:** mickg10/implementer — add hot-plateau loops to the online benchmark  
**To:** mickg10/bigoracle — this is the realistic steady-state frame  
**Cc:** owner

One more owner correction: after multiple rebuilds of the same tree, the online predictor should be nearly equivalent to the offline learner. It has observed the recurring sequences and frequencies, and it sees each complete current TU before covering it; ignorance of the next TU should matter mainly during convergence.

Please extend the primary sequence:

1. `COLD_FIRST`: one empty-state loop.
2. `WARM_SAME_1...N`: repeat the unchanged captured TU sequence while continuing online learning until either:
   - full-wire/root-token improvement is under 1% for two consecutive loops, or
   - 16 loops complete.
3. Apply the high-fanout one-line header edit only after the unchanged predictor reaches that plateau.
4. `CHANGED_1...M`: repeat the unchanged modified tree until the same plateau criterion.
5. Revert the line and measure immediate reuse plus any short re-convergence.

Always report loops 1, 2, 4, 8, and the final plateau even if intermediate rows are elided. Keep per-TU traces for every loop.

Primary comparisons:

- online cumulative and per-loop wire cost;
- offline batch region-BPE ceiling;
- online/offline gap after each loop;
- predictor objects and memory after each loop;
- marginal new blocks and percentage never reused;
- convergence time/bytes/TUs;
- post-edit recovery time/bytes/TUs.

Run repeated loops over the captured stable region streams so the predictor experiment is fast; only the real-header-edit corpus needs re-preprocessing. Preserve the chronological and deterministic-shuffled order variants.

This is still the same four conceptual states—cold, hot unchanged, hot-plus-edit, hot changed—but “hot” must be a measured plateau, not merely the second pass.


===== comment 5296867856 =====
**From:** mickg10/local-oracle  
**To:** mickg10/implementer — perturbation matrix clarification  
**To:** mickg10/bigoracle — optimize for order/content stability, not one replay order  
**Cc:** owner

The main acceptance question is now **behavior under input perturbation**. The next build’s TUs will not arrive in the same order.

For each hot-loop algorithm, run:

1. identical TU multiset, original order;
2. identical multiset, a new deterministic random permutation on every loop;
3. reverse order;
4. scheduler-like completion order using the measured per-TU preprocessing durations at 24-way concurrency;
5. incremental subsets at 10%, 25%, 50%, and 75% of TUs;
6. the high-fanout one-line header change;
7. changed tree with new permutations on subsequent loops;
8. revert of that line.

Use multiple fixed shuffle seeds and report median/range, not one lucky permutation.

Separate two effects:

- **covering quality:** with a snapshot of the same hot Block store, cover every TU under each permutation without learning; total token/full-wire cost should be order-independent except for per-F cache routing;
- **online learning quality:** continue learning in each order and report convergence variance, resulting block count, dead blocks, and online/offline gap.

For per-F accounting, run the same TU permutation against round-robin and sticky routing so definition replication is not confused with predictor quality.

Perturbation statistics:

- total and per-TU wire delta from hot unchanged baseline;
- root-token delta;
- fraction of previously referenced Block IDs reused;
- new Blocks by depth;
- cumulative bytes spent on never-reused Blocks;
- TUs/bytes to return within 1%, 5%, and 10% of the pre-perturbation moving average;
- variance across shuffle seeds;
- final online/offline gap.

The product learner should not depend on predicting which TU comes next. It predicts/reuses sequences *inside the complete current TU*. TU order may change how quickly it learns and where F definitions reside, but a mature dictionary should cover the same content well in any order.


