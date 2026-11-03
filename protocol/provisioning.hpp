#pragma once

#include <cstddef>
#include <cstdint>

namespace nxpctl::protocol {

inline constexpr std::uint32_t ProvisioningCommandId = 65001;
inline constexpr std::size_t ApiHostCapacity = 254;
inline constexpr std::size_t DeviceIdCapacity = 65;
inline constexpr std::size_t EnrollmentCodeCapacity = 129;

/*
 * One-time, volatile provisioning request sent over the local nxpctl HIPC
 * service. The sysmodule copies it into private RAM and wipes the IPC buffer
 * copy in the NRO immediately after dispatch. No filesystem persistence.
 */
struct ProvisioningRequest {
    char api_host[ApiHostCapacity]{};
    char device_id[DeviceIdCapacity]{};
    char enrollment_code[EnrollmentCodeCapacity]{};
};

static_assert(sizeof(ProvisioningRequest) == 448);

inline bool IsValidProvisioningRequest(const ProvisioningRequest& request) noexcept {
    const auto length = [](const char* value, std::size_t capacity) {
        for (std::size_t i = 0; i < capacity; ++i) if (value[i] == '\0') return i;
        return capacity;
    };
    const auto host_length = length(request.api_host, ApiHostCapacity);
    const auto id_length = length(request.device_id, DeviceIdCapacity);
    const auto code_length = length(request.enrollment_code, EnrollmentCodeCapacity);
    if (host_length == 0 || host_length >= ApiHostCapacity ||
        id_length == 0 || id_length >= DeviceIdCapacity ||
        code_length < 20 || code_length >= EnrollmentCodeCapacity) return false;

    for (std::size_t i = 0; i < host_length; ++i) {
        const char c = request.api_host[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '.' || c == '-')) return false;
    }
    if (request.api_host[0] == '.' || request.api_host[host_length - 1] == '.') return false;
    for (std::size_t i = 0; i < id_length; ++i) {
        const char c = request.device_id[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-')) return false;
    }
    if (!((request.device_id[0] >= 'a' && request.device_id[0] <= 'z') ||
          (request.device_id[0] >= 'A' && request.device_id[0] <= 'Z') ||
          (request.device_id[0] >= '0' && request.device_id[0] <= '9'))) return false;
    for (std::size_t i = 0; i < code_length; ++i) {
        const unsigned char c = static_cast<unsigned char>(request.enrollment_code[i]);
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
              (c >= '0' && c <= '9') || c == '_' || c == '-')) return false;
    }
    return true;
}

} // namespace nxpctl::protocol
