import hashlib
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest
import zipfile

from tools.release_evidence import EvidenceError, collect, git_snapshot, log_identity


class ReleaseEvidenceTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.base = Path(self.temp.name)
        self.repo = self.base / "repo"
        self.repo.mkdir()
        self.git("init", "-q")
        self.git("config", "user.email", "test@example.invalid")
        self.git("config", "user.name", "Test")
        (self.repo / ".gitignore").write_text("build-m7-release/\nsdkconfig.m7release\n")
        (self.repo / "source.cpp").write_text("int value = 1;\n")
        self.git("add", ".")
        self.git("commit", "-qm", "fixture")
        self.sha = self.git("rev-parse", "HEAD").strip()
        self.out = self.base / "evidence.zip"
        build = self.repo / "build-m7-release"
        build.mkdir()
        (build / "test.elf").write_bytes(b"test ELF bytes")
        (build / "test.bin").write_bytes(b"test BIN bytes")
        (build / "qemu_sf2.bin").write_bytes(b"asset image not redistributed")
        (self.repo / "sdkconfig.m7release").write_text("CONFIG_GM_SYNTH_I2S_QUAL_MASTER_GAIN_Q15=32767\n")

    def git(self, *args):
        return subprocess.check_output(["git", "-C", str(self.repo), *args], stderr=subprocess.PIPE).decode()

    def run_collect(self, **kwargs):
        return collect(self.repo, self.out, self.sha, **kwargs)

    def test_preserves_tracked_index_and_untracked_state(self):
        (self.repo / "source.cpp").write_text("int value = 2;\n")
        self.git("add", "source.cpp")
        (self.repo / "source.cpp").write_text("int value = 3;\n")
        (self.repo / "private-untracked.txt").write_text("DO NOT COLLECT THIS CONTENT")
        before = git_snapshot(self.repo)
        report = self.run_collect()
        self.assertEqual(before, git_snapshot(self.repo))
        self.assertTrue(report["git_snapshot_unchanged_during_collection"])
        with zipfile.ZipFile(self.out) as z:
            self.assertIn(b"int value = 2;", z.read("git/staged.patch"))
            self.assertIn(b"int value = 3;", z.read("git/unstaged.patch"))
            self.assertFalse(any("private-untracked" in n for n in z.namelist()))
            self.assertFalse(any(b"DO NOT COLLECT THIS CONTENT" in z.read(n) for n in z.namelist()))
            self.assertFalse(any("qemu_sf2.bin" in n for n in z.namelist()))
        self.assertTrue(any(r["source"].endswith("qemu_sf2.bin") and r["archive_member"] is None
                            for r in report["files"]))

    def test_copies_binaries_and_matches_hash_without_approval(self):
        digest = hashlib.sha256(b"test ELF bytes").hexdigest()
        log = self.base / "run.log"
        log.write_text(f"\x1b[0;32mApp version: {self.sha[:7]}-dirty\x1b[0m\n"
                       f"ELF file SHA256: {digest[:9]}...\nReturned from app_main()\n")
        report = self.run_collect(logs=(log,))
        self.assertFalse(report["release_approved"])
        self.assertEqual(report["historical_build_provenance"], "UNRESOLVED")
        self.assertEqual(report["logs"][0]["local_elf_prefix_matches"][digest[:9]], [digest])
        with zipfile.ZipFile(self.out) as z:
            self.assertEqual(z.read("build/test.elf"), b"test ELF bytes")
            self.assertEqual(z.read("build/test.bin"), b"test BIN bytes")
            self.assertEqual(json.loads(z.read("manifest.json")), report)

    def test_refuses_existing_output(self):
        self.out.write_bytes(b"existing evidence")
        with self.assertRaises(EvidenceError):
            self.run_collect()
        self.assertEqual(self.out.read_bytes(), b"existing evidence")

    def test_refuses_output_inside_checkout(self):
        self.out = self.repo / "out.zip"
        with self.assertRaises(EvidenceError):
            self.run_collect()
        self.assertFalse(self.out.exists())

    def test_refuses_output_symlink_into_checkout(self):
        link = self.base / "linked"
        link.symlink_to(self.repo, target_is_directory=True)
        self.out = link / "out.zip"
        with self.assertRaises(EvidenceError):
            self.run_collect()
        self.assertFalse(self.out.exists())

    def test_missing_build_is_warning_not_success_gate(self):
        report = self.run_collect(build_dir="absent-build")
        self.assertIn("NO_LOCAL_ELF_FOUND", report["warnings"])
        self.assertFalse(report["release_approved"])

    def test_missing_log_does_not_create_archive(self):
        with self.assertRaises(EvidenceError):
            self.run_collect(logs=(self.base / "missing.log",))
        self.assertFalse(self.out.exists())

    def test_refuses_external_build_symlink(self):
        outside = self.base / "external.elf"
        outside.write_bytes(b"not a repository artifact")
        elf = self.repo / "build-m7-release/test.elf"
        elf.unlink()
        elf.symlink_to(outside)
        with self.assertRaises(EvidenceError):
            self.run_collect()
        self.assertFalse(self.out.exists())

    def test_multiple_boots_and_dirty_suffix_are_visible(self):
        result = log_identity(b"App version: 1234567-dirty\nELF file SHA256: abcdef012...\n"
                              b"App version: 1234567\nReturned from app_main()\n")
        self.assertEqual(result["boot_version_count"], 2)
        self.assertEqual(result["app_main_return_count"], 1)
        self.assertTrue(result["dirty_banner"])

    def test_current_head_difference_is_not_silently_accepted(self):
        (self.repo / "notes.md").write_text("later docs\n")
        self.git("add", "notes.md")
        self.git("commit", "-qm", "later")
        report = self.run_collect()
        self.assertIn("CURRENT_HEAD_DIFFERS_FROM_TEST_CHECKPOINT", report["warnings"])
        self.assertFalse(report["release_approved"])

    def test_requires_full_commit_sha(self):
        with self.assertRaises(EvidenceError):
            collect(self.repo, self.out, self.sha[:7])
        self.assertFalse(self.out.exists())

    def test_detached_worktree_supported(self):
        linked = self.base / "linked-worktree"
        self.git("worktree", "add", "--detach", str(linked), self.sha)
        report = collect(linked, self.out, self.sha)
        self.assertEqual(report["current_head"], self.sha)
        self.assertTrue(report["git_snapshot_unchanged_during_collection"])


if __name__ == "__main__":
    unittest.main()
