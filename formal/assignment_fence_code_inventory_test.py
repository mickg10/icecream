#!/usr/bin/env python3
"""Verify the formal branch maps real seams without enabling protocol behavior."""

from __future__ import annotations

import unittest
from pathlib import Path

import generate_assignment_fence_code_inventory as inventory

HERE = Path(__file__).resolve().parent
REPO = HERE.parent


class AssignmentFenceCodeInventoryTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.roots = inventory.roots(REPO)
        cls.matches = {
            query.transition: inventory.search(REPO, query, cls.roots)
            for query in inventory.QUERIES
        }
        cls.queries = {
            query.transition: query for query in inventory.QUERIES
        }

    def test_every_required_existing_seam_has_a_real_source_match(self) -> None:
        for transition, query in self.queries.items():
            if not query.required:
                continue
            with self.subTest(transition=transition):
                self.assertTrue(
                    self.matches[transition],
                    f"{transition}: no product source seam matched {query.pattern!r}",
                )
                for match in self.matches[transition]:
                    self.assertTrue((REPO / match.path).is_file())
                    self.assertGreater(match.line, 0)
                    self.assertTrue(match.text)

    def test_formal_branch_contains_no_new_protocol_product_symbols(self) -> None:
        for transition, query in self.queries.items():
            if query.required:
                continue
            with self.subTest(transition=transition):
                self.assertEqual(
                    self.matches[transition],
                    [],
                    f"{transition}: formal branch unexpectedly contains product "
                    f"symbols matching {query.pattern!r}",
                )

    def test_search_is_limited_to_product_and_test_roots(self) -> None:
        allowed = set(inventory.SOURCE_ROOT_CANDIDATES)
        self.assertTrue(set(self.roots) <= allowed)
        self.assertNotIn("formal", self.roots)
        self.assertNotIn("aidocs", self.roots)

    def test_planned_categories_are_explicit_and_complete(self) -> None:
        planned = {
            transition
            for transition, query in self.queries.items()
            if not query.required
        }
        self.assertEqual(
            planned,
            {
                "PreparePlanned",
                "ReadyPlanned",
                "RevokePlanned",
                "PendingClaimPlanned",
            },
        )


if __name__ == "__main__":
    unittest.main()
