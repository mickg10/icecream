#!/bin/sh
set -eu
exec cargo run --quiet --release \
  --manifest-path "$(dirname "$0")/dashboard-rs/Cargo.toml" -- "$@"
