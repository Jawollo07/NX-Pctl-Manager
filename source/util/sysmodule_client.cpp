// Read-only client for the NX-Pctl-Manager resident sysmodule.
// Copyright (C) 2026 Taylor. GPLv3-or-later (see LICENSE).
#include "sysmodule_client.hpp"
#include <cstddef>

namespace nxpctl {
static_assert(offsetof(RuntimeStatus, policy_revision) == 16,
              "RuntimeStatus must match the CMIF output alignment");

Result get_runtime_status(RuntimeStatus* out) {
    if (out == nullptr)
        return MAKERESULT(Module_Libnx, LibnxError_BadInput);

    *out = {};
    Service service{};
    Result rc = smGetService(&service, "nxpctl");
    if (R_FAILED(rc))
        return rc;

    // Command 65000 mirrors IRuntime::GetRuntimeStatus in the sysmodule.
    rc = serviceDispatchOut(&service, 65000, *out);
    serviceClose(&service);
    return rc;
}
} // namespace nxpctl
