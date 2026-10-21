# Performance Notes

Breadcrumb log for the low-CPU-overhead storage work. The release history is in
the GitHub releases; this file records the measurement method and the decisions
that follow from it.

## Current status

- `v0.9.1` is the current release.
- The benchmark harness is `gpuvramdisk-bench` (`tools/bench.cpp`).
- No optimized I/O path has been implemented yet.
- A real RX 7900 XTX run exposed pathological direct-mapped reads. The current
  code now keeps direct writes but stages reads through the GPU copy engine on
  discrete GPUs. That change still needs a real-hardware rerun.
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

Run the shared-memory proxy path (requires the same privileges as a mounted
drive):

```powershell
.\gpuvramdisk-bench.exe --gpu 0 --size 256 --ops 32768 --proxy-only
```

Compare direct mapping with forced staging:

```powershell
$env:GPURAMDRIVE_FORCE_STAGING = "1"
.\gpuvramdisk-bench.exe --gpu 0 --size 256 --ops 32768
Remove-Item Env:\GPURAMDRIVE_FORCE_STAGING
```

The proxy benchmark measures this process's shared-memory server and client. It
does not include ImDisk's own driver copy or scheduler behavior. For the full
stack, also run DiskSpd or CrystalDiskMark against a mounted volume at QD1 and
QD32, with 4 KiB random and 1 MiB sequential workloads.

Record at minimum:

| Measurement | Why it matters |
| --- | --- |
| Sequential MiB/s, read/write | Checks PCIe/GPU copy bandwidth. |
| Random 4 KiB IOPS, read/write | Exposes per-I/O latency and submission overhead. |
| Process CPU ms and CPU % | Shows whether throughput is bought with CPU time. |
| CPU microseconds per operation | The key low-overhead metric. |
| Direct versus forced staging | Determines whether BAR reads are helping or hurting. |
| Raw versus proxy | Isolates the ImDisk/proxy protocol cost. |

Treat CPU time from very short Wine runs as unreliable. Use runs long enough to
span several scheduler ticks, and prefer a real Windows host for the numbers
that drive design decisions.

## What NVMe does differently

- Multiple queue pairs, normally one per CPU, with no global lock.
- 64-byte commands and 16-byte completions.
- Direct DMA to and from the caller's buffers.
- Batched doorbell updates, MSI-X completion coalescing, optional polling.
- No per-I/O transition into a user-mode service.

Reference: <https://spdk.io/doc/nvme_spec.html>

## Measured baseline: RX 7900 XTX

The first real-hardware run used direct-mapped VRAM and produced this:

| Path | Sequential write | Sequential read | Random 4K write | Random 4K read |
| --- | ---: | ---: | ---: | ---: |
| Raw Vulkan | 5417 MiB/s | 30.8 MiB/s | 5421 MiB/s | 30.8 MiB/s |
| Shared-memory proxy | 4284 MiB/s | 30.8 MiB/s | 861 MiB/s | 29.7 MiB/s |

The read path is not hitting a bandwidth limit; it is hitting uncached BAR
reads one cache line at a time. Writes use write-combining and are much faster.
The proxy is not the read bottleneck because the proxy read result matches the
raw result almost exactly.

That run was captured before the staged-read change in `ba5615b`. On a direct
mapped discrete GPU, writes remain CPU-to-BAR copies, while reads now go through
a GPU copy into host-visible staging memory before the CPU copies them out.

### About the 800 GB/s number

The RX 7900 XTX has roughly 960 GB/s of *GPU-internal* memory bandwidth. That
number does not apply to data crossing PCIe from the CPU. A CPU-to-VRAM copy is
limited by the PCIe link (PCIe 4.0 x16 is about 32 GB/s theoretical, less in
practice), and a disk I/O path also pays for the ImDisk copies and protocol.
Reaching 800 GB/s over the disk interface would require the data to be consumed
or produced entirely on the GPU, not read from and written to Windows through a
CPU-visible block device.

## Current data path

```text
filesystem IRP
  -> ImDisk kernel driver
  -> shared-memory request + event
  -> user-mode proxy thread
  -> Vulkan copy/submit/fence wait, or a CPU copy on the direct-mapped path
  -> GPU memory
```

The likely costs, in order:

1. Kernel/user transition and event wake-up per I/O.
2. Serialized proxy request handling.
3. Shared-memory copy plus staging copy.
4. Vulkan submission and fence wait per staged transfer.
5. Uncached CPU reads when device-local memory is mapped directly.

The last item is confirmed by the RX 7900 XTX baseline. On a discrete GPU with
resizable BAR, direct writes can be fast enough to keep, while direct reads are
much slower than a GPU copy into host-visible staging memory.

The default policy is now:

- Staged GPU memory: stage both reads and writes.
- Direct-mapped integrated GPU: use direct CPU copies.
- Direct-mapped discrete GPU: direct writes, staged reads.

Force a mode for testing:

```powershell
$env:GPURAMDRIVE_DIRECT_READS = "1"   # force CPU reads from the BAR
$env:GPURAMDRIVE_STAGED_READS = "1"   # force reads through the copy engine
Remove-Item Env:\GPURAMDRIVE_DIRECT_READS, Env:\GPURAMDRIVE_STAGED_READS
```

If both override variables are set, staged reads win.

## Decision points

### Gate A: is the proxy the dominant cost?

Compare raw and proxy 4 KiB results. If proxy CPU per operation is high while
raw GPU storage is cheap, the proxy protocol is the ceiling. In that case,
micro-optimizing Vulkan copies will not produce NVMe-like behavior.

### Gate B: is direct mapping helping?

Run with and without `GPURAMDRIVE_FORCE_STAGING`, separately for reads and
writes. If direct reads are slow, use staging for reads and direct mapping only
for writes, or add a host read cache.

### Gate C: is a driver justified?

If Gate A shows the ImDisk proxy is the ceiling, the practical next step is a
signed Storport virtual miniport with multi-queue shared memory. A user-mode-only
design cannot remove the per-I/O kernel/user round trip.

## Candidate work

| Candidate | Expected effect | Risk |
| --- | --- | --- |
| Direct-write plus staged-read policy | Avoid slow uncached BAR reads | Implemented in `ba5615b`; needs real-hardware validation |
| Batch Vulkan submissions | Less submit/fence overhead | Limited by synchronous proxy requests |
| `VK_EXT_external_memory_host` import of proxy memory | Remove one CPU copy | Extension/pinning support varies |
| Host write-back cache for small I/O | Absorb 4 KiB writes, flush in large batches | Cache coherence and unmount flush rules |
| Custom Storport VMiniport with shared-memory rings | Remove ImDisk proxy and per-I/O events | WDK, signing, and driver maintenance |
| Kernel-only GPU BAR access | Closest to NVMe overhead | Not portable; needs GPU/vendor cooperation |

Recommended order: measure Gates A and B, then implement the smallest change
that addresses the measured bottleneck. Do not start with the custom driver until
the proxy cost is quantified.

## Breadcrumb log

- 2026-10-21, `ba5615b`: added the direct-write/staged-read policy after the
  RX 7900 XTX log showed 30.8 MiB/s direct reads but 5.4 GiB/s direct writes.
  Staged-read mode smoke-tested under Wine/lavapipe; real-hardware rerun needed.
- 2026-10-21: attached `gpuvramdisk-bench.exe` as a separate asset to the
  existing `v0.9.1` release and refreshed `SHA256SUMS`. The tagged archives were
  left unchanged; the benchmark asset was built from `1bf3cfa`.
- 2026-10-21, `1bf3cfa`: added `gpuvramdisk-bench` for raw Vulkan and
  shared-memory proxy measurements. Built cleanly with MinGW-w64; `--help`,
  raw, and proxy modes smoke-tested under Wine/lavapipe.
- 2026-10-21, `35516ee`: `v0.9.1` GUI startup fix; release published with
  Windows and source archives plus checksums.
