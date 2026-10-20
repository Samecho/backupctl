# Real S3 acceptance with temporary credentials

This workflow is ready for an AWS account and has not passed real S3 acceptance
yet. Offline SDK fixtures and IAM guard tests are separate evidence.

First finish registration and verify the account's Free plan. Review and deploy
the [private acceptance stack](../infra/README.md). It creates a new test bucket
and roles limited to its disposable `backupctl/test/` prefix. Check the stack's
resource list and IAM changes before deployment.

Copy these stack outputs into GitHub repository variables:

| Repository variable | Stack output |
| --- | --- |
| `S3_TEST_BUCKET` | `Bucket` |
| `S3_TEST_REGION` | `Region` |
| `S3_TEST_WRITER_ROLE` | `WriterRoleArn` |
| `S3_TEST_READER_ROLE` | `ReaderRoleArn` |
| `S3_TEST_CLEANUP_ROLE` | `CleanupRoleArn` |

These values identify the destination and roles. They do not contain AWS keys.
Do not add an AWS password, access key, or secret as a repository variable or
secret. The application uses the runner's 15-minute STS session. Nothing is
installed or configured in the developer's global environment.

Dispatch **S3 acceptance** on `main` after those variables have been checked.
There is no scheduled or push-triggered cloud run. The build job has no OIDC
permission. It builds and tests the executable before a separate job obtains
AWS credentials. All actions in this workflow are pinned to release commits.

The writer creates about 16 MiB of synthetic source input and saves four
snapshots: the first backup, an unchanged backup, an incremental backup, and an
interrupted job resumed after durable inventory. An unchanged backup must upload
zero chunk bytes. Every snapshot is verified and restored into a fresh state,
including old versions, empty entries, Unicode paths, permissions, and nanosecond
timestamps.

The cloud helper then checks SSE-S3 and stored SHA-256 checksums, conditional PUT
preservation, rejection of listing outside the allowed prefix, and rejection of
writer deletion. A separate read-only session initializes a new state and
verifies and restores all versions. Both CLI PUT/DELETE probes must be denied,
and an actual `backupctl backup` must return a permission error without publishing
another snapshot. A timeout or expired token cannot count as an IAM denial.

Finally, a separate cleanup session lists and deletes only this run's exact
`acceptance-<uuid>/` objects. Before its first deletion it checks that the listing
is complete, has at most 128 objects, contains no keys outside that child, and
totals at most 32 MiB. The cleanup step also runs after an earlier check fails.
Runner termination can prevent that step, so the bucket's seven-day expiration
rule is a fallback. Never store real backups under this disposable test prefix.

The `real-s3-evidence` artifact contains `summary.json`, command journals, and
separate writer, reader, and cleanup reports. `cloud-acceptance.json` passes only
when all three reports pass. `summary.json` records the binary hash, exact bucket
and prefix, snapshot IDs, and source inventories; its `passed` field describes
the original backup/recovery sequence. After cleanup it records that remote
objects are no longer retained. The reports do not contain credentials or the
synthetic source files. Artifacts expire after seven days, so save them in the
repository's validation records after inspection.

The small cloud run does not repeat the 50 GiB local performance test. S3 request,
storage, and transfer prices still apply to the account's credits or allowances.
Do not upgrade the account to a Paid plan to run this test. The bucket and roles
remain available afterward; remove them deliberately when no longer needed.
