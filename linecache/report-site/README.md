# Issue #16 evidence site

`index.html` is the self-contained, print-ready report for the C-only seed bake-off.
It has no external assets, build step, or runtime data dependency.

Regenerate it from the committed machine summaries with:

```sh
python3 linecache/render_codec_bakeoff_report.py
```

For ChatGPT Sites, use this directory as the content-led site project and request:

> Deploy this project with Sites. Keep the standalone report content and inline SVG
> figures unchanged, make it publicly viewable, and return the production URL.

Before deployment, confirm that the displayed branch, artifact hashes, and issue #16
links match the committed evidence branch. Sites should add its own project linkage;
do not hand-edit a project identifier into this directory.
