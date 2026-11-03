#include "../protocol/runtime_status.hpp"
#include <cassert>
#include <cstring>
#include <iostream>
int main() {
    using namespace nxpctl::protocol;
    assert(std::strcmp(ServiceName, "nxpctl")==0);
    assert(RuntimeStatusCommandId==65000);
    assert(RuntimeProtocolVersion==1);
    RuntimeStatus value{};
    value.protocol_version=RuntimeProtocolVersion;
    value.worker_state=static_cast<unsigned>(WorkerState::Running);
    value.worker_heartbeat=42;
    value.policy_revision=7;
    const auto *bytes=reinterpret_cast<const unsigned char*>(&value);
    std::uint64_t revision=0;
    std::memcpy(&revision, bytes+16, sizeof(revision));
    assert(revision==7);
    std::cout << "runtime protocol ABI OK; size=" << sizeof(value)
              << "; policy_revision_offset=" << offsetof(RuntimeStatus, policy_revision) << '\n';
}
