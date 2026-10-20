# Dense 50 GiB restore

`scripts/restore-scale.ps1` ran the executable built from revision
`55532a68696ba277a508133c687b44e48a674709`. Its SHA-256 is
`1ca60881d26873462ffe96fb18e84e858bce4b8b15ed9777d33b211059e1775a`.
The full toolchain and dependency records are in
[follow-up validation](../../../docs/validation/2026-10-20-followup/README.md).

This was an Ubuntu 24.04 container on Docker Desktop 29.5.3 and WSL2, with an Intel
Core i7-14700F host. Its cgroup limits were four CPUs (`400000 100000`) and 8 GiB
(`8589934592`). Work stayed on the container's Linux filesystem; the Windows project
mount was read-only. The exact platform and limits are in `environment.json`.

Every byte of the 50 GiB pseudorandom file was written from seed `20261020`, without
truncate or sparse seeks. Source file allocation was 53,687,103,488 bytes; restored
file allocation was 53,687,115,776 bytes. Both exceeded the 53,687,091,200 logical
bytes. The 12,800 unique chunks uploaded 53,688,448,000 encoded bytes, so this was
not a zero-data or high-compression substitute.

| Command | Wall time | Process peak RSS |
| --- | --- | --- |
| Backup | 294.675 s | 58.57 MiB |
| Verify | 141.910 s | 36.04 MiB |
| Restore | 453.086 s | 36.04 MiB |

Verify and restore used a fresh state database. After restore published successfully,
the independent Python inventory reread the whole file and matched its content,
relative path, permissions, and nanosecond mtime, including the root directory.
`dense-data.jsonl` contains all five original measurements and comparison records.
The backup buffer reservation was 119,865,344 bytes, within the 128 MiB budget.
All three processes stayed below 512 MiB RSS.

These are single offline acceptance runs. They do not establish repeatable disk
throughput, real S3 behavior, or power-loss durability. The same invocation also
runs the small-file case; its records are saved separately when complete.
