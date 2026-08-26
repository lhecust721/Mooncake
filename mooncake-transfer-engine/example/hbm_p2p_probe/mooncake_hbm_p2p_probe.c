// Copyright 2026 KVCache.AI
// SPDX-License-Identifier: GPL-2.0-only

#include <linux/atomic.h>
#include <linux/capability.h>
#include <linux/dma-mapping.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/overflow.h>
#include <linux/pci.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/uaccess.h>
#include <linux/vmalloc.h>

#include "hbm_p2p_probe_uapi.h"

#define MOONCAKE_HBM_P2P_MIN_ALIGNMENT 4096ULL
#define MOONCAKE_HBM_P2P_MAX_SIZE (256ULL * 1024ULL * 1024ULL)
#define MOONCAKE_HBM_P2P_PAGE_TABLE_VERSION 1U

/*
 * Minimal ABI copied from the public CANN driver ascend_kernel_hal.h. Keep it
 * private to this diagnostic module and reject unexpected table versions.
 */
struct p2p_page_info {
    u64 pa;
    u64 reserved[4];
};

struct p2p_page_table {
    u32 version;
    u64 page_size;
    struct p2p_page_info *pages_info;
    u64 page_num;
    u64 reserved[4];
};

extern int hal_kernel_p2p_get_pages(
    u64 va, u64 len, void (*free_callback)(void *data), void *data,
    struct p2p_page_table **page_table);
extern int hal_kernel_p2p_put_pages(struct p2p_page_table *page_table);

struct mooncake_hbm_callback_context {
    atomic_t invalidated;
};

static void mooncake_hbm_free_callback(void *data)
{
    struct mooncake_hbm_callback_context *context = data;

    if (context != NULL)
        atomic_set(&context->invalidated, 1);
}

static void mooncake_initialize_results(
    struct mooncake_hbm_p2p_probe_request *request)
{
    request->get_pages_status = -EINPROGRESS;
    request->nvme_device_status = -EOPNOTSUPP;
    request->dma_map_status = -EOPNOTSUPP;
    request->put_pages_status = -EINPROGRESS;
    request->page_table_version = 0;
    request->invalidated = 0;
    request->page_size = 0;
    request->page_count = 0;
    request->first_pa = 0;
    request->last_pa = 0;
    request->pa_run_count = 0;
    request->mapped_count = 0;
    request->first_dma = 0;
    request->last_dma = 0;
    request->dma_run_count = 0;
    request->dma_mask = 0;
    request->coherent_dma_mask = 0;
    request->nvme_vendor_id = 0;
    request->nvme_device_id = 0;
    request->nvme_class = 0;
    request->all_pa_aligned = 0;
    request->all_dma_aligned = 0;
    request->reserved1 = 0;
    memset(request->nvme_driver, 0, sizeof(request->nvme_driver));
    memset(request->reserved2, 0, sizeof(request->reserved2));
}

static int mooncake_record_page_table(
    struct mooncake_hbm_p2p_probe_request *request,
    const struct p2p_page_table *page_table)
{
    u64 i;
    u64 previous_pa = 0;
    u64 covered_size;

    if (page_table == NULL || page_table->pages_info == NULL ||
        page_table->page_size == 0 || page_table->page_num == 0)
        return -EINVAL;

    if (page_table->version != MOONCAKE_HBM_P2P_PAGE_TABLE_VERSION)
        return -EPROTO;

    if (page_table->page_num >
        MOONCAKE_HBM_P2P_MAX_SIZE / MOONCAKE_HBM_P2P_MIN_ALIGNMENT)
        return -E2BIG;
    if (check_mul_overflow(page_table->page_num, page_table->page_size,
                           &covered_size) ||
        covered_size < request->hbm_size)
        return -EOVERFLOW;

    request->page_table_version = page_table->version;
    request->page_size = page_table->page_size;
    request->page_count = page_table->page_num;
    request->first_pa = page_table->pages_info[0].pa;
    request->last_pa =
        page_table->pages_info[page_table->page_num - 1].pa;
    request->pa_run_count = 1;
    request->all_pa_aligned = 1;

    for (i = 0; i < page_table->page_num; ++i) {
        u64 pa = page_table->pages_info[i].pa;

        if (pa == 0 || (pa % page_table->page_size) != 0)
            request->all_pa_aligned = 0;
        if (i > 0 && pa != previous_pa + page_table->page_size)
            request->pa_run_count++;
        previous_pa = pa;
    }

    return request->all_pa_aligned ? 0 : -EADDRNOTAVAIL;
}

static int mooncake_map_for_nvme(
    struct mooncake_hbm_p2p_probe_request *request,
    const struct p2p_page_table *page_table)
{
    struct pci_dev *pdev;
    dma_addr_t *dma_addresses;
    const char *driver_name;
    u64 i;
    u64 previous_dma = 0;
    int status = 0;

    pdev = pci_get_domain_bus_and_slot(
        request->pci_domain, request->pci_bus,
        PCI_DEVFN(request->pci_device, request->pci_function));
    if (pdev == NULL) {
        request->nvme_device_status = -ENODEV;
        request->dma_map_status = -ENODEV;
        return -ENODEV;
    }

    request->nvme_vendor_id = pdev->vendor;
    request->nvme_device_id = pdev->device;
    request->nvme_class = pdev->class;
    request->dma_mask = pdev->dev.dma_mask != NULL ?
                            *pdev->dev.dma_mask : 0;
    request->coherent_dma_mask = pdev->dev.coherent_dma_mask;
    driver_name = dev_driver_string(&pdev->dev);
    if (driver_name != NULL)
        strscpy(request->nvme_driver, driver_name,
                sizeof(request->nvme_driver));

    if ((pdev->class >> 8) != PCI_CLASS_STORAGE_EXPRESS) {
        request->nvme_device_status = -ENOTBLK;
        request->dma_map_status = -ENOTBLK;
        pci_dev_put(pdev);
        return -ENOTBLK;
    }
    request->nvme_device_status = 0;

    dma_addresses = kvmalloc_array(page_table->page_num,
                                   sizeof(*dma_addresses), GFP_KERNEL);
    if (dma_addresses == NULL) {
        request->dma_map_status = -ENOMEM;
        pci_dev_put(pdev);
        return -ENOMEM;
    }

    request->all_dma_aligned = 1;
    request->dma_run_count = 0;
    for (i = 0; i < page_table->page_num; ++i) {
        dma_addr_t dma_address;

        dma_address = dma_map_resource(
            &pdev->dev, page_table->pages_info[i].pa,
            page_table->page_size, DMA_FROM_DEVICE, 0);
        if (dma_mapping_error(&pdev->dev, dma_address)) {
            status = -EIO;
            break;
        }

        dma_addresses[i] = dma_address;
        request->mapped_count++;
        if (i == 0) {
            request->first_dma = dma_address;
            request->dma_run_count = 1;
        } else if ((u64)dma_address !=
                   previous_dma + page_table->page_size) {
            request->dma_run_count++;
        }
        if (((u64)dma_address % MOONCAKE_HBM_P2P_MIN_ALIGNMENT) != 0)
            request->all_dma_aligned = 0;
        previous_dma = dma_address;
        request->last_dma = dma_address;
    }

    while (request->mapped_count > 0) {
        u64 index = request->mapped_count - 1;

        dma_unmap_resource(&pdev->dev, dma_addresses[index],
                           page_table->page_size, DMA_FROM_DEVICE, 0);
        request->mapped_count--;
    }

    /* Restore the successful mapping count after all mappings are removed. */
    request->mapped_count = i;
    request->dma_map_status = status;
    kvfree(dma_addresses);
    pci_dev_put(pdev);
    return status;
}

static long mooncake_hbm_p2p_ioctl(struct file *file, unsigned int command,
                                   unsigned long argument)
{
    struct mooncake_hbm_p2p_probe_request request;
    struct mooncake_hbm_callback_context *callback_context = NULL;
    struct p2p_page_table *page_table = NULL;
    void __user *user_request = (void __user *)argument;
    int status;

    (void)file;
    if (command != MOONCAKE_HBM_P2P_PROBE_IOCTL_RUN)
        return -ENOTTY;
    if (!capable(CAP_SYS_RAWIO))
        return -EPERM;
    if (copy_from_user(&request, user_request, sizeof(request)) != 0)
        return -EFAULT;
    if (request.abi_version != MOONCAKE_HBM_P2P_PROBE_ABI_VERSION)
        return -EPROTO;
    if ((request.flags & ~MOONCAKE_HBM_P2P_FLAG_MAP_NVME) != 0)
        return -EINVAL;
    if (request.hbm_size == 0 ||
        request.hbm_size > MOONCAKE_HBM_P2P_MAX_SIZE ||
        request.hbm_va + request.hbm_size < request.hbm_va ||
        (request.hbm_va % MOONCAKE_HBM_P2P_MIN_ALIGNMENT) != 0 ||
        (request.hbm_size % MOONCAKE_HBM_P2P_MIN_ALIGNMENT) != 0 ||
        request.pci_device > 31 || request.pci_function > 7)
        return -EINVAL;

    mooncake_initialize_results(&request);
    callback_context = kzalloc(sizeof(*callback_context), GFP_KERNEL);
    if (callback_context == NULL)
        return -ENOMEM;
    atomic_set(&callback_context->invalidated, 0);

    status = hal_kernel_p2p_get_pages(
        request.hbm_va, request.hbm_size, mooncake_hbm_free_callback,
        callback_context, &page_table);
    request.get_pages_status = status;
    if (status == 0) {
        status = mooncake_record_page_table(&request, page_table);
        if (status != 0)
            request.get_pages_status = status;
    }

    if (request.get_pages_status == 0 &&
        (request.flags & MOONCAKE_HBM_P2P_FLAG_MAP_NVME) != 0) {
        if (atomic_read(&callback_context->invalidated) != 0) {
            request.dma_map_status = -EPIPE;
        } else {
            mooncake_map_for_nvme(&request, page_table);
        }
    }

    if (page_table != NULL)
        request.put_pages_status = hal_kernel_p2p_put_pages(page_table);
    else
        request.put_pages_status = -ENODATA;
    request.invalidated = atomic_read(&callback_context->invalidated) != 0;

    kfree(callback_context);
    if (copy_to_user(user_request, &request, sizeof(request)) != 0)
        return -EFAULT;
    return 0;
}

static const struct file_operations mooncake_hbm_p2p_fops = {
    .owner = THIS_MODULE,
    .unlocked_ioctl = mooncake_hbm_p2p_ioctl,
};

static struct miscdevice mooncake_hbm_p2p_device = {
    .minor = MISC_DYNAMIC_MINOR,
    .name = "mooncake_hbm_p2p_probe",
    .fops = &mooncake_hbm_p2p_fops,
    .mode = 0600,
};

static int __init mooncake_hbm_p2p_init(void)
{
    return misc_register(&mooncake_hbm_p2p_device);
}

static void __exit mooncake_hbm_p2p_exit(void)
{
    misc_deregister(&mooncake_hbm_p2p_device);
}

module_init(mooncake_hbm_p2p_init);
module_exit(mooncake_hbm_p2p_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("KVCache.AI");
MODULE_DESCRIPTION("Ascend HBM page export and NVMe DMA mapping probe");
MODULE_SOFTDEP("pre: drv_devmm_host");
