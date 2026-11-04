#include "secure_crypto.hpp"

#include <switch/crypto/aes.h>
#include <switch/crypto/hmac.h>
#include <switch/crypto/sha256.h>
#include <switch/kernel/random.h>

#include <algorithm>
#include <cstring>
#include <cstdio>

namespace nxpctl::crypto {
namespace {

    void XorBlock(u8 dst[16], const u8 src[16]) {
        for (size_t i = 0; i < 16; ++i) dst[i] ^= src[i];
    }

    void ShiftRightOne(u8 value[16]) {
        u8 carry = 0;
        for (size_t i = 0; i < 16; ++i) {
            const u8 next = static_cast<u8>(value[i] & 1);
            value[i] = static_cast<u8>((value[i] >> 1) | (carry << 7));
            carry = next;
        }
    }

    /* GHASH multiplication in GF(2^128), big-endian bit convention. */
    void Gf128Multiply(const u8 x[16], const u8 y[16], u8 out[16]) {
        u8 z[16]{};
        u8 v[16];
        std::memcpy(v, y, sizeof(v));
        for (size_t bit = 0; bit < 128; ++bit) {
            if ((x[bit / 8] & static_cast<u8>(0x80 >> (bit % 8))) != 0) {
                XorBlock(z, v);
            }
            const bool lsb = (v[15] & 1) != 0;
            ShiftRightOne(v);
            v[0] ^= static_cast<u8>(0xe1 & static_cast<u8>(0 - static_cast<u8>(lsb)));
        }
        std::memcpy(out, z, sizeof(z));
        std::memset(z, 0, sizeof(z));
        std::memset(v, 0, sizeof(v));
    }

    void GhashBlock(u8 state[16], const u8 hash_subkey[16], const u8 block[16]) {
        XorBlock(state, block);
        u8 product[16];
        Gf128Multiply(state, hash_subkey, product);
        std::memcpy(state, product, sizeof(product));
        std::memset(product, 0, sizeof(product));
    }

    void GhashBytes(u8 state[16], const u8 hash_subkey[16], const u8 *data, size_t size) {
        while (size >= 16) {
            GhashBlock(state, hash_subkey, data);
            data += 16;
            size -= 16;
        }
        if (size != 0) {
            u8 final_block[16]{};
            std::memcpy(final_block, data, size);
            GhashBlock(state, hash_subkey, final_block);
            std::memset(final_block, 0, sizeof(final_block));
        }
    }

    void StoreBigEndian64(u8 out[8], u64 value) {
        for (size_t i = 0; i < 8; ++i) {
            out[7 - i] = static_cast<u8>(value & 0xff);
            value >>= 8;
        }
    }

    void IncrementCounter32(u8 counter[16]) {
        for (int i = 15; i >= 12; --i) {
            counter[i] = static_cast<u8>(counter[i] + 1);
            if (counter[i] != 0) break;
        }
    }

    bool ValidInput(const u8 *aad, size_t aad_size, const u8 *input, size_t input_size,
                    const u8 *output, const u8 *tag) {
        if (aad_size != 0 && aad == nullptr) return false;
        if (input_size != 0 && (input == nullptr || output == nullptr)) return false;
        if (tag == nullptr) return false;
        if (aad_size > (0xFFFFFFFFFFFFFFFFULL / 8) ||
            input_size > (0xFFFFFFFFFFFFFFFFULL / 8) ||
            input_size > MaxPlaintextSize) return false;
        return true;
    }

    void CalculateGhash(const Aes128Context &aes, const u8 *aad, size_t aad_size,
                        const u8 *ciphertext, size_t ciphertext_size, u8 out[16]) {
        u8 zero[16]{};
        u8 hash_subkey[16];
        aes128EncryptBlock(&aes, hash_subkey, zero);
        std::memset(out, 0, 16);
        if (aad_size != 0) GhashBytes(out, hash_subkey, aad, aad_size);
        if (ciphertext_size != 0) GhashBytes(out, hash_subkey, ciphertext, ciphertext_size);
        u8 lengths[16];
        StoreBigEndian64(lengths, static_cast<u64>(aad_size) * 8);
        StoreBigEndian64(lengths + 8, static_cast<u64>(ciphertext_size) * 8);
        GhashBlock(out, hash_subkey, lengths);
        std::memset(hash_subkey, 0, sizeof(hash_subkey));
        std::memset(lengths, 0, sizeof(lengths));
    }

    void CalculateTag(const Aes128Context &aes, const u8 nonce[12],
                      const u8 *aad, size_t aad_size,
                      const u8 *ciphertext, size_t ciphertext_size, u8 out_tag[16]) {
        u8 j0[16];
        std::memcpy(j0, nonce, 12);
        j0[12] = 0; j0[13] = 0; j0[14] = 0; j0[15] = 1;
        u8 mask[16];
        u8 hash[16];
        aes128EncryptBlock(&aes, mask, j0);
        CalculateGhash(aes, aad, aad_size, ciphertext, ciphertext_size, hash);
        std::memcpy(out_tag, mask, 16);
        XorBlock(out_tag, hash);
        std::memset(j0, 0, sizeof(j0));
        std::memset(mask, 0, sizeof(mask));
        std::memset(hash, 0, sizeof(hash));
    }

    bool ConstantTimeEqual(const u8 *a, const u8 *b, size_t size) {
        u8 difference = 0;
        for (size_t i = 0; i < size; ++i) difference |= static_cast<u8>(a[i] ^ b[i]);
        return difference == 0;
    }

    bool CryptCtr(const Aes128Context &aes, const u8 nonce[12],
                  const u8 *input, size_t size, u8 *output) {
        if (size == 0) return true;
        u8 counter[16];
        std::memcpy(counter, nonce, 12);
        counter[12] = 0; counter[13] = 0; counter[14] = 0; counter[15] = 1;
        size_t offset = 0;
        while (offset < size) {
            IncrementCounter32(counter);
            u8 stream[16];
            aes128EncryptBlock(&aes, stream, counter);
            const size_t block_size = std::min<size_t>(16, size - offset);
            for (size_t i = 0; i < block_size; ++i) {
                output[offset + i] = static_cast<u8>(input[offset + i] ^ stream[i]);
            }
            offset += block_size;
            std::memset(stream, 0, sizeof(stream));
        }
        std::memset(counter, 0, sizeof(counter));
        return true;
    }

}

bool DeriveAes128Key(u8 out_key[Aes128KeySize], const char *secret, size_t secret_size,
                     const char *device_id, const char *purpose) {
    if (out_key == nullptr || secret == nullptr || device_id == nullptr || purpose == nullptr ||
        secret_size < 16 || secret_size > 128) return false;
    const size_t device_size = std::strlen(device_id);
    if (device_size < 1 || device_size > 64 ||
        (std::strcmp(purpose, "enrollment") != 0 && std::strcmp(purpose, "device") != 0)) return false;
    for (size_t i = 0; i < device_size; ++i) {
        const char c = device_id[i];
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
              (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-')) return false;
    }

    u8 secret_digest[32];
    u8 prk[32];
    u8 okm[32];
    sha256CalculateHash(secret_digest, secret, secret_size);
    static constexpr char Salt[] = "NX-PCTL-AEAD-1";
    hmacSha256CalculateMac(prk, Salt, sizeof(Salt) - 1, secret_digest, sizeof(secret_digest));

    char info[128];
    const int info_size = std::snprintf(info, sizeof(info), "NX-PCTL-AEAD/1/%s/%s", purpose, device_id);
    if (info_size <= 0 || static_cast<size_t>(info_size) + 1 > sizeof(info)) {
        std::memset(secret_digest, 0, sizeof(secret_digest));
        std::memset(prk, 0, sizeof(prk));
        return false;
    }
    info[info_size] = 0x01;
    hmacSha256CalculateMac(okm, prk, sizeof(prk), info, static_cast<size_t>(info_size) + 1);
    std::memcpy(out_key, okm, Aes128KeySize);

    std::memset(secret_digest, 0, sizeof(secret_digest));
    std::memset(prk, 0, sizeof(prk));
    std::memset(okm, 0, sizeof(okm));
    std::memset(info, 0, sizeof(info));
    return true;
}

void GenerateNonce(u8 out_nonce[GcmNonceSize]) {
    randomGet(out_nonce, GcmNonceSize);
}

u64 GenerateRequestId() {
    return randomGet64();
}

bool Aes128GcmEncrypt(const u8 key[16], const u8 nonce[12],
                      const u8 *aad, size_t aad_size,
                      const u8 *plaintext, size_t plaintext_size,
                      u8 *ciphertext, u8 out_tag[16]) {
    if (key == nullptr || nonce == nullptr ||
        !ValidInput(aad, aad_size, plaintext, plaintext_size, ciphertext, out_tag)) return false;
    Aes128Context aes;
    aes128ContextCreate(&aes, key, true);
    if (!CryptCtr(aes, nonce, plaintext, plaintext_size, ciphertext)) return false;
    CalculateTag(aes, nonce, aad, aad_size, ciphertext, plaintext_size, out_tag);
    std::memset(&aes, 0, sizeof(aes));
    return true;
}

bool Aes128GcmDecrypt(const u8 key[16], const u8 nonce[12],
                      const u8 *aad, size_t aad_size,
                      const u8 *ciphertext, size_t ciphertext_size,
                      const u8 tag[16], u8 *out_plaintext) {
    if (key == nullptr || nonce == nullptr ||
        !ValidInput(aad, aad_size, ciphertext, ciphertext_size, out_plaintext, tag)) return false;
    Aes128Context aes;
    aes128ContextCreate(&aes, key, true);
    u8 expected_tag[16];
    CalculateTag(aes, nonce, aad, aad_size, ciphertext, ciphertext_size, expected_tag);
    if (!ConstantTimeEqual(expected_tag, tag, sizeof(expected_tag))) {
        if (out_plaintext != nullptr && ciphertext_size != 0) std::memset(out_plaintext, 0, ciphertext_size);
        std::memset(expected_tag, 0, sizeof(expected_tag));
        std::memset(&aes, 0, sizeof(aes));
        return false;
    }
    const bool result = CryptCtr(aes, nonce, ciphertext, ciphertext_size, out_plaintext);
    std::memset(expected_tag, 0, sizeof(expected_tag));
    std::memset(&aes, 0, sizeof(aes));
    return result;
}

bool RunAesGcmSelfTest() {
    const u8 key[16]{};
    const u8 nonce[12]{};
    const u8 plaintext[16]{};
    const u8 expected_ciphertext[16] = {
        0x03,0x88,0xda,0xce,0x60,0xb6,0xa3,0x92,
        0xf3,0x28,0xc2,0xb9,0x71,0xb2,0xfe,0x78
    };
    const u8 expected_tag[16] = {
        0xab,0x6e,0x47,0xd4,0x2c,0xec,0x13,0xbd,
        0xf5,0x3a,0x67,0xb2,0x12,0x57,0xbd,0xdf
    };
    u8 ciphertext[16];
    u8 tag[16];
    u8 decrypted[16];
    if (!Aes128GcmEncrypt(key, nonce, nullptr, 0, plaintext, sizeof(plaintext), ciphertext, tag)) return false;
    if (!ConstantTimeEqual(ciphertext, expected_ciphertext, sizeof(ciphertext)) ||
        !ConstantTimeEqual(tag, expected_tag, sizeof(tag))) return false;
    if (!Aes128GcmDecrypt(key, nonce, nullptr, 0, ciphertext, sizeof(ciphertext), tag, decrypted)) return false;
    if (!ConstantTimeEqual(decrypted, plaintext, sizeof(plaintext))) return false;
    tag[0] ^= 1;
    if (Aes128GcmDecrypt(key, nonce, nullptr, 0, ciphertext, sizeof(ciphertext), tag, decrypted)) return false;

    static constexpr char TestSecret[] = "enrollment-secret-material-32-bytes";
    static constexpr u8 ExpectedDerivedKey[16] = {
        0x6f,0xda,0x1b,0xce,0xf6,0x7c,0x28,0xa8,
        0x2b,0x47,0xcc,0x84,0x07,0x87,0xa4,0x1b
    };
    u8 derived_key[16];
    if (!DeriveAes128Key(derived_key, TestSecret, sizeof(TestSecret) - 1,
                         "switch-test", "device")) return false;
    const bool kdf_ok = ConstantTimeEqual(derived_key, ExpectedDerivedKey, sizeof(derived_key));
    if (!kdf_ok) {
        std::memset(derived_key, 0, sizeof(derived_key));
        return false;
    }

    /*
     * Cross-language fixture shared with server/tests/test_secure_transport.py.
     * It covers HKDF-SHA-256, the exact request AAD layout and AES-128-GCM
     * ciphertext/tag bytes used by the HTTP envelope.
     */
    const u8 fixture_nonce[12] = {
        0x00,0x01,0x02,0x03,0x04,0x05,0x06,0x07,0x08,0x09,0x0a,0x0b
    };
    static constexpr char FixtureAad[] =
        "NX-PCTL-AEAD/1\\nrequest\\nPOST\\n"
        "/v1/secure/switch-test/heartbeat\\nswitch-test\\n123";
    static constexpr char FixturePlaintext[] =
        "{\\"protocol_version\\":1,\\"uptime_seconds\\":42}";
    const u8 expected_fixture_ciphertext[42] = {
        0xd8,0x83,0x07,0x9f,0xd0,0xfd,0x89,0x55,0x16,0x2a,0xb0,0x7d,
        0xb6,0x6c,0x92,0x22,0x4a,0x14,0x7a,0x2b,0xcc,0xb1,0xee,0x92,
        0x4a,0x49,0xec,0x61,0x9d,0xc2,0xa5,0xe2,0x2f,0x2d,0xf2,0xd7,
        0xf5,0xde,0xaa,0x7a,0x52,0x9d
    };
    const u8 expected_fixture_tag[16] = {
        0xcf,0x0b,0x9a,0x99,0x43,0x7e,0x16,0x68,
        0x7b,0xf4,0x9a,0x4b,0x7a,0x88,0xad,0x4d
    };
    u8 fixture_ciphertext[sizeof(expected_fixture_ciphertext)]{};
    u8 fixture_tag[16]{};
    u8 fixture_plaintext[sizeof(FixturePlaintext) - 1]{};
    const bool fixture_encrypted = Aes128GcmEncrypt(
        derived_key, fixture_nonce,
        reinterpret_cast<const u8 *>(FixtureAad), sizeof(FixtureAad) - 1,
        reinterpret_cast<const u8 *>(FixturePlaintext), sizeof(FixturePlaintext) - 1,
        fixture_ciphertext, fixture_tag);
    const bool fixture_matches = fixture_encrypted &&
        ConstantTimeEqual(fixture_ciphertext, expected_fixture_ciphertext, sizeof(fixture_ciphertext)) &&
        ConstantTimeEqual(fixture_tag, expected_fixture_tag, sizeof(fixture_tag));
    const bool fixture_decrypted = fixture_matches && Aes128GcmDecrypt(
        derived_key, fixture_nonce,
        reinterpret_cast<const u8 *>(FixtureAad), sizeof(FixtureAad) - 1,
        fixture_ciphertext, sizeof(fixture_ciphertext), fixture_tag, fixture_plaintext);
    const bool fixture_plaintext_matches = fixture_decrypted &&
        ConstantTimeEqual(fixture_plaintext,
            reinterpret_cast<const u8 *>(FixturePlaintext), sizeof(fixture_plaintext));

    std::memset(derived_key, 0, sizeof(derived_key));
    std::memset(fixture_ciphertext, 0, sizeof(fixture_ciphertext));
    std::memset(fixture_tag, 0, sizeof(fixture_tag));
    std::memset(fixture_plaintext, 0, sizeof(fixture_plaintext));
    return fixture_matches && fixture_plaintext_matches;
}

}
