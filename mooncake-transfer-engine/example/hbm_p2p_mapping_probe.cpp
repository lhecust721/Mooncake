// Copyright 2026 KVCache.AI
// SPDX-License-Identifier: Apache-2.0

#include <acl/acl.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <cerrno>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <iostream>
#include <sstream>
#include <string>

#include "hbm_p2p_probe_uapi.h"

namespace {

struct Options {
    std::string device_node = "/dev/mooncake_hbm_p2p_probe";
    std::string nvme_bdf;
    size_t size = 4U * 1024U * 1024U;
    int device = 0;
    bool export_only = false;
};

void Print(const char *state, const std::string &test,
           const std::string &detail) {
    std::cout << '[' << state << "] " << test;
    if (!detail.empty()) std::cout << ": " << detail;
    std::cout << std::endl;
}

std::string ErrorDetail(int status) {
    if (status >= 0) return std::to_string(status);
    return std::to_string(status) + " (" + std::strerror(-status) + ")";
}

std::string Hex(uint64_t value) {
    std::ostringstream stream;
    stream << "0x" << std::hex << value;
    return stream.str();
}

bool ParseBdf(const std::string &text,
              mooncake_hbm_p2p_probe_request *request) {
    unsigned int domain = 0;
    unsigned int bus = 0;
    unsigned int device = 0;
    unsigned int function = 0;
    char trailing = '\0';

    if (std::sscanf(text.c_str(), "%x:%x:%x.%x%c", &domain, &bus,
                    &device, &function, &trailing) != 4 ||
        domain > 0xffff || bus > 0xff || device > 0x1f || function > 7) {
        return false;
    }
    request->pci_domain = static_cast<__u16>(domain);
    request->pci_bus = static_cast<__u8>(bus);
    request->pci_device = static_cast<__u8>(device);
    request->pci_function = static_cast<__u8>(function);
    return true;
}

bool Parse(int argc, char **argv, Options *options) {
    for (int i = 1; i < argc; ++i) {
        const std::string argument(argv[i]);
        if (argument == "--device" && i + 1 < argc) {
            options->device = std::stoi(argv[++i]);
        } else if (argument == "--size" && i + 1 < argc) {
            options->size = std::stoull(argv[++i]);
        } else if (argument == "--nvme-bdf" && i + 1 < argc) {
            options->nvme_bdf = argv[++i];
        } else if (argument == "--dev-node" && i + 1 < argc) {
            options->device_node = argv[++i];
        } else if (argument == "--export-only") {
            options->export_only = true;
        } else {
            return false;
        }
    }

    return options->size > 0 && options->size <= 256U * 1024U * 1024U &&
           options->size % 4096 == 0 &&
           (options->export_only || !options->nvme_bdf.empty());
}

void Usage(const char *program) {
    std::cerr << "usage: " << program
              << " [--device ID] [--size BYTES]"
                 " (--nvme-bdf DDDD:BB:DD.F | --export-only)"
                 " [--dev-node PATH]\n";
}

}  // namespace

int main(int argc, char **argv) {
    Options options;
    try {
        if (!Parse(argc, argv, &options)) {
            Usage(argv[0]);
            return 2;
        }
    } catch (const std::exception &error) {
        Print("FAIL", "arguments", error.what());
        Usage(argv[0]);
        return 2;
    }

    mooncake_hbm_p2p_probe_request request{};
    request.abi_version = MOONCAKE_HBM_P2P_PROBE_ABI_VERSION;
    request.hbm_size = options.size;
    if (!options.export_only) {
        request.flags = MOONCAKE_HBM_P2P_FLAG_MAP_NVME;
        if (!ParseBdf(options.nvme_bdf, &request)) {
            Print("FAIL", "nvme_bdf", "expected DDDD:BB:DD.F");
            return 2;
        }
    }

    aclError acl_status = aclInit(nullptr);
    if (acl_status != ACL_SUCCESS) {
        Print("FAIL", "acl_init", std::to_string(acl_status));
        return 3;
    }
    Print("PASS", "acl_init", "ACL runtime initialized");

    acl_status = aclrtSetDevice(options.device);
    if (acl_status != ACL_SUCCESS) {
        Print("FAIL", "acl_set_device", std::to_string(acl_status));
        aclFinalize();
        return 3;
    }
    Print("PASS", "acl_set_device", "device=" +
                                          std::to_string(options.device));

    void *hbm_buffer = nullptr;
    acl_status = aclrtMalloc(&hbm_buffer, options.size,
                             ACL_MEM_MALLOC_HUGE_FIRST);
    if (acl_status != ACL_SUCCESS || hbm_buffer == nullptr) {
        Print("FAIL", "acl_malloc_hbm", std::to_string(acl_status));
        aclrtResetDevice(options.device);
        aclFinalize();
        return 3;
    }

    request.hbm_va = reinterpret_cast<uintptr_t>(hbm_buffer);
    Print("PASS", "acl_malloc_hbm",
          "va=" + Hex(request.hbm_va) +
              " size=" + std::to_string(options.size));

    aclrtPtrAttributes attributes{};
    acl_status = aclrtPointerGetAttributes(hbm_buffer, &attributes);
    const bool device_memory =
        acl_status == ACL_SUCCESS &&
        attributes.location.type == ACL_MEM_LOCATION_TYPE_DEVICE;
    Print(device_memory ? "PASS" : "FAIL", "acl_pointer_attributes",
          "acl_status=" + std::to_string(acl_status) +
              " location_type=" +
              std::to_string(static_cast<int>(attributes.location.type)) +
              " device_id=" + std::to_string(attributes.location.id) +
              " acl_page_size=" + std::to_string(attributes.pageSize));
    if (!device_memory || request.hbm_va % 4096 != 0) {
        Print("FAIL", "hbm_precondition",
              "the pointer must be verified device memory and 4-KiB aligned");
        aclrtFree(hbm_buffer);
        aclrtResetDevice(options.device);
        aclFinalize();
        return 3;
    }
    Print("PASS", "hbm_precondition", "device memory and 4-KiB alignment");

    acl_status = aclrtMemset(hbm_buffer, options.size, 0xA5, options.size);
    if (acl_status != ACL_SUCCESS) {
        Print("FAIL", "acl_memset_hbm", std::to_string(acl_status));
        aclrtFree(hbm_buffer);
        aclrtResetDevice(options.device);
        aclFinalize();
        return 3;
    }
    Print("PASS", "acl_memset_hbm", "HBM allocation materialized");

    int probe_fd = open(options.device_node.c_str(), O_RDWR | O_CLOEXEC);
    if (probe_fd < 0) {
        Print("FAIL", "open_probe_device",
              options.device_node + ": " + std::strerror(errno));
        std::cout << "RESULT hbm_export=NOT_TESTED nvme_device=NOT_TESTED"
                     " nvme_dma_api_map=NOT_TESTED peer_dma=NOT_PROVEN"
                     " physical_nvme_dma_to_hbm=NOT_PROVEN"
                  << std::endl;
        aclrtFree(hbm_buffer);
        aclrtResetDevice(options.device);
        aclFinalize();
        return 4;
    }
    Print("PASS", "open_probe_device", options.device_node);

    errno = 0;
    const int ioctl_status =
        ioctl(probe_fd, MOONCAKE_HBM_P2P_PROBE_IOCTL_RUN, &request);
    const int ioctl_error = errno;
    close(probe_fd);

    if (ioctl_status != 0) {
        Print("FAIL", "probe_ioctl",
              std::string(std::strerror(ioctl_error)) +
                  " errno=" + std::to_string(ioctl_error));
        std::cout << "RESULT hbm_export=NOT_TESTED nvme_device=NOT_TESTED"
                     " nvme_dma_api_map=NOT_TESTED peer_dma=NOT_PROVEN"
                     " physical_nvme_dma_to_hbm=NOT_PROVEN"
                  << std::endl;
        aclrtFree(hbm_buffer);
        aclrtResetDevice(options.device);
        aclFinalize();
        return 4;
    }
    Print("PASS", "probe_ioctl", "kernel probe returned results");

    const bool export_pass =
        request.get_pages_status == 0 && request.page_count > 0 &&
        request.page_size > 0 && request.first_pa != 0 &&
        request.all_pa_aligned != 0 && request.invalidated == 0;
    Print(export_pass ? "PASS" : "FAIL", "hbm_page_export",
          "status=" + ErrorDetail(request.get_pages_status) +
              " table_version=" +
              std::to_string(request.page_table_version) +
              " page_size=" + std::to_string(request.page_size) +
              " page_count=" + std::to_string(request.page_count) +
              " pa_runs=" + std::to_string(request.pa_run_count) +
              " first_pa=" + Hex(request.first_pa) +
              " last_pa=" + Hex(request.last_pa) +
              " invalidated=" + std::to_string(request.invalidated));

    bool nvme_pass = options.export_only;
    bool dma_map_pass = options.export_only;
    if (options.export_only) {
        Print("SKIP", "nvme_device", "--export-only requested");
        Print("SKIP", "nvme_dma_api_map", "--export-only requested");
    } else {
        nvme_pass = request.nvme_device_status == 0;
        Print(nvme_pass ? "PASS" : "FAIL", "nvme_device",
              "bdf=" + options.nvme_bdf +
                  " status=" + ErrorDetail(request.nvme_device_status) +
                  " vendor=" + Hex(request.nvme_vendor_id) +
                  " device=" + Hex(request.nvme_device_id) +
                  " class=" + Hex(request.nvme_class) +
                  " driver=" + request.nvme_driver +
                  " dma_mask=" + Hex(request.dma_mask) +
                  " coherent_dma_mask=" +
                  Hex(request.coherent_dma_mask));

        dma_map_pass =
            request.dma_map_status == 0 && request.page_count > 0 &&
            request.mapped_count == request.page_count;
        Print(dma_map_pass ? "PASS" : "FAIL", "nvme_dma_api_map",
              "status=" + ErrorDetail(request.dma_map_status) +
                  " mapped=" + std::to_string(request.mapped_count) +
                  "/" + std::to_string(request.page_count) +
                  " dma_runs=" + std::to_string(request.dma_run_count) +
                  " first_iova=" + Hex(request.first_dma) +
                  " last_iova=" + Hex(request.last_dma) +
                  " aligned=" +
                  std::to_string(request.all_dma_aligned));
    }

    const bool put_pass = request.put_pages_status == 0;
    Print(put_pass ? "PASS" : "FAIL", "hbm_page_release",
          "status=" + ErrorDetail(request.put_pages_status));
    Print("INFO", "proof_boundary",
          "dma_map_resource success creates an NVMe-device DMA/IOVA mapping; "
          "it does not submit PRP/SGL, transfer data, exclude bounce buffers, "
          "or prove physical PCIe P2P");

    aclrtFree(hbm_buffer);
    aclrtResetDevice(options.device);
    aclFinalize();

    const bool success = device_memory && export_pass && nvme_pass &&
                         dma_map_pass && put_pass;
    const char *level = options.export_only
                            ? (success ? "L1_HBM_PAGES_EXPORTED"
                                       : "L0_HBM_EXPORT_FAILED")
                            : (success ? "L1_5_NVME_DMA_API_MAPPED"
                                       : "L1_5_NOT_REACHED");
    std::cout << "RESULT hbm_export=" << (export_pass ? "PASS" : "FAIL")
              << " nvme_device="
              << (options.export_only ? "SKIP" : nvme_pass ? "PASS" : "FAIL")
              << " nvme_dma_api_map="
              << (options.export_only ? "SKIP"
                                      : dma_map_pass ? "PASS" : "FAIL")
              << " pages_released=" << (put_pass ? "PASS" : "FAIL")
              << " peer_dma=NOT_PROVEN nvme_prp_sgl=NOT_TESTED"
                 " physical_nvme_dma_to_hbm=NOT_PROVEN level="
              << level << std::endl;
    if (success) return 0;
    if (!export_pass) return 5;
    if (!put_pass) return 6;
    return 7;
}
