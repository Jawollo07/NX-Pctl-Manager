// Shared versioned wire contract for the NRO <-> sysmodule HIPC endpoint.
#pragma once

#include <cstddef>
#include <cstdint>

namespace nxpctl::protocol {

inline constexpr char ServiceName[] = "nxpctl";
inline constexpr std::uint32_t RuntimeStatusCommandId = 65000;
inline constexpr std::uint32_t RuntimeProtocolVersion = 1;

enum class WorkerState : std::uint32_t {
    Stopped = 0,
    Starting = 1,
    Running = 2,
    Stopping = 3,
};

struct alignas(8) RuntimeStatus {
    std::uint32_t protocol_version;
    std::uint32_t worker_state;
    std::uint32_t worker_heartbeat;
    std::uint32_t reserved;
    std::uint64_t policy_revision;
};

[[nodiscard]] constexpr bool IsKnownWorkerState(std::uint32_t state) noexcept {
    switch (static_cast<WorkerState>(state)) {
        case WorkerState::Stopped:
        case WorkerState::Starting:
        case WorkerState::Running:
        case WorkerState::Stopping:
            return true;
        default:
            return false;
    }
}

// A client must not interpret fields from a future or malformed wire contract.
[[nodiscard]] constexpr bool IsCompatibleRuntimeStatus(const RuntimeStatus& status) noexcept {
    return status.protocol_version == RuntimeProtocolVersion
        && IsKnownWorkerState(status.worker_state)
        && status.reserved == 0;
}

static_assert(offsetof(RuntimeStatus, protocol_version) == 0);
static_assert(offsetof(RuntimeStatus, worker_state) == 4);
static_assert(offsetof(RuntimeStatus, worker_heartbeat) == 8);
static_assert(offsetof(RuntimeStatus, reserved) == 12);
static_assert(offsetof(RuntimeStatus, policy_revision) == 16);
static_assert(sizeof(RuntimeStatus) == 24);

} // namespace nxpctl::protocol
