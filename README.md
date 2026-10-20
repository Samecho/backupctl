# backupctl

A C++20 command-line tool for incremental directory backups. Files are split into
fixed-size chunks, hashed with SHA-256, compressed with zstd, and stored in a private
S3 prefix. SQLite tracks work that can be resumed after the process stops.

This repository is under development. The implementation and validation status are
tracked in [docs/progress.md](docs/progress.md).

The first release targets a single writer on Linux and a source directory that stays
unchanged during backup. It does not back up a live database, follow symbolic links,
delete history, or encrypt data on the client.

Build tools and dependencies stay in a project-specific container. Nothing needs to
be installed globally on the host. See [docs/design.md](docs/design.md) for the format,
commit protocol, and failure behavior.
