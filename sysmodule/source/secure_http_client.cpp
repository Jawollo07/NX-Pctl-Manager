#include "secure_http_client.hpp"
#include "secure_crypto.hpp"
#include "http_transport.hpp"

#include "../../protocol/http_request.hpp"
#include "../../protocol/http_response.hpp"

#include <switch/crypto/sha256.h>
#include <algorithm>
#include <cstdio>
#include <cstring>

namespace nxpctl::secure_http {
namespace {

    constexpr std::size_t EnvelopeBodyCapacity = 96 * 1024;
    constexpr std::size_t HttpRequestCapacity = 100 * 1024;
    constexpr std::size_t HttpResponseCapacity = 108 * 1024;

    /* Large network buffers live in BSS, never on the 16 KiB worker stack. */
    char g_envelope[EnvelopeBodyCapacity];
    char g_request[HttpRequestCapacity];
    char g_response[HttpResponseCapacity];
    char g_cipher_b64[EnvelopeBodyCapacity];
    u8 g_ciphertext[crypto::MaxPlaintextSize + crypto::GcmTagSize];
    u8 g_plaintext[crypto::MaxPlaintextSize];
    bool g_crypto_checked = false;
    bool g_crypto_valid = false;

    constexpr char Base64Alphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

    bool IsDeviceIdChar(char c) {
        return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
               (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
    }

    bool IsOperation(const char *operation) {
        static constexpr const char *Allowed[] = {
            "enroll", "auth", "policy", "heartbeat", "events", "policy-ack", "parent-auth"
        };
        if (!operation) return false;
        for (const char *candidate : Allowed) {
            if (std::strcmp(operation, candidate) == 0) return true;
        }
        return false;
    }

    std::size_t Base64EncodedSize(std::size_t size) {
        return ((size + 2) / 3) * 4 - ((3 - (size % 3)) % 3);
    }

    bool Base64UrlEncode(const u8 *input, std::size_t size,
                         char *output, std::size_t capacity,
                         std::size_t *out_size) {
        if ((!input && size) || !output || !out_size) return false;
        const std::size_t required = Base64EncodedSize(size);
        if (required > capacity) return false;
        std::size_t in = 0, out = 0;
        while (in + 3 <= size) {
            const u32 value = (static_cast<u32>(input[in]) << 16) |
                              (static_cast<u32>(input[in + 1]) << 8) |
                              input[in + 2];
            output[out++] = Base64Alphabet[(value >> 18) & 63];
            output[out++] = Base64Alphabet[(value >> 12) & 63];
            output[out++] = Base64Alphabet[(value >> 6) & 63];
            output[out++] = Base64Alphabet[value & 63];
            in += 3;
        }
        if (size - in == 1) {
            const u32 value = static_cast<u32>(input[in]) << 16;
            output[out++] = Base64Alphabet[(value >> 18) & 63];
            output[out++] = Base64Alphabet[(value >> 12) & 63];
        } else if (size - in == 2) {
            const u32 value = (static_cast<u32>(input[in]) << 16) |
                              (static_cast<u32>(input[in + 1]) << 8);
            output[out++] = Base64Alphabet[(value >> 18) & 63];
            output[out++] = Base64Alphabet[(value >> 12) & 63];
            output[out++] = Base64Alphabet[(value >> 6) & 63];
        }
        *out_size = out;
        return true;
    }

    int Base64Value(char c) {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '-') return 62;
        if (c == '_') return 63;
        return -1;
    }

    bool Base64UrlDecode(const char *input, std::size_t size,
                         u8 *output, std::size_t capacity,
                         std::size_t *out_size) {
        if (!input || !output || !out_size || (size % 4) == 1) return false;
        const std::size_t required = (size / 4) * 3 + ((size % 4) ? size % 4 - 1 : 0);
        if (required > capacity) return false;
        u32 accumulator = 0;
        unsigned bits = 0;
        std::size_t out = 0;
        for (std::size_t i = 0; i < size; ++i) {
            const int value = Base64Value(input[i]);
            if (value < 0) return false;
            accumulator = (accumulator << 6) | static_cast<u32>(value);
            bits += 6;
            if (bits >= 8) {
                bits -= 8;
                output[out++] = static_cast<u8>((accumulator >> bits) & 0xff);
            }
        }
        if (bits && (accumulator & ((1u << bits) - 1u)) != 0) return false;
        *out_size = out;
        return out == required;
    }

    void HexEncode(const u8 digest[32], char output[65]) {
        static constexpr char Hex[] = "0123456789abcdef";
        for (std::size_t i = 0; i < 32; ++i) {
            output[i * 2] = Hex[digest[i] >> 4];
            output[i * 2 + 1] = Hex[digest[i] & 0x0f];
        }
        output[64] = '\0';
    }

    bool BuildAad(char *output, std::size_t capacity, const char *direction,
                  const char *path, const char *device_id, u64 sequence,
                  std::size_t *out_size) {
        const int count = std::snprintf(output, capacity,
            "NX-PCTL-AEAD/1\n%s\nPOST\n%s\n%s\n%llu",
            direction, path, device_id,
            static_cast<unsigned long long>(sequence));
        if (count < 0 || static_cast<std::size_t>(count) >= capacity) return false;
        *out_size = static_cast<std::size_t>(count);
        return true;
    }

    bool Append(char *output, std::size_t capacity, std::size_t *used,
                const char *value, std::size_t length) {
        if (length > capacity - *used) return false;
        std::memcpy(output + *used, value, length);
        *used += length;
        return true;
    }

    bool ParseJsonString(const char *input, std::size_t length,
                         std::size_t *position, const char **value,
                         std::size_t *value_size) {
        if (*position >= length || input[*position] != '"') return false;
        const std::size_t start = ++(*position);
        while (*position < length && input[*position] != '"') {
            const unsigned char c = static_cast<unsigned char>(input[*position]);
            if (c < 0x20 || c == '\\') return false;
            ++(*position);
        }
        if (*position >= length) return false;
        *value = input + start;
        *value_size = *position - start;
        ++(*position);
        return true;
    }

    void SkipWhitespace(const char *input, std::size_t length, std::size_t *position) {
        while (*position < length &&
               (input[*position] == ' ' || input[*position] == '\n' ||
                input[*position] == '\r' || input[*position] == '\t')) ++(*position);
    }

    bool ParseEnvelope(const char *input, std::size_t length,
                       u64 *sequence, char key_selector[65],
                       u8 nonce[12], std::size_t *nonce_size,
                       u8 *ciphertext, std::size_t ciphertext_capacity,
                       std::size_t *ciphertext_size) {
        if (!input || !sequence || !key_selector || !nonce || !nonce_size ||
            !ciphertext || !ciphertext_size) return false;

        bool have_v = false, have_s = false, have_k = false, have_n = false, have_c = false;
        std::size_t position = 0;
        SkipWhitespace(input, length, &position);
        if (position >= length || input[position++] != '{') return false;

        while (position < length) {
            SkipWhitespace(input, length, &position);
            if (position < length && input[position] == '}') { ++position; break; }

            const char *key = nullptr;
            std::size_t key_size = 0;
            if (!ParseJsonString(input, length, &position, &key, &key_size)) return false;
            SkipWhitespace(input, length, &position);
            if (position >= length || input[position++] != ':') return false;
            SkipWhitespace(input, length, &position);

            if (key_size == 1 && key[0] == 'v') {
                if (have_v || position >= length || input[position++] != '1') return false;
                have_v = true;
            } else if (key_size == 1 && key[0] == 's') {
                if (have_s) return false;
                if (position >= length || input[position] < '0' || input[position] > '9') return false;
                u64 value = 0;
                do {
                    const u32 digit = static_cast<u32>(input[position++] - '0');
                    if (value > (UINT64_MAX - digit) / 10) return false;
                    value = value * 10 + digit;
                } while (position < length && input[position] >= '0' && input[position] <= '9');
                *sequence = value;
                have_s = true;
            } else if (key_size == 1 && key[0] == 'k') {
                const char *value = nullptr;
                std::size_t value_size = 0;
                if (have_k || !ParseJsonString(input, length, &position, &value, &value_size) ||
                    value_size != 64) return false;
                std::memcpy(key_selector, value, 64);
                key_selector[64] = '\0';
                have_k = true;
            } else if ((key_size == 1 && key[0] == 'n') || (key_size == 1 && key[0] == 'c')) {
                const bool is_nonce = key[0] == 'n';
                const char *value = nullptr;
                std::size_t value_size = 0;
                if ((is_nonce && have_n) || (!is_nonce && have_c) ||
                    !ParseJsonString(input, length, &position, &value, &value_size)) return false;
                std::size_t decoded_size = 0;
                if (is_nonce) {
                    if (!Base64UrlDecode(value, value_size, nonce, 12, &decoded_size) || decoded_size != 12)
                        return false;
                    *nonce_size = decoded_size;
                    have_n = true;
                } else {
                    if (!Base64UrlDecode(value, value_size, ciphertext, ciphertext_capacity, &decoded_size))
                        return false;
                    *ciphertext_size = decoded_size;
                    have_c = true;
                }
            } else {
                return false;
            }

            SkipWhitespace(input, length, &position);
            if (position < length && input[position] == ',') { ++position; continue; }
            if (position < length && input[position] == '}') { ++position; break; }
            return false;
        }

        SkipWhitespace(input, length, &position);
        return position == length && have_v && have_s && have_k && have_n && have_c &&
               *ciphertext_size >= crypto::GcmTagSize;
    }

}

Error Post(const char *host, const char *host_header, u16 port,
           const char *device_id, const char *operation,
           const char *secret, std::size_t secret_size,
           const char *purpose,
           const char *plaintext_json, std::size_t plaintext_size,
           char *out_plaintext, std::size_t out_capacity,
           std::size_t *out_size, u16 *out_http_status) {
    if (!host || !host_header || port == 0 || !device_id || !operation || !secret || !purpose ||
        !plaintext_json || !out_plaintext || !out_size || !out_http_status ||
        plaintext_size == 0 || plaintext_size > crypto::MaxPlaintextSize ||
        out_capacity == 0) return Error::InvalidArgument;

    *out_size = 0;
    *out_http_status = 0;
    out_plaintext[0] = '\0';

    if (!g_crypto_checked) {
        g_crypto_valid = crypto::RunAesGcmSelfTest();
        g_crypto_checked = true;
    }
    if (!g_crypto_valid) return Error::CryptoSelfTestFailed;

    const std::size_t device_length = std::strlen(device_id);
    if (device_length == 0 || device_length > 64 ||
        !((device_id[0] >= 'A' && device_id[0] <= 'Z') ||
          (device_id[0] >= 'a' && device_id[0] <= 'z') ||
          (device_id[0] >= '0' && device_id[0] <= '9')) || !IsOperation(operation))
        return Error::InvalidPath;
    for (std::size_t i = 0; i < device_length; ++i)
        if (!IsDeviceIdChar(device_id[i])) return Error::InvalidPath;

    char path[600];
    const int path_length = std::snprintf(path, sizeof(path),
        "/v1/secure/%s/%s", device_id, operation);
    if (path_length <= 0 || static_cast<std::size_t>(path_length) >= sizeof(path))
        return Error::InvalidPath;

    u8 key[crypto::Aes128KeySize];
    u8 digest[32];
    char selector[65];
    if (!crypto::DeriveAes128Key(key, secret, secret_size, device_id, purpose))
        return Error::InvalidArgument;
    sha256CalculateHash(digest, secret, secret_size);
    HexEncode(digest, selector);
    std::memset(digest, 0, sizeof(digest));

    /* SQLite INTEGER is signed 64-bit; keep the random replay ID in its safe range. */
    const u64 sequence = crypto::GenerateRequestId() & 0x7fffffffffffffffULL;
    u8 nonce[crypto::GcmNonceSize];
    crypto::GenerateNonce(nonce);

    char aad[1024];
    std::size_t aad_size = 0;
    if (!BuildAad(aad, sizeof(aad), "request", path, device_id, sequence, &aad_size))
        return Error::InvalidPath;

    u8 tag[crypto::GcmTagSize];
    if (!crypto::Aes128GcmEncrypt(key, nonce,
            reinterpret_cast<const u8 *>(aad), aad_size,
            reinterpret_cast<const u8 *>(plaintext_json), plaintext_size,
            g_ciphertext, tag)) {
        std::memset(key, 0, sizeof(key));
        return Error::EncodingFailed;
    }
    std::memcpy(g_ciphertext + plaintext_size, tag, sizeof(tag));
    const std::size_t encrypted_size = plaintext_size + sizeof(tag);

    std::size_t nonce_b64_size = 0, cipher_b64_size = 0;
    char nonce_b64[32];
    char *cipher_b64 = g_cipher_b64;
    if (!Base64UrlEncode(nonce, sizeof(nonce), nonce_b64, sizeof(nonce_b64), &nonce_b64_size) ||
        !Base64UrlEncode(g_ciphertext, encrypted_size, cipher_b64, sizeof(cipher_b64), &cipher_b64_size)) {
        std::memset(key, 0, sizeof(key));
        return Error::EncodingFailed;
    }

    std::size_t envelope_size = 0;
    const int prefix_size = std::snprintf(g_envelope, sizeof(g_envelope),
        "{\"v\":1,\"s\":%llu,\"k\":\"%s\",\"n\":\"",
        static_cast<unsigned long long>(sequence), selector);
    if (prefix_size < 0 || static_cast<std::size_t>(prefix_size) >= sizeof(g_envelope))
        return Error::EncodingFailed;
    envelope_size = static_cast<std::size_t>(prefix_size);
    if (!Append(g_envelope, sizeof(g_envelope), &envelope_size, nonce_b64, nonce_b64_size) ||
        !Append(g_envelope, sizeof(g_envelope), &envelope_size, "\",\"c\":\"", 7) ||
        !Append(g_envelope, sizeof(g_envelope), &envelope_size, cipher_b64, cipher_b64_size) ||
        !Append(g_envelope, sizeof(g_envelope), &envelope_size, "\"}", 2)) {
        std::memset(key, 0, sizeof(key));
        return Error::EncodingFailed;
    }
    g_envelope[envelope_size] = '\0';

    nxpctl::http::RequestView request_view{};
    if (nxpctl::http::BuildRequest("POST", host_header, path, nullptr,
            g_envelope, envelope_size, g_request, sizeof(g_request), &request_view) !=
            nxpctl::http::RequestError::None) {
        std::memset(key, 0, sizeof(key));
        return Error::RequestBuildFailed;
    }

    std::size_t response_size = 0;
    auto transport_error = net::HttpPost(host, port,
        request_view.data, request_view.length,
        g_response, sizeof(g_response), &response_size);
    if (transport_error != net::Error::None) {
        /*
         * Retry the byte-identical request once. Reusing the same random
         * request ID lets the backend return its cached encrypted response
         * if it committed the operation but the first response was lost.
         */
        response_size = 0;
        transport_error = net::HttpPost(host, port,
            request_view.data, request_view.length,
            g_response, sizeof(g_response), &response_size);
    }
    if (transport_error != net::Error::None) {
        std::memset(key, 0, sizeof(key));
        return Error::TransportFailed;
    }

    nxpctl::http::ResponseView http_response{};
    if (nxpctl::http::ParseResponse(g_response, response_size, &http_response) !=
        nxpctl::http::Error::None) {
        std::memset(key, 0, sizeof(key));
        return Error::InvalidHttpResponse;
    }
    *out_http_status = http_response.status_code;

    u64 response_sequence = 0;
    char response_selector[65];
    u8 response_nonce[12];
    std::size_t response_nonce_size = 0, response_ciphertext_size = 0;
    if (!ParseEnvelope(http_response.body, http_response.body_length,
            &response_sequence, response_selector, response_nonce, &response_nonce_size,
            g_ciphertext, sizeof(g_ciphertext), &response_ciphertext_size) ||
        response_sequence != sequence || std::strcmp(response_selector, selector) != 0) {
        std::memset(key, 0, sizeof(key));
        return Error::InvalidEnvelope;
    }

    const std::size_t response_plaintext_size = response_ciphertext_size - crypto::GcmTagSize;
    if (response_plaintext_size + 1 > out_capacity ||
        response_plaintext_size > crypto::MaxPlaintextSize) {
        std::memset(key, 0, sizeof(key));
        return Error::ResponseTooLarge;
    }

    if (!BuildAad(aad, sizeof(aad), "response", path, device_id,
                  response_sequence, &aad_size) ||
        !crypto::Aes128GcmDecrypt(key, response_nonce, 
            reinterpret_cast<const u8 *>(aad), aad_size,
            g_ciphertext, response_plaintext_size,
            g_ciphertext + response_plaintext_size,
            g_plaintext)) {
        std::memset(key, 0, sizeof(key));
        std::memset(g_plaintext, 0, sizeof(g_plaintext));
        return Error::ResponseAuthenticationFailed;
    }

    std::memcpy(out_plaintext, g_plaintext, response_plaintext_size);
    out_plaintext[response_plaintext_size] = '\0';
    *out_size = response_plaintext_size;

    std::memset(key, 0, sizeof(key));
    std::memset(g_plaintext, 0, response_plaintext_size);
    std::memset(g_ciphertext, 0, response_ciphertext_size);
    return Error::None;
}

}
