#include "../protocol/provisioning.hpp"

#include <cassert>
#include <cstring>

using nxpctl::protocol::ParseProvisioningConfig;
using nxpctl::protocol::ProvisioningRequest;

int main() {
    constexpr char valid[] =
        "# comment\r\n"
        "; another comment\r\n"
        "  api_host = portal.example.net  \r\n"
        "device_id=switch-01\n"
        "enrollment_code=AbCdEf0123456789_-abcdefgh\n";
    ProvisioningRequest request{};
    assert(ParseProvisioningConfig(valid, sizeof(valid) - 1, request));
    assert(std::strcmp(request.api_host, "portal.example.net") == 0);
    assert(std::strcmp(request.device_id, "switch-01") == 0);
    assert(std::strcmp(request.enrollment_code, "AbCdEf0123456789_-abcdefgh") == 0);

    constexpr char duplicate[] =
        "api_host=portal.example.net\n"
        "api_host=other.example.net\n"
        "device_id=switch-01\n"
        "enrollment_code=AbCdEf0123456789_-abcdefgh\n";
    assert(!ParseProvisioningConfig(duplicate, sizeof(duplicate) - 1, request));

    constexpr char unknown[] =
        "api_host=portal.example.net\n"
        "device_id=switch-01\n"
        "enrollment_code=AbCdEf0123456789_-abcdefgh\n"
        "admin_token=secret\n";
    assert(!ParseProvisioningConfig(unknown, sizeof(unknown) - 1, request));

    constexpr char missing[] =
        "api_host=portal.example.net\n"
        "device_id=switch-01\n";
    assert(!ParseProvisioningConfig(missing, sizeof(missing) - 1, request));

    constexpr char malformed[] =
        "api_host=https://portal.example.net\n"
        "device_id=switch-01\n"
        "enrollment_code=AbCdEf0123456789_-abcdefgh\n";
    assert(!ParseProvisioningConfig(malformed, sizeof(malformed) - 1, request));

    constexpr char quoted[] =
        "api_host=portal.example.net\n"
        "device_id=switch-01\n"
        "enrollment_code=\"AbCdEf0123456789_-abcdefgh\"\n";
    assert(!ParseProvisioningConfig(quoted, sizeof(quoted) - 1, request));

    constexpr char embedded_nul[] = {
        'a','p','i','_','h','o','s','t','=','x','\0','\n'
    };
    assert(!ParseProvisioningConfig(embedded_nul, sizeof(embedded_nul), request));
    assert(!ParseProvisioningConfig(valid, 1025, request));
    assert(!ParseProvisioningConfig(nullptr, 10, request));
    return 0;
}
