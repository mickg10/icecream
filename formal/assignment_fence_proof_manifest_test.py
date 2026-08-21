#!/usr/bin/env python3
"""Static contract for the focused proof-bearing assignment-fence matrix."""

from __future__ import annotations

import json
import re
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
MANIFEST = HERE / 'assignment-fence-proof-checks-v1.json'

NEW_IDS = [
    'p49-strict-within-epoch',
    'p49-pipelined-within-epoch',
    'p49-pipelined-claim-before-prepare',
    'p49-pipelined-revoke-race',
    'p49-pipelined-claim-before-prepare-witness',
    'p49-pipelined-revoke-race-witness',
    'p49-pipelined-default-allow-mutant',
    'p49-pipelined-side-effect-before-prepare-mutant',
    'p49-pipelined-unbounded-pending-mutant',
    'p49-pipelined-pending-liveness',
]


class AssignmentFenceProofManifestTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.document = json.loads(MANIFEST.read_text(encoding='utf-8'))
        cls.rows = cls.document['checks']
        cls.by_id = {row['id']: row for row in cls.rows}

    def test_new_rows_are_complete_and_ordered(self) -> None:
        actual = [row['id'] for row in self.rows if row['id'].startswith('p49-')]
        self.assertEqual(actual, NEW_IDS)
        for check_id in NEW_IDS:
            row = self.by_id[check_id]
            self.assertEqual(row['module'], 'AssignmentFenceAdmissionModes')
            self.assertEqual(row['workers'], 1)
            self.assertEqual(row['toolchains'], ['stable', 'differential'])
            self.assertTrue((HERE / row['config']).is_file())

    def test_counterexamples_have_exact_trace_contracts(self) -> None:
        for row in self.rows:
            if not row['id'].startswith('p49-') or row['expected'] == 'pass':
                continue
            trace_path = HERE / row['trace_manifest']
            trace = json.loads(trace_path.read_text(encoding='utf-8'))
            self.assertEqual(trace['property'], row['property'])
            self.assertEqual(trace['expected_result'], 'counterexample')
            self.assertEqual(trace['metadata']['module'], row['module'])
            self.assertEqual(trace['metadata']['config'], row['config'])
            self.assertTrue(trace['required_subsequence'])
            self.assertTrue(trace['final_all'])

    def test_configs_are_direct_and_reduction_free(self) -> None:
        for check_id in NEW_IDS:
            row = self.by_id[check_id]
            text = (HERE / row['config']).read_text(encoding='utf-8')
            self.assertEqual(len(re.findall(r'^SPECIFICATION\s+', text, re.M)), 1)
            direct = re.findall(r'^(INVARIANT|PROPERTY)\s+(\w+)\s*$', text, re.M)
            self.assertIn((
                'PROPERTY' if row['kind'] == 'liveness' else 'INVARIANT',
                row['property'],
            ), direct)
            self.assertEqual(len(re.findall(r'^CHECK_DEADLOCK\s+', text, re.M)), 1)
            self.assertNotRegex(text, re.compile(r'^(CONSTRAINT|ACTION_CONSTRAINT|SYMMETRY|VIEW)\b', re.M))

    def test_proof_row_is_real_and_unique(self) -> None:
        proofs = self.document.get('proofs', [])
        self.assertEqual(len(proofs), 1)
        self.assertEqual(proofs[0]['file'], 'AssignmentFenceCoreProof.tla')
        self.assertEqual(proofs[0]['expected'], 'pass')


if __name__ == '__main__':
    unittest.main()
