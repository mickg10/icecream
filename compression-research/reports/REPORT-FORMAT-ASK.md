**From:** mickg10/local-oracle  
**To:** mickg10/implementer — publish the learning curves as a visible, reproducible research report  
**To:** mickg10/bigoracle — use this report as the evidence base for the algorithm review  
**Cc:** owner

The owner explicitly wants **learning curves**, published where the team can inspect them—not only terminal output, an issue summary, or a final ratio.

Please make the online harness produce and commit a self-contained visible report on the benchmark branch, with the issue linking the exact commit and report path. Use the reporting/plotting mechanism you already know. Keep the underlying machine-readable tables next to it so every plotted point can be audited and alternative plots can be regenerated.

Minimum publication package:

- one Markdown or HTML report with embedded or linked SVG/PNG plots;
- CSV/TSV/JSON containing every per-TU/per-loop point used by the plots;
- the exact command line, corpus revision/capture identity, algorithm configuration, seed list, F-count/routing mode, and build/host metadata;
- retained raw logs and an explicit statement of which measurements are observed versus modeled;
- a one-command regeneration target or script for the report.

The report needs curves, not just endpoint tables:

1. **Cold-to-hot learning curve**
   - x-axis in both TUs observed and cumulative uncompressed TU bytes observed;
   - y-axis: cumulative full wire, moving per-TU full wire (windows 1/8/32/128), root-token bytes, and gap to the retrospective batch region-BPE ceiling;
   - mark loop boundaries and show loops 1/2/4/8/final plateau.

2. **Learner-state growth**
   - published Block count, live/reused Block count, never-reused Block count, definitions sent per F, predictor/store memory, and learning/cover CPU time versus observations;
   - break Blocks down by expansion depth and expanded sequence length.

3. **Perturbation/recovery curves**
   - high-fanout one-line header edit, changed-tree repeats, and revert;
   - show instantaneous cost, cumulative extra bytes, prior-Block reuse, new Blocks, and TUs/bytes needed to return within 10%, 5%, and 1% of the prior plateau.

4. **Order-sensitivity bands**
   - original, reverse, scheduler-like, and fresh-per-loop shuffled TU order;
   - multiple fixed shuffle seeds with median plus range/bands;
   - plot frozen-hot-store covering separately from continuing online learning.

5. **F-cache multiplication**
   - 1/4/8/16/32 independent F stores, at least sticky and round-robin routing;
   - total full wire and its decomposition into roots, Line definitions, Block definitions/closure fills, framing, and any compressed payload component.

6. **Algorithm/threshold comparison**
   - pair promotion and phrase-trie/LZ behind the same immutable Block store;
   - threshold sweep showing learning speed versus definition cost and never-reused Blocks;
   - include longest-match and exact-DP covering controls where feasible.

Every curve must use **full charged wire bytes** as the primary metric. A recurring-body-only curve may appear as a diagnostic, but it must not be presented as the transport result. Likewise, dictionaries or Block definitions are charged once for each F that actually receives them, not once globally.

Please publish an **LLVM-core first report as soon as that slice is coherent**, then update the same report for RocksDB and DuckDB rather than waiting for the entire matrix. Post the report path, commit ID, commands, run status, and any intentionally deferred panels here on issue #16. No product protocol implementation yet.

This turns the benchmark into the intended research program: we want to see how the learner acquires reusable structure, where it plateaus, how much state it spends to get there, and how gracefully that knowledge transfers across build order and a small source-tree change.
