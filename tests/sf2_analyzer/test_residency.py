import sys
import unittest
from pathlib import Path
from types import SimpleNamespace

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))

from tools.sf2_analyzer.residency import (
    exact_worst_active_ready_pair,
    gm_programs_for_transition_pressure,
)


def program(bank, number, name, sample_ids, aligned_bytes):
    return {
        "bank": bank,
        "program": number,
        "name": name,
        "sample_ids": list(sample_ids),
        "aligned_pcm_bytes": aligned_bytes,
    }


class ResidencyPressureTests(unittest.TestCase):
    def test_exact_pair_uses_sample_union_not_program_sum(self):
        # Sample 1 is deliberately shared by A/B so the exact union must dedupe
        # it instead of adding each program's working-set total independently.
        sf2 = SimpleNamespace(
            shdr=[
                SimpleNamespace(pcm_bytes=100),
                SimpleNamespace(pcm_bytes=200),
                SimpleNamespace(pcm_bytes=400),
                SimpleNamespace(pcm_bytes=800),
                SimpleNamespace(pcm_bytes=0),  # terminal EOS
            ]
        )
        programs = [
            program(0, 0, "A", [0, 1], 300),
            program(0, 1, "B", [1, 2], 600),
            program(128, 0, "Drum", [3], 800),
        ]

        result = exact_worst_active_ready_pair(sf2, programs)
        self.assertIsNotNone(result)
        self.assertEqual(result["active_program"]["program"], 1)
        self.assertEqual(result["ready_program"]["bank"], 128)
        self.assertEqual(result["sample_ids"], [1, 2, 3])
        self.assertEqual(result["unique_sample_count"], 3)
        self.assertEqual(result["shared_sample_count"], 0)
        self.assertEqual(result["aligned_pcm_bytes"], 1400)

    def test_pair_reports_shared_bytes(self):
        sf2 = SimpleNamespace(
            shdr=[
                SimpleNamespace(pcm_bytes=100),
                SimpleNamespace(pcm_bytes=200),
                SimpleNamespace(pcm_bytes=0),
            ]
        )
        programs = [
            program(0, 0, "A", [0, 1], 300),
            program(0, 1, "B", [1], 200),
        ]
        result = exact_worst_active_ready_pair(sf2, programs)
        self.assertEqual(result["aligned_pcm_bytes"], 300)
        self.assertEqual(result["shared_sample_count"], 1)
        self.assertEqual(result["shared_aligned_pcm_bytes"], 200)

    def test_gm_domain_preserves_first_duplicate_identity(self):
        first = program(0, 0, "First", [0], 100)
        duplicate = program(0, 0, "Later duplicate", [1], 200)
        melodic_127 = program(0, 127, "Last melodic", [2], 300)
        non_gm = program(1, 0, "Variation", [3], 400)
        drum = program(128, 0, "Standard drum", [4], 500)
        drum_alt = program(128, 8, "Room drum", [5], 600)

        selected = gm_programs_for_transition_pressure(
            [first, duplicate, melodic_127, non_gm, drum, drum_alt]
        )
        self.assertEqual(len(selected), 3)
        self.assertIs(selected[0], first)
        self.assertIs(selected[1], melodic_127)
        self.assertIs(selected[2], drum)

    def test_less_than_two_programs_has_no_transition_pair(self):
        sf2 = SimpleNamespace(
            shdr=[SimpleNamespace(pcm_bytes=100), SimpleNamespace(pcm_bytes=0)]
        )
        self.assertIsNone(exact_worst_active_ready_pair(sf2, []))
        self.assertIsNone(
            exact_worst_active_ready_pair(
                sf2, [program(0, 0, "Only", [0], 100)]
            )
        )


if __name__ == "__main__":
    unittest.main()
