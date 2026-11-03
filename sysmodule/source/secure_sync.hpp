#pragma once
#include "../../protocol/provisioning.hpp"
#include "../../protocol/policy.hpp"
#include <stratosphere.hpp>

namespace nxpctl::sync {
    void Configure(const protocol::ProvisioningRequest& request);
    bool IsConfigured();
    bool IsEnrolled();
    void RunCycle(policy::PolicyStore& store, u64 uptime_seconds);
    void Clear();
}
