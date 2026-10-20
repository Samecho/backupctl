#!/usr/bin/env python3
"""Run a bounded synthetic acceptance sequence in a fresh private S3 prefix."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import random
import subprocess
import time
import uuid


def inventory(directory):
    result = {}
    for path in sorted(directory.rglob("*")):
        info = path.stat()
        result[path.relative_to(directory).as_posix()] = (
            path.is_dir(), None if path.is_dir() else hashlib.sha256(path.read_bytes()).hexdigest(),
            info.st_mode & 0o777, info.st_mtime_ns,
        )
    info = directory.stat()
    result["."] = (True, None, info.st_mode & 0o777, info.st_mtime_ns)
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("binary", type=Path)
    parser.add_argument("--work", type=Path, required=True, help="New directory for synthetic files and evidence")
    parser.add_argument("--bucket")
    parser.add_argument("--prefix", help="Dedicated test parent prefix; a unique child is appended")
    parser.add_argument("--region")
    parser.add_argument("--profile", help="Existing local AWS profile name; never pass credentials")
    parser.add_argument("--read-profile", help="Optional existing restore-only profile for fresh-state checks")
    parser.add_argument("--test-crash", action="store_true", help="Requires a test-enabled executable")
    parser.add_argument("--local-store", type=Path, help="Exercise the harness offline; does not validate S3")
    args = parser.parse_args()
    if not args.local_store and not all([args.bucket, args.prefix, args.region, args.profile]):
        parser.error("real S3 requires bucket, dedicated prefix, region, and an existing profile")
    if args.local_store and any([args.bucket, args.prefix, args.region, args.profile, args.read_profile]):
        parser.error("choose either real S3 or the offline harness check")
    binary = args.binary.resolve()
    args.work.mkdir(parents=True, exist_ok=False)
    prefix = f"{args.prefix.rstrip('/')}/acceptance-{uuid.uuid4().hex}" if args.prefix else None
    backend = (["--local-store", str(args.local_store.resolve())] if args.local_store else
               ["--bucket", args.bucket, "--prefix", prefix, "--region", args.region])
    summary = {"backend": "local-harness-check" if args.local_store else "real-s3",
               "bucket": args.bucket, "prefix": prefix, "region": args.region,
               "profile": args.profile, "read_profile": args.read_profile,
               "binary_sha256": hashlib.sha256(binary.read_bytes()).hexdigest(),
               "remote_objects_retained": True, "passed": False}
    summary_path = args.work / "summary.json"
    summary_path.write_text(json.dumps(summary, indent=2) + "\n")
    with (args.work / "events.jsonl").open("w") as log:
        def run(state, verb, *options, profile=None, crash=False):
            environment = dict(os.environ)
            if profile:
                environment.update(AWS_PROFILE=profile, AWS_EC2_METADATA_DISABLED="true")
            if crash:
                environment["BACKUPCTL_FAILPOINT"] = "scan_after"
            started = time.perf_counter()
            process = subprocess.run([str(binary), verb, "--state", str(state), *map(str, options)],
                                     capture_output=True, text=True, env=environment, timeout=600)
            row = {"command": verb, "returncode": process.returncode,
                   "wall_seconds": time.perf_counter() - started,
                   "results": [json.loads(line) for line in process.stdout.splitlines() if line.strip()]}
            if process.stderr.strip():
                row["diagnostics"] = process.stderr.strip().splitlines()
            log.write(json.dumps(row) + "\n")
            log.flush()
            print(json.dumps(row), flush=True)
            if crash:
                assert process.returncode == 86, "crash check needs a test-enabled build"
                return json.loads(process.stderr.splitlines()[0])["job_id"]
            if process.returncode:
                raise RuntimeError(f"{verb} failed; see events.jsonl")
            return row["results"]

        source = args.work / "source"
        source.mkdir()
        (source / "empty-directory").mkdir()
        (source / "empty-file").touch()
        block = random.Random(20261020).randbytes(4 * 1024 * 1024)
        (source / "data.bin").write_bytes(block + b"x" * (4 * 1024 * 1024) + b"tail")
        (source / "duplicate.bin").write_bytes((source / "data.bin").read_bytes())
        (source / "空 文件.txt").write_text("synthetic acceptance input\n", encoding="utf-8")
        os.chmod(source / "data.bin", 0o640)
        original = inventory(source)
        state = args.work / "state"
        run(state, "init", *backend, profile=args.profile)
        first = run(state, "backup", "--source", source, profile=args.profile)[0]
        unchanged = run(state, "backup", "--source", source, profile=args.profile)[0]
        assert unchanged["chunk_upload_bytes"] == 0
        with (source / "data.bin").open("r+b") as output:
            output.write(b"changed first block")
        (source / "duplicate.bin").unlink()
        (source / "added").write_bytes(b"new version")
        current = inventory(source)
        third = run(state, "backup", "--source", source, profile=args.profile)[0]
        assert third["new_chunks"] > 0
        snapshots = [first, unchanged, third]
        if args.test_crash:
            job = run(state, "backup", "--source", source, profile=args.profile, crash=True)
            resumed = run(state, "resume", "--job", job, profile=args.profile)[0]
            assert resumed["job_id"] == job
            assert resumed["chunk_upload_bytes"] == 0
            snapshots.append(resumed)
        listed = run(state, "list", profile=args.profile)
        assert {row["snapshot_id"] for row in listed} == {row["snapshot_id"] for row in snapshots}
        fresh = args.work / "fresh-state"
        read_profile = args.read_profile or args.profile
        run(fresh, "init", *backend, profile=read_profile)
        for index, snapshot in enumerate(snapshots):
            run(fresh, "verify", "--snapshot", snapshot["snapshot_id"], profile=read_profile)
            target = args.work / f"restored-{index}"
            run(fresh, "restore", "--snapshot", snapshot["snapshot_id"], "--target", target,
                profile=read_profile)
            assert inventory(target) == (original if index < 2 else current)
        summary.update(passed=True, snapshot_ids=[row["snapshot_id"] for row in snapshots],
                       unchanged_chunk_upload_bytes=0, fresh_state_restore=True)
        summary_path.write_text(json.dumps(summary, indent=2) + "\n")
        print(json.dumps(summary), flush=True)


if __name__ == "__main__":
    main()
