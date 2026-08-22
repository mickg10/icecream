#!/usr/bin/env bash
set -euo pipefail

cat >&2 <<'EOF'
UNSUPPORTED: Fedora 28 cannot build this product line.
Its archived repositories provide GCC 8 and Boost 1.66, while this tree
requires a compiler with the selected C++23 feature set and Boost >= 1.74.
Use the Fedora latest builder; this row will not fetch replacement toolchains.
EOF
exit 78
