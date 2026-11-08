#include "../protocol/parent_auth.hpp"

#include <cassert>
#include <cstdint>
#include <iostream>

int main() {
    using namespace nxpctl::parent_auth;
    const std::uint8_t secret[] = "12345678901234567890";
    Authenticator auth;

    assert(auth.Authenticate("287082", 59, 100) == Result::NotConfigured);
    assert(auth.Configure(secret, sizeof(secret) - 1));
    assert(auth.Authenticate("000000", 59, 100) == Result::InvalidCode);
    assert(auth.FailedAttempts() == 1);
    assert(!auth.IsAuthenticated(100));

    assert(auth.Authenticate("287082", 59, 101) == Result::Authenticated);
    assert(auth.IsAuthenticated(101));
    assert(auth.AuthenticatedUntil() == 101 + SessionLifetimeSeconds);
    assert(auth.LastAcceptedCounter() == 1);

    // The same TOTP counter is rejected even while the prior session is active.
    assert(auth.Authenticate("287082", 59, 102) == Result::InvalidCode);
    assert(auth.FailedAttempts() == 1);
    assert(auth.IsAuthenticated(102));

    auth.EndSession();
    assert(!auth.IsAuthenticated(102));

    // Five invalid attempts activate a monotonic-time lockout.
    for (unsigned i = 0; i < MaxFailedAttempts - 1; ++i)
        assert(auth.Authenticate("111111", 89, 200 + i) == Result::InvalidCode);
    assert(auth.Authenticate("111111", 89, 204) == Result::LockedOut);
    assert(auth.LockoutUntil() == 204 + LockoutSeconds);
    assert(auth.Authenticate("050471", 89, 205) == Result::LockedOut);
    assert(auth.Authenticate("050471", 89, 204 + LockoutSeconds) == Result::Authenticated);

    // Session automatically expires after the configured duration.
    assert(auth.IsAuthenticated(204 + LockoutSeconds));
    assert(!auth.IsAuthenticated(204 + LockoutSeconds + SessionLifetimeSeconds));

    auth.Clear();
    assert(!auth.IsConfigured());
    assert(auth.Authenticate("050471", 89, 9999) == Result::NotConfigured);

    std::cout << "parent auth state machine OK; 5 failures => 5m lock; session=15m\n";
}
