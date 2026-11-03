#pragma once
#include <switch.h>
#include "../../../protocol/runtime_status.hpp"
namespace nxpctl {
using RuntimeStatus = protocol::RuntimeStatus;
Result get_runtime_status(RuntimeStatus* out);
}
