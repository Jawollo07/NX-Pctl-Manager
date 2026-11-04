#pragma once
#include "../../protocol/usage_snapshot.hpp"

namespace nxpctl::usage_store {
    // Uses the sysmodule's own device save data and a device-derived AES-GCM key.
    // The credential store must have initialized/mounted the save data first.
    bool Load(usage::Snapshot* out);
    bool HasAnyRecord();
    bool Save(const usage::Snapshot& snapshot);
}
