#include "http_transport.hpp"

#include <stratosphere/os.hpp>
#include <stratosphere/socket.hpp>

extern "C" {
#include <switch/services/bsd.h>
}

#include <netdb.h>
#include <fcntl.h>
#include <poll.h>
#include <cerrno>
#include <cstring>
#include <cstdio>

namespace nxpctl::net {
namespace {

    namespace os = ams::os;
    using ams::TimeSpan;

    constexpr std::size_t SocketPoolSize = 512 * 1024;
    alignas(0x1000) u8 g_socket_memory[SocketPoolSize];
    bool g_initialized = false;

    /*
     * getaddrinfo() is synchronous and its internal sfdnsres wait cannot be
     * bounded with SO_RCVTIMEO/poll on the eventual TCP socket. Run exactly
     * one resolver request on a dedicated thread instead. If it exceeds the
     * caller's deadline, the request is left alive and owns all of its state;
     * subsequent callers fail fast until it exits and can be reaped safely.
     */
    constexpr s64 DnsTimeoutNs = 5'000'000'000LL;
    constexpr size_t ResolverStackSize = 0x8000;
    alignas(os::ThreadStackAlignment) u8 g_resolver_stack[ResolverStackSize];
    os::ThreadType g_resolver_thread;
    os::EventType g_resolver_event;
    bool g_resolver_event_initialized = false;
    bool g_resolver_thread_created = false;
    int g_resolver_result = EAI_FAIL;
    addrinfo *g_resolver_addresses = nullptr;
    char g_resolver_host[254]{};
    char g_resolver_service[6]{};

    void ResolverThreadMain(void *) {
        addrinfo hints{};
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        hints.ai_protocol = IPPROTO_TCP;

        g_resolver_addresses = nullptr;
        g_resolver_result = ::getaddrinfo(
            g_resolver_host, g_resolver_service, &hints, &g_resolver_addresses);

        /* Publish result and pointer before waking the waiting HTTP worker. */
        os::SignalEvent(std::addressof(g_resolver_event));
    }

    void ReapResolverThread() {
        if (!g_resolver_thread_created) return;

        os::WaitThread(std::addressof(g_resolver_thread));
        os::DestroyThread(std::addressof(g_resolver_thread));
        g_resolver_thread_created = false;
    }

    Error ResolveAddresses(const char *host, const char *service, addrinfo **out) {
        *out = nullptr;

        if (g_resolver_thread_created) {
            if (!os::TryWaitThread(std::addressof(g_resolver_thread))) {
                /* A timed-out DNS call is still in flight; never spawn another. */
                return Error::Timeout;
            }

            ReapResolverThread();
            if (g_resolver_addresses != nullptr) {
                ::freeaddrinfo(g_resolver_addresses);
                g_resolver_addresses = nullptr;
            }
        }

        const size_t host_length = std::strlen(host);
        const size_t service_length = std::strlen(service);
        if (host_length == 0 || host_length >= sizeof(g_resolver_host) ||
            service_length == 0 || service_length >= sizeof(g_resolver_service)) {
            return Error::InvalidArgument;
        }

        std::memset(g_resolver_host, 0, sizeof(g_resolver_host));
        std::memset(g_resolver_service, 0, sizeof(g_resolver_service));
        std::memcpy(g_resolver_host, host, host_length);
        std::memcpy(g_resolver_service, service, service_length);
        g_resolver_addresses = nullptr;
        g_resolver_result = EAI_FAIL;
        os::ClearEvent(std::addressof(g_resolver_event));

        const ams::Result create_result = os::CreateThread(
            std::addressof(g_resolver_thread),
            ResolverThreadMain,
            nullptr,
            g_resolver_stack,
            sizeof(g_resolver_stack),
            31,
            3);
        if (R_FAILED(create_result)) return Error::ResolutionFailed;

        g_resolver_thread_created = true;
        os::SetThreadNamePointer(
            std::addressof(g_resolver_thread), "ams.NxPctlManager.DNS");
        os::StartThread(std::addressof(g_resolver_thread));

        if (!os::TimedWaitEvent(
                std::addressof(g_resolver_event),
                TimeSpan::FromNanoSeconds(DnsTimeoutNs))) {
            /*
             * Do not terminate a thread inside getaddrinfo(): its resolver
             * internals may still own service/session memory. The thread and
             * its static buffers remain valid until a later call reaps it.
             */
            return Error::Timeout;
        }

        ReapResolverThread();
        if (g_resolver_result != 0 || g_resolver_addresses == nullptr) {
            if (g_resolver_addresses != nullptr) {
                ::freeaddrinfo(g_resolver_addresses);
                g_resolver_addresses = nullptr;
            }
            return Error::ResolutionFailed;
        }

        *out = g_resolver_addresses;
        g_resolver_addresses = nullptr;
        return Error::None;
    }

    int WaitFor(int fd, short events, int timeout_ms) {
        pollfd descriptor{};
        descriptor.fd = fd;
        descriptor.events = events;
        for (;;) {
            const int result = ::bsdPoll(&descriptor, 1, timeout_ms);
            if (result > 0) {
                if (descriptor.revents & (POLLERR | POLLNVAL)) return -1;
                if (descriptor.revents & (events | POLLHUP)) return 1;
                continue;
            }
            if (result == 0) return 0;
            if (errno != EINTR) return -1;
        }
    }

    bool SendAll(int fd, const char *data, std::size_t size) {
        std::size_t sent = 0;
        while (sent < size) {
            const int ready = WaitFor(fd, POLLOUT, 8000);
            if (ready <= 0) return false;
            const ssize_t count = ams::socket::Send(fd, data + sent, size - sent,
                                                     ams::socket::MsgFlag::Msg_None);
            if (count < 0) {
                const auto error = ams::socket::GetLastError();
                if (error == ams::socket::Errno::EIntr ||
                    error == ams::socket::Errno::EAgain) continue;
                return false;
            }
            if (count == 0) return false;
            sent += static_cast<std::size_t>(count);
        }
        return true;
    }

    Error ConnectAddress(const addrinfo *address, int *out_fd) {
        const int fd = ams::socket::Socket(
            static_cast<ams::socket::Family>(address->ai_family),
            static_cast<ams::socket::Type>(address->ai_socktype),
            static_cast<ams::socket::Protocol>(address->ai_protocol));
        if (fd < 0) return Error::ConnectFailed;

        const int old_flags = ::bsdFcntl(fd, F_GETFL, 0);
        if (old_flags < 0 || ::bsdFcntl(fd, F_SETFL, old_flags | O_NONBLOCK) < 0) {
            ams::socket::Close(fd);
            return Error::ConnectFailed;
        }

        const auto *sock_address =
            reinterpret_cast<const ams::socket::SockAddr *>(address->ai_addr);
        int result = ams::socket::Connect(fd, sock_address,
                                          static_cast<ams::socket::SockLenT>(address->ai_addrlen));
        if (result < 0) {
            const auto error = ams::socket::GetLastError();
            if (error != ams::socket::Errno::EInProgress &&
                error != ams::socket::Errno::EAgain) {
                ams::socket::Close(fd);
                return Error::ConnectFailed;
            }
            const int ready = WaitFor(fd, POLLOUT, 5000);
            if (ready <= 0) {
                ams::socket::Close(fd);
                return ready == 0 ? Error::Timeout : Error::ConnectFailed;
            }
            int socket_error = 0;
            socklen_t error_size = sizeof(socket_error);
            if (::bsdGetSockOpt(fd, SOL_SOCKET, SO_ERROR, &socket_error, &error_size) < 0 ||
                socket_error != 0) {
                ams::socket::Close(fd);
                return Error::ConnectFailed;
            }
        }

        *out_fd = fd;
        return Error::None;
    }

}

Error Initialize() {
    if (g_initialized) return Error::None;

    ams::socket::SystemConfigLightDefault config(
        g_socket_memory,
        sizeof(g_socket_memory),
        ams::socket::SystemConfigLightDefault::DefaultAllocatorPoolSize,
        1);
    const auto result = ams::socket::Initialize(config);
    if (R_FAILED(result)) return Error::SocketInitialization;

    os::InitializeEvent(
        std::addressof(g_resolver_event), false, os::EventClearMode_ManualClear);
    g_resolver_event_initialized = true;
    g_initialized = true;
    return Error::None;
}

void Finalize() {
    if (!g_initialized) return;

    /*
     * The BSD resolver may still be using its service session after a DNS
     * timeout. Join it before finalizing the socket service.
     */
    if (g_resolver_thread_created) ReapResolverThread();
    if (g_resolver_addresses != nullptr) {
        ::freeaddrinfo(g_resolver_addresses);
        g_resolver_addresses = nullptr;
    }
    if (g_resolver_event_initialized) {
        os::FinalizeEvent(std::addressof(g_resolver_event));
        g_resolver_event_initialized = false;
    }

    ams::socket::Finalize();
    g_initialized = false;
}

Error HttpPost(const char *host, u16 port,
               const char *request, std::size_t request_size,
               char *response, std::size_t response_capacity,
               std::size_t *out_response_size) {
    if (!host || !*host || !request || request_size == 0 ||
        !response || response_capacity == 0 || !out_response_size) {
        return Error::InvalidArgument;
    }
    *out_response_size = 0;

    const Error init_error = Initialize();
    if (init_error != Error::None) return init_error;

    char service[6];
    const int service_length = std::snprintf(service, sizeof(service), "%u", port);
    if (service_length <= 0 || static_cast<std::size_t>(service_length) >= sizeof(service))
        return Error::InvalidArgument;

    addrinfo *addresses = nullptr;
    const Error resolution = ResolveAddresses(host, service, &addresses);
    if (resolution != Error::None) return resolution;

    int fd = -1;
    Error connect_error = Error::ConnectFailed;
    for (const addrinfo *candidate = addresses; candidate; candidate = candidate->ai_next) {
        connect_error = ConnectAddress(candidate, &fd);
        if (connect_error == Error::None) break;
    }
    ::freeaddrinfo(addresses);
    if (fd < 0) return connect_error;

    Error result = Error::None;
    if (!SendAll(fd, request, request_size)) {
        result = Error::SendFailed;
    } else {
        std::size_t received = 0;
        for (;;) {
            const int ready = WaitFor(fd, POLLIN, 8000);
            if (ready == 0) { result = Error::Timeout; break; }
            if (ready < 0) { result = Error::ReceiveFailed; break; }

            if (received == response_capacity) {
                result = Error::ResponseTooLarge;
                break;
            }
            const ssize_t count = ams::socket::Recv(
                fd, response + received, response_capacity - received,
                ams::socket::MsgFlag::Msg_None);
            if (count == 0) break;
            if (count < 0) {
                const auto error = ams::socket::GetLastError();
                if (error == ams::socket::Errno::EIntr ||
                    error == ams::socket::Errno::EAgain) continue;
                result = Error::ReceiveFailed;
                break;
            }
            received += static_cast<std::size_t>(count);
        }
        *out_response_size = received;
    }

    ams::socket::Shutdown(fd, ams::socket::ShutdownMethod::Shut_RdWr);
    ams::socket::Close(fd);
    return result;
}

}
