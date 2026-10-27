#pragma once

#include <string>

namespace remote_control
{
struct SyncResult
{
    bool configured = false;
    bool connected = false;
    bool policy_received = false;
    bool policy_applied = false;
    std::string message;
};

// Reads /switch/nx_pctl_server.conf, validates an HTTPS URL, synchronizes the
// desired daily limit and reports device status. Requires NX_PCTL_REMOTE_CONTROL.
SyncResult synchronize();

// Gate check: succeeds only when the configured device token is accepted by the server.
bool authenticate(std::string& message);
}
