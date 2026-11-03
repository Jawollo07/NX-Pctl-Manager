// Strict, allocation-free HTTP/1.x response parser for bounded device API replies.
#pragma once

#include <cstddef>
#include <cstdint>

namespace nxpctl::http {

inline constexpr std::size_t MaxHeaderBytes = 8 * 1024;
inline constexpr std::size_t MaxBodyBytes = 16 * 1024;
inline constexpr std::size_t MaxHeaderCount = 32;

enum class Error : std::uint8_t {
    None = 0, NullInput, Incomplete, HeaderTooLarge, BodyTooLarge,
    InvalidStatusLine, InvalidHeader, TooManyHeaders, DuplicateContentLength,
    MissingContentLength, UnsupportedTransferEncoding, UnsupportedContentEncoding,
    InvalidContentLength, LengthMismatch
};

struct ResponseView {
    std::uint16_t status_code = 0;
    const char* body = nullptr;
    std::size_t body_length = 0;
};

namespace detail {
inline bool IsTokenChar(unsigned char c) noexcept {
    if ((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z')
        || (c >= 'a' && c <= 'z')) return true;
    switch (c) {
        case '!': case '#': case '$': case '%': case '&': case '\'': case '*':
        case '+': case '-': case '.': case '^': case '_': case '`': case '|': case '~':
            return true;
        default: return false;
    }
}
inline char Lower(char c) noexcept {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}
inline bool EqualsIgnoreCase(const char* data, std::size_t length, const char* expected) noexcept {
    std::size_t n = 0;
    while (expected[n] != '\0') ++n;
    if (length != n) return false;
    for (std::size_t i = 0; i < n; ++i)
        if (Lower(data[i]) != expected[i]) return false;
    return true;
}
inline bool FindCrlf(const char* data, std::size_t length, std::size_t start,
                     std::size_t* end) noexcept {
    for (std::size_t i = start; i + 1 < length; ++i) {
        if (data[i] == '\r' && data[i + 1] == '\n') { *end = i; return true; }
        if (data[i] == '\n' || data[i] == '\r') return false;
    }
    return false;
}
} // namespace detail

// Parses exactly one complete response. The input buffer remains caller-owned;
// returned body points into that buffer. Chunked transfer and compression are
// intentionally unsupported so framing is unambiguous and bounded.
[[nodiscard]] inline Error ParseResponse(const char* input, std::size_t length,
                                         ResponseView* output) noexcept {
    if (input == nullptr || output == nullptr) return Error::NullInput;
    *output = {};
    if (length == 0) return Error::Incomplete;
    if (length > MaxHeaderBytes + MaxBodyBytes + 4) return Error::BodyTooLarge;

    std::size_t line_end = 0;
    if (!detail::FindCrlf(input, length, 0, &line_end)) return Error::InvalidStatusLine;
    if (line_end < 12 || line_end > 32) return Error::InvalidStatusLine;
    const bool http11 = line_end >= 12 && input[0] == 'H' && input[1] == 'T'
        && input[2] == 'T' && input[3] == 'P' && input[4] == '/' && input[5] == '1'
        && input[6] == '.' && input[7] == '1' && input[8] == ' ';
    const bool http10 = line_end >= 12 && input[0] == 'H' && input[1] == 'T'
        && input[2] == 'T' && input[3] == 'P' && input[4] == '/' && input[5] == '1'
        && input[6] == '.' && input[7] == '0' && input[8] == ' ';
    if (!http10 && !http11) return Error::InvalidStatusLine;
    if (input[9] < '1' || input[9] > '5' || input[10] < '0' || input[10] > '9'
        || input[11] < '0' || input[11] > '9') return Error::InvalidStatusLine;
    if (line_end > 12 && input[12] != ' ') return Error::InvalidStatusLine;
    for (std::size_t i = 12; i < line_end; ++i) {
        const auto c = static_cast<unsigned char>(input[i]);
        if (c < 0x20 || c > 0x7e) return Error::InvalidStatusLine;
    }
    const auto status = static_cast<std::uint16_t>((input[9] - '0') * 100
        + (input[10] - '0') * 10 + (input[11] - '0'));

    std::size_t position = line_end + 2;
    std::size_t header_count = 0;
    bool have_length = false;
    std::size_t content_length = 0;
    while (true) {
        if (position > MaxHeaderBytes || position > length) return Error::HeaderTooLarge;
        std::size_t end = 0;
        if (!detail::FindCrlf(input, length, position, &end)) {
            if (length - position > MaxHeaderBytes) return Error::HeaderTooLarge;
            return Error::Incomplete;
        }
        if (end == position) { position = end + 2; break; }
        if (++header_count > MaxHeaderCount) return Error::TooManyHeaders;
        if (end - position > MaxHeaderBytes) return Error::HeaderTooLarge;

        std::size_t colon = position;
        while (colon < end && input[colon] != ':') {
            if (!detail::IsTokenChar(static_cast<unsigned char>(input[colon])))
                return Error::InvalidHeader;
            ++colon;
        }
        if (colon == position || colon == end) return Error::InvalidHeader;
        std::size_t value_start = colon + 1, value_end = end;
        while (value_start < value_end && (input[value_start] == ' ' || input[value_start] == '\t'))
            ++value_start;
        while (value_end > value_start && (input[value_end - 1] == ' ' || input[value_end - 1] == '\t'))
            --value_end;
        for (std::size_t i = value_start; i < value_end; ++i) {
            const auto c = static_cast<unsigned char>(input[i]);
            if ((c < 0x20 && c != '\t') || c == 0x7f) return Error::InvalidHeader;
        }
        const auto name_length = colon - position;
        const auto value_length = value_end - value_start;
        if (detail::EqualsIgnoreCase(input + position, name_length, "content-length")) {
            if (have_length) return Error::DuplicateContentLength;
            if (value_length == 0) return Error::InvalidContentLength;
            std::size_t parsed = 0;
            for (std::size_t i = value_start; i < value_end; ++i) {
                const char c = input[i];
                if (c < '0' || c > '9') return Error::InvalidContentLength;
                const auto digit = static_cast<std::size_t>(c - '0');
                if (parsed > (MaxBodyBytes - digit) / 10) return Error::BodyTooLarge;
                parsed = parsed * 10 + digit;
            }
            content_length = parsed;
            have_length = true;
        } else if (detail::EqualsIgnoreCase(input + position, name_length, "transfer-encoding")) {
            return Error::UnsupportedTransferEncoding;
        } else if (detail::EqualsIgnoreCase(input + position, name_length, "content-encoding")) {
            if (!detail::EqualsIgnoreCase(input + value_start, value_length, "identity"))
                return Error::UnsupportedContentEncoding;
        }
        position = end + 2;
    }
    if (!have_length) return Error::MissingContentLength;
    if (content_length > MaxBodyBytes) return Error::BodyTooLarge;
    if (length - position < content_length) return Error::Incomplete;
    if (length - position != content_length) return Error::LengthMismatch;

    output->status_code = status;
    output->body = input + position;
    output->body_length = content_length;
    return Error::None;
}

} // namespace nxpctl::http
