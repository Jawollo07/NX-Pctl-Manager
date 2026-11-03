#include "http_transport.hpp"

#include <stratosphere/socket.hpp>

#include <sys/socket.h>
#include <netdb.h>
#include <poll.h>
#include <fcntl.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>
#include <cstdio>

namespace nxpctl::net {
namespace {

    constexpr std::size_t SocketPoolSize = 512 * 1024;
    alignas(0x1000) u8 g_socket_memory[SocketPoolSize];
    bool g_initialized = false;

    int WaitFor(int fd, short events, int timeout_ms) {
        pollfd descriptor{};
        descriptor.fd = fd;
        descriptor.events = events;
        for (;;) {
            const int result = ::poll(&descriptor, 1, timeout_ms);
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
            const ssize_t count = ::send(fd, data + sent, size - sent, 0);
            if (count < 0) {
                if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) continue;
                return false;
            }
            if (count == 0) return false;
            sent += static_cast<std::size_t>(count);
        }
        return true;
    }

    Error ConnectAddress(const addrinfo *address, int *out_fd) {
        const int fd = ::socket(address->ai_family, address->ai_socktype, address->ai_protocol);
        if (fd < 0) return Error::ConnectFailed;

        const int old_flags = ::fcntl(fd, F_GETFL, 0);
        if (old_flags < 0 || ::fcntl(fd, F_SETFL, old_flags | O_NONBLOCK) < 0) {
            ::close(fd);
            return Error::ConnectFailed;
        }

        int result = ::connect(fd, address->ai_addr, address->ai_addrlen);
        if (result < 0 && errno != EINPROGRESS && errno != EWOULDBLOCK) {
            ::close(fd);
            return Error::ConnectFailed;
        }
        if (result < 0) {
            const int ready = WaitFor(fd, POLLOUT, 5000);
            if (ready <= 0) {
                ::close(fd);
                return ready == 0 ? Error::Timeout : Error::ConnectFailed;
            }
            int socket_error = 0;
            socklen_t error_size = sizeof(socket_error);
            if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &socket_error, &error_size) < 0 ||
                socket_error != 0) {
                ::close(fd);
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

    g_initialized = true;
    return Error::None;
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

    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    addrinfo *addresses = nullptr;
    if (::getaddrinfo(host, service, &hints, &addresses) != 0 || !addresses)
        return Error::ResolutionFailed;

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
            const ssize_t count = ::recv(fd, response + received,
                                         response_capacity - received, 0);
            if (count == 0) break;
            if (count < 0) {
                if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) continue;
                result = Error::ReceiveFailed;
                break;
            }
            received += static_cast<std::size_t>(count);
        }
        *out_response_size = received;
    }

    ::shutdown(fd, SHUT_RDWR);
    ::close(fd);
    return result;
}

}
