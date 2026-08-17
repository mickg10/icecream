#!/usr/bin/env python3

import tempfile
import unittest
from pathlib import Path

from measure_whole_stream_reference import (
    boundaries,
    copy_and_hash,
    load_file_list,
)


class WholeStreamReferenceTest(unittest.TestCase):
    def test_boundaries_deduplicate_short_final(self) -> None:
        self.assertEqual(
            boundaries(34, (100, 200), True),
            [
                {
                    "files": 34,
                    "requested": [100, 200, "full"],
                    "complete": True,
                }
            ],
        )

    def test_boundaries_retain_eligible_prefixes(self) -> None:
        self.assertEqual(
            boundaries(250, (100, 200), True),
            [
                {"files": 100, "requested": [100], "complete": False},
                {"files": 200, "requested": [200], "complete": False},
                {"files": 250, "requested": ["full"], "complete": True},
            ],
        )

    def test_ordered_copy_is_exact(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            first = root / "first"
            second = root / "second"
            first.write_bytes(b"abc")
            second.write_bytes(b"def\n")
            manifest = root / "manifest.txt"
            manifest.write_text(f"{first}\n{second}\n")
            files = load_file_list(manifest)
            output = root / "out"
            with output.open("wb") as destination:
                raw_bytes, digest = copy_and_hash(files, destination)
            self.assertEqual(output.read_bytes(), b"abcdef\n")
            self.assertEqual(raw_bytes, 7)
            self.assertEqual(
                digest,
                "ae0666f161fed1a5dde998bbd0e140550d2da0db27db1d0e31e370f2bd366a57",
            )


if __name__ == "__main__":
    unittest.main()
