#!/usr/bin/env python3
"""Adversarial manifests, local process exclusion, and failure publication rules."""
import hashlib
import json
import os
from pathlib import Path
import signal
import sqlite3
import subprocess
import sys
import tempfile
import time
import unittest

BINARY = str(Path(sys.argv.pop(1)).resolve())


class SecurityTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="backupctl-security-")
        self.root = Path(self.temporary.name)
        self.source = self.root / "source"
        self.source.mkdir()
        (self.source / "file").write_bytes(b"test contents")
        self.state = self.root / "state"
        self.remote = self.root / "remote"
        self.call("init", "--local-store", self.remote)

    def tearDown(self):
        self.temporary.cleanup()

    def call(self, command, *args, ok=True, env=None):
        result = subprocess.run([BINARY, command, "--state", str(self.state), *map(str, args)],
                                env={**os.environ, **(env or {})}, text=True,
                                capture_output=True, timeout=90)
        if ok:
            self.assertEqual(result.returncode, 0, result.stderr)
            return json.loads(result.stdout) if result.stdout.strip() else None
        self.assertNotEqual(result.returncode, 0, result.stdout)
        return json.loads(result.stderr.strip().splitlines()[-1])

    def replace_object(self, key, data):
        path = self.remote / "objects" / key
        (path / "data").write_bytes(data)
        meta = json.loads((path / "meta.json").read_text())
        meta["size"] = len(data)
        meta["metadata"]["encoded-sha256"] = hashlib.sha256(data).hexdigest()
        (path / "meta.json").write_text(json.dumps(meta))

    def test_untrusted_manifest_paths_sizes_order_and_conflicts(self):
        first = self.call("backup", "--source", self.source)
        snapshot = first["snapshot_id"]
        commit_key = f"snapshots/{snapshot}.json"
        manifest_key = f"manifests/{snapshot}.jsonl"
        original_commit = json.loads((self.remote / "objects" / commit_key / "data").read_bytes())
        original = (self.remote / "objects" / manifest_key / "data").read_bytes()
        mutations = [
            ("parent traversal", lambda rows: rows[2].update(path="../escaped")),
            ("absolute", lambda rows: rows[2].update(path=str(self.root / "escaped"))),
            ("NUL", lambda rows: rows[2].update(path="bad\0name")),
            ("duplicate root", lambda rows: rows.insert(2, dict(rows[1]))),
            ("missing parent", lambda rows: rows[2].update(path="missing/child")),
            ("file directory conflict", lambda rows: rows[2].update(path=".")),
            ("oversized record", lambda rows: rows[0].update(padding="x" * 70000)),
            ("decompression limit", lambda rows: rows[3].update(raw_size=4 * 1024 * 1024 + 1)),
            ("negative size", lambda rows: rows[2].update(size=-1)),
            ("integer overflow", lambda rows: rows[2].update(size=2**80)),
            ("wrong chunk file", lambda rows: rows[3].update(file_id=2)),
            ("wrong chunk key", lambda rows: rows[3].update(key="repo.json")),
            ("wrong file hash", lambda rows: rows[2].update(sha256="0" * 64)),
            ("wrong totals", lambda rows: rows[-1].update(files=99)),
            ("records after end", lambda rows: rows.append(dict(rows[-1]))),
        ]
        for name, mutate in mutations:
            with self.subTest(name=name):
                rows = [json.loads(line) for line in original.splitlines()]
                mutate(rows)
                data = b"".join(json.dumps(row).encode() + b"\n" for row in rows)
                self.replace_object(manifest_key, data)
                commit = dict(original_commit, manifest_size=len(data),
                              manifest_sha256=hashlib.sha256(data).hexdigest())
                self.replace_object(commit_key, json.dumps(commit).encode())
                self.call("verify", "--snapshot", snapshot, ok=False)
                target = self.root / "restore"
                self.call("restore", "--snapshot", snapshot, "--target", target, ok=False)
                self.assertFalse(target.exists())
                self.assertFalse((self.root / "escaped").exists())

    def test_disk_full_and_existing_symlink_do_not_publish(self):
        snapshot = self.call("backup", "--source", self.source)["snapshot_id"]
        target = self.root / "restore"
        failure = self.call("restore", "--snapshot", snapshot, "--target", target, ok=False,
                            env={"BACKUPCTL_TEST_DISK_FULL": "1"})
        self.assertEqual(failure["error"], "io")
        self.assertFalse(target.exists())
        target.symlink_to(self.root / "missing", target_is_directory=True)
        self.call("restore", "--snapshot", snapshot, "--target", target, ok=False)
        self.assertTrue(target.is_symlink())

    def test_source_state_overlap_and_incompatible_init(self):
        self.call("init", "--local-store", self.root / "other-store", ok=False)
        self.assertFalse((self.root / "other-store").exists())
        result = subprocess.run([BINARY, "init", "--state", str(self.source / "state"),
                                 "--local-store", str(self.remote)], capture_output=True)
        self.assertEqual(result.returncode, 0)
        result = subprocess.run([BINARY, "backup", "--state", str(self.source / "state"),
                                 "--source", str(self.source)], capture_output=True, text=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(json.loads(result.stderr)["error"], "configuration")

    def test_special_files_non_utf8_and_permission_denial(self):
        fifo = self.source / "fifo"
        os.mkfifo(fifo)
        self.assertEqual(self.call("backup", "--source", self.source, ok=False)["error"], "unsupported")
        fifo.unlink()
        bad = os.fsencode(self.source) + b"/\xff"
        descriptor = os.open(bad, os.O_CREAT | os.O_WRONLY, 0o600)
        os.close(descriptor)
        self.assertEqual(self.call("backup", "--source", self.source, ok=False)["error"], "unsupported")
        os.unlink(bad)
        if os.geteuid() != 0:
            os.chmod(self.source / "file", 0)
            try:
                failure = self.call("backup", "--source", self.source, ok=False)
                self.assertEqual(failure["error"], "permission")
            finally:
                os.chmod(self.source / "file", 0o600)

    def slow_process(self):
        for i in range(8):
            (self.source / str(i)).write_bytes(bytes([i]) * 8192)
        process = subprocess.Popen([BINARY, "backup", "--state", str(self.state),
                                    "--source", str(self.source)],
                                   env={**os.environ, "BACKUPCTL_TEST_DELAY_MS": "150"},
                                   stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        deadline = time.monotonic() + 15
        while True:
            try:
                with sqlite3.connect(self.state / "state.db") as db:
                    row = db.execute("SELECT phase FROM jobs LIMIT 1").fetchone()
                    if row and row[0] == "uploading":
                        return process
            except sqlite3.Error:
                pass
            self.assertLess(time.monotonic(), deadline)
            time.sleep(0.005)

    def test_second_writer_rejected_and_sigterm_joins_workers(self):
        process = self.slow_process()
        try:
            failure = self.call("backup", "--source", self.source, ok=False)
            self.assertEqual(failure["error"], "busy")
            process.send_signal(signal.SIGTERM)
            _, errors = process.communicate(timeout=10)
            self.assertNotEqual(process.returncode, 0)
            self.assertEqual(json.loads(errors.strip().splitlines()[-1])["error"], "cancelled")
            self.assertIsNone(self.call("list"))
        finally:
            if process.poll() is None:
                process.kill()
                process.communicate()

    def test_mutation_while_uploading_rejects_commit(self):
        process = self.slow_process()
        try:
            (self.source / "file").write_bytes(b"updated during upload")
            _, errors = process.communicate(timeout=15)
            self.assertNotEqual(process.returncode, 0)
            self.assertEqual(json.loads(errors.strip().splitlines()[-1])["error"], "source_changed")
            self.assertIsNone(self.call("list"))
        finally:
            if process.poll() is None:
                process.kill()
                process.communicate()


if __name__ == "__main__":
    unittest.main()
