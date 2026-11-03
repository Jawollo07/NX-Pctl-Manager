// Read-only client for the NX-Pctl-Manager resident sysmodule.
// Copyright (C) 2026 Taylor. GPLv3-or-later (see LICENSE).
#include "sysmodule_client.hpp"

namespace nxpctl {
static_assert(offsetof(RuntimeStatus, worker_state) == 4,
              "RuntimeStatus worker_state CMIF offset changed");
static_assert(offsetof(RuntimeStatus, worker_heartbeat) == 8,
              "RuntimeStatus worker_heartbeat CMIF offset changed");
static_assert(offsetof(RuntimeStatus, worker_restart_count) == 12,
              "RuntimeStatus worker_restart_count CMIF offset changed");
static_assert(offsetof(RuntimeStatus, policy_revision) == 16,
              "RuntimeStatus policy_revision CMIF alignment changed");
static_assert(sizeof(RuntimeStatus) == 24,
              "RuntimeStatus CMIF output size changed");

Result get_runtime_status(RuntimeStatus* out) {
    if (out == nullptr)
        return MAKERESULT(Module_Libnx, LibnxError_BadInput);

    *out = {};
    Service service{};
    Result rc = smGetService(&service, protocol::ServiceName);
    if (R_FAILED(rc))
        return rc;

    // Command 65000 mirrors IRuntime::GetRuntimeStatus in the sysmodule.
    rc = serviceDispatchOut(&service, protocol::RuntimeStatusCommandId, *out);
    serviceClose(&service);
    if (R_FAILED(rc))
        *out = {};
    return rc;
}
} // namespace nxpctl
