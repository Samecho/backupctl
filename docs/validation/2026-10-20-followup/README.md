# Follow-up validation

These records cover the exception-state and incomplete-inventory fixes at commit
`55532a68696ba277a508133c687b44e48a674709`. The executable hash, toolchain, container
image, and exact test counts are recorded in `provenance.json`.

- `local-release.log`: the real Windows `scripts/dev.ps1` run, building and testing
  on the container's Linux filesystem. All five CTest suites passed in 36.67 seconds.
  They contain 34 named tests, including the process-termination subcases.
- `dependencies.txt`: the pinned vcpkg dependency versions used by that image.
- `gcc.log`, `clang.log`, `asan-ubsan.log`, and `tsan.log`: full logs from the
  [passing Linux workflow](https://github.com/Samecho/backupctl/actions/runs/39474502721).
  Each configuration ran all 34 named tests. The release job also passed the dense
  restore smoke check and the offline cloud acceptance harness.
- `local-harness.jsonl` and `local-harness-summary.json`: the S3 acceptance script
  exercised against the offline store, including interruption, same-job resume,
  unchanged deduplication, and four fresh-state restores with metadata comparison.

New regression coverage includes actual filesystem and JSON exceptions in worker
tasks, failure while recording an error in SQLite, invalid job resource settings
before workers start, and interruption before or during the first inventory. A
resumed incomplete inventory is refused even when the source has not changed.

Docker startup was repaired by moving stale zero-byte IPC endpoints into the
project cache while its backend was stopped. Existing images, containers, volumes,
and settings were preserved. Compilers and dependencies were installed only inside
the project image; no host packages, shell profiles, or Git settings were changed.

The harness result is explicitly `local-harness-check`. No AWS profile, credentials,
or test bucket was available, so this does not establish real S3/IAM acceptance.
The runnable cloud entry point is documented in [S3 setup](../../s3.md).

Full resource acceptance is saved separately: the
[50 GiB random-file restore](../../../benchmarks/results/2026-10-20-restore-local/README.md)
passed in the four-CPU, 8-GiB local Linux container, and the
[100,000-file restore](../../../benchmarks/results/2026-10-20-restore-ci/README.md)
passed on independent Linux CI. Both used a fresh state database and an independent
inventory comparison. The duplicate local small-file phase was stopped after the
CI result passed; its partial result is not counted as a successful acceptance run.
