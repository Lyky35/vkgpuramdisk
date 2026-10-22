# Performance Notes

Breadcrumb log for the low-CPU-overhead storage work. The release history is in
the GitHub releases; this file records the measurement method and the decisions
that follow from it.

## Current status

- `v0.9.1` is the last published release and still uses the old ImDisk proxy.
- The WinSpd backend is implemented, but it has not been measured on real
  Windows hardware yet.
- The old RX 7900 XTX baseline exposed pathological direct-mapped reads. The
  current code keeps direct writes but stages reads through the GPU copy engine
  on discrete GPUs.
- WinSpd runs 8 user-mode dispatcher threads by default, uses a 4 MiB maximum
  transfer, and is backed by a pool of 8 concurrent Vulkan I/O contexts by
  default.
- Wine with Mesa lavapipe is only a functional smoke test.

## Running the benchmark

Build first:

```sh
cmake -S . -B build -G "Unix Makefiles" \
  -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-w64.cmake \
  -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(nproc)"
```

Run the raw Vulkan storage path:

```powershell
.\gpuvramdisk-bench.exe --gpu 0 --size 256 --ops 32768 --raw-only
```

The legacy shared-memory proxy path remains available for comparison with the
old design:

```powershell
.\gpuvramdisk-bench.exe --gpu 0 --size 256 --ops 32768 --proxy-only
```

Compare direct mapping with forced staging:

```powershell
$env:GPURAMDRIVE_FORCE_STAGING = "1"
.\gpuvramdisk-bench.exe --gpu 0 --size 256 --ops 32768
Remove-Item Env:\GPURAMDRIVE_FORCE_STAGING
```

The proxy benchmark measures this process's old shared-memory server and client.
It is not the WinSpd path. For the full stack, mount a WinSpd drive and run
DiskSpd or CrystalDiskMark against the volume at QD1 and QD32, with 4 KiB random
and 1 MiB sequential workloads.

Record at minimum:

| Measurement | Why it matters |
| --- | --- |
| Sequential MiB/s, read/write | Checks PCIe/GPU copy bandwidth. |
| Random 4 KiB IOPS, read/write | Exposes per-I/O latency and submission overhead. |
| Process CPU ms and CPU % | Shows whether throughput is bought with CPU time. |
| CPU microseconds per operation | The key low-overhead metric. |
| Direct versus forced staging | Determines whether BAR reads are helping or hurting. |
| WinSpd versus legacy proxy | Shows whether the rewrite removed the old ceiling. |
| 1, 2, 4, 8, 16 contexts | Checks whether the Vulkan context pool scales. |

Treat CPU time from very short Wine runs as unreliable. Use runs long enough to
span several scheduler ticks, and prefer a real Windows host for the numbers
that drive design decisions.

## Measured baseline: RX 7900 XTX

This was captured before the WinSpd rewrite, using the old ImDisk proxy:

| Path | Sequential write | Sequential read | Random 4K write | Random 4K read |
| --- | ---: | ---: | ---: | ---: |
| Raw Vulkan | 5417 MiB/s | 30.8 MiB/s | 5421 MiB/s | 30.8 MiB/s |
| Legacy shared-memory proxy | 4284 MiB/s | 30.8 MiB/s | 861 MiB/s | 29.7 MiB/s |

The read path was not hitting a bandwidth limit; it was hitting uncached BAR
reads one cache line at a time. Writes use write-combining and were much faster.
The proxy read result matched raw almost exactly, so the proxy was not the read
bottleneck in that run.

That run predates the staged-read change in `ba5615b`. On a direct-mapped
discrete GPU, writes remain CPU-to-BAR copies, while reads now go through a GPU
copy into host-visible staging memory before the CPU copies them out.

### About the 800 GB/s number

The RX 7900 XTX has roughly 960 GB/s of *GPU-internal* memory bandwidth. That
number does not apply to data crossing PCIe from the CPU. A CPU-to-VRAM copy is
limited by the PCIe link (PCIe 4.0 x16 is about 32 GB/s theoretical, less in
practice). A disk I/O path also pays for the storage driver, user-mode
dispatcher, Vulkan submission, and one or more copies. Reaching 800 GB/s over
the disk interface would require the data to be consumed or produced entirely
on the GPU, not read from and written to Windows through a CPU-visible block
device.

## Current data path

```text
filesystem IRP
  -> WinSpd Storport miniport
  -> user-mode dispatcher threads (8 by default)
  -> Vulkan copy/submit/fence wait, or a CPU copy on the direct-mapped path
  -> GPU memory
```

The likely costs, in order:

1. Kernel/user transition and WinSpd request dispatch per I/O.
2. Synchronous Vulkan submission and fence wait per staged transfer.
3. Shared staging copy for reads on direct-mapped discrete GPUs.
4. Uncached CPU reads when device-local memory is mapped directly.
5. `diskpart` only during mount/format, not in the I/O path.

The default policy is:

- Staged GPU memory: stage both reads and writes.
- Direct-mapped integrated GPU: use direct CPU copies.
- Direct-mapped discrete GPU: direct writes, staged reads.

Force a mode for testing:

```powershell
$env:GPURAMDRIVE_DIRECT_READS = "1"   # force CPU reads from the BAR
$env:GPURAMDRIVE_STAGED_READS = "1"   # force reads through the copy engine
Remove-Item Env:\GPURAMDRIVE_DIRECT_READS, Env:\GPURAMDRIVE_STAGED_READS
```

If both override variables are set, staged reads win. Set
`GPURAMDRIVE_IO_CONTEXTS` to change the Vulkan context count (1–32).

## Decision points

### Gate A: did WinSpd remove the proxy ceiling?

Run the same DiskSpd workload against a WinSpd volume and against the old
ImDisk proxy volume. If WinSpd improves 4 KiB IOPS and CPU time per operation,
the dispatcher rewrite is paying off. If not, the remaining cost is likely
Vulkan submission/fence waits or PCIe copies.

### Gate B: is direct mapping helping?

Run with and without `GPURAMDRIVE_FORCE_STAGING`, separately for reads and
writes. If direct reads are slow, keep staging for reads and direct mapping only
for writes, or add a host read cache.

### Gate C: does the context pool scale?

Measure 1, 2, 4, 8, 16, and 32 contexts at QD32. If throughput flattens early,
the bottleneck is outside the Vulkan context pool.

### Gate D: is a custom driver still justified?

WinSpd removes the single serialized shared-memory proxy, but it still uses a
kernel-to-user dispatcher. A custom signed Storport miniport with multi-queue
shared memory could remove more overhead, but it needs WDK, signing, and
ongoing driver maintenance. Do not start there until Gates A–C are measured.

## Candidate work

| Candidate | Expected effect | Risk |
| --- | --- | --- |
| WinSpd backend | Remove ImDisk/proxy copies and serialization | Beta driver, needs real-hardware validation |
| Concurrent Vulkan I/O contexts | Overlap submissions and fence waits | Implemented, needs QD scaling data |
| Direct-write plus staged-read policy | Avoid slow uncached BAR reads | Implemented in `ba5615b`; needs real-hardware rerun |
| Batch Vulkan submissions | Less submit/fence overhead | More complex completion handling |
| Host write-back cache for small I/O | Absorb 4 KiB writes, flush in large batches | Cache coherence and unmount flush rules |
| Custom Storport VMiniport with shared-memory rings | Remove the WinSpd user-mode dispatcher | WDK, signing, and driver maintenance |
| Kernel-only GPU BAR access | Closest to NVMe overhead | Not portable; needs GPU/vendor cooperation |

## Breadcrumb log

- 2026-10-21: replaced the ImDisk storage path with a WinSpd backend,
  concurrent Vulkan I/O contexts, MBR creation, and `diskpart`-based format and
  assignment. Builds cleanly with MinGW-w64; Wine smoke tests pass, but the
  WinSpd mount path is untested on real elevated Windows.
- 2026-10-21, `ba5615b`: added the direct-write/staged-read policy after the
  RX 7900 XTX log showed 30.8 MiB/s direct reads but 5.4 GiB/s direct writes.
  Staged-read mode smoke-tested under Wine/lavapipe; real-hardware rerun needed.
- 2026-10-21: attached `gpuvramdisk-bench.exe` as a separate asset to the
  existing `v0.9.1` release and refreshed `SHA256SUMS`. The asset was later
  rebuilt from `ba5615b` to include staged reads. The tagged archives were left
  unchanged.
- 2026-10-21, `1bf3cfa`: added `gpuvramdisk-bench` for raw Vulkan and
  shared-memory proxy measurements. Built cleanly with MinGW-w64; `--help`,
  raw, and proxy modes smoke-tested under Wine/lavapipe.
- 2026-10-21, `35516ee`: `v0.9.1` GUI startup fix; release published with
  Windows and source archives plus checksums.
