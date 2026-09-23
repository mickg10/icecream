# iceccd webgui Playwright test

This test boots a real local `iceccd` process with:

- `--webgui`
- `--webgui-port <ephemeral>`
- `--webgui-addr 127.0.0.1`
- `--no-remote -m 0`

Then it uses Playwright to verify:

- dashboard loads and renders key metrics
- styles are applied (non-trivial background)
- API endpoints return valid JSON
- job history endpoint reports `capacity=20000`

It requires Node.js/npm, Playwright (Chromium is installed by the runner), a
configured native build, and a writable temporary directory. It is a local
opt-in test and does not contact a farm.

## Run

From repo root:

```bash
./tests/webgui/run_playwright.sh
```
