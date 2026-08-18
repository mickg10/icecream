#!/usr/bin/env python3
"""Extract test-side wire from a curve TSV given the prefix TU count."""
import sys, csv
path, prefix = sys.argv[1], int(sys.argv[2])
rows = list(csv.DictReader(open(path), delimiter="\t"))
assert len(rows) > prefix, f"{len(rows)} rows <= prefix {prefix}"
base = int(rows[prefix-1]["cumulative_wire_bytes"]) if prefix else 0
end = int(rows[-1]["cumulative_wire_bytes"])
test_rows = rows[prefix:]
raw = sum(int(r["raw_bytes"]) for r in test_rows)
exact = all(r["exact"] == "true" for r in rows)
exact_test = all(r["exact"] == "true" for r in test_rows)
print(f"prefix_tus={prefix} test_tus={len(test_rows)} test_raw={raw} "
      f"prefix_wire={base} total_wire={end} test_wire={end-base} "
      f"exact_all={exact} exact_test={exact_test}")
