#!/usr/bin/env python3
"""Read-only source/build evidence collector. Collection is NOT release approval.

Run this file from outside the checkout before any rebuild, reset or checkout.
Uses Python's standard library and Git; no ESP-IDF environment is required.
No upload, fetch, build, flash, stash, clean or Git write is performed.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile
from datetime import datetime, timezone
import zipfile

FROZEN_M8 = "bb31908bf4af5c966e2f17075e54b08d9a3d057c"
ANSI = re.compile(r"\x1b\[[0-?]*[ -/]*[@-~]")
NOTICE = """This is a current-state evidence snapshot, NOT a release approval.
A clean worktree today does not prove the sources used for an earlier build.
Matching a truncated log ELF hash supports identification, not proof of complete
binary identity. A full local ELF hash does not identify the flashed device by
itself. Review historical build inputs/diffs and log identity before merging.

The archive may contain proprietary tracked source diffs, local paths, firmware
binaries and logs. Inspect it before sharing. No network upload is performed.
Untracked file CONTENTS, Git remotes/config, credentials and environment dumps
are deliberately not collected. Explicit build/config/log inputs are included.
"""


class EvidenceError(RuntimeError):
    pass


def git(repo: Path, *args: str) -> bytes:
    env = dict(os.environ, GIT_OPTIONAL_LOCKS="0", GIT_TERMINAL_PROMPT="0")
    result = subprocess.run(
        ["git", "-C", str(repo), *args], env=env, stdout=subprocess.PIPE,
        stderr=subprocess.PIPE, check=False, timeout=60,
    )
    if result.returncode:
        raise EvidenceError(result.stderr.decode("utf-8", "replace").strip())
    return result.stdout


def inside(path: Path, directory: Path) -> bool:
    return path == directory or directory in path.parents


def git_snapshot(repo: Path) -> dict[str, bytes]:
    diff = ("diff", "--no-ext-diff", "--no-textconv")
    return {
        "head.txt": git(repo, "rev-parse", "HEAD"),
        "tree.txt": git(repo, "rev-parse", "HEAD^{tree}"),
        "status.txt": git(repo, "status", "--porcelain=v1", "--untracked-files=all"),
        "unstaged.patch": git(repo, *diff),
        "staged.patch": git(repo, *diff, "--cached"),
    }


def log_identity(data: bytes) -> dict:
    text = ANSI.sub("", data.decode("utf-8", "replace"))
    versions = re.findall(r"App version:\s*([^\r\n]+)", text)
    prefixes = re.findall(r"ELF file SHA256:\s*([0-9a-fA-F]+)", text)
    return {
        "app_versions": [value.strip() for value in versions],
        "elf_sha256_prefixes": [value.lower() for value in prefixes],
        "boot_version_count": len(versions),
        "app_main_return_count": text.count("Returned from app_main()"),
        "dirty_banner": any("-dirty" in value for value in versions),
    }


def collect(repo: Path, output: Path, expected: str = FROZEN_M8,
            build_dir: str = "build-m7-release", logs: tuple[Path, ...] = ()) -> dict:
    if not re.fullmatch(r"[0-9a-fA-F]{40}", expected):
        raise EvidenceError("--expected must be a full 40-character commit SHA")
    repo = Path(git(repo.resolve(), "rev-parse", "--show-toplevel").decode().strip())
    repo = repo.resolve()
    output = output.expanduser().resolve()
    gitdir = Path(git(repo, "rev-parse", "--absolute-git-dir").decode().strip()).resolve()
    common = Path(git(repo, "rev-parse", "--git-common-dir").decode().strip())
    common = (repo / common).resolve() if not common.is_absolute() else common.resolve()
    if any(inside(output, root) for root in (repo, gitdir, common)):
        raise EvidenceError("Output must be outside the worktree AND Git metadata directories")
    if output.exists():
        raise EvidenceError("Refusing to overwrite existing evidence: " + str(output))
    expected = git(repo, "rev-parse", "--verify", expected + "^{commit}").decode().strip()
    build = (repo / build_dir).resolve()
    if not inside(build, repo):
        raise EvidenceError("Build directory must be inside the specified checkout")
    before = git_snapshot(repo)
    warnings = ["HISTORICAL_BUILD_PROVENANCE_UNRESOLVED"]
    records: list[dict] = []
    identities: list[dict] = []
    manifest = {
        "schema_version": 1, "collected_utc": datetime.now(timezone.utc).isoformat(),
        "expected_commit": expected, "current_head": before["head.txt"].decode().strip(),
        "historical_build_provenance": "UNRESOLVED", "release_approved": False,
        "notice": NOTICE, "files": records, "logs": identities, "warnings": warnings,
    }
    if before["status.txt"].strip():
        warnings.append("CURRENT_WORKTREE_NOT_CLEAN")
    if manifest["current_head"] != expected:
        warnings.append("CURRENT_HEAD_DIFFERS_FROM_TEST_CHECKPOINT")
    if not logs:
        candidates = list((repo / "reports/m8-physical").glob("m8-run*.log"))
        candidates += list((repo / "reports/m7-physical").glob("m7-fix-run*.log"))
        logs = tuple(sorted(set(candidates)))
    for log in logs:
        if not log.is_file():
            raise EvidenceError("Log file not found: " + str(log))
    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="release-evidence-", dir=output.parent) as tmp:
        archive = Path(tmp) / "evidence.zip"
        with zipfile.ZipFile(archive, "w", compression=zipfile.ZIP_DEFLATED) as z:
            z.writestr("README.txt", NOTICE)
            for name, data in before.items():
                z.writestr("git/" + name, data)
            z.writestr("git/checkpoint-to-head.patch", git(
                repo, "diff", "--no-ext-diff", "--no-textconv", expected, "HEAD"))
            z.writestr("git/describe.txt", git(repo, "describe", "--always", "--dirty"))

            def capture(path: Path, member: str, copy: bool = True) -> dict | None:
                if not path.is_file():
                    return None
                resolved = path.resolve()
                if not inside(resolved, repo):
                    raise EvidenceError("Refusing external build/config symlink: " + str(path))
                start = path.stat()
                digest = hashlib.sha256()
                with path.open("rb") as stream:
                    if copy:
                        with z.open(member, "w") as target:
                            for chunk in iter(lambda: stream.read(1024 * 1024), b""):
                                digest.update(chunk)
                                target.write(chunk)
                    else:
                        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
                            digest.update(chunk)
                end = path.stat()
                if (start.st_size, start.st_mtime_ns, start.st_ino) != (
                        end.st_size, end.st_mtime_ns, end.st_ino):
                    raise EvidenceError("Input changed during collection: " + str(path))
                entry = {"source": str(path.relative_to(repo)), "bytes": end.st_size,
                         "sha256": digest.hexdigest(), "archive_member": member if copy else None}
                records.append(entry)
                return entry

            elfs = sorted(build.glob("*.elf"))
            if not elfs:
                warnings.append("NO_LOCAL_ELF_FOUND")
            for elf in elfs:
                capture(elf, "build/" + elf.name)
                capture(elf.with_suffix(".bin"), "build/" + elf.with_suffix(".bin").name)
            for name in ("project_description.json", "flasher_args.json", "flash_args",
                         "CMakeCache.txt", "config/sdkconfig.json", "config/sdkconfig.h",
                         "bootloader/bootloader.bin", "partition_table/partition-table.bin"):
                capture(build / name, "build/" + name)
            # Do not redistribute the asset/FATFS image; retain only its hash.
            capture(build / "qemu_sf2.bin", "", copy=False)
            for name in ("sdkconfig", "sdkconfig.m7release", "sdkconfig.defaults",
                         "sdkconfig.m7release.defaults", "dependencies.lock"):
                capture(repo / name, "config/" + name)
            if not (repo / "sdkconfig.m7release").is_file():
                warnings.append("SDKCONFIG_M7RELEASE_MISSING")
            for index, log in enumerate(logs, 1):
                data = log.read_bytes()
                member = f"logs/{index:02d}-{log.name}"
                z.writestr(member, data)
                identity = log_identity(data)
                identity.update(archive_member=member, sha256=hashlib.sha256(data).hexdigest())
                identities.append(identity)
                if identity["dirty_banner"]:
                    warnings.append(f"DIRTY_LOG:{index}")
                if identity["boot_version_count"] != 1:
                    warnings.append(f"LOG_BOOT_COUNT_NOT_ONE:{index}")
            if not identities:
                warnings.append("NO_LOGS_COLLECTED_COMPARE_WITH_PREVIOUSLY_SUPPLIED_LOGS")
            elf_hashes = [r["sha256"] for r in records if r["source"].endswith(".elf")]
            for identity in identities:
                identity["local_elf_prefix_matches"] = {
                    prefix: [digest for digest in elf_hashes if digest.startswith(prefix)]
                    for prefix in identity["elf_sha256_prefixes"]
                }
            after = git_snapshot(repo)
            manifest["git_snapshot_unchanged_during_collection"] = before == after
            if before != after:
                warnings.append("GIT_STATE_CHANGED_DURING_COLLECTION_DO_NOT_TREAT_AS_ATOMIC")
                for name, data in after.items():
                    z.writestr("git-after/" + name, data)
            z.writestr("manifest.json", json.dumps(manifest, indent=2, ensure_ascii=True) + "\n")
        # Atomic no-clobber publication, on the same filesystem as output.
        os.link(archive, output)
    return manifest


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", type=Path, default=Path.cwd())
    parser.add_argument("--output", type=Path, required=True, help="New ZIP outside the checkout")
    parser.add_argument("--expected", default=FROZEN_M8)
    parser.add_argument("--build-dir", default="build-m7-release")
    parser.add_argument("--log", type=Path, action="append", default=[])
    args = parser.parse_args()
    try:
        result = collect(args.repo, args.output, args.expected, args.build_dir, tuple(args.log))
    except (OSError, EvidenceError, subprocess.TimeoutExpired) as error:
        print(f"Evidence collection failed: {error}", file=sys.stderr)
        return 2
    print(f"Evidence saved: {args.output.expanduser().resolve()}")
    print("Historical source provenance: UNRESOLVED (collection is not approval)")
    print("Warnings: " + ", ".join(result["warnings"]))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
