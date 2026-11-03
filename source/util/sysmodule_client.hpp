#pragma once
#include <switch.h>
#include "../../protocol/runtime_status.hpp"
#include "../../protocol/provisioning.hpp"
namespace nxpctl {
using RuntimeStatus = protocol::RuntimeStatus;
Result get_runtime_status(RuntimeStatus* out);
Result configure_enrollment(const protocol::ProvisioningRequest* request);
}
