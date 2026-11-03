#include "../protocol/provisioning.hpp"

#include <cassert>
#include <cstring>

int main() {
    nxpctl::protocol::ProvisioningRequest request{};
    std::strcpy(request.api_host, "api.example.test");
    std::strcpy(request.device_id, "switch-01");
    std::strcpy(request.enrollment_code, "abcdefghijklmnopqrstuvwxyz_0123456789");
    assert(nxpctl::protocol::IsValidProvisioningRequest(request));

    request.api_host[0] = '/';
    assert(!nxpctl::protocol::IsValidProvisioningRequest(request));
    request.api_host[0] = 'a';

    request.enrollment_code[0] = '"';
    assert(!nxpctl::protocol::IsValidProvisioningRequest(request));
    request.enrollment_code[0] = 'a';

    request.device_id[0] = '-';
    assert(!nxpctl::protocol::IsValidProvisioningRequest(request));
    request.device_id[0] = 's';

    request.enrollment_code[19] = '\0';
    assert(!nxpctl::protocol::IsValidProvisioningRequest(request));

    return 0;
}
