# Design

## Scope

`backupctl` backs up a static local Linux directory to one private S3 bucket and a
dedicated prefix. It preserves regular files, directories, permission bits (`0777`),
and nanosecond mtime. It rejects symlinks, special files, invalid UTF-8 paths, and
unreadable input. UID/GID, ACLs, xattrs, sparse layout, and hard-link identity are out
of scope. The application never creates a bucket or deletes a remote object.

Commands: `init`, `backup`, `resume`, `status`, `list`, `verify`, and `restore`.
Every command emits JSON. Failures have an error category and, where one exists, a
job ID. Defaults are four workers, 4 MiB chunks, and a 128 MiB data buffer budget.
One worker is the serial reference path.

## Storage

All keys are relative to the configured prefix:

| Key | Contents |
| --- | --- |
| `repo.json` | Format version, repository ID, chunk size, codec |
| `chunks/v1/zstd/HH/SHA256` | One independently compressed raw chunk |
| `manifests/ID.jsonl` | Header, directories, files, chunk references, end record |
| `snapshots/ID.json` | Final commit: manifest key, size, hash, counts |

Raw chunk and file hashes use OpenSSL EVP SHA-256. zstd uses level 3. Encoded object
bytes have a separate checksum. ETag is never treated as a content hash. Objects are
immutable and created conditionally; an incompatible existing object is an error.
JSON uses nlohmann/json, with bounded record sizes. The manifest hash lives in the
commit record, so the format has no self-reference.

## Local state and publication

SQLite uses a rollback journal, `synchronous=EXTRA`, and short transactions. Only the
coordinator writes the database. File and chunk indexes stay on disk. A local flock
prevents concurrent writers to one state directory. State and local storage must be
outside the source directory.

Jobs move through `scanning -> uploading -> manifest -> committing -> committed`.
An interrupted or failed job keeps its original job and snapshot IDs. `resume`
checks the original source identity and inventory before continuing. Remote success
is recorded locally only after a successful request or reconciliation.
The initial inventory is one transaction. A job left in `scanning` has no durable
baseline; `resume` refuses it and requires a new backup job. It does not rescan a
possibly changed source under the old snapshot ID.

Chunks are uploaded first, then the full manifest, then a small commit descriptor
using `If-None-Match: *`. Only commit descriptors are visible to `list`. A lost final
response is reconciled by reading the same descriptor and comparing its exact
contents. An unresolved outcome stays `commit_unknown`; it is never reported as a
successful backup. There is no cross-object S3 transaction.

The scanner checks device, inode, type, size, mode, mtime, and ctime before and after
reading, then rescans the directory before commit. This detects common changes; it
does not provide an instantaneous filesystem snapshot or defend against a malicious
writer restoring timestamps.

## Restore and resource limits

Restore first downloads and verifies the complete manifest. It validates every
relative path and record, uses a disk index to detect conflicts, and writes into a
private temporary sibling directory. Every decoded chunk and complete file is
hashed again. Files and directories are synced before a Linux no-replace rename
publishes the destination. An existing target is always rejected.

The worker pool owns a bounded number of raw/compressed chunks. Completed results
are bounded as well; workers never write SQLite. A slow store applies backpressure.
Failure or cancellation stops scheduling and joins all workers. The S3 adapter owns
the finite retry policy; the scheduler does not add retries.

## Error categories

`configuration`, `permission`, `source_changed`, `unsupported`, `integrity`,
`network_exhausted`, `commit_unknown`, `cancelled`, `busy`, and `io`.

Process termination recovery is tested separately from power-loss durability.
ASan/UBSan and TSan use separate builds. Performance reports must include raw
measurements, environment, input seed, and the exact commit. Goals such as 50 GiB,
100,000 files, and 512 MiB RSS are not claimed until actually measured.
