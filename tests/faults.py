#!/usr/bin/env python3
"""Deterministic checkpoint recovery and fixed-seed process termination tests."""
import hashlib
import json
import os
from pathlib import Path
import random
import shutil
import sqlite3
import subprocess
import sys
import tempfile
import time
import unittest

BINARY = str(Path(sys.argv.pop(1)).resolve())
POINTS = [
    "scan_after", "chunk_put_before", "chunk_put_after", "checkpoint_before",
    "checkpoint_after", "source_checked", "manifest_after", "commit_before",
    "commit_after", "committed_after",
]
SEED = 20261020


class FaultTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="backupctl-fault-")
        self.root = Path(self.temporary.name)
        self.source = self.root / "source"
        self.source.mkdir()
        for i in range(3):
            (self.source / str(i)).write_bytes(bytes([65 + i]) * 8192)
        self.attempt = 0
        self.fresh()

    def tearDown(self):
        self.temporary.cleanup()

    def run_cli(self, command, *args, env=None, success=True):
        result = subprocess.run([BINARY, command, "--state", str(self.state), *map(str, args)],
                                env={**os.environ, **(env or {})}, capture_output=True,
                                text=True, timeout=90)
        if success:
            self.assertEqual(result.returncode, 0, result.stderr)
        return result

    def fresh(self):
        self.attempt += 1
        self.state = self.root / f"state-{self.attempt}"
        self.remote = self.root / f"remote-{self.attempt}"
        self.run_cli("init", "--local-store", self.remote)

    def job(self):
        with sqlite3.connect(self.state / "state.db") as db:
            return db.execute("SELECT id,snapshot,phase FROM jobs ORDER BY created DESC LIMIT 1").fetchone()

    def resume_and_check(self, before):
        job, snapshot, _ = before
        result = json.loads(self.run_cli("resume", "--job", job).stdout)
        self.assertEqual(result["snapshot_id"], snapshot)
        self.assertEqual(result["phase"], "committed")
        listing = self.run_cli("list").stdout.strip().splitlines()
        self.assertEqual(len(listing), 1)
        self.run_cli("verify", "--snapshot", snapshot)
        self.assertEqual(len(list((self.remote / "objects/chunks").rglob("data"))), 3)
        return snapshot

    def test_each_checkpoint_ten_times(self):
        for point in POINTS:
            for repeat in range(10):
                with self.subTest(point=point, repeat=repeat):
                    self.fresh()
                    result = self.run_cli("backup", "--source", self.source,
                                          env={"BACKUPCTL_FAILPOINT": point}, success=False)
                    self.assertEqual(result.returncode, 86, result.stderr)
                    before = self.job()
                    visible = self.run_cli("list").stdout.strip().splitlines()
                    self.assertEqual(len(visible), 1 if point in {"commit_after", "committed_after"} else 0)
                    self.resume_and_check(before)

    def test_fixed_seed_fifty_sigkills(self):
        generator = random.Random(SEED)
        positions = []
        for iteration in range(50):
            self.fresh()
            delay = generator.uniform(0.004, 0.16)
            process = subprocess.Popen(
                [BINARY, "backup", "--state", str(self.state), "--source", str(self.source)],
                env={**os.environ, "BACKUPCTL_TEST_DELAY_MS": "40"},
                stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
            )
            # Wait for the durable job record. A kill during initial configuration has no job to resume.
            deadline = time.monotonic() + 15
            while True:
                try:
                    before = self.job()
                    if before and before[2] != "scanning":
                        break
                except sqlite3.Error:
                    pass
                self.assertLess(time.monotonic(), deadline)
                time.sleep(0.002)
            time.sleep(delay)
            self.assertIsNone(process.poll(), "injected delay must keep the process running until SIGKILL")
            process.kill()
            process.communicate(timeout=10)
            self.assertEqual(process.returncode, -9)
            positions.append({"iteration": iteration, "delay_seconds": delay, "phase": self.job()[2]})
            self.resume_and_check(before)
        print(json.dumps({"seed": SEED, "sigkill_positions": positions}))

    def test_lost_response_and_unresolved_commit(self):
        for kind in ["chunks", "manifests", "snapshots"]:
            self.fresh()
            result = self.run_cli("backup", "--source", self.source,
                                  env={"BACKUPCTL_TEST_LOST_RESPONSE": kind})
            self.assertEqual(json.loads(result.stdout)["phase"], "committed")
            self.resume_and_check(self.job())
        self.fresh()
        result = self.run_cli("backup", "--source", self.source,
                              env={"BACKUPCTL_TEST_COMMIT_READ_FAILURE": "1"}, success=False)
        self.assertNotEqual(result.returncode, 0)
        failure = json.loads(result.stderr.strip().splitlines()[-1])
        self.assertEqual(failure["error"], "commit_unknown")
        self.assertEqual(self.job()[2], "commit_unknown")
        self.resume_and_check(self.job())

    def test_resume_refuses_source_change_and_permission_denial(self):
        self.run_cli("backup", "--source", self.source,
                     env={"BACKUPCTL_FAILPOINT": "chunk_put_after"}, success=False)
        job = self.job()[0]
        (self.source / "0").write_bytes(b"changed")
        result = self.run_cli("resume", "--job", job, success=False)
        self.assertEqual(json.loads(result.stderr.strip().splitlines()[-1])["error"], "source_changed")
        self.assertEqual(self.run_cli("list").stdout, "")
        self.fresh()
        result = self.run_cli("backup", "--source", self.source,
                              env={"BACKUPCTL_TEST_PUT_PERMISSION": "1"}, success=False)
        self.assertEqual(json.loads(result.stderr.strip().splitlines()[-1])["error"], "permission")
        self.assertEqual(self.run_cli("list").stdout, "")

    def test_restore_publication_checkpoints(self):
        result = self.run_cli("backup", "--source", self.source)
        snapshot = json.loads(result.stdout)["snapshot_id"]
        for point in ["restore_publish_before", "restore_publish_after"]:
            for repeat in range(10):
                target = self.root / f"restore-{point}-{repeat}"
                result = self.run_cli("restore", "--snapshot", snapshot, "--target", target,
                                      env={"BACKUPCTL_FAILPOINT": point}, success=False)
                self.assertEqual(result.returncode, 86)
                self.assertEqual(target.exists(), point.endswith("after"))
                if target.exists():
                    for path in self.source.iterdir():
                        self.assertEqual((target / path.name).read_bytes(), path.read_bytes())

    def test_conflicting_commit_stays_unresolved_and_is_not_overwritten(self):
        result = self.run_cli("backup", "--source", self.source,
                              env={"BACKUPCTL_FAILPOINT": "commit_before"}, success=False)
        self.assertEqual(result.returncode, 86)
        job, snapshot, _ = self.job()
        with sqlite3.connect(self.state / "state.db") as db:
            planned = json.loads(db.execute("SELECT commit_json FROM jobs WHERE id=?", (job,)).fetchone()[0])
        planned["created_ns"] += 1
        payload = json.dumps(planned).encode()
        path = self.remote / "objects/snapshots" / f"{snapshot}.json"
        path.mkdir(parents=True)
        (path / "data").write_bytes(payload)
        (path / "meta.json").write_text(json.dumps({
            "size": len(payload), "metadata": {"encoded-sha256": hashlib.sha256(payload).hexdigest()}
        }))
        result = self.run_cli("resume", "--job", job, success=False)
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(json.loads(result.stderr.strip().splitlines()[-1])["error"], "integrity")
        self.assertEqual(self.job()[2], "commit_unknown")
        self.assertEqual((path / "data").read_bytes(), payload)


if __name__ == "__main__":
    unittest.main()
