#pragma once

#include <unistd.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <string>
#include <string_view>
#include <thread>

#include <glog/logging.h>

namespace mooncake::offload_perf {

using Clock = std::chrono::steady_clock;

inline bool Enabled() {
    static const bool enabled = [] {
        const char* value = std::getenv("MOONCAKE_OFFLOAD_PERF_TRACE");
        if (value == nullptr) return false;
        const std::string_view parsed(value);
        return parsed == "1" || parsed == "true" || parsed == "TRUE" ||
               parsed == "on" || parsed == "ON";
    }();
    return enabled;
}

inline void LogDuration(std::string_view stage, uint64_t batch_id,
                        size_t key_count, uint64_t bytes,
                        Clock::time_point start, Clock::time_point end,
                        bool success, int error_code,
                        std::string_view endpoint = {}) {
    if (!Enabled()) return;

    const auto duration_us =
        std::chrono::duration_cast<std::chrono::microseconds>(end - start)
            .count();
    const auto end_steady_ns =
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            end.time_since_epoch())
            .count();
    const auto thread_id =
        std::hash<std::thread::id>{}(std::this_thread::get_id());

    LOG(INFO) << "MOONCAKE_OFFLOAD_PERF"
              << " stage=" << stage << " batch_id=" << batch_id
              << " keys=" << key_count << " bytes=" << bytes
              << " duration_us=" << duration_us
              << " end_steady_ns=" << end_steady_ns
              << " process_id=" << getpid()
              << " thread_id=" << thread_id
              << " success=" << (success ? 1 : 0)
              << " error_code=" << error_code
              << " endpoint=" << (endpoint.empty() ? "-" : endpoint);
}

}  // namespace mooncake::offload_perf
