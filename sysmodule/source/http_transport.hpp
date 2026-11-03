#pragma once

#include <stratosphere.hpp>
#include <cstddef>
#include <cstdint>

namespace nxpctl::net {

    enum class Error : u8 {
        None = 0,
        InvalidArgument,
        SocketInitialization,
        ResolutionFailed,
        ConnectFailed,
        SendFailed,
        ReceiveFailed,
        Timeout,
        ResponseTooLarge
    };

    constexpr s32 DefaultHttpPort = 80;
    constexpr s64 DnsTimeoutNs = 5'000'000'000LL;
    constexpr s64 ConnectTimeoutNs = 5'000'000'000LL;
    constexpr s64 IoTimeoutNs = 8'000'000'000LL;

    /*
     * Initializes Atmosphere's system BSD socket client once. The caller owns
     * request/response buffers; this function never allocates those buffers.
     * DNS uses sfdnsres on a dedicated, single-flight resolver thread. The
     * HTTP worker waits at most five seconds; a late resolver is reaped before
     * another lookup can start.
     */
    Error Initialize();
    void Finalize();

    /*
     * Sends one complete HTTP request and reads one complete response until
     * the peer closes the connection. The request must contain
     * "Connection: close". All socket waits are bounded.
     */
    Error HttpPost(const char *host, u16 port,
                   const char *request, std::size_t request_size,
                   char *response, std::size_t response_capacity,
                   std::size_t *out_response_size);

}
