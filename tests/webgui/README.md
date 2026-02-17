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

## Run

From repo root:

```bash
./tests/webgui/run_playwright.sh
```
