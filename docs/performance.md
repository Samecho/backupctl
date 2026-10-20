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

CLI counters distinguish successful encoded upload payload from logical bytes,
and chunk payload from manifest/commit overhead. An unchanged backup may have zero
new chunk bytes while still issuing HEAD, PUT, and GET requests. Worker phase times
are sums across workers and can overlap; they must not be added to end-to-end time.
Peak RSS is from `getrusage`. The buffer-bound counter is a conservative reservation
for owned raw/encoded buffers and SDK stream copies, rather than a total-RSS limit.
CPU time and block-I/O operation counts are reported separately.

Raw results and the observed tradeoff will be added after the benchmark completes.
No target MB/s or expected speedup is treated as a result.
