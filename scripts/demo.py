#!/usr/bin/env python3
"""Run the full offline demo with a test-enabled binary and a new work directory."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import random
import sqlite3
import subprocess


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("binary", type=Path)
    parser.add_argument("--work", type=Path, required=True)
    args = parser.parse_args()
    binary = args.binary.resolve()
    root = args.work.resolve()
    root.mkdir()  # Refuse to reuse an existing directory.
    source, state, remote = root / "source", root / "state", root / "remote"
    source.mkdir()
    (source / "empty directory").mkdir()
    (source / "空 文件.txt").touch()
    data = random.Random(20261020).randbytes(4 * 1024 * 1024)
    (source / "large.bin").write_bytes(data + b"a" * (4 * 1024 * 1024) + b"tail")
    (source / "duplicate.bin").write_bytes(data)
    original = {str(p.relative_to(source)): hashlib.sha256(p.read_bytes()).hexdigest()
                for p in source.rglob("*") if p.is_file()}
    with (root / "events.jsonl").open("w") as log:
        def run(label, command, *options, env=None, expected=0):
            process = subprocess.run([str(binary), command, "--state", str(state), *map(str, options)],
                                     capture_output=True, text=True,
                                     env={**os.environ, **(env or {})}, timeout=120)
            assert process.returncode == expected, process.stderr
            value = json.loads(process.stdout) if process.stdout.strip() else None
            log.write(json.dumps({"step": label, "exit": process.returncode,
                                  "result": value, "diagnostics": process.stderr}) + "\n")
            log.flush()
            print(f"{label}: exit={process.returncode}", flush=True)
            return value

        run("initialize", "init", "--local-store", remote)
        first = run("first backup", "backup", "--source", source)
        same = run("unchanged backup", "backup", "--source", source)
        assert same["chunk_upload_bytes"] == 0
        with (source / "large.bin").open("r+b") as output:
            output.seek(4 * 1024 * 1024)
            output.write(b"partial change")
        (source / "new").write_bytes(b"new version")
        (source / "空 文件.txt").unlink()
        run("incremental backup", "backup", "--source", source)
        (source / "interrupted").write_bytes(b"new content for interrupted job")
        run("interrupt after remote chunk PUT", "backup", "--source", source,
            env={"BACKUPCTL_FAILPOINT": "chunk_put_after"}, expected=86)
        with sqlite3.connect(state / "state.db") as database:
            job, snapshot = database.execute("SELECT id,snapshot FROM jobs ORDER BY created DESC LIMIT 1").fetchone()
        run("interrupted job status", "status", "--job", job)
        resumed = run("resume the same snapshot", "resume", "--job", job)
        assert resumed["snapshot_id"] == snapshot
        run("restore original version", "restore", "--snapshot", first["snapshot_id"],
            "--target", root / "old-version")
        restored = {str(p.relative_to(root / "old-version")): hashlib.sha256(p.read_bytes()).hexdigest()
                    for p in (root / "old-version").rglob("*") if p.is_file()}
        assert original == restored
        run("verify original version", "verify", "--snapshot", first["snapshot_id"])
        manifest = remote / "objects/manifests" / f"{first['snapshot_id']}.jsonl" / "data"
        reference = next(row for row in map(json.loads, manifest.read_text().splitlines()) if row["type"] == "chunk")
        chunk = remote / "objects" / reference["key"] / "data"
        payload = bytearray(chunk.read_bytes())
        payload[0] ^= 0xff
        chunk.write_bytes(payload)
        run("detect corrupted chunk", "verify", "--snapshot", first["snapshot_id"], expected=1)
        run("refuse corrupted restore", "restore", "--snapshot", first["snapshot_id"],
            "--target", root / "must-not-publish", expected=1)
        assert not (root / "must-not-publish").exists()
    print(f"Demo complete. Evidence: {root / 'events.jsonl'}")


if __name__ == "__main__":
    main()
