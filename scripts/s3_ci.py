#!/usr/bin/env python3
"""Check scoped real-S3 access and clean up one disposable acceptance prefix."""
import argparse
import json
import os
from pathlib import Path
import re
import subprocess
import time

from s3_acceptance import inventory

MAX_OBJECTS = 128
MAX_BYTES = 32 * 1024 * 1024


def load_summary(work, bucket, region, require_passed=True):
    summary = json.loads((work / "summary.json").read_text())
    if summary.get("backend") != "real-s3" or summary.get("credential_source") != "temporary-environment":
        raise ValueError("cloud checks require real S3 and temporary session credentials")
    if summary.get("bucket") != bucket or summary.get("region") != region:
        raise ValueError("acceptance destination differs from the configured test destination")
    if not re.fullmatch(r"backupctl/test/acceptance-[0-9a-f]{32}", summary.get("prefix", "")):
        raise ValueError("refusing to touch a prefix outside one disposable acceptance run")
    if require_passed and not summary.get("passed"):
        raise ValueError("the writer acceptance did not pass")
    return summary


def session_environment():
    environment = dict(os.environ)
    if not all(environment.get(name) for name in
               ["AWS_ACCESS_KEY_ID", "AWS_SECRET_ACCESS_KEY", "AWS_SESSION_TOKEN"]):
        raise ValueError("temporary AWS session credentials are required")
    if any(name.startswith("AWS_ENDPOINT_URL") or name.startswith("BACKUPCTL_TEST_") or
           name == "BACKUPCTL_FAILPOINT"
           for name in environment):
        raise ValueError("cloud checks cannot use an endpoint override or a test failpoint")
    environment.pop("AWS_PROFILE", None)
    environment.update(AWS_CONFIG_FILE="/dev/null", AWS_SHARED_CREDENTIALS_FILE="/dev/null",
                       AWS_EC2_METADATA_DISABLED="true", AWS_PAGER="",
                       AWS_MAX_ATTEMPTS="3", AWS_RETRY_MODE="standard")
    return environment


class Cloud:
    def __init__(self, work, summary, environment):
        self.work = work
        self.summary = summary
        self.environment = environment

    def event(self, row):
        with (self.work / "cloud-events.jsonl").open("a") as output:
            output.write(json.dumps(row) + "\n")
        print(json.dumps(row), flush=True)

    def request(self, operation, *options, expected_error=None):
        started = time.perf_counter()
        result = subprocess.run(
            ["aws", "--no-cli-pager", "--region", self.summary["region"],
             "--cli-connect-timeout", "5", "--cli-read-timeout", "30", "s3api", operation,
             "--bucket", self.summary["bucket"], *map(str, options),
             "--no-paginate", "--output", "json"],
            env=self.environment, capture_output=True, text=True, timeout=100,
        )
        match = re.search(r"An error occurred \(([^)]+)\)", result.stderr)
        error = match[1] if match else None
        self.event({"operation": operation, "returncode": result.returncode,
                    "error": error, "expected_error": expected_error,
                    "wall_seconds": time.perf_counter() - started})
        if expected_error:
            if not result.returncode or error != expected_error:
                raise RuntimeError(f"{operation} must fail with {expected_error}; got {error}")
            return None
        if result.returncode:
            raise RuntimeError(f"{operation} failed with {error}; see cloud-events.jsonl")
        return json.loads(result.stdout) if result.stdout.strip() else {}

    def objects(self):
        prefix = self.summary["prefix"] + "/"
        response = self.request("list-objects-v2", "--prefix", prefix, "--max-keys", MAX_OBJECTS)
        objects = response.get("Contents", [])
        if response.get("IsTruncated") or len(objects) > MAX_OBJECTS:
            raise ValueError("refusing an acceptance run with too many objects")
        if any(not item["Key"].startswith(prefix) or item["Size"] < 0 for item in objects):
            raise ValueError("S3 listing contains an object outside this acceptance run")
        if sum(item["Size"] for item in objects) > MAX_BYTES:
            raise ValueError("refusing an acceptance run larger than 32 MiB")
        return objects

    def cpp(self, binary, state, verb, *options, denied=False):
        result = subprocess.run(
            [str(binary.resolve()), verb, "--state", str(state), *map(str, options)],
            env=self.environment, capture_output=True, text=True, timeout=600,
        )
        rows = [json.loads(line) for line in result.stdout.splitlines() if line.strip()]
        error = json.loads(result.stderr.splitlines()[-1]).get("error") if result.stderr.strip() else None
        self.event({"command": verb, "returncode": result.returncode, "results": rows, "error": error})
        if denied:
            if not result.returncode or error != "permission":
                raise RuntimeError("restore-only backup must fail with a permission error")
        elif result.returncode:
            raise RuntimeError(f"{verb} failed with {error}; see cloud-events.jsonl")
        return rows


def inspect_writer(cloud):
    summary, work = cloud.summary, cloud.work
    probe = work / "conditional-probe.txt"
    probe.write_bytes(b"immutable synthetic acceptance probe\n")
    key = summary["prefix"] + "/acceptance-probe"
    cloud.request("put-object", "--key", key, "--body", probe, "--if-none-match", "*",
                  "--server-side-encryption", "AES256", "--checksum-algorithm", "SHA256")
    replacement = work / "conditional-replacement.txt"
    replacement.write_bytes(b"this replacement must be rejected\n")
    cloud.request("put-object", "--key", key, "--body", replacement, "--if-none-match", "*",
                  "--server-side-encryption", "AES256", expected_error="PreconditionFailed")
    downloaded = work / "conditional-downloaded.txt"
    cloud.request("get-object", "--key", key, downloaded)
    if downloaded.read_bytes() != probe.read_bytes():
        raise RuntimeError("conditional PUT replaced an existing object")
    cloud.request("delete-object", "--key", key, expected_error="AccessDenied")
    cloud.request("list-objects-v2", "--prefix", "", "--max-keys", 1, expected_error="AccessDenied")
    cloud.request("list-objects-v2", "--prefix", "backupctl/outside-test/", "--max-keys", 1,
                  expected_error="AccessDenied")
    objects = cloud.objects()
    if not objects:
        raise RuntimeError("acceptance repository is empty")
    for item in objects:
        metadata = cloud.request("head-object", "--key", item["Key"], "--checksum-mode", "ENABLED")
        if metadata.get("ServerSideEncryption") != "AES256" or not metadata.get("ChecksumSHA256"):
            raise RuntimeError("an acceptance object is missing SSE-S3 or a SHA-256 checksum")
    return {"conditional_put_preserved_original": True, "writer_delete_denied": True,
            "outside_prefix_list_denied": True, "sse_s3_and_sha256_checked": True,
            "object_count": len(objects), "stored_bytes": sum(item["Size"] for item in objects)}


def inspect_reader(cloud, binary):
    summary, work = cloud.summary, cloud.work
    state = work / "reader-state"
    if state.exists():
        raise ValueError("reader checks require a fresh state directory")
    cloud.cpp(binary, state, "init", "--bucket", summary["bucket"], "--region", summary["region"],
              "--prefix", summary["prefix"])
    listed = cloud.cpp(binary, state, "list")
    if {row["snapshot_id"] for row in listed} != set(summary["snapshot_ids"]):
        raise RuntimeError("restore-only listing differs from the writer snapshots")
    for index, snapshot in enumerate(summary["snapshot_ids"]):
        cloud.cpp(binary, state, "verify", "--snapshot", snapshot)
        target = work / f"reader-restored-{index}"
        cloud.cpp(binary, state, "restore", "--snapshot", snapshot, "--target", target)
        actual = {name: list(value) for name, value in inventory(target).items()}
        expected = summary["original_inventory" if index < 2 else "current_inventory"]
        if actual != expected:
            raise RuntimeError("restore-only content or metadata differs from the source inventory")
    cloud.request("put-object", "--key", summary["prefix"] + "/reader-write-denied",
                  "--body", work / "conditional-probe.txt", "--server-side-encryption", "AES256",
                  expected_error="AccessDenied")
    cloud.request("delete-object", "--key", summary["prefix"] + "/acceptance-probe",
                  expected_error="AccessDenied")
    cloud.request("list-objects-v2", "--prefix", "", "--max-keys", 1, expected_error="AccessDenied")
    cloud.cpp(binary, state, "backup", "--source", work / "source", denied=True)
    after = cloud.cpp(binary, state, "list")
    if {row["snapshot_id"] for row in after} != set(summary["snapshot_ids"]):
        raise RuntimeError("a failed restore-only backup published a snapshot")
    return {"fresh_read_only_restore": True, "content_and_metadata_match": True,
            "read_only_put_and_delete_denied": True, "outside_prefix_list_denied": True,
            "cpp_backup_permission_error": True, "snapshot_count": len(after)}


def cleanup(cloud):
    # Validate the whole bounded listing before deleting any object.
    objects = cloud.objects()
    for item in objects:
        cloud.request("delete-object", "--key", item["Key"])
    if cloud.objects():
        raise RuntimeError("acceptance objects remain after cleanup")
    cloud.summary.update(remote_objects_retained=False, cleanup_passed=True)
    (cloud.work / "summary.json").write_text(json.dumps(cloud.summary, indent=2) + "\n")
    return {"deleted_objects": len(objects), "remaining_objects": 0}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("mode", choices=["writer", "reader", "cleanup"])
    parser.add_argument("--work", type=Path, required=True)
    parser.add_argument("--bucket", required=True)
    parser.add_argument("--region", required=True)
    parser.add_argument("--binary", type=Path)
    args = parser.parse_args()
    if args.mode == "reader" and not args.binary:
        parser.error("reader checks require --binary")
    summary = load_summary(args.work, args.bucket, args.region, require_passed=args.mode != "cleanup")
    cloud = Cloud(args.work, summary, session_environment())
    report = {"mode": args.mode, "passed": False, "prefix": summary["prefix"]}
    report_path = args.work / f"{args.mode}-checks.json"
    report_path.write_text(json.dumps(report, indent=2) + "\n")
    if args.mode == "writer":
        report.update(inspect_writer(cloud))
    elif args.mode == "reader":
        report.update(inspect_reader(cloud, args.binary))
    else:
        report.update(cleanup(cloud))
    report["passed"] = True
    report_path.write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps(report), flush=True)


if __name__ == "__main__":
    main()
