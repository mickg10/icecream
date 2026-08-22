#!/usr/bin/env bash
set -euo pipefail

cat >&2 <<'EOF'
UNSUPPORTED: Fedora 28 packages are not produced for this product line.
Its archived repositories cannot satisfy the C++23 and Boost >= 1.74 build
baseline, so there is intentionally no Fedora 28 artifact to verify.
EOF
exit 78
