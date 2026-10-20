#!/usr/bin/env python3
"""Synthetic offline benchmark. Never reads personal files or contacts S3."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import random
import shutil
import statistics
import subprocess
import tempfile
import time

CHUNK = 4 * 1024 * 1024
SEED = 20261020


def command(*args):
    return subprocess.check_output(args, text=True).strip()


def run_cli(binary, state, verb, *args, delay=0):
    started = time.perf_counter()
    process = subprocess.run([str(binary), verb, "--state", str(state), *map(str, args)],
                             capture_output=True, text=True,
                             env={**os.environ, "BACKUPCTL_TEST_DELAY_MS": str(delay)}, timeout=1800)
    elapsed = time.perf_counter() - started
    if process.returncode:
        raise RuntimeError(f"{verb}: {process.stderr}")
    return json.loads(process.stdout), elapsed


def generate(source, kind, size_mib, small_files):
    generator = random.Random(SEED)
    source.mkdir()
    if kind in {"compressible", "random", "delayed"}:
        with (source / "large.bin").open("wb") as output:
            for i in range(size_mib // 4):
                block = generator.randbytes(CHUNK) if kind == "random" else bytes([i % 251]) * CHUNK
                output.write(block)
    else:
        for i in range(small_files):
            directory = source / f"d{i // 1000:03}"
            directory.mkdir(exist_ok=True)
            (directory / f"f{i:06}").write_bytes(bytes([i % 16]) * 256)
        if kind == "mixed":
            with (source / "large.bin").open("wb") as output:
                for i in range(size_mib // 4):
                    output.write(generator.randbytes(CHUNK) if i % 2 else b"x" * CHUNK)


def contents_digest(source):
    digest = hashlib.sha256()
    total = 0
    for path in sorted(source.rglob("*")):
        if path.is_file():
            file_hash = hashlib.sha256()
            with path.open("rb") as data:
                while block := data.read(CHUNK):
                    file_hash.update(block)
                    total += len(block)
            digest.update(str(path.relative_to(source)).encode() + b"\0" + file_hash.digest())
    return digest.hexdigest(), total


def mutate(source, change):
    path = source / "large.bin"
    if not path.exists():
        path = next(source.rglob("f*"))
    if change == "partial":
        with path.open("r+b") as output:
            output.write(b"changed block")
        (source / "added").write_bytes(b"new file")
    elif change == "insert":
        temporary = path.with_name(path.name + ".new")
        with path.open("rb") as original, temporary.open("wb") as output:
            output.write(b"insertion")
            shutil.copyfileobj(original, output, CHUNK)
        temporary.replace(path)
    elif change == "wide":
        generator = random.Random(SEED + 1)
        with path.open("r+b") as output:
            offset = 0
            while offset < path.stat().st_size:
                output.seek(offset)
                output.write(generator.randbytes(min(4096, path.stat().st_size - offset)))
                offset += CHUNK
        (source / "added").unlink(missing_ok=True)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("binary", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--size-mib", type=int, default=64)
    parser.add_argument("--small-files", type=int, default=1000)
    parser.add_argument("--repeats", type=int, default=5)
    parser.add_argument("--stress", action="store_true")
    args = parser.parse_args()
    assert args.size_mib >= 8 and args.size_mib % 4 == 0 and args.repeats >= 5
    binary = args.binary.resolve()
    args.output.mkdir(parents=True, exist_ok=True)
    environment = {
        "commit": command("git", "rev-parse", "HEAD"), "seed": SEED,
        "binary_sha256": hashlib.sha256(binary.read_bytes()).hexdigest(),
        "platform": platform.platform(), "cpu": command("lscpu"),
        "memory": Path("/proc/meminfo").read_text(), "compiler": command("c++", "--version"),
        "cmake": command("cmake", "--version"), "build": "Release, C++20, same durability and hashes",
        "vcpkg_baseline": json.loads(Path("vcpkg.json").read_text())["builtin-baseline"],
        "storage": "local filesystem test store", "network": "offline", "region": None,
        "size_mib": args.size_mib, "small_files": args.small_files, "repeats": args.repeats,
    }
    status = binary.parent / "vcpkg_installed/vcpkg/status"
    if status.exists():
        (args.output / "dependencies.txt").write_text(status.read_text())
    rows = []
    (args.output / "environment.json").write_text(json.dumps(environment, indent=2) + "\n")
    with (args.output / "raw.jsonl").open("w") as raw:
        def record(value):
            rows.append(value)
            raw.write(json.dumps(value, sort_keys=True) + "\n")
            raw.flush()

        for kind in ["compressible", "random", "mixed", "small", "delayed"]:
            for workers in [1, 2, 4, 8]:
                for repeat in range(args.repeats):
                    with tempfile.TemporaryDirectory(prefix="backupctl-benchmark-") as temporary:
                        root = Path(temporary)
                        source, state = root / "source", root / "state"
                        generate(source, kind, args.size_mib, args.small_files)
                        environment["filesystem"] = command("stat", "-f", "-c", "%T", str(root))
                        run_cli(binary, state, "init", "--local-store", root / "remote")
                        for change in ["cold", "unchanged", "partial", "insert", "wide"]:
                            mutate(source, change)
                            digest, logical_bytes = contents_digest(source)
                            report, wall = run_cli(binary, state, "backup", "--source", source,
                                                   "--workers", workers, "--memory-mib", 128,
                                                   delay=40 if kind == "delayed" else 0)
                            assert report["data_buffer_bound_bytes"] <= 128 * 1024 * 1024
                            assert report["peak_inflight_chunks"] <= report["inflight_limit"]
                            if change == "unchanged":
                                assert report["chunk_upload_bytes"] == 0
                            record({"dataset": kind, "change": change, "workers": workers,
                                    "repeat": repeat, "dataset_sha256": digest,
                                    "logical_bytes": logical_bytes, "wall_seconds": wall, "report": report})
                        verified, verify_wall = run_cli(binary, state, "verify", "--snapshot", report["snapshot_id"])
                        restored, restore_wall = run_cli(binary, state, "restore", "--snapshot", report["snapshot_id"],
                                                        "--target", root / "restored")
                        assert contents_digest(root / "restored")[0] == digest
                        record({"dataset": kind, "change": "restore_verify", "workers": workers,
                                "repeat": repeat, "verify_wall_seconds": verify_wall,
                                "restore_wall_seconds": restore_wall, "verified": verified, "report": restored})
                        print(f"{kind}: workers={workers}, repeat={repeat + 1}/{args.repeats}", flush=True)
        if args.stress:
            with tempfile.TemporaryDirectory(prefix="backupctl-stress-") as temporary:
                root = Path(temporary)
                source = root / "source"
                source.mkdir()
                with (source / "sparse-zero.bin").open("wb") as output:
                    output.truncate(50 * 1024**3)
                state = root / "state"
                run_cli(binary, state, "init", "--local-store", root / "remote")
                report, wall = run_cli(binary, state, "backup", "--source", source)
                assert report["logical_bytes"] == 50 * 1024**3 and report["peak_rss_kib"] <= 512 * 1024
                record({"dataset": "50-GiB-sparse-zero", "change": "resource_acceptance",
                        "workers": 4, "wall_seconds": wall, "report": report,
                        "note": "logical input is sparse; this is not a physical-disk throughput result"})
            with tempfile.TemporaryDirectory(prefix="backupctl-small-stress-") as temporary:
                root = Path(temporary)
                source, state = root / "source", root / "state"
                generate(source, "small", args.size_mib, 100000)
                run_cli(binary, state, "init", "--local-store", root / "remote")
                report, wall = run_cli(binary, state, "backup", "--source", source)
                assert report["files"] == 100000 and report["peak_rss_kib"] <= 512 * 1024
                record({"dataset": "100000-small-files", "change": "resource_acceptance",
                        "workers": 4, "wall_seconds": wall, "report": report})
    (args.output / "environment.json").write_text(json.dumps(environment, indent=2) + "\n")
    summary = []
    for key in sorted({(r["dataset"], r["change"], r["workers"]) for r in rows if "wall_seconds" in r}):
        selected = [r for r in rows if (r["dataset"], r["change"], r["workers"]) == key]
        times = [r["wall_seconds"] for r in selected]
        summary.append({"dataset": key[0], "change": key[1], "workers": key[2],
                        "runs": len(times), "median_seconds": statistics.median(times),
                        "min_seconds": min(times), "max_seconds": max(times),
                        "max_rss_kib": max(r["report"]["peak_rss_kib"] for r in selected)})
    (args.output / "summary.json").write_text(json.dumps(summary, indent=2) + "\n")


if __name__ == "__main__":
    main()
