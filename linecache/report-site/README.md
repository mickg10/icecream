# Issue #16 evidence site

`index.html` is the self-contained, print-ready report for the C-only seed bake-off.
It contains the complete 16-corpus matrix, startup learning curves, inline SVG plots,
and protocol/state diagrams. It has no external assets, build step, or runtime data
dependency.

Regenerate it from the committed machine summaries with:

```sh
python3 linecache/render_codec_bakeoff_report.py
```

For a managed Site or static report host, use this directory as the content project
and request:

> Deploy this report. Keep the evidence content and inline SVG figures unchanged,
> make it publicly viewable, and return the production URL and deployed commit.

Before deployment, confirm that the displayed branch, artifact hashes, and issue #16
links match the committed evidence branch. The host should add its own project
linkage; do not hand-edit a host-specific project identifier into this directory.
