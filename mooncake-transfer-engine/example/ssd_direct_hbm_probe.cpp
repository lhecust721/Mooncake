// Copyright 2026 KVCache.AI
// SPDX-License-Identifier: Apache-2.0

#include <acl/acl.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/statvfs.h>
#include <sys/uio.h>

#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <iostream>
#include <string>
#include <vector>

#ifdef MOONCAKE_DIRECT_PROBE_HAS_URING
#include <liburing.h>
#endif

#ifndef O_DIRECT
#define O_DIRECT 040000
#endif

namespace {

struct Options {
    std::string file;
    size_t size = 4U * 1024U * 1024U;
    off_t offset = 0;
    int device = 0;
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

void Emit(const std::string &test, const std::string &status, ssize_t bytes,
          int error, long long duration_us, bool content_match,
          const std::string &detail = {}) {
    std::cout << "{\"test\":\"" << Escape(test) << "\",\"status\":\""
              << Escape(status) << "\",\"bytes\":" << bytes
              << ",\"errno\":" << error << ",\"duration_us\":"
              << duration_us << ",\"content_match\":"
              << (content_match ? "true" : "false") << ",\"detail\":\""
              << Escape(detail) << "\"}" << std::endl;
}

void EmitMetadata(const Options &options, const void *device_buffer,
                  const aclrtPtrAttributes &attributes,
                  unsigned long filesystem_block_size,
                  size_t direct_alignment) {
    const auto address = reinterpret_cast<uintptr_t>(device_buffer);
    std::cout << "{\"type\":\"probe_metadata\",\"file\":\""
              << Escape(options.file) << "\",\"size\":" << options.size
              << ",\"offset\":" << options.offset
              << ",\"device_id\":" << options.device
              << ",\"device_address\":" << address
              << ",\"device_page_size\":" << attributes.pageSize
              << ",\"filesystem_block_size\":" << filesystem_block_size
              << ",\"staging_host_alignment\":" << direct_alignment
              << ",\"address_aligned_4k\":"
              << (address % 4096 == 0 ? "true" : "false")
              << ",\"size_aligned_4k\":"
              << (options.size % 4096 == 0 ? "true" : "false")
              << ",\"offset_aligned_4k\":"
              << (options.offset % 4096 == 0 ? "true" : "false")
              << ",\"size_aligned_io\":"
              << (options.size % direct_alignment == 0 ? "true" : "false")
              << ",\"offset_aligned_io\":"
              << (options.offset % direct_alignment == 0 ? "true" : "false")
              << "}"
              << std::endl;
}

bool Parse(int argc, char **argv, Options *options) {
    for (int i = 1; i < argc; ++i) {
        const std::string arg(argv[i]);
        if (arg == "--file" && i + 1 < argc) {
            options->file = argv[++i];
        } else if (arg == "--size" && i + 1 < argc) {
            options->size = std::stoull(argv[++i]);
        } else if (arg == "--offset" && i + 1 < argc) {
            options->offset = static_cast<off_t>(std::stoll(argv[++i]));
        } else if (arg == "--device" && i + 1 < argc) {
            options->device = std::stoi(argv[++i]);
        } else {
            std::cerr << "usage: " << argv[0]
                      << " --file PATH [--size BYTES] [--offset BYTES]"
                         " [--device ID]\n";
            return false;
        }
    }
    return !options->file.empty() && options->size > 0;
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

bool VerifyDevice(void *device_buffer, const std::vector<char> &expected,
                  size_t bytes) {
    std::vector<char> observed(bytes);
    const aclError result = aclrtMemcpy(observed.data(), observed.size(),
                                        device_buffer, bytes,
                                        ACL_MEMCPY_DEVICE_TO_HOST);
    return result == ACL_SUCCESS &&
           std::memcmp(observed.data(), expected.data(), bytes) == 0;
}

}  // namespace

int main(int argc, char **argv) {
    Options options;
    try {
        if (!Parse(argc, argv, &options)) return 2;
    } catch (const std::exception &error) {
        std::cerr << "invalid argument: " << error.what() << '\n';
        return 2;
    }

    const int buffered_fd = open(options.file.c_str(), O_RDONLY);
    if (buffered_fd < 0) {
        Emit("control_buffered_host", "setup_failed", -1, errno, 0, false,
             std::strerror(errno));
        return 1;
    }
    std::vector<char> expected(options.size);
    long long duration_us = 0;
    int io_error = 0;
    const ssize_t control_bytes =
        TimedPread(buffered_fd, expected.data(), expected.size(),
                   options.offset, &duration_us, &io_error);
    close(buffered_fd);
    Emit("control_buffered_host", control_bytes == (ssize_t)options.size
                                      ? "verified"
                                      : "failed",
         control_bytes, io_error, duration_us,
         control_bytes == (ssize_t)options.size,
         control_bytes < 0 ? std::strerror(io_error) : "");
    if (control_bytes != (ssize_t)options.size) return 1;

    aclError acl_result = aclInit(nullptr);
    if (acl_result != ACL_SUCCESS) {
        Emit("acl_init", "setup_failed", 0, 0, 0, false,
             std::to_string(acl_result));
        return 1;
    }
    acl_result = aclrtSetDevice(options.device);
    if (acl_result != ACL_SUCCESS) {
        Emit("acl_set_device", "setup_failed", 0, 0, 0, false,
             std::to_string(acl_result));
        aclFinalize();
        return 1;
    }
    void *device_buffer = nullptr;
    acl_result = aclrtMalloc(&device_buffer, options.size,
                            ACL_MEM_MALLOC_HUGE_FIRST);
    if (acl_result != ACL_SUCCESS || device_buffer == nullptr) {
        Emit("acl_malloc_hbm", "setup_failed", 0, 0, 0, false,
             std::to_string(acl_result));
        aclrtResetDevice(options.device);
        aclFinalize();
        return 1;
    }

    aclrtPtrAttributes attributes{};
    acl_result = aclrtPointerGetAttributes(device_buffer, &attributes);
    const bool is_device =
        acl_result == ACL_SUCCESS &&
        attributes.location.type == ACL_MEM_LOCATION_TYPE_DEVICE;
    Emit("acl_pointer_attributes", is_device ? "verified" : "failed", 0, 0,
         0, is_device,
         "acl_result=" + std::to_string(acl_result) +
             ",location_type=" +
             std::to_string(static_cast<int>(attributes.location.type)) +
             ",device_id=" + std::to_string(attributes.location.id));
    struct statvfs filesystem_stats {};
    const unsigned long filesystem_block_size =
        statvfs(options.file.c_str(), &filesystem_stats) == 0
            ? filesystem_stats.f_bsize
            : 0;
    size_t direct_alignment = 4096;
    while (direct_alignment < filesystem_block_size) direct_alignment *= 2;
    EmitMetadata(options, device_buffer, attributes, filesystem_block_size,
                 direct_alignment);

    bool buffered_verified = false;
    int fd = open(options.file.c_str(), O_RDONLY);
    if (fd >= 0) {
        const ssize_t bytes = TimedPread(fd, device_buffer, options.size,
                                         options.offset, &duration_us,
                                         &io_error);
        buffered_verified = is_device && bytes == (ssize_t)options.size &&
                            VerifyDevice(device_buffer, expected, bytes);
        Emit("buffered_pread_to_hbm",
             buffered_verified ? "verified" : "unsupported_or_failed", bytes,
             io_error, duration_us, buffered_verified,
             bytes < 0 ? std::strerror(io_error) : "");
        close(fd);
    } else {
        Emit("buffered_pread_to_hbm", "open_failed", -1, errno, 0, false,
             std::strerror(errno));
    }

    bool direct_verified = false;
    bool uring_verified = false;
    bool staging_control_verified = false;
    fd = open(options.file.c_str(), O_RDONLY | O_DIRECT);
    if (fd >= 0) {
        void *aligned_host = nullptr;
        const int allocation_result =
            posix_memalign(&aligned_host, direct_alignment, options.size);
        if (allocation_result == 0 && aligned_host != nullptr) {
            const ssize_t host_bytes =
                TimedPread(fd, aligned_host, options.size, options.offset,
                           &duration_us, &io_error);
            Emit("staging_control_ssd_to_host",
                 host_bytes == (ssize_t)options.size ? "verified" : "failed",
                 host_bytes, io_error, duration_us,
                 host_bytes == (ssize_t)options.size,
                 host_bytes < 0 ? std::strerror(io_error) : "");
            if (host_bytes == (ssize_t)options.size) {
                const auto copy_start = std::chrono::steady_clock::now();
                const aclError copy_result = aclrtMemcpy(
                    device_buffer, options.size, aligned_host, options.size,
                    ACL_MEMCPY_HOST_TO_DEVICE);
                const auto copy_end = std::chrono::steady_clock::now();
                const auto copy_us =
                    std::chrono::duration_cast<std::chrono::microseconds>(
                        copy_end - copy_start)
                        .count();
                staging_control_verified =
                    copy_result == ACL_SUCCESS &&
                    VerifyDevice(device_buffer, expected, options.size);
                Emit("staging_control_host_to_hbm",
                     staging_control_verified ? "verified" : "failed",
                     copy_result == ACL_SUCCESS
                         ? static_cast<ssize_t>(options.size)
                         : -1,
                     0, copy_us, staging_control_verified,
                     "acl_result=" + std::to_string(copy_result));
                const auto staging_total_us = duration_us + copy_us;
                Emit("staging_control_total",
                     staging_control_verified ? "verified" : "failed",
                     staging_control_verified
                         ? static_cast<ssize_t>(options.size)
                         : -1,
                     0, staging_total_us, staging_control_verified,
                     "aligned pageable host baseline; not Mooncake FabricMem");
            }
            free(aligned_host);
        } else {
            Emit("staging_control_total", "allocation_failed", -1,
                 allocation_result, 0, false,
                 "posix_memalign direct-I/O alignment failed");
        }

        const ssize_t bytes = TimedPread(fd, device_buffer, options.size,
                                         options.offset, &duration_us,
                                         &io_error);
        direct_verified = is_device && bytes == (ssize_t)options.size &&
                          VerifyDevice(device_buffer, expected, bytes);
        Emit("odirect_pread_to_hbm",
             direct_verified ? "verified" : "unsupported_or_failed", bytes,
             io_error, duration_us, direct_verified,
             bytes < 0 ? std::strerror(io_error) : "");

#ifdef MOONCAKE_DIRECT_PROBE_HAS_URING
        io_uring ring{};
        int uring_result = io_uring_queue_init(2, &ring, 0);
        if (uring_result == 0) {
            iovec iov{device_buffer, options.size};
            uring_result = io_uring_register_buffers(&ring, &iov, 1);
            if (uring_result == 0) {
                io_uring_sqe *sqe = io_uring_get_sqe(&ring);
                io_uring_prep_read_fixed(sqe, fd, device_buffer, options.size,
                                         options.offset, 0);
                const auto start = std::chrono::steady_clock::now();
                uring_result = io_uring_submit(&ring);
                io_uring_cqe *cqe = nullptr;
                if (uring_result >= 0)
                    uring_result = io_uring_wait_cqe(&ring, &cqe);
                int completion = uring_result;
                if (uring_result == 0 && cqe != nullptr) {
                    completion = cqe->res;
                    io_uring_cqe_seen(&ring, cqe);
                }
                duration_us = std::chrono::duration_cast<std::chrono::microseconds>(
                                  std::chrono::steady_clock::now() - start)
                                  .count();
                uring_verified = is_device && completion == (int)options.size &&
                                 VerifyDevice(device_buffer, expected,
                                              options.size);
                Emit("io_uring_fixed_odirect_to_hbm",
                     uring_verified ? "verified" : "unsupported_or_failed",
                     completion, completion < 0 ? -completion : 0, duration_us,
                     uring_verified);
                io_uring_unregister_buffers(&ring);
            } else {
                Emit("io_uring_fixed_odirect_to_hbm", "register_failed", -1,
                     -uring_result, 0, false);
            }
            io_uring_queue_exit(&ring);
        } else {
            Emit("io_uring_fixed_odirect_to_hbm", "queue_init_failed", -1,
                 -uring_result, 0, false);
        }
#else
        Emit("io_uring_fixed_odirect_to_hbm", "not_built", 0, 0, 0, false,
             "liburing was not found at configure time");
#endif
        close(fd);
    } else {
        Emit("odirect_pread_to_hbm", "open_failed", -1, errno, 0, false,
             std::strerror(errno));
    }

    const bool any_posix_to_hbm =
        buffered_verified || direct_verified || uring_verified;
    const bool odirect_to_hbm = direct_verified || uring_verified;
    const char *validation_level =
        odirect_to_hbm ? "L1_ODIRECT_ACCEPTED"
                       : is_device ? "L0_HBM_IDENTIFIED" : "L0_NOT_REACHED";
    std::cout << "{\"summary\":true,\"validation_level\":\""
              << validation_level << "\",\"kernel_io_to_hbm_verified\":"
              << (any_posix_to_hbm ? "true" : "false")
              << ",\"buffered_posix_to_hbm_verified\":"
              << (buffered_verified ? "true" : "false")
              << ",\"odirect_to_hbm_verified\":"
              << (odirect_to_hbm ? "true" : "false")
              << ",\"staging_control_verified\":"
              << (staging_control_verified ? "true" : "false")
              << ",\"peer_dma_mapping_proven\":false"
              << ",\"physical_dma_direct_proven\":false,"
                 "\"proof_note\":\"Successful content verification proves "
                 "kernel I/O accepted the HBM address; it does not prove the "
                 "absence of driver-internal staging or physical NVMe DMA to "
                 "HBM. Correlate I/O destination, driver behavior, and runtime "
                 "profiling.\"}"
              << std::endl;
    std::cerr << "RESULT kernel_io_to_hbm="
              << (any_posix_to_hbm ? "VERIFIED" : "NOT_VERIFIED")
              << " odirect_to_hbm="
              << (odirect_to_hbm ? "VERIFIED" : "NOT_VERIFIED")
              << " staging_control="
              << (staging_control_verified ? "VERIFIED" : "NOT_VERIFIED")
              << " level=" << validation_level
              << " peer_dma_mapping=NOT_PROVEN"
                 " physical_nvme_dma_to_hbm=NOT_PROVEN"
              << std::endl;

    aclrtFree(device_buffer);
    aclrtResetDevice(options.device);
    aclFinalize();
    return 0;
}
