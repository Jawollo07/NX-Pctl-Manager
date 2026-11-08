// RFC 6238 TOTP verifier (HMAC-SHA1, six decimal digits).
// Horizon-independent; secret provisioning and durable replay state are owned
// by the platform integration, never by this header.
#pragma once

#include <cstddef>
#include <cstdint>

namespace nxpctl::totp {

inline constexpr std::uint64_t TimeStepSeconds = 30;
inline constexpr std::uint32_t Modulus = 1'000'000;
inline constexpr std::uint64_t DefaultWindow = 1;

namespace detail {

struct Sha1 {
    std::uint32_t h[5]{
        0x67452301u, 0xEFCDAB89u, 0x98BADCFEu,
        0x10325476u, 0xC3D2E1F0u
    };
    std::uint64_t total_bytes = 0;
    std::uint8_t block[64]{};
    std::size_t used = 0;

    static constexpr std::uint32_t RotL(std::uint32_t x, unsigned n) noexcept {
        return (x << n) | (x >> (32 - n));
    }

    void Transform(const std::uint8_t* input) noexcept {
        std::uint32_t w[80]{};
        for (unsigned i = 0; i < 16; ++i) {
            const unsigned j = i * 4;
            w[i] = (static_cast<std::uint32_t>(input[j]) << 24)
                 | (static_cast<std::uint32_t>(input[j + 1]) << 16)
                 | (static_cast<std::uint32_t>(input[j + 2]) << 8)
                 | static_cast<std::uint32_t>(input[j + 3]);
        }
        for (unsigned i = 16; i < 80; ++i)
            w[i] = RotL(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);

        std::uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
        for (unsigned i = 0; i < 80; ++i) {
            std::uint32_t f = 0, k = 0;
            if (i < 20) {
                f = (b & c) | ((~b) & d); k = 0x5A827999u;
            } else if (i < 40) {
                f = b ^ c ^ d; k = 0x6ED9EBA1u;
            } else if (i < 60) {
                f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDCu;
            } else {
                f = b ^ c ^ d; k = 0xCA62C1D6u;
            }
            const std::uint32_t temp = RotL(a, 5) + f + e + k + w[i];
            e = d; d = c; c = RotL(b, 30); b = a; a = temp;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
    }

    void Update(const std::uint8_t* data, std::size_t size) noexcept {
        total_bytes += size;
        while (size != 0) {
            const std::size_t take = size < sizeof(block) - used
                ? size : sizeof(block) - used;
            for (std::size_t i = 0; i < take; ++i) block[used + i] = data[i];
            used += take; data += take; size -= take;
            if (used == sizeof(block)) {
                Transform(block);
                used = 0;
            }
        }
    }

    void Final(std::uint8_t out[20]) noexcept {
        const std::uint64_t bit_length = total_bytes * 8;
        block[used++] = 0x80;
        if (used > 56) {
            while (used < 64) block[used++] = 0;
            Transform(block);
            used = 0;
        }
        while (used < 56) block[used++] = 0;
        for (unsigned i = 0; i < 8; ++i)
            block[56 + i] = static_cast<std::uint8_t>(bit_length >> (56 - i * 8));
        Transform(block);
        for (unsigned i = 0; i < 5; ++i) {
            out[i * 4] = static_cast<std::uint8_t>(h[i] >> 24);
            out[i * 4 + 1] = static_cast<std::uint8_t>(h[i] >> 16);
            out[i * 4 + 2] = static_cast<std::uint8_t>(h[i] >> 8);
            out[i * 4 + 3] = static_cast<std::uint8_t>(h[i]);
        }
    }
};

inline void HmacSha1(const std::uint8_t* key, std::size_t key_size,
                     const std::uint8_t* message, std::size_t message_size,
                     std::uint8_t out[20]) noexcept {
    std::uint8_t normalized[64]{};
    if (key_size > sizeof(normalized)) {
        Sha1 hash{};
        hash.Update(key, key_size);
        hash.Final(out);
        for (unsigned i = 0; i < 20; ++i) normalized[i] = out[i];
    } else {
        for (std::size_t i = 0; i < key_size; ++i) normalized[i] = key[i];
    }

    std::uint8_t inner_pad[64]{}, outer_pad[64]{};
    for (unsigned i = 0; i < 64; ++i) {
        inner_pad[i] = static_cast<std::uint8_t>(normalized[i] ^ 0x36);
        outer_pad[i] = static_cast<std::uint8_t>(normalized[i] ^ 0x5c);
    }

    std::uint8_t inner_digest[20]{};
    Sha1 inner{};
    inner.Update(inner_pad, sizeof(inner_pad));
    inner.Update(message, message_size);
    inner.Final(inner_digest);

    Sha1 outer{};
    outer.Update(outer_pad, sizeof(outer_pad));
    outer.Update(inner_digest, sizeof(inner_digest));
    outer.Final(out);

    volatile std::uint8_t* wipe = normalized;
    for (std::size_t i = 0; i < sizeof(normalized); ++i) wipe[i] = 0;
    wipe = inner_pad;
    for (std::size_t i = 0; i < sizeof(inner_pad); ++i) wipe[i] = 0;
    wipe = outer_pad;
    for (std::size_t i = 0; i < sizeof(outer_pad); ++i) wipe[i] = 0;
    wipe = inner_digest;
    for (std::size_t i = 0; i < sizeof(inner_digest); ++i) wipe[i] = 0;
}

inline std::uint32_t DynamicTruncate(const std::uint8_t digest[20]) noexcept {
    const unsigned offset = digest[19] & 0x0f;
    return ((static_cast<std::uint32_t>(digest[offset]) & 0x7fu) << 24)
         | (static_cast<std::uint32_t>(digest[offset + 1]) << 16)
         | (static_cast<std::uint32_t>(digest[offset + 2]) << 8)
         | static_cast<std::uint32_t>(digest[offset + 3]);
}

inline bool ConstantTimeEqual6(const char* supplied, std::uint32_t expected) noexcept {
    char digits[6]{};
    for (int i = 5; i >= 0; --i) {
        digits[i] = static_cast<char>('0' + (expected % 10));
        expected /= 10;
    }
    std::uint8_t diff = 0;
    for (unsigned i = 0; i < 6; ++i)
        diff |= static_cast<std::uint8_t>(supplied[i] ^ digits[i]);
    volatile char* wipe = digits;
    for (unsigned i = 0; i < 6; ++i) wipe[i] = 0;
    return diff == 0;
}

} // namespace detail

[[nodiscard]] inline bool GenerateCode(
    const std::uint8_t* secret, std::size_t secret_size,
    std::uint64_t time_counter, std::uint32_t* out_code) noexcept {
    if (secret == nullptr || secret_size == 0 || secret_size > 64 || out_code == nullptr) return false;
    std::uint8_t message[8]{};
    for (unsigned i = 0; i < 8; ++i)
        message[i] = static_cast<std::uint8_t>(time_counter >> (56 - i * 8));
    std::uint8_t digest[20]{};
    detail::HmacSha1(secret, secret_size, message, sizeof(message), digest);
    *out_code = detail::DynamicTruncate(digest) % Modulus;
    volatile std::uint8_t* wipe = digest;
    for (unsigned i = 0; i < sizeof(digest); ++i) wipe[i] = 0;
    return true;
}

[[nodiscard]] inline bool Verify(
    const std::uint8_t* secret, std::size_t secret_size,
    std::uint64_t unix_time_seconds, const char* six_digit_code,
    std::uint64_t last_accepted_counter,
    std::uint64_t* out_matched_counter,
    std::uint64_t window = DefaultWindow) noexcept {
    if (secret == nullptr || secret_size == 0 || six_digit_code == nullptr
        || out_matched_counter == nullptr || secret_size > 64 || window > 2) return false;
    for (unsigned i = 0; i < 6; ++i)
        if (six_digit_code[i] < '0' || six_digit_code[i] > '9') return false;
    if (six_digit_code[6] != '\0') return false;

    const std::uint64_t current = unix_time_seconds / TimeStepSeconds;
    const std::uint64_t first = current > window ? current - window : 0;
    const std::uint64_t last = current > UINT64_MAX - window
        ? UINT64_MAX : current + window;
    bool matched = false;
    std::uint64_t best = 0;

    for (std::uint64_t counter = first; counter <= last; ++counter) {
        if (counter > last_accepted_counter) {
            std::uint32_t expected = 0;
            if (GenerateCode(secret, secret_size, counter, &expected)
                && detail::ConstantTimeEqual6(six_digit_code, expected)) {
                matched = true;
                if (counter > best) best = counter;
            }
        }
        if (counter == UINT64_MAX) break;
    }
    if (!matched) return false;
    *out_matched_counter = best;
    return true;
}

} // namespace nxpctl::totp
