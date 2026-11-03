#pragma once

#include <stratosphere.hpp>
#include <cstddef>
#include <cstdint>

namespace nxpctl::secure_http {

    enum class Error : u8 {
        None = 0,
        InvalidArgument,
        CryptoSelfTestFailed,
        InvalidPath,
        EncodingFailed,
        RequestBuildFailed,
        TransportFailed,
        InvalidHttpResponse,
        InvalidEnvelope,
        ResponseAuthenticationFailed,
        ResponseTooLarge
    };

    /*
     * Performs one authenticated-encryption POST. secret/purpose are
     * "enrollment" or "device" credentials as defined by secure-http-protocol.md.
     * plaintext_json must be a complete JSON object. The decrypted response
     * is copied to caller-owned storage and NUL-terminated.
     */
    Error Post(const char *host, u16 port,
               const char *device_id, const char *operation,
               const char *secret, std::size_t secret_size,
               const char *purpose,
               const char *plaintext_json, std::size_t plaintext_size,
               char *out_plaintext, std::size_t out_capacity,
               std::size_t *out_size, u16 *out_http_status);

}
