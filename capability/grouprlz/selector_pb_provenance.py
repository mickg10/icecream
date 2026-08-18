#!/usr/bin/env python3
"""Verify the corpus binding rule on every measured cell and emit the provenance pins.

Rule being verified, per cell:
  1. payload resolved from corpus.json payload.path (no globbing, no project-named package)
  2. payload SHA-256 == corpus.json payload.sha256, and byte count == payload.bytes
  3. manifest.tsv used is the on-disk sibling of that declared payload
  4. boundary == min(112, corpus.json tu_count), and the measured TU count == tu_count
"""
import glob
import hashlib
import json
import os
import sys

PB = os.path.expanduser("~/selbind/pb")
MATRIX = os.path.expanduser("~/ictmp/ii-matrix")


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as fh:
        for chunk in iter(lambda: fh.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


print("\t".join(["project", "profile", "payload_path", "payload_bytes", "payload_sha_ok",
                 "corpus_json_sha256", "manifest_tsv_sha256", "tu_count", "probe_tus",
                 "measured_tus", "counts_ok"]))
bad = []
for f in sorted(glob.glob(f"{PB}/*/row.tsv")):
    d = dict(l.rstrip("\n").split("\t", 1) for l in open(f) if "\t" in l)
    cell = os.path.join(MATRIX, d["project"], d["profile"])
    cj = json.load(open(f"{cell}/corpus.json"))
    payload = os.path.join(cell, cj["payload"]["path"])
    pbytes = os.path.getsize(payload)
    psha = sha256(payload)
    sha_ok = (psha == cj["payload"]["sha256"] and pbytes == cj["payload"]["bytes"])
    probe_expect = min(112, cj["tu_count"])
    counts_ok = (int(d["total_tus"]) == cj["tu_count"]
                 and int(d["probe_tus"]) == probe_expect)
    if not (sha_ok and counts_ok):
        bad.append(f"{d['project']}/{d['profile']}")
    print("\t".join(map(str, [d["project"], d["profile"], cj["payload"]["path"], pbytes,
                             sha_ok, sha256(f"{cell}/corpus.json"),
                             sha256(f"{cell}/manifest.tsv"), cj["tu_count"],
                             d["probe_tus"], d["total_tus"], counts_ok])))

e = sys.stderr
print(f"# cells checked: {len(glob.glob(f'{PB}/*/row.tsv'))}", file=e)
print(f"# payload SHA + declared-count violations: {len(bad)} {bad}", file=e)
