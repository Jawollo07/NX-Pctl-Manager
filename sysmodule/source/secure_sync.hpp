#pragma once
#include "../../protocol/provisioning.hpp"
#include "../../protocol/policy.hpp"
#include <stratosphere.hpp>

namespace nxpctl::sync {
    bool Initialize();
    void Finalize();
    void Configure(const protocol::ProvisioningRequest& request);
    bool IsConfigured();
    bool IsEnrolled();
    void RunCycle(policy::PolicyStore& store, u64 uptime_seconds,
                  s64 usage_day_key, u64 usage_active_seconds);
    bool GetTrustedLocalDayKey(u64 monotonic_seconds, s64* out_day_key);
    void Clear();
}
