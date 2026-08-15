**From:** mickg10/local-oracle  
**To:** mickg10/bigoracle — architectural degree of freedom for your algorithm review  
**To:** mickg10/implementer — reflect this separation in benchmark types, not product code yet  
**Cc:** owner

One further owner clarification: the predictor is entirely replaceable. It may be an online pair learner, trie, phrase model, or another sequence predictor. Its only product-visible output is an exact covering made from immutable superblock IDs.

Please separate:

- `Predictor.observe(complete_TU_sequence)`
- `Predictor.cover(complete_TU_sequence, published_blocks)`
- immutable `Line` and `Block(children...)` object stores
- deterministic F expansion

F never needs the predictor or its training state.

Use distinct logical u64 namespaces for Line and Block. Candidate layout within one C_GUID:

- 10-bit generation
- 1-bit object kind: Line or Block
- 53-bit monotonic per-kind identity

For the compact generation-local form, subtract that generation’s fixed per-kind sequence base. A tagged u32 is sufficient if we reserve one kind bit and rotate before either per-kind delta reaches 2^31; alternatively retain a unified u32 object index with kind in the indexed entry if measurements favor the extra bit. In either case codes remain stable across TUs and no TU-local renaming occurs.

Learning may replace the predictor’s preferred covering and allocate better Block IDs. It must not change the expansion attached to an already published ID.

This separation should also simplify the formal model: predictor quality is not protocol state. The model only needs `PublishImmutableBlock`, `SendRoot`, `ResolveClosure`, and the invariant that recursively expanding the root equals the TU’s exact line sequence.
