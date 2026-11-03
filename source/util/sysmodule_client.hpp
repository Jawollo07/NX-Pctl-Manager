// Read-only client for the NX-Pctl-Manager resident sysmodule.
// Copyright (C) 2026 Taylor. GPLv3-or-later (see LICENSE).
#pragma once
#include <switch.h>
#include <cstdint>

namespace nxpctl {
struct RuntimeStatus {
    std::uint32_t protocol_version;
    std::uint32_t worker_state;
    std::uint32_t worker_heartbeat;
    std::uint64_t policy_revision;
};
Result get_runtime_status(RuntimeStatus* out);
} // namespace nxpctl
