# 100,000-file round trip

The [Restore scale workflow](https://github.com/Samecho/backupctl/actions/runs/39475472549)
passed at revision `0acc71fd17774bbf41086ce8ed8931471af588a0`. This revision contains
the C++ fixes validated at `55532a6`; the intervening commit only saved evidence.
The exact binary hash and runner details are in `environment.json`.

The Ubuntu 24.04 runner exposed two logical AMD EPYC 9V45 CPUs and about 8 GB RAM.
The offline input contained 100,000 files of 256 bytes in 100 subdirectories, with
64 distinct payloads. Both source and restore allocated 409,600,000 file bytes for
25,600,000 logical bytes. This exercises file/reference indexes and metadata at
large file counts; it is not an incompressible-data throughput test.

| Command | Wall time | Process peak RSS |
| --- | --- | --- |
| Backup | 217.342 s | 35.99 MiB |
| Verify | 5.108 s | 35.99 MiB |
| Restore | 39.824 s | 35.99 MiB |

Verify and restore used a newly initialized state database. The final independent
inventory matched every file's content hash, relative path, mode, and nanosecond
mtime, including directory metadata. All three processes stayed below 512 MiB RSS.
These are single acceptance runs, not repeated throughput estimates or S3 results.
