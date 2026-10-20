#!/usr/bin/env python3
"""Exercise the actual SDK and TLS against a local fixture, not real Amazon S3."""
import base64
from collections import Counter
import hashlib
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import io
import os
from pathlib import Path
import ssl
import subprocess
import sys
import tempfile
import threading
from urllib.parse import parse_qs, unquote, urlsplit
import unittest
from xml.sax.saxutils import escape

BINARY = str(Path(sys.argv.pop(1)).resolve())


class Fixture:
    def __init__(self):
        self.objects = {}
        self.calls = Counter()
        self.failures = 0
        self.deny = False
        self.drop_commit_response = False
        self.bad_checksum = False
        self.oversize = False
        self.problems = []
        self.lock = threading.Lock()


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *args):
        pass

    def read_chunked(self, stream):
        payload = bytearray()
        trailers = {}
        while True:
            line = stream.readline(1024)
            size = int(line.strip().split(b";", 1)[0], 16)
            if size == 0:
                while True:
                    trailer = stream.readline(8192)
                    if trailer in {b"\r\n", b"\n", b""}:
                        return bytes(payload), trailers
                    name, value = trailer.decode().split(":", 1)
                    trailers[name.lower()] = value.strip()
            payload.extend(stream.read(size))
            if stream.read(2) != b"\r\n":
                raise ValueError("invalid chunk terminator")

    def read_payload(self):
        trailers = {}
        if self.headers.get("Transfer-Encoding", "").lower() == "chunked":
            payload, trailers = self.read_chunked(self.rfile)
        else:
            payload = self.rfile.read(int(self.headers.get("Content-Length", "0")))
        if "aws-chunked" in self.headers.get("Content-Encoding", ""):
            payload, aws_trailers = self.read_chunked(io.BytesIO(payload))
            trailers.update(aws_trailers)
        return payload, trailers

    def reply(self, code, data=b"", headers=None):
        self.send_response(code)
        headers = headers or {}
        for key, value in headers.items():
            self.send_header(key, str(value))
        if "Content-Length" not in headers:
            self.send_header("Content-Length", str(len(data)))
        self.send_header("Connection", "close")
        self.end_headers()
        if self.command != "HEAD":
            try:
                self.wfile.write(data)
            except (BrokenPipeError, ConnectionResetError, ssl.SSLError):
                pass  # A bounded client deliberately stops oversized responses.
        self.close_connection = True

    def handle_request(self):
        fixture = self.server.fixture
        url = urlsplit(self.path)
        key = unquote(url.path).removeprefix("/test-bucket/")
        query = parse_qs(url.query)
        with fixture.lock:
            fixture.calls[(self.command, key)] += 1
            if not self.headers.get("Authorization", "").startswith("AWS4-HMAC-SHA256 "):
                fixture.problems.append("request was not signed with SigV4")
            if self.command == "GET" and query.get("list-type") == ["2"]:
                prefix = query.get("prefix", [""])[0]
                keys = sorted(k for k in fixture.objects if k.startswith(prefix))
                body = '<ListBucketResult xmlns="http://s3.amazonaws.com/doc/2006-03-01/">'
                body += '<Name>test-bucket</Name><IsTruncated>false</IsTruncated>'
                for item in keys:
                    body += f'<Contents><Key>{escape(item)}</Key><Size>{len(fixture.objects[item][0])}</Size></Contents>'
                self.reply(200, (body + '</ListBucketResult>').encode(), {"Content-Type": "application/xml"})
                return
            if self.command == "PUT":
                payload, trailers = self.read_payload()
                if fixture.deny:
                    self.reply(403, b"<Error><Code>AccessDenied</Code></Error>")
                    return
                if fixture.failures and "/chunks/" in key:
                    fixture.failures -= 1
                    self.reply(503, b"<Error><Code>SlowDown</Code></Error>")
                    return
                if self.headers.get("If-None-Match") != "*":
                    fixture.problems.append("PUT did not use If-None-Match")
                checksum = base64.b64encode(hashlib.sha256(payload).digest()).decode()
                supplied = self.headers.get("x-amz-checksum-sha256") or trailers.get("x-amz-checksum-sha256")
                if supplied != checksum:
                    fixture.problems.append("PUT checksum did not match encoded bytes")
                if self.headers.get("x-amz-server-side-encryption") != "AES256":
                    fixture.problems.append("PUT did not request SSE-S3")
                if key in fixture.objects:
                    self.reply(412, b"<Error><Code>PreconditionFailed</Code></Error>")
                    return
                metadata = {name.lower(): value for name, value in self.headers.items()
                            if name.lower().startswith("x-amz-meta-")}
                fixture.objects[key] = (payload, metadata, checksum)
                if fixture.drop_commit_response and "/snapshots/" in key:
                    fixture.drop_commit_response = False
                    self.close_connection = True
                    self.connection.shutdown(2)
                    return
                self.reply(200, headers={"x-amz-checksum-sha256": checksum, "ETag": '"unused-etag"'})
                return
            if key not in fixture.objects:
                # A scoped ListBucket policy may produce HEAD 403 for a missing key.
                self.reply(403 if self.command == "HEAD" else 404,
                           b"<Error><Code>NoSuchKey</Code></Error>")
                return
            payload, metadata, checksum = fixture.objects[key]
            if fixture.bad_checksum and "/chunks/" in key:
                checksum = base64.b64encode(b"x" * 32).decode()
            if fixture.oversize and self.command == "GET" and "/chunks/" in key:
                payload = b"x" * (8 * 1024 * 1024)
            self.reply(200, payload, {**metadata, "Content-Length": len(payload),
                                     "x-amz-checksum-sha256": checksum})

    do_HEAD = handle_request
    do_GET = handle_request
    do_PUT = handle_request


class S3ContractTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="backupctl-sdk-")
        self.root = Path(self.temporary.name)
        self.source = self.root / "source"
        self.source.mkdir()
        (self.source / "file").write_bytes(b"test data" * 8192)
        self.state = self.root / "state"
        key, cert = self.root / "key.pem", self.root / "cert.pem"
        subprocess.run(["openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes", "-days", "1",
                        "-subj", "/CN=127.0.0.1", "-addext", "subjectAltName=IP:127.0.0.1",
                        "-keyout", str(key), "-out", str(cert)],
                       check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        self.fixture = Fixture()
        self.server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        self.server.fixture = self.fixture
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        context.load_cert_chain(cert, key)
        self.server.socket = context.wrap_socket(self.server.socket, server_side=True)
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.thread.start()
        self.environment = {
            **os.environ, "BACKUPCTL_TEST_S3_ENDPOINT": f"https://127.0.0.1:{self.server.server_port}",
            "BACKUPCTL_TEST_CA": str(cert), "AWS_ACCESS_KEY_ID": "FIXTUREKEY",
            "AWS_SECRET_ACCESS_KEY": "fixture-secret-used-only-by-the-local-test",
            "AWS_EC2_METADATA_DISABLED": "true", "AWS_CONFIG_FILE": "/dev/null",
            "AWS_SHARED_CREDENTIALS_FILE": "/dev/null", "NO_PROXY": "127.0.0.1",
        }
        self.environment.pop("AWS_SESSION_TOKEN", None)
        self.call("init", "--bucket", "test-bucket", "--prefix", "isolated/test", "--region", "us-east-1")

    def tearDown(self):
        self.server.shutdown()
        self.server.server_close()
        self.thread.join()
        self.assertEqual(self.fixture.problems, [])
        self.temporary.cleanup()

    def call(self, command, *args, ok=True, state=None):
        result = subprocess.run([BINARY, command, "--state", str(state or self.state), *map(str, args)],
                                env=self.environment, capture_output=True, text=True, timeout=45)
        if ok:
            self.assertEqual(result.returncode, 0, result.stderr + repr(self.fixture.problems))
            return json.loads(result.stdout) if result.stdout.strip() else None
        self.assertNotEqual(result.returncode, 0, result.stdout)
        return json.loads(result.stderr.strip().splitlines()[-1])

    def test_sdk_roundtrip_conditional_write_and_lost_commit_response(self):
        self.fixture.drop_commit_response = True
        first = self.call("backup", "--source", self.source)
        second = self.call("backup", "--source", self.source)
        self.assertEqual(second["chunk_upload_bytes"], 0)
        fresh = self.root / "fresh"
        self.call("init", "--bucket", "test-bucket", "--prefix", "isolated/test",
                  "--region", "us-east-1", state=fresh)
        self.call("verify", "--snapshot", first["snapshot_id"], state=fresh)
        self.call("restore", "--snapshot", first["snapshot_id"], "--target", self.root / "restored", state=fresh)
        self.assertEqual((self.root / "restored/file").read_bytes(), (self.source / "file").read_bytes())

    def test_sdk_has_three_attempts_and_permission_errors_fail_fast(self):
        self.fixture.failures = 10
        failure = self.call("backup", "--source", self.source, ok=False)
        self.assertEqual(failure["error"], "network_exhausted")
        calls = sum(count for (method, key), count in self.fixture.calls.items()
                    if method == "PUT" and "/chunks/" in key)
        self.assertEqual(calls, 3)
        self.fixture.failures = 0
        self.call("resume", "--job", failure["job_id"])
        (self.source / "new").write_bytes(b"new bytes")
        self.fixture.deny = True
        before = sum(count for (method, _), count in self.fixture.calls.items() if method == "PUT")
        failure = self.call("backup", "--source", self.source, ok=False)
        after = sum(count for (method, _), count in self.fixture.calls.items() if method == "PUT")
        self.assertEqual(failure["error"], "permission")
        self.assertEqual(after - before, 1)

    def test_sdk_download_checksum_is_enforced(self):
        snapshot = self.call("backup", "--source", self.source)["snapshot_id"]
        self.fixture.bad_checksum = True
        failure = self.call("verify", "--snapshot", snapshot, ok=False)
        self.assertEqual(failure["error"], "integrity")
        self.call("restore", "--snapshot", snapshot, "--target", self.root / "bad", ok=False)
        self.assertFalse((self.root / "bad").exists())

    def test_stream_download_limit_and_tls_verification(self):
        snapshot = self.call("backup", "--source", self.source)["snapshot_id"]
        self.fixture.oversize = True
        failure = self.call("verify", "--snapshot", snapshot, ok=False)
        self.assertEqual(failure["error"], "integrity")
        self.fixture.oversize = False
        key, unrelated = self.root / "unrelated-key.pem", self.root / "unrelated-cert.pem"
        subprocess.run(["openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes", "-days", "1",
                        "-subj", "/CN=unrelated-ca", "-keyout", str(key), "-out", str(unrelated)],
                       check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        self.environment["BACKUPCTL_TEST_CA"] = str(unrelated)
        self.call("verify", "--snapshot", snapshot, ok=False)


if __name__ == "__main__":
    unittest.main()
