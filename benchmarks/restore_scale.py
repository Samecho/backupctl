#!/usr/bin/env python3
"""Dense-data and large-directory round trips on a Linux filesystem."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import random
import shutil
import subprocess
import time

CHUNK = 4 * 1024 * 1024
SEED = 20261020


def command(*args):
    return subprocess.check_output(args, text=True).strip()


def inventory(root):
    digest = hashlib.sha256()
    total = allocated = files = 0
    for directory, names, filenames in os.walk(root):
        names.sort()
        for name in [".", *sorted(filenames)]:
            path = Path(directory) / name
            info = path.stat()
            relative = path.relative_to(root).as_posix()
            record = [relative, path.is_dir(), info.st_mode & 0o777, info.st_mtime_ns]
            if path.is_file():
                file_hash = hashlib.sha256()
                with path.open("rb") as source:
                    while block := source.read(CHUNK):
                        file_hash.update(block)
                record.extend([info.st_size, file_hash.hexdigest()])
                total += info.st_size
                allocated += info.st_blocks * 512
                files += 1
            digest.update(json.dumps(record, ensure_ascii=False).encode() + b"\n")
    return {"sha256": digest.hexdigest(), "bytes": total, "files": files,
            "allocated_file_bytes": allocated}


def generate(source, size_mib, small_files):
    source.mkdir()
    if size_mib:
        generator = random.Random(SEED)
        with (source / "dense-mixed.bin").open("wb") as output:
            for _ in range(size_mib // 4):
                # Every block is written, with unique random bytes and a compressible half.
                output.write(generator.randbytes(CHUNK // 2))
                output.write(b"\0" * (CHUNK // 2))
            output.flush()
            os.fsync(output.fileno())
    for i in range(small_files):
        directory = source / f"d{i // 1000:04}"
        directory.mkdir(exist_ok=True)
        (directory / f"f{i:06}").write_bytes((i % 64).to_bytes(8, "little") + bytes([i % 16]) * 248)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("binary", type=Path)
    parser.add_argument("--work", type=Path, required=True, help="New scratch directory on Linux ext4")
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--size-mib", type=int, default=50 * 1024)
    parser.add_argument("--small-files", type=int, default=100000)
    parser.add_argument("--source-revision", default=None, help="Revision when .git is outside the container")
    args = parser.parse_args()
    if args.size_mib < 0 or args.size_mib % 4 or args.small_files < 0:
        parser.error("size must be a nonnegative multiple of 4 MiB; file count must be nonnegative")
    if not args.size_mib and not args.small_files:
        parser.error("at least one workload is required")
    binary = args.binary.resolve()
    args.work.mkdir(parents=True, exist_ok=False)
    args.output.mkdir(parents=True, exist_ok=True)
    filesystem = command("stat", "-f", "-c", "%T", str(args.work))
    if filesystem in {"9p", "drvfs", "fuseblk"}:
        raise RuntimeError("run the scratch workload on a Linux filesystem, not the Windows source mount")
    required = args.size_mib * 1024**2 * 3 + args.small_files * 32768 + 2 * 1024**3
    if shutil.disk_usage(args.work).free < required:
        raise RuntimeError(f"workload needs at least {required} free bytes")
    environment = {
        "source_revision": args.source_revision or command("git", "rev-parse", "HEAD"),
        "binary_sha256": hashlib.sha256(binary.read_bytes()).hexdigest(),
        "platform": platform.platform(), "cpu": command("lscpu"), "filesystem": filesystem,
        "memory": Path("/proc/meminfo").read_text(), "seed": SEED,
        "size_mib": args.size_mib, "small_files": args.small_files,
        "storage": "offline local store; dense source and dense restore", "network": "offline",
    }
    (args.output / "environment.json").write_text(json.dumps(environment, indent=2) + "\n")
    with (args.output / "raw.jsonl").open("w") as log:
        def record(row):
            log.write(json.dumps(row, sort_keys=True) + "\n")
            log.flush()
            print(json.dumps(row), flush=True)

        def run(state, verb, *options):
            started = time.perf_counter()
            process = subprocess.run([str(binary), verb, "--state", str(state), *map(str, options)],
                                     capture_output=True, text=True, timeout=3600)
            if process.returncode:
                raise RuntimeError(f"{verb}: {process.stderr}")
            report = json.loads(process.stdout)
            if verb in {"backup", "verify", "restore"}:
                assert report["peak_rss_kib"] <= 512 * 1024, report
            return {"command": verb, "wall_seconds": time.perf_counter() - started, "report": report}

        for label, size_mib, count in [("dense-data", args.size_mib, 0),
                                      ("small-files", 0, args.small_files)]:
            if not size_mib and not count:
                continue
            root = args.work / label
            root.mkdir()
            source, state = root / "source", root / "state"
            print(f"Generating {label}: {size_mib} MiB, {count} small files", flush=True)
            generate(source, size_mib, count)
            before = inventory(source)
            assert before["bytes"] == size_mib * 1024**2 + count * 256
            assert before["allocated_file_bytes"] >= before["bytes"], before
            record({"dataset": label, "command": "source_inventory", "inventory": before})
            run(state, "init", "--local-store", root / "remote")
            backup = run(state, "backup", "--source", source)
            record({"dataset": label, **backup})
            snapshot = backup["report"]["snapshot_id"]
            fresh_state = root / "fresh-state"
            run(fresh_state, "init", "--local-store", root / "remote")
            record({"dataset": label, **run(fresh_state, "verify", "--snapshot", snapshot)})
            restored = root / "restored"
            record({"dataset": label, **run(fresh_state, "restore", "--snapshot", snapshot,
                                           "--target", restored)})
            after = inventory(restored)
            assert after["sha256"] == before["sha256"], (before, after)
            assert after["allocated_file_bytes"] >= after["bytes"], after
            record({"dataset": label, "command": "restore_inventory", "inventory": after,
                    "matches_source": True, "fresh_state": True})
            # Only remove the synthetic workload just created by this invocation.
            shutil.rmtree(root)


if __name__ == "__main__":
    main()
