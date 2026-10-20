# Implementation and validation

| Step | Work | Status |
| --- | --- | --- |
| 0 | Scope, format, state and error boundaries | Written |
| 1 | Isolated CMake build, codec, local round trip | Implemented; offline tests pass |
| 2 | Deduplication, zstd, SQLite, streaming manifests | Implemented; offline tests pass |
| 3 | AWS SDK S3 adapter and private-prefix instructions | Implemented; HTTPS fixture passes; real S3 pending |
| 4 | Resume, conditional commit, lost-response reconciliation | Implemented; process fault matrix passes |
| 5 | Bounded concurrency, backpressure, cancellation | Implemented; queue/budget and cancellation tests pass |
| 6 | Corruption, path safety, source changes, fault matrix | Implemented; offline boundary tests pass |
| 7 | Benchmarks, Linux CI, demo, documentation | Delivered; measured offline results are in the repository |

No AWS test bucket or profile is available in this development session. Real S3/IAM
acceptance remains pending even after the adapter compiles and offline tests pass.

## Validation evidence

The [Linux workflow](../.github/workflows/linux.yml) builds GCC Release and Clang
Release, then runs ASan/UBSan and TSan in separate builds. All four configurations
passed at [commit a64cdfb](https://github.com/Samecho/backupctl/actions/runs/39385675792).
The [saved logs and demo](validation/2026-10-20/README.md) include their exact source
revision and executable hash. Later checks are available in the Actions history.
The checks use synthetic files only. No personal input or cloud credentials are
needed.

| Area | Evidence |
| --- | --- |
| Codec | Known SHA-256 vectors; random/compressible round trips; corrupt, oversized, and trailing zstd frames |
| A1–A2 | Empty trees/files/directories, UTF-8 and spaces, nested paths, multi-chunk files, modes and nanosecond mtime |
| A3–A6 | Unchanged chunk payload is zero; same-job and cross-version dedup; added/deleted/modified files; old snapshots remain recoverable |
| A7 | Restore using a newly initialized state directory with no original file/chunk index |
| A8–A9 | Missing/corrupt objects, altered manifest hashes, crafted traversal/absolute/NUL paths, duplicate/conflicting paths, order/size/hash violations, record and decode limits |
| A10 | Existing target and dangling symlink refused; symlinks, FIFO and invalid UTF-8 source names rejected; unreadable input tested as a non-root user |
| Recovery | Ten backup checkpoints ×10 repeats; 50 seeded actual SIGKILLs; two restore publication checkpoints ×10 repeats |
| Unknown outcomes | Lost chunk/manifest/commit response; unreadable commit stays unresolved; conflicting descriptor is never overwritten |
| Resources | Bounded queue and results; requested workers reduced to fit the budget; one writer lock; SIGTERM joins workers; status works without remote access |
| Write failures | Permission errors and injected ENOSPC refuse commit/restore publication |
| SDK contract | Actual SDK + local HTTPS: SigV4 header, conditional PUT, encoded SHA-256, SSE-S3, missing-key HEAD 403 reconciliation, three attempts on 503, one on denied PUT, lost commit response, checksum rejection, bounded downloads, unrelated CA refused |
| Demonstration | `scripts/demo.py` runs the complete first/incremental/interrupted/old-version/corruption sequence in CI |
| Measurements | 500 backup runs plus 100 verify/restore pairs; separate 50 GiB and 100,000-file backup resource checks |

The random termination test records seed `20261020`, requested delay, observed phase,
confirmed byte checkpoint, and remote chunk count. CTest's full log is kept as a CI
artifact. Deterministic process failpoints exit without normal cleanup. Tests do
not weaken hashes, persistence, or publication rules after a failure.

## Known limits and unverified behavior

- Real S3/IAM, temporary credential renewal, and a physical second machine have not
  been exercised. The new-state tests establish independence from the old SQLite
  index, while the HTTPS fixture establishes SDK request/response behavior.
- Source data must stay static. Identity checks and rescans detect common changes;
  they do not implement an instantaneous snapshot. An interrupted initial inventory
  has no durable baseline. Resume refuses that job and asks for a new backup; it
  never silently replaces the baseline with a later scan.
- The scope is one writer per remote repository. flock protects one local state,
  not multiple clients using different state directories against the same prefix.
- Restore publication uses Linux `renameat2(RENAME_NOREPLACE)`. Unsupported filesystems
  fail. Existing data is never replaced as a fallback.
- Crash tests cover process termination. Power loss and kernel crashes need separate
  tests. SIGKILL may leave private local temporary directories and uncommitted remote
  objects; automatic garbage collection is intentionally absent.
- Version 1 limits manifest size to 1 GiB, records to 64 KiB, paths to 4095 bytes,
  and directory depth to 256. Timestamps outside the signed nanosecond range fail.
- The resource measurements use a 2-vCPU, approximately 8-GB Linux CI runner and the
  offline store. The 50 GiB input is sparse zero data. A full 50 GiB physical-data
  restore and a large real S3 transfer remain unmeasured.
- Sanitizers instrument application code. The pinned third-party binaries are built
  separately by vcpkg; a passing run does not prove the inside of every dependency.
