#include "../protocol/http_response.hpp"

#include <cassert>
#include <cstring>

using nxpctl::http::Error;
using nxpctl::http::ParseResponse;
using nxpctl::http::ResponseView;

int main() {
    {
        constexpr char response[] = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: 8\r\n\r\n{\"ok\":1}";
        ResponseView view{};
        assert(ParseResponse(response, sizeof(response) - 1, &view) == Error::None);
        assert(view.status_code == 200);
        assert(view.body_length == 8);
        assert(std::memcmp(view.body, "{\"ok\":1}", 8) == 0);
    }
    {
        constexpr char response[] = "HTTP/1.0 410 Gone\r\ncontent-length: 0\r\n\r\n";
        ResponseView view{};
        assert(ParseResponse(response, sizeof(response) - 1, &view) == Error::None);
        assert(view.status_code == 410 && view.body_length == 0);
    }
    {
        constexpr char response[] = "HTTP/1.1 200 OK\r\nContent-Length: 1\r\ncontent-length: 1\r\n\r\nx";
        ResponseView view{};
        assert(ParseResponse(response, sizeof(response) - 1, &view) == Error::DuplicateContentLength);
    }
    {
        constexpr char response[] = "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\nContent-Length: 1\r\n\r\nx";
        ResponseView view{};
        assert(ParseResponse(response, sizeof(response) - 1, &view) == Error::UnsupportedTransferEncoding);
    }
    {
        constexpr char response[] = "HTTP/1.1 200 OK\r\nContent-Length: 1\r\n\r\nxy";
        ResponseView view{};
        assert(ParseResponse(response, sizeof(response) - 1, &view) == Error::LengthMismatch);
    }
    {
        constexpr char response[] = "HTTP/1.1 200 OK\r\nContent-Length: 3\r\n\r\nx";
        ResponseView view{};
        assert(ParseResponse(response, sizeof(response) - 1, &view) == Error::Incomplete);
    }
    {
        constexpr char response[] = "HTTP/1.1 200 OK\r\n\r\n";
        ResponseView view{};
        assert(ParseResponse(response, sizeof(response) - 1, &view) == Error::MissingContentLength);
    }
    {
        constexpr char response[] = "HTTP/1.1 200 OK\r\nContent-Length: 98305\r\n\r\n";
        ResponseView view{};
        assert(ParseResponse(response, sizeof(response) - 1, &view) == Error::BodyTooLarge);
    }
    {
        constexpr char response[] = "HTTP/1.1 200 OK\nContent-Length: 0\n\n";
        ResponseView view{};
        assert(ParseResponse(response, sizeof(response) - 1, &view) == Error::InvalidStatusLine);
    }
    {
        ResponseView view{};
        assert(ParseResponse(nullptr, 0, &view) == Error::NullInput);
        assert(ParseResponse("", 0, &view) == Error::Incomplete);
    }
    return 0;
}
