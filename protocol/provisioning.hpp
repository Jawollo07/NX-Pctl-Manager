#pragma once

#include <cstddef>
#include <cstdint>

namespace nxpctl::protocol {

inline constexpr std::uint32_t ProvisioningCommandId = 65001;
inline constexpr std::size_t ApiUrlCapacity = 254;
inline constexpr std::size_t DeviceIdCapacity = 65;
inline constexpr std::size_t EnrollmentCodeCapacity = 129;
inline constexpr std::size_t ProvisioningConfigMaxSize = 1024;

struct ProvisioningRequest {
    char api_url[ApiUrlCapacity]{};
    char device_id[DeviceIdCapacity]{};
    char enrollment_code[EnrollmentCodeCapacity]{};
};

static_assert(sizeof(ProvisioningRequest) == 448);

inline bool IsValidApiUrl(const char* url, std::size_t capacity,
                          bool allow_legacy_host = false) noexcept {
    std::size_t length = 0;
    while (length < capacity && url[length] != '\0') ++length;
    if (length == 0 || length >= capacity) return false;

    std::size_t authority = 0;
    if (length >= 7 && __builtin_memcmp(url, "http://", 7) == 0) {
        authority = 7;
    } else if (!allow_legacy_host) {
        return false;
    }

    const std::size_t host_start = authority;
    std::size_t host_end = host_start;
    while (host_end < length && url[host_end] != ':' && url[host_end] != '/') ++host_end;
    const std::size_t host_length = host_end - host_start;
    if (host_length == 0 || host_length > 253) return false;

    const auto is_alnum = [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
               (c >= '0' && c <= '9');
    };
    std::size_t label_length = 0;
    for (std::size_t i = host_start; i < host_end; ++i) {
        const char c = url[i];
        if (c == '.') {
            if (label_length == 0 || label_length > 63 ||
                !is_alnum(url[i - 1]) || !is_alnum(url[i + 1])) return false;
            label_length = 0;
        } else {
            if (!is_alnum(c) && c != '-') return false;
            if (++label_length > 63) return false;
        }
    }
    if (label_length == 0 || label_length > 63 ||
        !is_alnum(url[host_start]) || !is_alnum(url[host_end - 1])) return false;

    if (host_end == length) return true;
    if (url[host_end] != ':') return false;
    const std::size_t port_start = host_end + 1;
    if (port_start == length) return false;
    unsigned int port = 0;
    for (std::size_t i = port_start; i < length; ++i) {
        const char c = url[i];
        if (c < '0' || c > '9') return false;
        port = port * 10 + static_cast<unsigned int>(c - '0');
        if (port > 65535) return false;
    }
    return port > 0;
}

inline bool IsValidProvisioningRequest(const ProvisioningRequest& request) noexcept {
    const auto length = [](const char* value, std::size_t capacity) {
        for (std::size_t i = 0; i < capacity; ++i) if (value[i] == '\0') return i;
        return capacity;
    };
    const auto url_length = length(request.api_url, ApiUrlCapacity);
    const auto id_length = length(request.device_id, DeviceIdCapacity);
    const auto code_length = length(request.enrollment_code, EnrollmentCodeCapacity);
    if (url_length >= ApiUrlCapacity || id_length == 0 || id_length >= DeviceIdCapacity ||
        code_length < 20 || code_length >= EnrollmentCodeCapacity ||
        !IsValidApiUrl(request.api_url, ApiUrlCapacity)) return false;

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

inline bool ParseProvisioningConfig(const char* input, std::size_t size,
                                    ProvisioningRequest& request) noexcept {
    request = {};
    if (input == nullptr || size == 0 || size > ProvisioningConfigMaxSize) return false;

    unsigned int seen = 0;
    std::size_t pos = 0;
    while (pos < size) {
        const std::size_t line_start = pos;
        while (pos < size && input[pos] != '\n') {
            const unsigned char c = static_cast<unsigned char>(input[pos]);
            if (c == 0 || (c < 0x20 && c != '\r' && c != '\t')) return false;
            ++pos;
        }
        std::size_t line_end = pos;
        if (pos < size && input[pos] == '\n') ++pos;
        if (line_end > line_start && input[line_end - 1] == '\r') --line_end;

        std::size_t begin = line_start;
        while (begin < line_end && (input[begin] == ' ' || input[begin] == '\t')) ++begin;
        while (line_end > begin && (input[line_end - 1] == ' ' || input[line_end - 1] == '\t')) --line_end;
        if (begin == line_end || input[begin] == '#' || input[begin] == ';') continue;

        std::size_t equal = begin;
        while (equal < line_end && input[equal] != '=') ++equal;
        if (equal == line_end) return false;
        std::size_t key_end = equal;
        while (key_end > begin && (input[key_end - 1] == ' ' || input[key_end - 1] == '\t')) --key_end;
        std::size_t value_begin = equal + 1;
        while (value_begin < line_end && (input[value_begin] == ' ' || input[value_begin] == '\t')) ++value_begin;
        while (line_end > value_begin && (input[line_end - 1] == ' ' || input[line_end - 1] == '\t')) --line_end;
        if (value_begin == line_end) return false;

        char* destination = nullptr;
        std::size_t capacity = 0;
        unsigned int bit = 0;
        const std::size_t key_size = key_end - begin;
        if (key_size == 7 && __builtin_memcmp(input + begin, "api_url", 7) == 0) {
            destination = request.api_url; capacity = ApiUrlCapacity; bit = 1;
        } else if (key_size == 9 && __builtin_memcmp(input + begin, "device_id", 9) == 0) {
            destination = request.device_id; capacity = DeviceIdCapacity; bit = 2;
        } else if (key_size == 15 && __builtin_memcmp(input + begin, "enrollment_code", 15) == 0) {
            destination = request.enrollment_code; capacity = EnrollmentCodeCapacity; bit = 4;
        } else {
            return false;
        }
        if ((seen & bit) != 0 || line_end - value_begin >= capacity) return false;
        const std::size_t value_size = line_end - value_begin;
        for (std::size_t i = 0; i < value_size; ++i) {
            const unsigned char c = static_cast<unsigned char>(input[value_begin + i]);
            if (c < 0x21 || c > 0x7e || c == '"' || c == '\\') return false;
        }
        __builtin_memcpy(destination, input + value_begin, value_size);
        destination[value_size] = '\0';
        seen |= bit;
    }
    return seen == 7 && IsValidProvisioningRequest(request);
}

} // namespace nxpctl::protocol
