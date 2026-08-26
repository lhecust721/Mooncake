// Copyright 2026 KVCache.AI
// SPDX-License-Identifier: Apache-2.0

#include <acl/acl.h>
#include <dirent.h>
#include <fcntl.h>
#include <linux/fs.h>
#include <linux/nvme_ioctl.h>
#include <limits.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>

#include <chrono>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <exception>
#include <fstream>
#include <iostream>
#include <limits>
#include <sstream>
#include <string>
#include <vector>

#include "hbm_p2p_probe_uapi.h"

namespace {

constexpr uint8_t kNvmeReadOpcode = 0x02;
constexpr size_t kMaximumProbeSize = 64U * 1024U * 1024U;

struct Options {
    std::string namespace_path;
    std::string nvme_bdf;
    std::string probe_device = "/dev/mooncake_hbm_p2p_probe";
    uint64_t slba = 0;
    size_t size = 4096U;
    uint32_t timeout_ms = 30000;
    int device = 0;
    bool slba_set = false;
    bool allow_mounted_read = false;
};

std::string Escape(const std::string &value) {
    std::string escaped;
    for (const char ch : value) {
        if (ch == '\\' || ch == '"') escaped.push_back('\\');
        if (ch == '\n') {
            escaped += "\\n";
        } else {
            escaped.push_back(ch);
        }
    }
    return escaped;
}

void Emit(const std::string &test, const std::string &status, int error,
          long long duration_us, bool content_match,
          const std::string &detail = {}) {
    std::cout << "{\"test\":\"" << Escape(test) << "\",\"status\":\""
              << Escape(status) << "\",\"errno\":" << error
              << ",\"duration_us\":" << duration_us
              << ",\"content_match\":"
              << (content_match ? "true" : "false") << ",\"detail\":\""
              << Escape(detail) << "\"}" << std::endl;
}

void Usage(const char *program) {
    std::cerr << "usage: " << program
              << " --namespace /dev/nvmeXnY --nvme-bdf DDDD:BB:DD.F"
                 " --slba LBA"
                 " [--size BYTES] [--device ID] [--timeout-ms MS]"
                 " [--probe-device PATH] [--allow-mounted-read]\n";
}

bool Parse(int argc, char **argv, Options *options) {
    for (int i = 1; i < argc; ++i) {
        const std::string argument(argv[i]);
        if (argument == "--namespace" && i + 1 < argc) {
            options->namespace_path = argv[++i];
        } else if (argument == "--nvme-bdf" && i + 1 < argc) {
            options->nvme_bdf = argv[++i];
        } else if (argument == "--probe-device" && i + 1 < argc) {
            options->probe_device = argv[++i];
        } else if (argument == "--slba" && i + 1 < argc) {
            options->slba = std::stoull(argv[++i], nullptr, 0);
            options->slba_set = true;
        } else if (argument == "--size" && i + 1 < argc) {
            options->size = std::stoull(argv[++i], nullptr, 0);
        } else if (argument == "--device" && i + 1 < argc) {
            options->device = std::stoi(argv[++i]);
        } else if (argument == "--timeout-ms" && i + 1 < argc) {
            options->timeout_ms = std::stoul(argv[++i], nullptr, 0);
        } else if (argument == "--allow-mounted-read") {
            options->allow_mounted_read = true;
        } else {
            return false;
        }
    }
    return !options->namespace_path.empty() && !options->nvme_bdf.empty() &&
           options->slba_set &&
           options->size > 0 && options->size <= kMaximumProbeSize &&
           options->size <= std::numeric_limits<uint32_t>::max() &&
           options->timeout_ms > 0;
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

bool ResolvePath(const std::string &path, std::string *resolved) {
    char buffer[PATH_MAX]{};
    if (realpath(path.c_str(), buffer) == nullptr) return false;
    *resolved = buffer;
    return true;
}

bool DirectoryHasEntries(const std::string &path) {
    DIR *directory = opendir(path.c_str());
    if (directory == nullptr) return false;
    bool has_entries = false;
    while (const dirent *entry = readdir(directory)) {
        if (std::strcmp(entry->d_name, ".") != 0 &&
            std::strcmp(entry->d_name, "..") != 0) {
            has_entries = true;
            break;
        }
    }
    closedir(directory);
    return has_entries;
}

bool FindMount(dev_t device, std::string *mount_point) {
    std::ifstream mounts("/proc/self/mountinfo");
    std::string line;
    const std::string expected = std::to_string(major(device)) + ":" +
                                 std::to_string(minor(device));
    const std::string namespace_link = "/sys/dev/block/" + expected;
    std::string namespace_sysfs;
    if (!ResolvePath(namespace_link, &namespace_sysfs)) return false;
    while (std::getline(mounts, line)) {
        std::istringstream fields(line);
        std::string mount_id;
        std::string parent_id;
        std::string major_minor;
        std::string root;
        std::string current_mount_point;
        if (fields >> mount_id >> parent_id >> major_minor >> root >>
                current_mount_point) {
            const std::string mount_link = "/sys/dev/block/" + major_minor;
            std::string mount_sysfs;
            if (!ResolvePath(mount_link, &mount_sysfs)) continue;
            if (mount_sysfs == namespace_sysfs ||
                (mount_sysfs.size() > namespace_sysfs.size() &&
                 mount_sysfs.compare(0, namespace_sysfs.size(),
                                     namespace_sysfs) == 0 &&
                 mount_sysfs[namespace_sysfs.size()] == '/')) {
                *mount_point = current_mount_point;
                return true;
            }
        }
    }
    return false;
}

ssize_t TimedPread(int fd, void *buffer, size_t size, off_t offset,
                   long long *duration_us, int *error) {
    errno = 0;
    const auto start = std::chrono::steady_clock::now();
    const ssize_t result = pread(fd, buffer, size, offset);
    const auto end = std::chrono::steady_clock::now();
    *duration_us = std::chrono::duration_cast<std::chrono::microseconds>(
                       end - start)
                       .count();
    *error = result < 0 ? errno : 0;
    return result;
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
        Emit("arguments", "failed", EINVAL, 0, false, error.what());
        Usage(argv[0]);
        return 2;
    }

    struct stat namespace_stat {};
    if (stat(options.namespace_path.c_str(), &namespace_stat) != 0) {
        Emit("namespace", "failed", errno, 0, false,
             "cannot stat namespace block device");
        return 3;
    }
    if (!S_ISBLK(namespace_stat.st_mode)) {
        Emit("namespace", "failed", ENOTBLK, 0, false,
             "path must be an NVMe namespace block device");
        return 3;
    }
    const std::string namespace_major_minor =
        std::to_string(major(namespace_stat.st_rdev)) + ":" +
        std::to_string(minor(namespace_stat.st_rdev));
    const std::string namespace_sysfs_link =
        "/sys/dev/block/" + namespace_major_minor;
    if (access((namespace_sysfs_link + "/partition").c_str(), F_OK) == 0) {
        Emit("namespace", "failed", EINVAL, 0, false,
             "partitions are not accepted; pass the whole NVMe namespace so "
             "SLBA has unambiguous namespace-relative semantics");
        return 3;
    }
    mooncake_hbm_p2p_probe_request bdf_check{};
    if (!ParseBdf(options.nvme_bdf, &bdf_check)) {
        Emit("nvme_bdf", "failed", EINVAL, 0, false,
             "expected DDDD:BB:DD.F");
        return 2;
    }
    std::string namespace_sysfs;
    std::string pci_sysfs;
    if (!ResolvePath(namespace_sysfs_link, &namespace_sysfs) ||
        !ResolvePath("/sys/bus/pci/devices/" + options.nvme_bdf, &pci_sysfs) ||
        namespace_sysfs.size() <= pci_sysfs.size() ||
        namespace_sysfs.compare(0, pci_sysfs.size(), pci_sysfs) != 0 ||
        namespace_sysfs[pci_sysfs.size()] != '/') {
        Emit("namespace_bdf_match", "failed", EXDEV, 0, false,
             "namespace does not belong to the requested NVMe PCI BDF");
        return 3;
    }
    Emit("namespace_bdf_match", "verified", 0, 0, true,
         options.namespace_path + " belongs to " + options.nvme_bdf);
    const bool has_holders =
        DirectoryHasEntries(namespace_sysfs_link + "/holders");
    if (has_holders && !options.allow_mounted_read) {
        Emit("namespace_holder_guard", "refused", EBUSY, 0, false,
             "namespace has device-mapper/RAID holders; use an isolated "
             "namespace or explicitly pass --allow-mounted-read");
        return 3;
    }
    Emit("namespace_holder_guard",
         has_holders ? "explicitly_allowed" : "verified", 0, 0, true,
         has_holders ? "namespace has holders" : "no holders");
    std::string mount_point;
    const bool mounted = FindMount(namespace_stat.st_rdev, &mount_point);
    if (mounted && !options.allow_mounted_read) {
        Emit("mounted_namespace_guard", "refused", EBUSY, 0, false,
             "namespace is mounted at " + mount_point +
                 "; use an unmounted test namespace or explicitly pass "
                 "--allow-mounted-read for the fixed read-only opcode");
        return 3;
    }
    Emit("mounted_namespace_guard", mounted ? "explicitly_allowed" : "verified",
         0, 0, true, mounted ? "mounted at " + mount_point : "not mounted");

    const int namespace_fd =
        open(options.namespace_path.c_str(), O_RDONLY | O_CLOEXEC);
    if (namespace_fd < 0) {
        Emit("open_namespace", "failed", errno, 0, false,
             std::strerror(errno));
        return 3;
    }

    int logical_block_size = 0;
    uint64_t capacity_bytes = 0;
    if (ioctl(namespace_fd, BLKSSZGET, &logical_block_size) != 0 ||
        ioctl(namespace_fd, BLKGETSIZE64, &capacity_bytes) != 0 ||
        logical_block_size <= 0) {
        Emit("namespace_geometry", "failed", errno, 0, false,
             std::strerror(errno));
        close(namespace_fd);
        return 3;
    }
    if (options.size % static_cast<size_t>(logical_block_size) != 0) {
        Emit("namespace_geometry", "failed", EINVAL, 0, false,
             "size must be a multiple of the namespace logical block size");
        close(namespace_fd);
        return 2;
    }
    const uint64_t block_count = options.size / logical_block_size;
    if (block_count == 0 || block_count > 65536 ||
        options.slba > capacity_bytes / logical_block_size ||
        block_count > capacity_bytes / logical_block_size - options.slba) {
        Emit("namespace_geometry", "failed", ERANGE, 0, false,
             "requested LBA range is invalid or exceeds one NVMe command");
        close(namespace_fd);
        return 2;
    }
    const uint64_t byte_offset = options.slba * logical_block_size;

    errno = 0;
    const int namespace_id = ioctl(namespace_fd, NVME_IOCTL_ID);
    if (namespace_id <= 0) {
        Emit("nvme_namespace_id", "failed", errno ? errno : ENODEV, 0,
             false, "NVME_IOCTL_ID did not return a namespace ID");
        close(namespace_fd);
        return 3;
    }
    std::cout << "{\"type\":\"l2_probe_metadata\",\"namespace\":\""
              << Escape(options.namespace_path) << "\",\"namespace_id\":"
              << namespace_id << ",\"slba\":" << options.slba
              << ",\"block_count\":" << block_count
              << ",\"logical_block_size\":" << logical_block_size
              << ",\"size\":" << options.size
              << ",\"mounted\":" << (mounted ? "true" : "false")
              << ",\"device_id\":" << options.device << "}" << std::endl;

    std::vector<char> expected(options.size);
    long long reference_us = 0;
    int reference_error = 0;
    const ssize_t reference_bytes =
        TimedPread(namespace_fd, expected.data(), expected.size(),
                   static_cast<off_t>(byte_offset), &reference_us,
                   &reference_error);
    Emit("control_namespace_pread",
         reference_bytes == static_cast<ssize_t>(options.size) ? "verified"
                                                               : "failed",
         reference_error, reference_us,
         reference_bytes == static_cast<ssize_t>(options.size),
         reference_bytes < 0 ? std::strerror(reference_error) : "");
    if (reference_bytes != static_cast<ssize_t>(options.size)) {
        close(namespace_fd);
        return 3;
    }

    aclError acl_status = aclInit(nullptr);
    if (acl_status != ACL_SUCCESS) {
        Emit("acl_init", "failed", 0, 0, false,
             std::to_string(acl_status));
        close(namespace_fd);
        return 4;
    }
    acl_status = aclrtSetDevice(options.device);
    if (acl_status != ACL_SUCCESS) {
        Emit("acl_set_device", "failed", 0, 0, false,
             std::to_string(acl_status));
        aclFinalize();
        close(namespace_fd);
        return 4;
    }
    void *hbm_buffer = nullptr;
    acl_status =
        aclrtMalloc(&hbm_buffer, options.size, ACL_MEM_MALLOC_HUGE_FIRST);
    if (acl_status != ACL_SUCCESS || hbm_buffer == nullptr) {
        Emit("acl_malloc_hbm", "failed", 0, 0, false,
             std::to_string(acl_status));
        aclrtResetDevice(options.device);
        aclFinalize();
        close(namespace_fd);
        return 4;
    }
    aclrtPtrAttributes attributes{};
    acl_status = aclrtPointerGetAttributes(hbm_buffer, &attributes);
    const bool device_memory =
        acl_status == ACL_SUCCESS &&
        attributes.location.type == ACL_MEM_LOCATION_TYPE_DEVICE;
    Emit("acl_pointer_attributes", device_memory ? "verified" : "failed", 0,
         0, device_memory,
         "acl_status=" + std::to_string(acl_status) +
             ",location_type=" +
             std::to_string(static_cast<int>(attributes.location.type)) +
             ",device_id=" + std::to_string(attributes.location.id) +
             ",page_size=" + std::to_string(attributes.pageSize));
    if (!device_memory ||
        aclrtMemset(hbm_buffer, options.size, 0xA5, options.size) !=
            ACL_SUCCESS) {
        Emit("hbm_precondition", "failed", EFAULT, 0, false,
             "HBM pointer validation or initialization failed");
        aclrtFree(hbm_buffer);
        aclrtResetDevice(options.device);
        aclFinalize();
        close(namespace_fd);
        return 4;
    }

    mooncake_hbm_p2p_probe_request mapping_request{};
    mapping_request.abi_version = MOONCAKE_HBM_P2P_PROBE_ABI_VERSION;
    mapping_request.flags = MOONCAKE_HBM_P2P_FLAG_MAP_NVME;
    mapping_request.hbm_va = reinterpret_cast<uintptr_t>(hbm_buffer);
    mapping_request.hbm_size = options.size;
    if (!ParseBdf(options.nvme_bdf, &mapping_request)) {
        Emit("nvme_bdf", "failed", EINVAL, 0, false,
             "expected DDDD:BB:DD.F");
        aclrtFree(hbm_buffer);
        aclrtResetDevice(options.device);
        aclFinalize();
        close(namespace_fd);
        return 2;
    }
    const int probe_fd = open(options.probe_device.c_str(), O_RDWR | O_CLOEXEC);
    if (probe_fd < 0) {
        Emit("hbm_nvme_dma_preflight", "failed", errno, 0, false,
             options.probe_device + ": " + std::strerror(errno));
        aclrtFree(hbm_buffer);
        aclrtResetDevice(options.device);
        aclFinalize();
        close(namespace_fd);
        return 5;
    }
    errno = 0;
    const int mapping_ioctl_status =
        ioctl(probe_fd, MOONCAKE_HBM_P2P_PROBE_IOCTL_RUN, &mapping_request);
    const int mapping_ioctl_error = errno;
    close(probe_fd);
    const bool mapping_verified =
        mapping_ioctl_status == 0 && mapping_request.get_pages_status == 0 &&
        mapping_request.nvme_device_status == 0 &&
        mapping_request.dma_map_status == 0 &&
        mapping_request.mapped_count == mapping_request.page_count &&
        mapping_request.put_pages_status == 0 &&
        mapping_request.invalidated == 0;
    Emit("hbm_nvme_dma_preflight",
         mapping_verified ? "verified" : "failed",
         mapping_ioctl_status < 0 ? mapping_ioctl_error : 0, 0,
         mapping_verified,
         "get_pages_status=" +
             std::to_string(mapping_request.get_pages_status) +
             ",dma_map_status=" +
             std::to_string(mapping_request.dma_map_status) +
             ",put_pages_status=" +
             std::to_string(mapping_request.put_pages_status) +
             ",pages=" + std::to_string(mapping_request.page_count) +
             ",mapped=" + std::to_string(mapping_request.mapped_count) +
             ",pa_runs=" + std::to_string(mapping_request.pa_run_count) +
             ",dma_runs=" + std::to_string(mapping_request.dma_run_count));
    if (!mapping_verified) {
        aclrtFree(hbm_buffer);
        aclrtResetDevice(options.device);
        aclFinalize();
        close(namespace_fd);
        return 5;
    }

    struct nvme_passthru_cmd command {};
    command.opcode = kNvmeReadOpcode;
    command.nsid = namespace_id;
    command.addr = reinterpret_cast<uintptr_t>(hbm_buffer);
    command.data_len = static_cast<uint32_t>(options.size);
    command.cdw10 = static_cast<uint32_t>(options.slba);
    command.cdw11 = static_cast<uint32_t>(options.slba >> 32);
    command.cdw12 = static_cast<uint32_t>(block_count - 1);
    command.timeout_ms = options.timeout_ms;

    errno = 0;
    const auto command_start = std::chrono::steady_clock::now();
    const int command_status = ioctl(namespace_fd, NVME_IOCTL_IO_CMD, &command);
    const int command_errno = command_status < 0 ? errno : 0;
    const auto command_us =
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - command_start)
            .count();

    bool content_match = false;
    aclError copy_status = ACL_SUCCESS;
    if (command_status == 0) {
        std::vector<char> observed(options.size);
        copy_status = aclrtMemcpy(observed.data(), observed.size(), hbm_buffer,
                                 options.size, ACL_MEMCPY_DEVICE_TO_HOST);
        content_match = copy_status == ACL_SUCCESS &&
                        std::memcmp(observed.data(), expected.data(),
                                    options.size) == 0;
    }
    std::string command_detail =
        "ioctl_status=" + std::to_string(command_status) +
        ",nvme_result=" + std::to_string(command.result) +
        ",acl_copy_status=" + std::to_string(copy_status);
    if (command_status < 0)
        command_detail += ",error=" + std::string(std::strerror(command_errno));
    Emit("nvme_ioctl_read_to_hbm",
         command_status == 0 && content_match ? "verified"
                                              : "unsupported_or_failed",
         command_errno, command_us, content_match, command_detail);

    const bool verified = command_status == 0 && content_match;
    const bool user_mapping_blocked = command_status < 0 &&
                                      (command_errno == EFAULT ||
                                       command_errno == EOPNOTSUPP);
    const char *level = verified
                            ? "L2_CANDIDATE_NVME_READ_VERIFIED"
                            : user_mapping_blocked
                                  ? "L2_BLOCKED_USER_BUFFER_MAPPING"
                                  : "L2_CANDIDATE_NOT_VERIFIED";
    std::cout
        << "{\"summary\":true,\"validation_level\":\"" << level
        << "\",\"nvme_read_to_hbm_content_verified\":"
        << (verified ? "true" : "false")
        << ",\"same_hbm_nvme_dma_preflight_verified\":true"
        << ",\"user_buffer_mapping_blocked\":"
        << (user_mapping_blocked ? "true" : "false")
        << ",\"physical_nvme_dma_to_hbm_proven\":false,"
           "\"proof_note\":\"Content verification proves that the NVMe "
           "passthrough read accepted the HBM address and produced the "
           "expected bytes. Without direct/fallback counters and PCIe/IOMMU "
           "tracing it does not prove physical P2P or exclude internal "
           "staging.\"}"
        << std::endl;
    std::cerr << "RESULT nvme_read_to_hbm="
              << (verified ? "VERIFIED" : "NOT_VERIFIED")
              << " content_match=" << (content_match ? "PASS" : "FAIL")
              << " dma_preflight=PASS"
              << " errno=" << command_errno << " level=" << level
              << " physical_nvme_dma_to_hbm=NOT_PROVEN" << std::endl;

    aclrtFree(hbm_buffer);
    aclrtResetDevice(options.device);
    aclFinalize();
    close(namespace_fd);
    return verified ? 0 : 8;
}
