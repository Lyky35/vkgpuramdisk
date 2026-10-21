# Performance Notes

Breadcrumb log for the low-CPU-overhead storage work. The release history is in
the GitHub releases; this file records the measurement method and the decisions
that follow from it.

## Current status

- `v0.9.1` is the current release.
- The benchmark harness is `gpuvramdisk-bench` (`tools/bench.cpp`).
- No optimized I/O path has been implemented yet.
- No benchmark result from a real discrete GPU or a real ImDisk mount is checked
  in yet. Wine with Mesa lavapipe is only a functional smoke test.

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

The last item is a hypothesis that must be measured. On a discrete GPU with
resizable BAR, direct writes may be reasonable, while direct reads may be much
slower than a GPU copy into host-visible staging memory.

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
| Direct-write plus staged-read policy | Avoid slow uncached BAR reads | Needs per-GPU validation |
| Batch Vulkan submissions | Less submit/fence overhead | Limited by synchronous proxy requests |
| `VK_EXT_external_memory_host` import of proxy memory | Remove one CPU copy | Extension/pinning support varies |
| Host write-back cache for small I/O | Absorb 4 KiB writes, flush in large batches | Cache coherence and unmount flush rules |
| Custom Storport VMiniport with shared-memory rings | Remove ImDisk proxy and per-I/O events | WDK, signing, and driver maintenance |
| Kernel-only GPU BAR access | Closest to NVMe overhead | Not portable; needs GPU/vendor cooperation |

Recommended order: measure Gates A and B, then implement the smallest change
that addresses the measured bottleneck. Do not start with the custom driver until
the proxy cost is quantified.

## Breadcrumb log

- 2026-10-21, `1bf3cfa`: added `gpuvramdisk-bench` for raw Vulkan and
  shared-memory proxy measurements. Built cleanly with MinGW-w64; `--help`,
  raw, and proxy modes smoke-tested under Wine/lavapipe.
- 2026-10-21, `35516ee`: `v0.9.1` GUI startup fix; release published with
  Windows and source archives plus checksums.
