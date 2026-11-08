#include "../protocol/totp.hpp"

#include <cassert>
#include <cstdint>
#include <cstring>
#include <iostream>

int main() {
    using namespace nxpctl::totp;
    const std::uint8_t secret[] = "12345678901234567890";
    struct Vector { std::uint64_t time; const char* code; };
    constexpr Vector vectors[] = {
        {59, "287082"},
        {1111111109, "081804"},
        {1111111111, "050471"},
        {1234567890, "005924"},
        {2000000000, "279037"},
        {20000000000ULL, "353130"},
    };

    for (const auto& vector : vectors) {
        std::uint64_t matched = UINT64_MAX;
        assert(Verify(secret, sizeof(secret) - 1, vector.time, vector.code,
                      0, &matched, 0));
        assert(matched == vector.time / TimeStepSeconds);
    }

    // Counter window accepts one 30-second step of clock skew.
    std::uint64_t matched = 0;
    assert(Verify(secret, sizeof(secret) - 1, 89, "287082", 0, &matched, 1));
    assert(matched == 1);

    // A successfully consumed counter cannot be replayed.
    assert(!Verify(secret, sizeof(secret) - 1, 59, "287082", 1, &matched, 1));

    // Strict six-digit decimal input; malformed strings never authenticate.
    assert(!Verify(secret, sizeof(secret) - 1, 59, "28708", 0, &matched));
    assert(!Verify(secret, sizeof(secret) - 1, 59, "28708x", 0, &matched));
    assert(!Verify(secret, sizeof(secret) - 1, 59, "28 082", 0, &matched));
    assert(!Verify(secret, sizeof(secret) - 1, 59, "287083", 0, &matched));

    std::uint32_t generated = 0;
    assert(GenerateCode(secret, sizeof(secret) - 1, 1, &generated));
    assert(generated == 287082);

    std::cout << "TOTP RFC 6238 vectors OK; 6 digits; 30s step; replay rejection\n";
}
