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

## 5. Export real HBM pages and map them for an NVMe PCI device

The second probe crosses two additional kernel-facing gates without issuing an
NVMe command:

```text
aclrtMalloc HBM in the user process
        -> ioctl from the same TGID
        -> hal_kernel_p2p_get_pages
        -> HBM physical-page table
        -> dma_map_resource(NVMe struct device, DMA_FROM_DEVICE)
        -> immediately dma_unmap_resource + hal_kernel_p2p_put_pages
```

It is intentionally split into a GPL kernel module and an ACL user program.
The module performs no file I/O, submits no NVMe command, changes no block
device state, and retains neither HBM pages nor DMA mappings after the ioctl.
The default allocation is 4 MiB and the module rejects allocations above 256
MiB.

### 5.1 Prerequisites and offline build

Build the user program through the existing A3 build directory:

```bash
cmake --build build-a3-ssd-direct --target hbm_p2p_mapping_probe -j
```

The kernel module must be built for the exact running host kernel. A container
that has no `/lib/modules/$(uname -r)/build` cannot build or load it merely by
installing `kmod`. Build on the host, or read-only mount the matching kernel
headers and modules into the container.

```bash
cd mooncake-transfer-engine/example/hbm_p2p_probe

make KDIR=/lib/modules/$(uname -r)/build
```

The probe does not directly link against `hal_kernel_p2p_get_pages` or
`hal_kernel_p2p_put_pages`. At module initialization it obtains both exports
from the already loaded `drv_devmm_host` through `__symbol_get`, holds the
provider module references while the probe is loaded, and releases them with
`__symbol_put` on failure or module exit. This avoids requiring a private
Ascend Driver `Module.symvers`, which is commonly absent from binary driver
installations.

The matching kernel build tree still needs its own complete `Module.symvers`.
Check the two runtime lookup APIs before building:

```bash
grep -wE '__symbol_get|__symbol_put' \
  /lib/modules/$(uname -r)/build/Module.symvers
```

Do not pass the kernel's `Module.symvers` through `KBUILD_EXTRA_SYMBOLS`: Kbuild
already loads it from `KDIR`, and loading it again produces duplicate-export
warnings. Do not use `KBUILD_MODPOST_WARN=1` or fabricate Ascend CRC entries.

The installed driver already passes the export prerequisite when the following
symbols are visible:

```bash
grep -E '(__ksymtab|__kstrtab).*hal_kernel_p2p_(get|put)_pages' \
  /proc/kallsyms
```

Load the diagnostic module on the host. This requires `CAP_SYS_MODULE`; running
the probe device requires `CAP_SYS_RAWIO`.

```bash
insmod ./mooncake_hbm_p2p_probe.ko
test -c /dev/mooncake_hbm_p2p_probe && \
  echo 'PASS: /dev/mooncake_hbm_p2p_probe is ready'
```

If `insmod` reports that either Ascend P2P symbol could not be resolved, verify
that `drv_devmm_host` is loaded and that the two `__ksymtab` entries are visible
in `/proc/kallsyms`. Runtime lookup deliberately removes compile-time CRC
checking for these two vendor functions, so record the loaded driver's path,
SHA-256 and `srcversion`; the probe still rejects an unexpected page-table ABI
version before consuming the returned page table.

### 5.2 Choose the exact NVMe controller BDF

For the controller that owns `/dev/nvme2n1`, for example:

```bash
nvme_bdf=$(basename "$(readlink -f /sys/class/nvme/nvme2/device)")
echo "NVME_BDF=${nvme_bdf}"
cat "/sys/bus/pci/devices/${nvme_bdf}/class"
readlink -f "/sys/bus/pci/devices/${nvme_bdf}/driver"
```

The PCI class must be `0x010802` or another class whose base/subclass is
`0x0108` (NVM Express). The kernel probe rejects a BDF that is not an NVMe
controller.

### 5.3 Run with immediate terminal verdicts

First isolate HBM page export if desired:

```bash
./build-a3-ssd-direct/mooncake-transfer-engine/example/hbm_p2p_mapping_probe \
  --device 0 --size 4194304 --export-only
```

Then test NVMe-device DMA mapping:

```bash
./build-a3-ssd-direct/mooncake-transfer-engine/example/hbm_p2p_mapping_probe \
  --device 0 \
  --size 4194304 \
  --nvme-bdf "${nvme_bdf}"
```

For a single self-contained run that performs preflight checks, loads the local
module, streams the verdicts, and unloads only a module that it loaded itself:

```bash
bash scripts/ascend/run_hbm_p2p_mapping_probe.sh \
  --binary ./build-a3-ssd-direct/mooncake-transfer-engine/example/hbm_p2p_mapping_probe \
  --module ./mooncake-transfer-engine/example/hbm_p2p_probe/mooncake_hbm_p2p_probe.ko \
  --device 0 --size 4194304 --nvme-bdf "${nvme_bdf}"
```

The runner writes no report file and needs no network access. If udev is not
available in the container, it temporarily creates the misc-device node from
`/sys/class/misc/.../dev` and removes only that node during cleanup. Use
`--keep-module` to leave a module loaded by the runner in place.

Every stage prints one immediately readable line:

```text
[PASS] hbm_page_export: ... page_size=... page_count=... first_pa=...
[PASS] nvme_device: ... class=0x010802 driver=nvme dma_mask=...
[PASS] nvme_dma_api_map: ... mapped=N/N first_iova=...
[PASS] hbm_page_release: status=0
[INFO] proof_boundary: ...
RESULT hbm_export=PASS nvme_device=PASS nvme_dma_api_map=PASS \
  pages_released=PASS peer_dma=NOT_PROVEN nvme_prp_sgl=NOT_TESTED \
  physical_nvme_dma_to_hbm=NOT_PROVEN level=L1_5_NVME_DMA_API_MAPPED
```

Interpret the exit code together with the final `RESULT` line:

- `0`: every requested mapping/export check passed and pages were released;
- `2`: invalid command-line argument or alignment;
- `3`: ACL/HBM setup failed;
- `4`: the probe device/ioctl was unavailable, including missing capability;
- `5`: HBM page export failed;
- `6`: exported HBM pages could not be released cleanly;
- `7`: the NVMe BDF or DMA API mapping check failed.

`nvme_dma_api_map=PASS` means that Linux's DMA API produced addresses in the
selected NVMe device's DMA domain for every exported HBM page. It does **not**
prove that the NVMe controller can physically reach those addresses. This
probe deliberately does not create PRP/SGL entries and does not issue a read,
so the established level is named `L1_5_NVME_DMA_API_MAPPED`, not L2.

After testing:

```bash
rmmod mooncake_hbm_p2p_probe
```

The next level requires a separately reviewed, controlled NVMe command path,
content verification, PCIe/ACS/IOMMU evidence, and direct-versus-fallback
counters. Do not attach that experiment to a mounted production filesystem.

## 6. GDS-style benchmark matrix

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
