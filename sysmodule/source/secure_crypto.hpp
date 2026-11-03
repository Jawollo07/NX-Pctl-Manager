#pragma once

#include <stratosphere.hpp>

namespace nxpctl::crypto {

    constexpr size_t Aes128KeySize = 16;
    constexpr size_t GcmNonceSize = 12;
    constexpr size_t GcmTagSize = 16;
    constexpr size_t MaxPlaintextSize = 64 * 1024;

    /* secret is the raw UTF-8 invitation code or device credential. */
    bool DeriveAes128Key(u8 out_key[Aes128KeySize], const char *secret, size_t secret_size,
                         const char *device_id, const char *purpose);

    void GenerateNonce(u8 out_nonce[GcmNonceSize]);
    u64 GenerateRequestId();

    bool Aes128GcmEncrypt(const u8 key[Aes128KeySize],
                          const u8 nonce[GcmNonceSize],
                          const u8 *aad, size_t aad_size,
                          const u8 *plaintext, size_t plaintext_size,
                          u8 *ciphertext, u8 out_tag[GcmTagSize]);

    bool Aes128GcmDecrypt(const u8 key[Aes128KeySize],
                          const u8 nonce[GcmNonceSize],
                          const u8 *aad, size_t aad_size,
                          const u8 *ciphertext, size_t ciphertext_size,
                          const u8 tag[GcmTagSize],
                          u8 *out_plaintext);

    /* NIST SP 800-38D AES-128-GCM known-answer self-test. */
    bool RunAesGcmSelfTest();

}
