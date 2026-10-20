# Follow-up validation

These records cover the exception-state and incomplete-inventory fixes at commit
`55532a68696ba277a508133c687b44e48a674709`. The executable hash, toolchain, container
image, and exact test counts are recorded in `provenance.json`.

- `local-release.log`: the real Windows `scripts/dev.ps1` run, building and testing
  on the container's Linux filesystem. All five CTest suites passed in 36.67 seconds.
  They contain 34 named tests, including the process-termination subcases.
- `dependencies.txt`: the pinned vcpkg dependency versions used by that image.
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
