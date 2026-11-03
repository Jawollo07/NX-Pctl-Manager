#include "../protocol/runtime_status.hpp"

#include <cassert>
#include <cstring>
#include <iostream>

int main() {
    using namespace nxpctl::protocol;

    static_assert(RuntimeStatusCommandId == 65000);
    static_assert(RuntimeProtocolVersion == 1);
    static_assert(offsetof(RuntimeStatus, reserved) == 12);
    static_assert(IsKnownWorkerState(static_cast<std::uint32_t>(WorkerState::Stopped)));
    static_assert(IsKnownWorkerState(static_cast<std::uint32_t>(WorkerState::Starting)));
    static_assert(IsKnownWorkerState(static_cast<std::uint32_t>(WorkerState::Running)));
    static_assert(IsKnownWorkerState(static_cast<std::uint32_t>(WorkerState::Stopping)));
    static_assert(!IsKnownWorkerState(4));

    assert(std::strcmp(ServiceName, "nxpctl") == 0);

    RuntimeStatus value{};
    value.protocol_version = RuntimeProtocolVersion;
    value.worker_state = static_cast<std::uint32_t>(WorkerState::Running);
    value.worker_heartbeat = 42;
    value.policy_revision = 7;

    assert(IsCompatibleRuntimeStatus(value));

    const auto* bytes = reinterpret_cast<const unsigned char*>(&value);
    std::uint64_t revision = 0;
    std::memcpy(&revision, bytes + 16, sizeof(revision));
    assert(revision == 7);

    RuntimeStatus invalid = value;
    invalid.protocol_version = RuntimeProtocolVersion + 1;
    assert(!IsCompatibleRuntimeStatus(invalid));

    invalid = value;
    invalid.worker_state = 0xFFFFFFFFu;
    assert(!IsCompatibleRuntimeStatus(invalid));

    invalid = value;
    invalid.reserved = 1;
    assert(!IsCompatibleRuntimeStatus(invalid));

    std::cout << "runtime protocol ABI and validation OK; size=" << sizeof(value)
              << "; policy_revision_offset=" << offsetof(RuntimeStatus, policy_revision)
              << '\n';
}
