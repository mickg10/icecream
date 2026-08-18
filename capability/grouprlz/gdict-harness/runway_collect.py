#!/usr/bin/env python3
"""Assemble the short-runway P29-winner census from retained selector-cell runs."""
import csv, json, pathlib, sys

R = pathlib.Path("/home/ttuser/runway/runs")
C = pathlib.Path("/home/ttuser/runway/cells")
# classifier under test: P29 iff remaining_tus >= 1093, i.e. total job count >= 1205
RULE_TOTAL_TU = 1205
SHORT_RUNWAY_TU = 400
PIN = {
    "p29_source_sha256": "2fccb899d518441ca81357f7543028bfe58fa734155c550926e22ff3e4fc125f",
    "p29_stock_binary_sha256": "8adb8b394970dd4e67aa82222874ad9a95c2d808c577c649a290b0d7d52a2493",
    "p29_run_binary_sha256": "0a092e877a0862d7bce782b22800c9a756711df10bf3961652a1e02449ed0f2e",
    "grz2_binary_sha256": "647883b7a346ae48d76ea2ac542240e5c78b4a56049372ee6067f5ec94276af9",
}

w = csv.writer(sys.stdout, delimiter="\t", lineterminator="\n")
w.writerow(["corpus", "provenance", "total_tus", "raw_bytes", "mean_tu_bytes",
            "P29_bytes", "GRZ2_bytes", "winner", "p29_over_grz",
            "short_runway_p29_winner", "rule_predicts", "rule_correct",
            "z19_bytes", "grz_decode_exact", "p29_byte_exact",
            "p29_source_sha256", "p29_run_binary_sha256", "grz2_binary_sha256"])

for name in sys.argv[1:]:
    m = R / name / "measurement.json"
    if not m.is_file():
        print(f"# MISSING {name}", file=sys.stderr)
        continue
    d = json.loads(m.read_text())
    cj = json.loads((C / name / "corpus.json").read_text())
    tus, raw = int(cj["tu_count"]), int(cj["raw_bytes"])
    p29, grz = int(d["p29"]["wire_bytes"]), int(d["grz"]["wire_bytes"])
    winner = "p29" if p29 < grz else "grz"
    rule = "p29" if tus >= RULE_TOTAL_TU else "grz"
    # GRZ decode was byte-compared by the runner (cmp raw replay); exact.sha256 is its receipt
    grz_exact = (R / name / "grz" / "exact.sha256").is_file()
    p29_exact = "byte-exact=OK" in (R / name / "p29" / "full" / "grouped.stdout").read_text()
    z19 = d.get("whole_zstd", {}).get("z19_long_bytes", "")
    w.writerow([name, cj["profile"], tus, raw, round(raw / tus),
                p29, grz, winner, round(p29 / grz, 4),
                str(winner == "p29" and tus < SHORT_RUNWAY_TU).lower(),
                rule, str(rule == winner).lower(), z19,
                str(grz_exact).lower(), str(p29_exact).lower(),
                PIN["p29_source_sha256"], PIN["p29_run_binary_sha256"],
                PIN["grz2_binary_sha256"]])
