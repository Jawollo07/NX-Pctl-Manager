// TOTP parent-code helper (RFC 6238, HMAC-SHA1, 6 digits, 60-second period).
// Copyright (C) 2026 Taylor. GPLv3-or-later (see LICENSE).
#include "util/totp.hpp"

#include <algorithm>
#include <cstdio>
#include <ctime>
#include <cstring>
#include <string>
#include <vector>

#include <switch.h>

namespace totp
{
namespace
{
constexpr const char* SECRET_PATH = "/switch/nx_pctl_totp.key";
constexpr uint64_t PERIOD_SECONDS = 60;

uint32_t rotr(uint32_t x, unsigned n) { return (x >> n) | (x << (32 - n)); }

std::array<uint8_t, 20> sha1(const uint8_t* input, std::size_t length)
{
    std::vector<uint8_t> msg(input, input + length);
    const uint64_t bit_len = static_cast<uint64_t>(length) * 8;
    msg.push_back(0x80);
    while ((msg.size() % 64) != 56) msg.push_back(0);
    for (int i = 7; i >= 0; --i) msg.push_back(static_cast<uint8_t>(bit_len >> (i * 8)));

    uint32_t h0 = 0x67452301, h1 = 0xEFCDAB89, h2 = 0x98BADCFE;
    uint32_t h3 = 0x10325476, h4 = 0xC3D2E1F0;
    for (std::size_t off = 0; off < msg.size(); off += 64) {
        uint32_t w[80];
        for (int i = 0; i < 16; ++i) {
            const std::size_t p = off + static_cast<std::size_t>(i) * 4;
            w[i] = (uint32_t(msg[p]) << 24) | (uint32_t(msg[p+1]) << 16) |
                   (uint32_t(msg[p+2]) << 8) | uint32_t(msg[p+3]);
        }
        for (int i = 16; i < 80; ++i) w[i] = rotr(w[i-3] ^ w[i-8] ^ w[i-14] ^ w[i-16], 31);
        uint32_t a=h0, b=h1, c=h2, d=h3, e=h4;
        for (int i = 0; i < 80; ++i) {
            uint32_t f, k;
            if (i < 20)      { f = (b & c) | ((~b) & d); k = 0x5A827999; }
            else if (i < 40) { f = b ^ c ^ d;             k = 0x6ED9EBA1; }
            else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDC; }
            else             { f = b ^ c ^ d;             k = 0xCA62C1D6; }
            const uint32_t temp = rotr(a, 27) + f + e + k + w[i];
            e=d; d=c; c=rotr(b, 2); b=a; a=temp;
        }
        h0+=a; h1+=b; h2+=c; h3+=d; h4+=e;
    }
    std::array<uint8_t,20> out{};
    const uint32_t hs[] = {h0,h1,h2,h3,h4};
    for (int i=0;i<5;++i) for (int j=0;j<4;++j)
        out[i*4+j] = static_cast<uint8_t>(hs[i] >> (24-j*8));
    return out;
}

std::array<uint8_t,20> hmac_sha1(const uint8_t* key, std::size_t key_len,
                                 const uint8_t* msg, std::size_t msg_len)
{
    uint8_t k[64] = {};
    if (key_len > sizeof(k)) {
        auto digest = sha1(key, key_len);
        std::memcpy(k, digest.data(), digest.size());
    } else if (key_len) {
        std::memcpy(k, key, key_len);
    }
    uint8_t inner_pad[64], outer_pad[64];
    for (std::size_t i=0;i<64;++i) { inner_pad[i]=k[i]^0x36; outer_pad[i]=k[i]^0x5c; }
    std::vector<uint8_t> inner(inner_pad, inner_pad+64);
    inner.insert(inner.end(), msg, msg+msg_len);
    auto inner_hash = sha1(inner.data(), inner.size());
    std::vector<uint8_t> outer(outer_pad, outer_pad+64);
    outer.insert(outer.end(), inner_hash.begin(), inner_hash.end());
    auto result = sha1(outer.data(), outer.size());
    std::memset(k,0,sizeof(k));
    std::memset(inner_pad,0,sizeof(inner_pad));
    std::memset(outer_pad,0,sizeof(outer_pad));
    return result;
}

uint32_t code_for(const std::array<uint8_t,SECRET_BYTES>& secret, uint64_t step)
{
    uint8_t counter[8];
    for (int i=0;i<8;++i) counter[7-i]=static_cast<uint8_t>(step >> (i*8));
    auto hash = hmac_sha1(secret.data(), secret.size(), counter, sizeof(counter));
    const unsigned offset = hash[19] & 0x0f;
    const uint32_t binary = (uint32_t(hash[offset] & 0x7f) << 24) |
                            (uint32_t(hash[offset+1]) << 16) |
                            (uint32_t(hash[offset+2]) << 8) |
                            uint32_t(hash[offset+3]);
    return binary % 1000000;
}

bool write_secret(const std::array<uint8_t,SECRET_BYTES>& secret)
{
    std::FILE* f = std::fopen(SECRET_PATH, "wb");
    if (!f) return false;
    const bool ok = std::fwrite(secret.data(), 1, secret.size(), f) == secret.size();
    const bool closed = std::fclose(f) == 0;
    return ok && closed;
}
} // namespace

bool load_secret(std::array<uint8_t, SECRET_BYTES>& secret)
{
    std::FILE* f = std::fopen(SECRET_PATH, "rb");
    if (!f) return false;
    const std::size_t n = std::fread(secret.data(), 1, secret.size(), f);
    const int extra = std::fgetc(f);
    std::fclose(f);
    return n == secret.size() && extra == EOF;
}

bool load_or_create_secret(std::array<uint8_t, SECRET_BYTES>& secret)
{
    if (load_secret(secret)) return true;
    // Do not silently overwrite a malformed existing secret: it may be a
    // damaged or tampered key. The parent must remove it after authenticating.
    std::FILE* existing = std::fopen(SECRET_PATH, "rb");
    if (existing) { std::fclose(existing); return false; }

    randomGet(secret.data(), secret.size());
    if (write_secret(secret)) return true;
    std::memset(secret.data(), 0, secret.size());
    return false;
}

std::string base32(const uint8_t* data, std::size_t size)
{
    static constexpr char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ234567";
    std::string out;
    uint32_t buffer = 0;
    unsigned bits = 0;
    for (std::size_t i=0;i<size;++i) {
        buffer = (buffer << 8) | data[i];
        bits += 8;
        while (bits >= 5) {
            bits -= 5;
            out.push_back(alphabet[(buffer >> bits) & 31]);
        }
    }
    if (bits) out.push_back(alphabet[(buffer << (5-bits)) & 31]);
    return out;
}

bool verify_code(const std::string& code)
{
    if (code.size() != 6) return false;
    unsigned supplied = 0;
    for (char c : code) {
        if (c < '0' || c > '9') return false;
        supplied = supplied * 10 + static_cast<unsigned>(c - '0');
    }

    std::array<uint8_t,SECRET_BYTES> secret{};
    if (!load_secret(secret)) return false;
    const std::time_t now = std::time(nullptr);
    if (now < 0) { std::memset(secret.data(),0,secret.size()); return false; }
    const uint64_t step = static_cast<uint64_t>(now) / PERIOD_SECONDS;
    // Small clock-drift window: current period and one adjacent period each way.
    bool match = false;
    for (int delta=-1; delta<=1; ++delta) {
        if (delta < 0 && step == 0) continue;
        const uint64_t candidate = delta < 0 ? step-1 : step+static_cast<uint64_t>(delta);
        const uint32_t expected = code_for(secret, candidate);
        // Constant-time equality for the fixed-width integer code.
        uint32_t diff = expected ^ supplied;
        diff |= diff >> 16; diff |= diff >> 8; diff |= diff >> 4;
        diff |= diff >> 2; diff |= diff >> 1;
        match |= (diff & 1u) == 0;
    }
    std::memset(secret.data(),0,secret.size());
    return match;
}
} // namespace totp
