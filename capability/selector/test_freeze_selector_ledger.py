#!/usr/bin/env python3
"""Self-contained gates for freeze_selector_ledger.py."""

from __future__ import annotations

import csv
import hashlib
import importlib.util
import io
import json
from pathlib import Path
import tempfile
import unittest


MODULE_PATH = Path(__file__).with_name("freeze_selector_ledger.py")
SPEC = importlib.util.spec_from_file_location("freeze_selector_ledger", MODULE_PATH)
assert SPEC is not None and SPEC.loader is not None
ledger = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(ledger)


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


class FreezeSelectorLedgerTest(unittest.TestCase):
    def make_cell(self, root: Path) -> tuple[Path, Path]:
        cell = root / "project" / "profile"
        cell.mkdir(parents=True)
        payload = b"the-current-declared-payload"
        (cell / "ii.tar.zst").write_bytes(payload)
        # This older generation must never be selected by name matching.
        (cell / "project-profile.ii.tar.zst").write_bytes(b"old-package")
        manifest_rows = [
            {
                "ordinal": "0",
                "relative_path": "ii/00000000.ii",
                "raw_bytes": "3",
                "sha256": sha256(b"abc"),
            },
            {
                "ordinal": "1",
                "relative_path": "ii/00000001.ii",
                "raw_bytes": "2",
                "sha256": sha256(b"de"),
            },
        ]
        manifest = io.StringIO(newline="")
        writer = csv.DictWriter(
            manifest,
            delimiter="\t",
            lineterminator="\n",
            fieldnames=("ordinal", "relative_path", "raw_bytes", "sha256"),
        )
        writer.writeheader()
        writer.writerows(manifest_rows)
        (cell / "manifest.tsv").write_text(manifest.getvalue())
        corpus = {
            "schema": "ice-ii-corpus-v1",
            "project": "project",
            "profile": "profile",
            "tu_count": 2,
            "raw_bytes": 5,
            "payload": {
                "path": "ii.tar.zst",
                "bytes": len(payload),
                "sha256": sha256(payload),
            },
        }
        corpus_path = cell / "corpus.json"
        corpus_path.write_text(json.dumps(corpus, indent=2, sort_keys=True) + "\n")
        return cell, corpus_path

    def test_declared_generation_is_frozen(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            cell, _ = self.make_cell(root)
            rows = ledger.read_matrix(root, 1)
            self.assertEqual(rows[0]["tu_count"], "2")
            self.assertEqual(rows[0]["raw_bytes"], "5")
            self.assertEqual(rows[0]["payload_path"], "ii.tar.zst")
            self.assertEqual(rows[0]["payload_sha256"], sha256(b"the-current-declared-payload"))
            self.assertNotEqual(
                rows[0]["payload_sha256"], sha256((cell / "project-profile.ii.tar.zst").read_bytes())
            )
            rendered = ledger.render_tsv(rows)
            self.assertEqual(rendered.count("\n"), 2)
            self.assertIn("corpus_sha256\tmanifest_sha256\n", rendered)

    def test_manifest_extent_drift_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            _, corpus_path = self.make_cell(root)
            corpus = json.loads(corpus_path.read_text())
            corpus["raw_bytes"] = 6
            corpus_path.write_text(json.dumps(corpus))
            with self.assertRaisesRegex(RuntimeError, "manifest raw extent differs"):
                ledger.read_matrix(root, 1)

    def test_payload_path_escape_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            _, corpus_path = self.make_cell(root)
            corpus = json.loads(corpus_path.read_text())
            corpus["payload"]["path"] = "../ii.tar.zst"
            corpus_path.write_text(json.dumps(corpus))
            with self.assertRaisesRegex(RuntimeError, "invalid payload.path"):
                ledger.read_matrix(root, 1)


if __name__ == "__main__":
    unittest.main()
