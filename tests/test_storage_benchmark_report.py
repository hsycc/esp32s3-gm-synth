import unittest

from tools.storage_benchmark.report import parse_log, render_markdown


GOOD_LOG = """
I (154) gm_synth: Detected physical PSRAM: 8388608 bytes (8.00 MiB)
I (174) gm_synth: QEMU FATFS mounted: base=/sf2 partition=qemu_sf2
I (184) gm_synth: Storage benchmark VFS open: path=/sf2/test.sf2 file_bytes=5898214
I (204) gm_synth: Storage benchmark SF2 parsed: playable_samples=67
I (204) gm_synth: Storage benchmark source: path=/sf2/test.sf2 smpl_offset=544 smpl_bytes=5887930 benchmark_bytes=4194304
I (314) storage_bench: chunk=4096 bytes=4194304 chunks=1024 elapsed=100000 us throughput=40.000 MiB/s latency_us[min/avg/p50/p95/p99/max]=90/97/95/110/130/160
I (424) storage_bench: chunk=8192 bytes=4194304 chunks=512 elapsed=90000 us throughput=44.444 MiB/s latency_us[min/avg/p50/p95/p99/max]=170/175/172/190/215/250
I (534) storage_bench: chunk=16384 bytes=4194304 chunks=256 elapsed=85000 us throughput=47.059 MiB/s latency_us[min/avg/p50/p95/p99/max]=320/330/325/355/390/420
I (634) storage_bench: chunk=32768 bytes=4194304 chunks=128 elapsed=82000 us throughput=48.780 MiB/s latency_us[min/avg/p50/p95/p99/max]=630/640/635/680/720/780
I (744) storage_bench: chunk=65536 bytes=4194304 chunks=64 elapsed=80000 us throughput=50.000 MiB/s latency_us[min/avg/p50/p95/p99/max]=1240/1250/1245/1320/1400/1500
I (744) gm_synth: Residency ready: capacity=4194304 used=0 largest_free_span=4194304 free_spans=1
"""


class StorageBenchmarkReportTests(unittest.TestCase):
    def test_good_hardware_log_passes(self) -> None:
        report = parse_log(GOOD_LOG, expected_bytes=4194304, expected_samples=67)

        self.assertTrue(report.passed)
        self.assertEqual(report.psram_bytes, 8388608)
        self.assertEqual(report.file_bytes, 5898214)
        self.assertEqual(report.playable_samples, 67)
        self.assertEqual(report.benchmark_bytes, 4194304)
        self.assertEqual(len(report.chunks), 5)
        self.assertEqual(report.chunks[0].chunk_size, 4096)
        self.assertEqual(report.chunks[-1].p99_us, 1400)

    def test_missing_chunk_fails(self) -> None:
        log = GOOD_LOG.replace(
            "I (744) storage_bench: chunk=65536 bytes=4194304 chunks=64 elapsed=80000 us throughput=50.000 MiB/s latency_us[min/avg/p50/p95/p99/max]=1240/1250/1245/1320/1400/1500\n",
            "",
        )
        report = parse_log(log, expected_bytes=4194304, expected_samples=67)

        self.assertFalse(report.passed)
        self.assertIn("missing chunk result: 65536", report.errors)

    def test_panic_signature_fails(self) -> None:
        report = parse_log(
            GOOD_LOG + "Guru Meditation Error: Core 0 panic'ed\n",
            expected_bytes=4194304,
            expected_samples=67,
        )

        self.assertFalse(report.passed)
        self.assertTrue(any("Guru Meditation" in error for error in report.errors))

    def test_wrong_expected_bytes_fails(self) -> None:
        report = parse_log(GOOD_LOG, expected_bytes=262144, expected_samples=67)

        self.assertFalse(report.passed)
        self.assertTrue(any("benchmark byte mismatch" in error for error in report.errors))

    def test_markdown_contains_tail_latency_columns(self) -> None:
        report = parse_log(GOOD_LOG, expected_bytes=4194304, expected_samples=67)
        markdown = render_markdown(report, title="N16R8 Flash Benchmark")

        self.assertIn("Status: **PASS**", markdown)
        self.assertIn("P95 us", markdown)
        self.assertIn("P99 us", markdown)
        self.assertIn("50.000", markdown)


if __name__ == "__main__":
    unittest.main()
