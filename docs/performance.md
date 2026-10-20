# Performance measurements

The benchmark uses only generated files and the local store. It does not measure
S3 bandwidth, IAM behavior, packet-level network traffic, or power-loss durability.

```sh
python3 benchmarks/run.py build/release/backupctl --output benchmark-results \
  --size-mib 64 --small-files 1000 --repeats 5 --stress
```

The script records the exact commit and executable hash, CPU and memory, filesystem,
compiler, dependency status, data seed and content digest. It runs compressible,
random, mixed, and small-file inputs with 1, 2, 4, and 8 requested workers, at the
same 128 MiB budget. Each configuration has five repetitions, each including a cold
backup, unchanged backup, partial update, insertion, broad update, verification, and
restore. The modified datasets are identical across worker counts for a given seed.

The delayed-store case adds 40 ms per local PUT and small-object GET to expose
storage waiting without creating cloud costs. It uses the same codec, raw hashes,
checkpoints, fsyncs, and commit protocol for every worker count. Comparing this
serial path to bounded concurrency is the optimization experiment.

The `--stress` cases separately check one 50 GiB logical sparse-zero file and
100,000 small files against the 512 MiB peak-RSS target. Sparse input does not
represent 50 GiB of physical disk reads, and these single acceptance runs are not
used to claim stable throughput. The 50 GiB run tests backup resource use rather
than allocating a 50 GiB restored directory on the CI runner.

For a dense-data round trip and a 100,000-file restore, use the separate script:

```sh
python3 benchmarks/restore_scale.py build/release/backupctl \
  --work benchmark-data/restore-scale --output out/restore-scale
```

It writes all 50 GiB of pseudorandom input, checks physical allocation,
backs up and verifies, restores from a fresh state database, and compares contents,
modes, and timestamps. It repeats the full sequence for 100,000 small files.
The workload needs about 155 GiB of free Linux filesystem space. Peak RSS for backup,
verify, and restore must each stay under 512 MiB. Input and restored data are removed
after each successful comparison; failed workloads remain available for inspection.

On Windows, first run `scripts/dev.ps1`, then `scripts/restore-scale.ps1`. The latter
uses a temporary Linux container limited to four CPUs and 8 GiB, with the project
mounted read-only. Results are exported under `out/restore-scale-<uuid>`. Only its
own container is removed. The manual Restore scale workflow defaults to 1 GiB and
100,000 files to fit the smaller hosted runner disk; it is not a 50 GiB substitute.

CLI counters distinguish successful encoded upload payload from logical bytes,
and chunk payload from manifest/commit overhead. An unchanged backup may have zero
new chunk bytes while still issuing HEAD, PUT, and GET requests. Worker phase times
are sums across workers and can overlap; they must not be added to end-to-end time.
Peak RSS is from `getrusage`. The buffer-bound counter is a conservative reservation
for owned raw/encoded buffers and SDK stream copies, rather than a total-RSS limit.
CPU time and block-I/O operation counts are reported separately.

## Results from 2026-10-20

The [raw JSON Lines](../benchmarks/results/2026-10-20/raw.jsonl),
[summary](../benchmarks/results/2026-10-20/summary.json),
[environment](../benchmarks/results/2026-10-20/environment.json), and
[dependency status](../benchmarks/results/2026-10-20/dependencies.txt) come from
[this benchmark run](https://github.com/Samecho/backupctl/actions/runs/39383194605).
It measured commit `d8cd241c78606ce2b2c75b7785734d0732e1eb1b` on an Ubuntu 24.04
runner with **2 logical CPUs and about 8 GB RAM**, GCC 13.3.0. This differs from the
proposed 4-vCPU reference machine. `stat -f` reported the Linux filesystem type as
`ext2/ext3`; no Windows mount was used for the measurements.

Dependencies were AWS SDK 1.11.900, zstd 1.5.7, OpenSSL 3.6.5, SQLite 3.53.4,
nlohmann/json 3.12.0, and GTest 1.18.0. The raw file contains 602 records: 500 backup
runs, 100 combined verify/restore records, and two resource acceptance runs.

### Storage-wait experiment

For the cold 64 MiB delayed-store workload, five repetitions gave:

| Requested / active workers | Median wall time | Min–max | Maximum RSS |
| --- | --- | --- | --- |
| 1 / 1 | 0.969 s | 0.967–0.970 s | 38.4 MiB |
| 2 / 2 | 0.559 s | 0.559–0.559 s | 41.1 MiB |
| 4 / 4 | 0.421 s | 0.420–0.426 s | 50.9 MiB |
| 8 / 5 | 0.400 s | 0.397–0.401 s | 55.7 MiB |

In the serial path, median storage work was 0.664 s, versus 0.018 s compression
and 0.042 s chunk hashing. That identifies storage waiting as the dominant chunk
stage in this experiment. The implementation change from the serial path to the
bounded pool is recorded in commit `806181f`; `--workers 1` retains the reference
path. Comparing both paths in the same measured executable keeps codec, checksums,
checkpoints, and fsync behavior equal.

Four workers overlapped that storage wait and reduced the measured median by
**2.30×**, at a cost of about 12.5 MiB more observed RSS. Aggregate storage work
stayed at 0.664 s. User CPU time increased from a median 0.112 s to 0.123 s.
The five-slot reservation bound was 114.3 MiB, below the same 128 MiB budget.
Eight requested workers therefore became five active workers rather than exceeding
the budget. This is an injected-latency local result, not a measured S3 speedup.

More workers did not help every workload. Without injected delay, compressible
data took median 0.166 s with one worker, 0.113 s with two, and 0.118 s with four.
For 1,000 tiny files, one worker took 2.049 s and four took 2.080 s. Files are read
sequentially and concurrency is within a file's chunks, so a one-chunk tiny file
has little work to overlap. Its SQLite and filesystem metadata costs remain.
The data does not support a general claim that eight workers are best.

### Resource acceptance

| Input | Wall time | Peak RSS | Result |
| --- | --- | --- | --- |
| 50 GiB logical sparse-zero file | 64.55 s | 38.4 MiB | Backup committed; buffer bound ≤128 MiB |
| 100,000 small files | 209.04 s | 42.4 MiB | All files indexed and committed; buffer bound ≤128 MiB |

The largest backup RSS across all recorded workloads was **61.0 MiB**, below the
512 MiB target in this environment. All unchanged runs reported zero new chunk
payload, and all regular workload verify/restore checks passed. These observations
cover the offline store. A 50 GiB physical-data restore, real S3 throughput, and
power-loss durability were outside that original run. The full restore checks below
close the large-file and large-directory gaps; real S3 and power loss remain untested.
No perf privileges or host security settings were changed; the analysis uses
recorded phase timing and CPU counters.

## Full restore follow-up

The [dense-file records](../benchmarks/results/2026-10-20-restore-local/README.md)
measure a fully written 50 GiB pseudorandom input, 12,800 unique chunks, and a real
50 GiB restored file. This used Ubuntu 24.04 on Docker Desktop/WSL2 with an Intel
i7-14700F host, limited to four CPUs and 8 GiB. The application revision was `55532a6`.

| Dense 50 GiB command | Wall time | Peak RSS |
| --- | --- | --- |
| Backup | 294.675 s | 58.57 MiB |
| Verify | 141.910 s | 36.04 MiB |
| Restore | 453.086 s | 36.04 MiB |

Both source and restored file allocations exceeded their logical size. Verify and
restore used a fresh local state database. A separate complete reread matched the
restored contents, path, mode, and nanosecond mtime. All three processes passed the
512 MiB RSS limit; the backup kept its buffer reservation within 128 MiB.

The [100,000-file CI records](../benchmarks/results/2026-10-20-restore-ci/README.md)
also pass backup, verify, and fresh-state restore with an independent inventory
comparison. That runner had two logical EPYC 9V45 CPUs and about 8 GB RAM. The files
were 256 bytes each with 64 distinct payloads. Backup took 217.342 seconds, verify
5.108 seconds, and restore 39.824 seconds; process RSS peaked at 35.99 MiB for each.

These single acceptance runs establish the tested scale and memory bounds. They
use different hardware from the earlier repeated benchmark and are not a speedup
comparison. They cover the offline store, with all checksums and persistence enabled.
