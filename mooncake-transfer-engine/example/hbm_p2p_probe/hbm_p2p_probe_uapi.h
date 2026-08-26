// Copyright 2026 KVCache.AI
// SPDX-License-Identifier: Apache-2.0

#ifndef MOONCAKE_HBM_P2P_PROBE_UAPI_H_
#define MOONCAKE_HBM_P2P_PROBE_UAPI_H_

#include <linux/ioctl.h>
#include <linux/types.h>

#define MOONCAKE_HBM_P2P_PROBE_ABI_VERSION 1U
#define MOONCAKE_HBM_P2P_PROBE_DRIVER_NAME_MAX 32U

#define MOONCAKE_HBM_P2P_FLAG_MAP_NVME (1U << 0)

/*
 * Fixed-width UAPI shared by the GPL kernel probe and the ACL user process.
 * The kernel fills every field after put_pages has completed and all temporary
 * DMA mappings have been removed.
 */
struct mooncake_hbm_p2p_probe_request {
    __u32 abi_version;
    __u32 flags;

    __u64 hbm_va;
    __u64 hbm_size;

    __u16 pci_domain;
    __u8 pci_bus;
    __u8 pci_device;
    __u8 pci_function;
    __u8 reserved0[3];

    __s32 get_pages_status;
    __s32 nvme_device_status;
    __s32 dma_map_status;
    __s32 put_pages_status;

    __u32 page_table_version;
    __u32 invalidated;

    __u64 page_size;
    __u64 page_count;
    __u64 first_pa;
    __u64 last_pa;
    __u64 pa_run_count;
    __u64 mapped_count;
    __u64 first_dma;
    __u64 last_dma;
    __u64 dma_run_count;
    __u64 dma_mask;
    __u64 coherent_dma_mask;

    __u32 nvme_vendor_id;
    __u32 nvme_device_id;
    __u32 nvme_class;
    __u32 all_pa_aligned;
    __u32 all_dma_aligned;
    __u32 reserved1;

    char nvme_driver[MOONCAKE_HBM_P2P_PROBE_DRIVER_NAME_MAX];
    __u64 reserved2[8];
};

#define MOONCAKE_HBM_P2P_PROBE_IOC_MAGIC 0xB7
#define MOONCAKE_HBM_P2P_PROBE_IOCTL_RUN                              \
    _IOWR(MOONCAKE_HBM_P2P_PROBE_IOC_MAGIC, 1,                       \
          struct mooncake_hbm_p2p_probe_request)

#endif  // MOONCAKE_HBM_P2P_PROBE_UAPI_H_
