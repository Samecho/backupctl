#!/usr/bin/env python3
"""Check the destination and deletion bounds used by the real-S3 CI helper."""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
import s3_ci


class FakeCloud(s3_ci.Cloud):
    def __init__(self, work, summary, responses):
        super().__init__(work, summary, {})
        self.responses = iter(responses)
        self.calls = []

    def request(self, operation, *options, expected_error=None):
        self.calls.append((operation, options))
        return next(self.responses)


class CloudGuardsTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="backupctl-cloud-guards-")
        self.work = Path(self.temporary.name)
        self.summary = {"backend": "real-s3", "credential_source": "temporary-environment",
                        "bucket": "private-test-bucket", "region": "ca-central-1",
                        "prefix": "backupctl/test/acceptance-" + "a" * 32,
                        "passed": True, "remote_objects_retained": True}
        self.save()

    def tearDown(self):
        self.temporary.cleanup()

    def save(self):
        (self.work / "summary.json").write_text(json.dumps(self.summary))

    def load(self, **options):
        return s3_ci.load_summary(self.work, "private-test-bucket", "ca-central-1", **options)

    def test_cleanup_requires_one_exact_generated_child(self):
        for prefix in ["", "backupctl/test", "backupctl/test/", "backupctl/production",
                       "backupctl/test/acceptance-" + "a" * 32 + "/child",
                       "backupctl/test/acceptance-" + "a" * 32 + "-sibling",
                       "backupctl/test/../production", "backupctl/test/acceptance-short"]:
            with self.subTest(prefix=prefix):
                self.summary["prefix"] = prefix
                self.save()
                with self.assertRaises(ValueError):
                    self.load(require_passed=False)

    def test_wrong_destination_and_offline_evidence_are_rejected(self):
        for key, value in [("bucket", "a-different-bucket"), ("region", "us-east-1"),
                           ("backend", "sdk-https-fixture"), ("backend", "local-harness-check"),
                           ("credential_source", "profile")]:
            with self.subTest(key=key, value=value):
                original = self.summary[key]
                self.summary[key] = value
                self.save()
                with self.assertRaises(ValueError):
                    self.load(require_passed=False)
                self.summary[key] = original

    def test_a_failed_run_can_be_cleaned_but_cannot_pass_reader_checks(self):
        self.summary["passed"] = False
        self.save()
        with self.assertRaises(ValueError):
            self.load()
        self.assertFalse(self.load(require_passed=False)["passed"])

    def test_all_objects_are_checked_before_any_delete(self):
        key = self.summary["prefix"] + "/repo.json"
        bad_responses = [
            {"IsTruncated": True, "Contents": [{"Key": key, "Size": 1}]},
            {"Contents": [{"Key": key, "Size": 1}, {"Key": "backupctl/production/secret", "Size": 1}]},
            {"Contents": [{"Key": self.summary["prefix"] + "-sibling/file", "Size": 1}]},
            {"Contents": [{"Key": key, "Size": s3_ci.MAX_BYTES + 1}]},
            {"Contents": [{"Key": key, "Size": -1}]},
            {"Contents": [{"Key": key, "Size": 1}] * (s3_ci.MAX_OBJECTS + 1)},
        ]
        for response in bad_responses:
            with self.subTest(response=response):
                cloud = FakeCloud(self.work, self.summary, [response])
                with self.assertRaises(ValueError):
                    s3_ci.cleanup(cloud)
                self.assertEqual([call[0] for call in cloud.calls], ["list-objects-v2"])

    def test_cleanup_deletes_only_listed_objects_then_verifies_empty(self):
        keys = [self.summary["prefix"] + "/repo.json", self.summary["prefix"] + "/chunks/test"]
        response = {"IsTruncated": False, "Contents": [{"Key": key, "Size": 5} for key in keys]}
        cloud = FakeCloud(self.work, self.summary, [response, {}, {}, {"IsTruncated": False}])
        result = s3_ci.cleanup(cloud)
        self.assertEqual(result, {"deleted_objects": 2, "remaining_objects": 0})
        self.assertEqual([call for call in cloud.calls if call[0] == "delete-object"],
                         [("delete-object", ("--key", key)) for key in keys])
        self.assertEqual(cloud.calls[0][1][1], self.summary["prefix"] + "/")
        saved = json.loads((self.work / "summary.json").read_text())
        self.assertFalse(saved["remote_objects_retained"])
        self.assertTrue(saved["cleanup_passed"])

    def test_sessions_need_a_token_and_cannot_override_the_endpoint(self):
        environment = {"AWS_ACCESS_KEY_ID": "fixture", "AWS_SECRET_ACCESS_KEY": "fixture",
                       "AWS_SESSION_TOKEN": "fixture-session"}
        with patch.dict(os.environ, environment, clear=True):
            self.assertEqual(s3_ci.session_environment()["AWS_CONFIG_FILE"], "/dev/null")
        for changed in [{"AWS_SESSION_TOKEN": ""}, {"AWS_ENDPOINT_URL_S3": "https://example.com"},
                        {"BACKUPCTL_TEST_S3_ENDPOINT": "https://127.0.0.1:1"},
                        {"BACKUPCTL_TEST_CA": "unrelated-ca.pem"}, {"BACKUPCTL_FAILPOINT": "scan_after"}]:
            with self.subTest(changed=changed), patch.dict(os.environ, {**environment, **changed}, clear=True):
                with self.assertRaises(ValueError):
                    s3_ci.session_environment()

    def test_a_network_error_or_expired_token_cannot_count_as_iam_denial(self):
        cloud = s3_ci.Cloud(self.work, self.summary, {})
        for code, diagnostic, accepted in [
            (0, "", False),
            (255, "Connection timed out", False),
            (254, "An error occurred (ExpiredToken) when calling PutObject", False),
            (254, "An error occurred (AccessDenied) when calling PutObject", True),
        ]:
            with self.subTest(diagnostic=diagnostic):
                result = subprocess.CompletedProcess([], code, stdout="{}", stderr=diagnostic)
                with patch.object(s3_ci.subprocess, "run", return_value=result):
                    if accepted:
                        cloud.request("put-object", "--key", "test", expected_error="AccessDenied")
                    else:
                        with self.assertRaises(RuntimeError):
                            cloud.request("put-object", "--key", "test", expected_error="AccessDenied")


if __name__ == "__main__":
    unittest.main()
