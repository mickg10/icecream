# Codec templates

This directory is the shared, header-only home for codec algorithms whose
wire bytes are selected by named parameter tuples and whose storage, history,
and failure policy are supplied by compile-time resource providers.

Phase 0 contains the provider contracts, tuple catalogue, and frozen format
description.  Phase 3a adds `p29_intern.h`, the wire-neutral P29 Line/Region
interner.  It reproduces the retained research identities while obtaining all
owned tables and byte arenas from a bounded provider; its product test adapter
publishes the resulting ordinals through `CObjectArena`.  No production call
site uses the interner until the later P29-v1 landing.

Phase 3b adds the provider-owned `P29Serializer`/`P29Deserializer` in
`p29_wire.h`.  It reproduces the retained P29-v1 core streams, stages one TU
transactionally, materializes every TU exactly, and publishes decoded Regions
and Blocks only on commit.  It is still an unadvertised codec template: no
product endpoint, profile id, peer transition, or Protocol-50 carriage uses it.

The vectors under `unittests/codec_golden/` are immutable witnesses cut from
the named product and research revisions.  Never regenerate a golden merely
to make a test pass.  A legitimate byte-format change gets a new tuple id and
a new golden set while the old tuple remains checked against its old vectors.

Golden inputs are identity/smoke fixtures only.  Effectiveness gates use a
corpus of at least 1,000 translation units.  The interner micro-gate therefore
uses the retained 2,498-TU Firefox manifest in addition to the small identity
vectors.
