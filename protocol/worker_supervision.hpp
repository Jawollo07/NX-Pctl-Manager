// Horizon-independent bounded worker restart policy.
#pragma once

#include <cstdint>

namespace nxpctl::protocol {

inline constexpr std::uint32_t MaxWorkerRestarts = 3;

struct WorkerRestartBudget {
    std::uint32_t restart_count = 0;

    [[nodiscard]] constexpr bool TryConsume() noexcept {
        if (restart_count >= MaxWorkerRestarts) {
            return false;
        }
        ++restart_count;
        return true;
    }
};

} // namespace nxpctl::protocol
