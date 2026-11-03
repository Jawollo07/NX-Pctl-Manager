// Read-only client for the NX-Pctl-Manager resident sysmodule.
// Copyright (C) 2026 Taylor. GPLv3-or-later (see LICENSE).
#include "sysmodule_client.hpp"

namespace nxpctl {
static_assert(offsetof(RuntimeStatus, policy_revision) == 16,
              "RuntimeStatus must match the CMIF output alignment");

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
    return rc;
}
} // namespace nxpctl
