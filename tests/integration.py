#!/usr/bin/env python3
"""Offline CLI acceptance checks. All data is synthetic and temporary."""
import hashlib
import json
import os
from pathlib import Path
import random
import sqlite3
import subprocess
import sys
import tempfile
import unittest

BINARY = str(Path(sys.argv.pop(1)).resolve())


class BackupTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="backupctl-")
        self.root = Path(self.temporary.name)
        self.source = self.root / "source"
        self.source.mkdir()
        self.state = self.root / "state"
        self.remote = self.root / "remote"
        self.call("init", "--local-store", self.remote)

    def tearDown(self):
        self.temporary.cleanup()

    def call(self, command, *args, ok=True, env=None, state=None):
        result = subprocess.run(
            [BINARY, command, "--state", str(state or self.state), *map(str, args)],
            capture_output=True, text=True, env={**os.environ, **(env or {})}, timeout=90,
        )
        if ok:
            self.assertEqual(result.returncode, 0, result.stderr)
            return json.loads(result.stdout) if result.stdout.strip() else None
        self.assertNotEqual(result.returncode, 0, result.stdout)
        return json.loads(result.stderr.strip().splitlines()[-1]) if result.stderr.strip() else result

    def backup(self, **kwargs):
        return self.call("backup", "--source", self.source, **kwargs)

    def inventory(self, directory):
        return {
            str(path.relative_to(directory)): (
                path.is_dir(),
                None if path.is_dir() else hashlib.sha256(path.read_bytes()).hexdigest(),
                path.stat().st_mode & 0o777,
                path.stat().st_mtime_ns,
            )
            for path in directory.rglob("*")
        }

    def test_roundtrip_dedup_incremental_and_fresh_state(self):
        (self.source / "empty directory").mkdir()
        (self.source / "空 文件.txt").touch()
        (self.source / "nested").mkdir()
        data = random.Random(42).randbytes(4 * 1024 * 1024 + 123)
        (self.source / "nested/big").write_bytes(data)
        (self.source / "copy").write_bytes(data)
        (self.source / "compressible").write_bytes(b"a" * (4 * 1024 * 1024 + 10))
        os.chmod(self.source / "copy", 0o640)
        original = self.inventory(self.source)
        first = self.backup()
        self.assertGreater(first["chunk_upload_bytes"], 0)
        self.assertEqual(first["new_chunks"], 4)
        second = self.backup()
        self.assertEqual(second["chunk_upload_bytes"], 0)
        self.call("verify", "--snapshot", first["snapshot_id"])
        (self.source / "copy").unlink()
        (self.source / "new").write_bytes(b"new version")
        third = self.backup()
        self.assertGreater(third["chunk_upload_bytes"], 0)
        fresh = self.root / "fresh-state"
        self.call("init", "--local-store", self.remote, state=fresh)
        restored = self.root / "restored"
        self.call("restore", "--snapshot", first["snapshot_id"], "--target", restored, state=fresh)
        self.assertEqual(original, self.inventory(restored))
        current = self.root / "current"
        self.call("restore", "--snapshot", third["snapshot_id"], "--target", current)
        self.assertEqual(self.inventory(self.source), self.inventory(current))
        self.call("restore", "--snapshot", first["snapshot_id"], "--target", restored, ok=False)

    def test_empty_tree_and_source_symlink(self):
        result = self.backup()
        target = self.root / "empty-restore"
        self.call("restore", "--snapshot", result["snapshot_id"], "--target", target)
        self.assertEqual(list(target.iterdir()), [])
        (self.source / "link").symlink_to("/etc/passwd")
        failure = self.backup(ok=False)
        self.assertEqual(failure["error"], "unsupported")
        self.assertIn("job_id", failure)

    def test_missing_chunk_refuses_restore_publication(self):
        (self.source / "file").write_bytes(b"some bytes")
        result = self.backup()
        data = next((self.remote / "objects/chunks").rglob("data"))
        data.unlink()
        self.call("verify", "--snapshot", result["snapshot_id"], ok=False)
        target = self.root / "bad-restore"
        self.call("restore", "--snapshot", result["snapshot_id"], "--target", target, ok=False)
        self.assertFalse(target.exists())

    def test_parallel_limits_and_same_job_dedup(self):
        block = b"z" * (4 * 1024 * 1024)
        (self.source / "repeated").write_bytes(block * 8)
        result = self.call("backup", "--source", self.source, "--workers", 8, "--memory-mib", 128)
        self.assertEqual(result["new_chunks"], 1)
        self.assertGreater(result["peak_inflight_chunks"], 1)
        self.assertLessEqual(result["peak_inflight_chunks"], result["inflight_limit"])
        self.assertLessEqual(result["data_buffer_bound_bytes"], result["data_budget_bytes"])
        self.call("verify", "--snapshot", result["snapshot_id"])
        limited = self.call("backup", "--source", self.source, "--workers", 8, "--memory-mib", 32)
        self.assertEqual(limited["active_workers"], 1)
        self.assertEqual(limited["chunk_upload_bytes"], 0)

    def test_status_works_without_remote_access(self):
        (self.source / "file").write_bytes(b"contents")
        result = self.backup()
        (self.remote / "objects/repo.json/data").unlink()
        status = self.call("status", "--job", result["job_id"])
        self.assertEqual(status["phase"], "committed")
        self.assertEqual(status["commit_confirmation"], "previously_confirmed")
        self.assertFalse(status["remote_checked"])

    def test_worker_filesystem_and_json_errors_are_persisted(self):
        (self.source / "file").write_bytes(b"contents")
        chunks = self.remote / "objects/chunks"
        chunks.write_bytes(b"not a directory")
        for workers in [1, 4]:
            failure = self.call("backup", "--source", self.source, "--workers", workers, ok=False)
            self.assertEqual(failure["error"], "io")
            status = self.call("status", "--job", failure["job_id"])
            self.assertEqual(status["error_category"], failure["error"])
            self.assertEqual(status["error_message"], failure["message"])
        chunks.unlink()
        self.backup()
        metadata = next(chunks.rglob("meta.json"))
        metadata.write_text("{broken json", encoding="utf-8")
        for workers in [1, 4]:
            failure = self.call("backup", "--source", self.source, "--workers", workers, ok=False)
            self.assertEqual(failure["error"], "integrity")
            status = self.call("status", "--job", failure["job_id"])
            self.assertEqual(status["error_category"], failure["error"])
            self.assertEqual(status["error_message"], failure["message"])

    def test_error_recording_failure_preserves_original_error(self):
        (self.source / "file").write_bytes(b"contents")
        self.backup()
        metadata = next((self.remote / "objects/chunks").rglob("meta.json"))
        metadata.write_text("{broken json", encoding="utf-8")
        with sqlite3.connect(self.state / "state.db") as database:
            database.executescript("""
                CREATE TRIGGER deny_error_record BEFORE UPDATE OF error_category ON jobs
                WHEN NEW.error_category != '' BEGIN
                    SELECT RAISE(ABORT, 'secondary checkpoint failure');
                END;
            """)
        failure = self.backup(ok=False)
        self.assertEqual(failure["error"], "integrity")
        self.assertIn("parse_error", failure["message"])
        self.assertNotIn("secondary checkpoint failure", failure["message"])


if __name__ == "__main__":
    unittest.main()
