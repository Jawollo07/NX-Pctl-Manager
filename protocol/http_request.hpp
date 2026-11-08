// Bounded HTTP/1.1 request builder for the NX-Pctl device API.
#pragma once

#include <cstddef>
#include <cstdint>

namespace nxpctl::http {

inline constexpr std::size_t MaxRequestBodyBytes = 96 * 1024;

enum class RequestError : std::uint8_t {
    None = 0, NullArgument, InvalidMethod, InvalidHost, InvalidPath,
    InvalidToken, BodyTooLarge, OutputTooSmall
};

struct RequestView {
    const char* data = nullptr;
    std::size_t length = 0;
};

namespace request_detail {
inline bool IsHeaderSafe(const char* value, std::size_t length) noexcept {
    for (std::size_t i = 0; i < length; ++i) {
        const auto c = static_cast<unsigned char>(value[i]);
        if (c < 0x21 || c > 0x7e) return false;
    }
    return true;
}
inline std::size_t Length(const char* value) noexcept {
    std::size_t n = 0;
    while (value[n] != '\0') ++n;
    return n;
}
inline bool Append(char* output, std::size_t capacity, std::size_t* used,
                   const char* value, std::size_t length) noexcept {
    if (length > capacity - *used) return false;
    for (std::size_t i = 0; i < length; ++i) output[(*used)++] = value[i];
    return true;
}
inline bool AppendLiteral(char* output, std::size_t capacity, std::size_t* used,
                          const char* value) noexcept {
    return Append(output, capacity, used, value, Length(value));
}
inline bool AppendUnsigned(char* output, std::size_t capacity, std::size_t* used,
                           std::size_t value) noexcept {
    char digits[20];
    std::size_t count = 0;
    do { digits[count++] = static_cast<char>('0' + value % 10); value /= 10; } while (value);
    if (count > capacity - *used) return false;
    while (count) output[(*used)++] = digits[--count];
    return true;
}
} // namespace request_detail

// Writes one complete HTTP/1.1 request into caller-owned storage.
// Host header accepts a DNS hostname or IPv4 address, optionally followed by a TCP port.
// token may be null/empty for enrollment and authentication endpoints.
[[nodiscard]] inline RequestError BuildRequest(
    const char* method, const char* host, const char* path,
    const char* token, const char* body, std::size_t body_length,
    char* output, std::size_t capacity, RequestView* result) noexcept {
    if (!method || !host || !path || !output || !result) return RequestError::NullArgument;
    *result = {};
    const auto method_length = request_detail::Length(method);
    if (!((method_length == 3 && method[0]=='G' && method[1]=='E' && method[2]=='T')
       || (method_length == 4 && method[0]=='P' && method[1]=='O' && method[2]=='S' && method[3]=='T')))
        return RequestError::InvalidMethod;

    const auto host_length = request_detail::Length(host);
    if (host_length == 0 || host_length > 260 || !request_detail::IsHeaderSafe(host, host_length))
        return RequestError::InvalidHost;
    bool seen_port_separator = false;
    for (std::size_t i = 0; i < host_length; ++i) {
        const char c = host[i];
        if (c == ':') {
            if (seen_port_separator || i == 0 || i + 1 == host_length) return RequestError::InvalidHost;
            seen_port_separator = true;
            continue;
        }
        if (seen_port_separator) {
            if (c < '0' || c > '9') return RequestError::InvalidHost;
        } else {
            const bool valid = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
                || (c >= '0' && c <= '9') || c == '.' || c == '-';
            if (!valid) return RequestError::InvalidHost;
        }
    }
    const auto path_length = request_detail::Length(path);
    if (path_length == 0 || path[0] != '/' || path_length > 512)
        return RequestError::InvalidPath;
    for (std::size_t i = 0; i < path_length; ++i) {
        const auto c = static_cast<unsigned char>(path[i]);
        if (c < 0x21 || c > 0x7e || c == '\\' || c == '#') return RequestError::InvalidPath;
    }
    if (body_length > MaxRequestBodyBytes) return RequestError::BodyTooLarge;
    if (body_length && !body) return RequestError::NullArgument;

    std::size_t token_length = 0;
    if (token && token[0]) {
        token_length = request_detail::Length(token);
        if (token_length > 512 || !request_detail::IsHeaderSafe(token, token_length))
            return RequestError::InvalidToken;
    }

    std::size_t used = 0;
    using namespace request_detail;
    if (!Append(output, capacity, &used, method, method_length)
        || !AppendLiteral(output, capacity, &used, " ")
        || !Append(output, capacity, &used, path, path_length)
        || !AppendLiteral(output, capacity, &used, " HTTP/1.1\r\nHost: ")
        || !Append(output, capacity, &used, host, host_length)
        || !AppendLiteral(output, capacity, &used, "\r\nAccept: application/json\r\nAccept-Encoding: identity\r\nConnection: close\r\n")) {
        return RequestError::OutputTooSmall;
    }
    if (token_length) {
        if (!AppendLiteral(output, capacity, &used, "Authorization: Bearer ")
            || !Append(output, capacity, &used, token, token_length)
            || !AppendLiteral(output, capacity, &used, "\r\n"))
            return RequestError::OutputTooSmall;
    }
    if (body_length) {
        if (!AppendLiteral(output, capacity, &used, "Content-Type: application/json\r\n"))
            return RequestError::OutputTooSmall;
    }
    if (!AppendLiteral(output, capacity, &used, "Content-Length: ")
        || !AppendUnsigned(output, capacity, &used, body_length)
        || !AppendLiteral(output, capacity, &used, "\r\n\r\n")
        || !Append(output, capacity, &used, body ? body : "", body_length))
        return RequestError::OutputTooSmall;

    if (used >= capacity) return RequestError::OutputTooSmall;
    output[used] = '\0';
    result->data = output;
    result->length = used;
    return RequestError::None;
}

} // namespace nxpctl::http
