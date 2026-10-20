# Offline validation evidence

The logs and demonstration transcript are copied from GitHub Actions, with their
original timings and process-termination positions. `provenance.json` identifies the
tested commit, workflow run, executable hash, and suite counts.

- `gcc.log`: GCC Release, nine C++ tests and the Python acceptance suites.
- `clang.log`: the same suites built with Clang.
- `asan-ubsan.log`: application code instrumented with ASan and UBSan.
- `tsan.log`: a separate Clang TSan build, including concurrent uploads.
- `demo.jsonl`: first backup, unchanged backup, incremental backup, interruption,
  status, resume, old-version restore, verify, and corrupted-object refusal.

CTest has five suite entries. Together they contain 30 named tests, including the
100 deterministic backup interruptions, 50 seeded SIGKILLs, and 20 restore
publication interruptions run as subcases. The SDK suite talks to a local HTTPS
fixture with a private temporary CA; it does not contact Amazon S3.

The release executable is also available as `backupctl-linux-x64` in the workflow
artifacts. It is a Linux x86-64 test-enabled binary built on Ubuntu 24.04. Downloaded
artifact files may need `chmod +x backupctl` on Linux. A production build can disable
test hooks with `BUILD_TESTING=OFF`.

These logs establish process-termination recovery and offline correctness. They do
not establish IAM permissions, real S3 behavior, power-loss durability, or production
readiness. Benchmark evidence is kept separately under `benchmarks/results/2026-10-20`.
