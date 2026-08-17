# A3 SSD restore baseline and GDS-inspired direct-HBM validation

This workflow targets source commit `e9c61075720039bcfc5fffd19f847608402be3d0`
(`v0.3.11.post1`). It instruments and validates the current serialized path. It
does not implement the pipelined-staging proposal.

## 1. Record source, wheel, package, and loaded libraries

Run this while the vLLM/Mooncake process is serving. Pass every relevant worker
PID when multiple ranks are present.

```bash
python3 scripts/ascend/collect_a3_ssd_direct_env.py \
  --pid 12345 --pid 12346 \
  --wheel /path/to/mooncake_transfer_engine_npu-0.3.11.post1-*.whl \
  --ssd-path /nvme/mooncake_offload \
  --npu-bdf 0000:01:00.0 \
  --nvme-bdf 0000:81:00.0 \
  --strict --strict-platform \
  --output artifacts/a3-environment.json
```

Replace the example BDFs with the exact NPU and NVMe controller addresses from
`lspci -D -nn`. Repeat `--npu-bdf` and `--nvme-bdf` when multiple devices are
used.

The JSON includes `pip show`, distribution metadata, the wheel hash, installed
native-library hashes, and selected loaded `.so` paths/hashes from
`/proc/<pid>/maps`. Review command errors: missing `nvme`, insufficient `/proc`
permissions, or a stale PID make the record incomplete.

The command also prints an immediate `PASS`/`FAIL` summary, wheel hashes, and
every selected loaded `.so` path/hash. With `--strict`, exit code 3 means the
identity chain is mismatched or incomplete: source/version, wheel versus
installed native files, PID maps, or loaded versus installed package libraries.
Omit `--strict` when collecting partial evidence or an intentionally different
build.

`--strict-platform` returns exit code 4 when the GDS-style platform evidence is
incomplete. The collector records and immediately summarizes:

- PCIe tree and the selected NPU/NVMe BDF details, including link, NUMA and ACS
  lines exposed by `lspci`;
- kernel IOMMU groups, command line, relevant modules and
  `CONFIG_PCI_P2PDMA`/SMMU/IOMMU options;
- SSD mount/filesystem, NVMe identity, logical/physical block size, minimum and
  optimal I/O sizes;
- CANN/driver versions and the exact running package/native-library identity.

`evidence_complete=PASS` means the requested evidence was collected. It never
means peer DMA is supported. `direct_dma_qualified` intentionally remains `NO`
until an Ascend driver/API can register HBM pages for storage DMA and the
filesystem/block layer is shown to use that mapping.

## 2. Measure the existing serialized restore path

Enable the opt-in trace only for profiling runs:

```bash
export MOONCAKE_OFFLOAD_PERF_TRACE=1
# Start the normal workload and retain owner/requester logs.
python3 scripts/ascend/analyze_offload_perf_trace.py \
  owner.log requester.log \
  --output artifacts/offload-baseline.json
```

`ssd_to_staging` is the owner-side `BatchLoad` interval.
`ssd_rpc_wait` is the requester-visible RPC interval and includes more than SSD
I/O. `staging_to_hbm` measures the blocking transfer into the destination KV
buffers. The calculated overlap gain is an upper-bound model, not a measured
pipeline result. Keep warm-up, object sizes, SSD cache state, request order,
concurrency, and rank topology fixed across comparisons.
The command prints the stage latency/bandwidth table and overlap upper bound to
the terminal immediately, while retaining the full JSON report locally.

For a live terminal view during a workload, merge the owner/requester log stream
and pipe it to stdin. Each completed stage is printed immediately; press
`Ctrl-C` to print the accumulated summary:

```bash
tail -F owner.log requester.log | \
  python3 scripts/ascend/analyze_offload_perf_trace.py - --live \
    --output artifacts/offload-live.json
```

## 3. Probe whether Linux file I/O accepts an HBM destination

Build on A3 with the same CANN environment used for Mooncake:

```bash
cmake -S . -B build -DUSE_ASCEND_DIRECT=ON
cmake --build build --target ssd_direct_hbm_probe -j
```

Choose an existing, read-only file on the same filesystem as Mooncake SSD
offload. The offset and size must satisfy that filesystem's `O_DIRECT`
alignment requirements.

```bash
./build/mooncake-transfer-engine/example/ssd_direct_hbm_probe \
  --file /nvme/mooncake_offload/probe.bin \
  --size 4194304 --offset 0 --device 0 \
  | tee artifacts/ssd-direct-hbm-probe.jsonl
```

Interpretation:

- `control_buffered_host=verified` proves the input and length are readable.
- `probe_metadata` records the HBM address, `aclrtPtrAttributes.pageSize`, file
  system block size, and 4-KiB address/size/offset alignment.
- `staging_control_*` measures an `O_DIRECT` SSD-to-aligned-host read followed
  by synchronous Host-to-HBM copy. This is a portable comparison baseline, not
  a claim that it is identical to Mooncake FabricMem staging.
- `buffered_pread_to_hbm=verified` only proves buffered POSIX I/O accepted the
  address. It is not a direct-storage result because the page cache may be used.
- `odirect_pread_to_hbm` or `io_uring_fixed_odirect_to_hbm=verified` proves
  only the L1 condition: O_DIRECT I/O accepted the HBM address and copied-back
  content matched.
- `physical_dma_direct_proven` intentionally remains `false`. A successful
  system call alone cannot exclude driver-internal staging. Proving physical
  NVMe-to-HBM DMA additionally requires destination-address tracing, driver or
  IOMMU evidence, absence of an intermediate copy, and matching runtime traces.
- An unsupported direct attempt is a validation result, not a probe-process
  failure. Setup/control failures return nonzero.

The final terminal line starts with `RESULT` and reports the staging comparison,
kernel-I/O result, O_DIRECT result and validation level. Peer-DMA mapping and
physical NVMe DMA always remain `NOT_PROVEN` in this standalone probe.

## 4. Validation levels

Use these levels consistently in reports:

- **L0 — HBM identified:** `aclrtPointerGetAttributes` confirms Device memory
  and records its page size.
- **L1 — O_DIRECT accepted:** an O_DIRECT call completed into HBM and the
  copied-back bytes match. This still does not prove peer DMA.
- **L2 — peer-DMA mapping proven:** an Ascend/vendor buffer-registration or
  page-export mechanism, filesystem/block support and direct/fallback counters
  prove that storage can address the HBM pages.
- **L3 — physical NVMe-to-HBM DMA proven:** L2 plus PCIe/IOMMU/SMMU and runtime
  traces show NVMe DMA targeting HBM with no CPU/FabricMem bounce buffer.

The current standalone probe can automatically reach at most L1. Do not label
it “A3 Direct Storage” until L2 is established.

## 5. GDS-style benchmark matrix

After L2 exists, compare the same file, offsets and cache state across:

1. SSD to pageable/aligned Host, then Host to HBM;
2. SSD to the production Mooncake/FabricMem staging buffer, then HBM;
3. SSD to HBM through the direct candidate API.

Sweep I/O size, queue depth, concurrency and buffer reuse. Record p50/p95/p99,
GiB/s, CPU utilization/memory bandwidth, NVMe queue depth, NPU copy-engine
activity, direct bytes, fallback bytes, registration time and end-to-end TTFT.
Use actual vLLM KV block slices before production integration; the standalone
probe uses one contiguous `aclrtMalloc` allocation and does not validate
scatter/gather KV layouts.

For a defensible result, run buffered and `O_DIRECT` cases across multiple sizes
and correlate with `iostat`, CPU memory-copy profiling, block tracing, and Ascend
copy-engine profiling. Do not compare cold SSD reads against cached control
runs.
