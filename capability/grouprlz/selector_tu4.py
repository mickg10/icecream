#!/usr/bin/env python3
"""Build the 4x TU map from a single-pass one, without materialising the 4x .ii.

The .tu map is N+1 cumulative u64 byte offsets into the .ii stream, the last being the
stream length.  Repeating the corpus 4x just shifts each pass by p * total.
"""
import struct
import sys

src, dst, reps = sys.argv[1], sys.argv[2], int(sys.argv[3]) if len(sys.argv) > 3 else 4
d = open(src, "rb").read()
if len(d) % 8:
    sys.exit("%s: not a whole number of u64 offsets" % src)
off = list(struct.unpack("<%dQ" % (len(d) // 8), d))
if off[0] != 0 or any(off[i] > off[i + 1] for i in range(len(off) - 1)):
    sys.exit("%s: offsets are not non-decreasing from 0" % src)
total = off[-1]
out = []
for p in range(reps):
    out.extend(o + p * total for o in off[:-1])
out.append(total * reps)
open(dst, "wb").write(struct.pack("<%dQ" % len(out), *out))
print("tu4: %d TUs -> %d, stream %d -> %d bytes" % (len(off) - 1, len(out) - 1, total, total * reps))
