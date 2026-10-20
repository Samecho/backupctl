# backupctl

A C++20 command-line tool for incremental directory backups. Files are split into
fixed-size chunks, hashed with SHA-256, compressed with zstd, and stored in a private
S3 prefix. SQLite tracks work that can be resumed after the process stops.

The tool targets a single writer on Linux and a source directory that stays
unchanged during backup. It does not back up a live database, follow symbolic links,
delete history, or encrypt data on the client.

The CLI supports `init`, `backup`, `resume`, `status`, `list`, `verify`, and `restore`.
An unchanged backup uploads no new chunk payload. It still sends metadata and storage
requests. Restore checks the manifest, each decoded chunk, and each complete file
before publishing a new directory. It always refuses an existing target.

GCC and Clang offline tests run on Linux. The S3 adapter compiles against the pinned
AWS SDK, but a real bucket and IAM role have not been tested yet. See
[validation status](docs/progress.md) for the distinction between implemented and
verified behavior.

## Build without global installs

On Windows with Docker Desktop already available:

```powershell
.\scripts\dev.ps1
```

This builds a project-specific image, copies source into the container's Linux
filesystem, runs the tests, and exports `out/release/backupctl`. The host source mount
is read-only. The temporary build container is removed on exit. Dependencies and
compilers are installed only inside the image. Remove the image later with
`docker image rm backupctl-dev:local`.

On Linux with an existing compiler, CMake, Ninja, Git, and Python 3:

```sh
bash scripts/bootstrap.sh
export VCPKG_ROOT="$PWD/.deps/vcpkg"
export VCPKG_DEFAULT_BINARY_CACHE="$PWD/.cache/vcpkg"
export VCPKG_DISABLE_METRICS=1
cmake --preset release
cmake --build --preset release -j4
ctest --preset release
cmake --install build/release --prefix "$PWD/out/install"
```

All native dependencies use one vcpkg manifest with a pinned baseline. There are no
global package installs, Git configuration changes, shell profile edits, or cloud
resources created by these scripts. Native build output stays under this repository.

Release test builds include environment-controlled fault injection. For an installed
binary without those hooks, configure a separate build with `-DBUILD_TESTING=OFF`.
`BACKUPCTL_S3=OFF` disables the adapter; use `VCPKG_MANIFEST_NO_DEFAULT_FEATURES=ON`
to omit its dependency feature too. The default build includes S3.

## Try a local repository

Keep `input`, `state`, and `remote` in separate directories. The local store is a
test substitute for S3 and follows the same immutable-object interface.

```sh
mkdir -p demo-data/input/empty
printf 'some contents\n' > demo-data/input/example.txt
build/release/backupctl init --state demo-data/state --local-store demo-data/remote
build/release/backupctl backup --state demo-data/state --source demo-data/input
build/release/backupctl list --state demo-data/state
build/release/backupctl verify --state demo-data/state --snapshot SNAPSHOT_ID
build/release/backupctl restore --state demo-data/state --snapshot SNAPSHOT_ID --target demo-data/restored
```

Use the snapshot ID from `backup` or `list`. Normal results go to stdout as JSON;
diagnostics go to stderr as JSON Lines. A backup prints its job ID as soon as the
job is durable. Errors return nonzero and include that ID when a job exists.

```sh
build/release/backupctl status --state demo-data/state --job JOB_ID
build/release/backupctl resume --state demo-data/state --job JOB_ID
```

`status` works from local state even if remote access is unavailable. It distinguishes
a previously confirmed commit from a fresh remote check; use `verify` to check the
remote contents again.

`resume` retains the original snapshot ID and checks the frozen source inventory.
It reads input again and reconciles existing remote chunks; it does not upload
confirmed chunks again. An interrupted restore starts over in a new temporary
directory. It may leave a private temporary sibling behind after SIGKILL.

The complete offline demonstration uses synthetic files and a fresh directory:

```sh
python3 scripts/demo.py build/release/backupctl --work demo-data/full-demo
```

It covers the first backup, no-change backup, incremental changes, interruption,
resume, old-version restore, and refusal to publish corrupted content. Its fault
injection step requires a test-enabled build.

## S3, format, tests, and measurements

- [Private S3 and prefix-scoped IAM setup](docs/s3.md)
- [Architecture and commit/recovery protocol](docs/design.md)
- [Object and manifest format](docs/format.md)
- [Validation matrix and known limits](docs/progress.md)
- [Benchmark procedure and measured results](docs/performance.md)

Defaults: 4 workers, 4 MiB chunks, 128 MiB data budget. Use `--workers 1` for the
serial reference path. The budget limits active workers and in-flight chunks, even
when more workers are requested. It covers data buffers; total process RSS also
includes SQLite, thread stacks, runtime code, and SDK allocations.
