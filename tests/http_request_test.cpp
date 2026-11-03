#include "../protocol/http_request.hpp"

#include <cassert>
#include <cstring>

using nxpctl::http::BuildRequest;
using nxpctl::http::RequestError;
using nxpctl::http::RequestView;

int main() {
    char buffer[4096]{};
    RequestView request{};
    {
        constexpr char body[] = "{\"device_id\":\"switch-a\",\"enrollment_code\":\"abc\"}";
        assert(BuildRequest("POST", "api.example.net", "/v1/devices/enroll",
            nullptr, body, sizeof(body)-1, buffer, sizeof(buffer), &request) == RequestError::None);
        assert(request.data[request.length] == '\0');
        assert(std::strstr(request.data, "POST /v1/devices/enroll HTTP/1.1\r\n") == request.data);
        assert(std::strstr(request.data, "Host: api.example.net\r\n") != nullptr);
        assert(std::strstr(request.data, "Content-Type: application/json\r\n") != nullptr);
        assert(std::strstr(request.data, "Content-Length: 48\r\n") != nullptr);
        assert(std::memcmp(request.data + request.length - (sizeof(body)-1), body, sizeof(body)-1) == 0);
    }
    {
        assert(BuildRequest("GET", "api.example.net", "/v1/devices/switch-a/policy",
            "token-123", nullptr, 0, buffer, sizeof(buffer), &request) == RequestError::None);
        assert(std::strstr(request.data, "Authorization: Bearer token-123\r\n") != nullptr);
        assert(std::strstr(request.data, "Content-Length: 0\r\n\r\n") != nullptr);
        assert(std::strstr(request.data, "Content-Type:") == nullptr);
    }
    {
        assert(BuildRequest("DELETE", "api.example.net", "/", nullptr, nullptr, 0,
            buffer, sizeof(buffer), &request) == RequestError::InvalidMethod);
        assert(BuildRequest("GET", "api.example.net\r\nX-Evil: yes", "/", nullptr, nullptr, 0,
            buffer, sizeof(buffer), &request) == RequestError::InvalidHost);
        assert(BuildRequest("GET", "api.example.net", "/\r\nInjected: yes", nullptr, nullptr, 0,
            buffer, sizeof(buffer), &request) == RequestError::InvalidPath);
        assert(BuildRequest("GET", "api.example.net", "/", "token\r\nInjected: yes", nullptr, 0,
            buffer, sizeof(buffer), &request) == RequestError::InvalidToken);
    }
    {
        char small[8]{};
        assert(BuildRequest("GET", "api.example.net", "/", nullptr, nullptr, 0,
            small, sizeof(small), &request) == RequestError::OutputTooSmall);
    }
    {
        assert(BuildRequest("POST", "api.example.net", "/", nullptr, "x", 98305,
            buffer, sizeof(buffer), &request) == RequestError::BodyTooLarge);
    }
    return 0;
}
