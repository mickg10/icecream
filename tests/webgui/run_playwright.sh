#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(cd -- "${script_dir}/../.." && pwd)"

cd "${script_dir}"

echo "== install webgui test deps"
npm install --no-package-lock --no-fund --no-audit

echo "== build daemon"
cd "${repo_root}"
make -j"$(nproc)"

echo "== ensure playwright chromium"
cd "${script_dir}"
npx playwright install chromium

echo "== run playwright webgui tests"
npx playwright test --config "${script_dir}/playwright.config.cjs" "${script_dir}/webgui.spec.cjs"
