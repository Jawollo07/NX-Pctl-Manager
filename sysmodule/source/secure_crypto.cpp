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

    using Block = u8[16];

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
            if (lsb) v[0] ^= 0xe1;
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
    const size_t purpose_size = std::strlen(purpose);
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
    return true;
}

}
