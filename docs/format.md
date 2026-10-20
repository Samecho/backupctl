# Format version 1

All S3 keys are below the configured prefix. `repo.json` contains `version: 1`, a
32-character lowercase hexadecimal `repo_id`, `chunk_size: 4194304`, and
`codec: "zstd-v1-level3"`. These settings cannot change within an existing repository.
Job and snapshot IDs are independent random 128-bit values represented as 32 hex
characters. IDs are generated with OpenSSL's random source.

## Chunk objects

`chunks/v1/zstd/HH/HASH` contains one complete zstd frame compressed at level 3.
`HASH` is the SHA-256 of raw bytes; `HH` is its first two characters. Empty files have
no chunks. Every nonfinal chunk of a file has exactly 4 MiB of raw data.

Metadata includes `repo-id`, `raw-sha256`, `raw-size`, `codec`, and `encoded-sha256`.
An existing compatible object is reused after HEAD. Conflicting metadata fails the
job. The encoded checksum and raw content hash have different purposes. The SDK's
S3 SHA-256 checksum covers the compressed PUT/GET bytes; decode verifies raw hashes.
All PUTs are conditional and never replace an existing object.

## JSON Lines manifest

`manifests/SNAPSHOT.jsonl` is UTF-8, with one JSON object followed by a newline per
record. Record length including newline is at most 64 KiB; a full manifest is at
most 1 GiB. The record sequence is:

1. `header`: `version`, `repo_id`, `snapshot_id`, `chunk_size`, `codec`.
2. `directory`: `path`, `mode`, `mtime_ns`. The first directory is `.`. Parents
   precede children; all directories precede files.
3. `file`: sequential positive `id`, `path`, `size`, `mode`, `mtime_ns`, `sha256`.
4. Zero or more `chunk` records immediately after their file: `file_id`, zero-based
   `sequence`, `raw_size`, `sha256`, `codec`, `key`.
5. `end`: `files`, `directories`, `bytes`, `chunks`. Nothing follows this record.

The file hash is present in the file record because file processing completes before
manifest generation. Each chunk gets its own record so a large file does not create
an unbounded JSON array. Paths and references are indexed in SQLite while parsing;
the parser does not keep the whole manifest or all paths in memory.

Paths are relative UTF-8 Linux paths with no NUL, empty component, `.` component,
`..` component, leading slash, or trailing slash. `.` is allowed only for the root
directory. Paths are at most 4095 bytes and components at most 255 bytes. Directory
depth is at most 256. Duplicate paths and file/directory ancestor conflicts fail
before restore writes file contents. Permission bits are limited to `0777`; special
permission bits are excluded. Timestamps are signed 64-bit nanoseconds since epoch.

## Commit descriptor

`snapshots/SNAPSHOT.json` is a small JSON object (at most 64 KiB) written last. It
contains `version`, `repo_id`, `snapshot_id`, `manifest_key`, `manifest_size`,
`manifest_sha256`, `created_ns`, and the same totals as `end`.

Only these descriptors are listed as snapshots. A manifest or chunk without a
descriptor is uncommitted data. A timed-out final PUT is reconciled against the
same descriptor. Exact equal contents succeed; a conflict fails; an unreadable
outcome is `commit_unknown`. Resume never invents another snapshot ID for that job.

The manifest hash is stored in this descriptor rather than inside the manifest.
Verification downloads the full manifest before trusting records and downloads,
decodes, and hashes every referenced chunk and complete file.

## Local state

`config.json` binds backend settings to the repository ID and contains no credentials.
`state.db` contains `jobs`, `files`, `chunks`, `file_chunks`, and `checkpoints`.
Only the coordinator writes it, under a local flock. A chunk reference and its
confirmed byte count are recorded in the same SQLite transaction after remote
confirmation. SQLite uses rollback journaling and `synchronous=EXTRA`.

The database is unnecessary for restoring a committed version. Initialize a new
state directory against the same remote repository, then list, verify, or restore.
