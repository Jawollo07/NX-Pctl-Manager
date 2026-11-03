#include "secure_sync.hpp"
#include "secure_http_client.hpp"
#include "../../protocol/policy_json.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace nxpctl::sync {
namespace {
    constexpr u16 ApiPort = 80;
    constexpr u32 AuthRefreshCycles = 10; // Worker cadence is one minute.
    constexpr std::size_t ResponseCapacity = 20 * 1024;

    std::atomic_flag g_lock = ATOMIC_FLAG_INIT;
    protocol::ProvisioningRequest g_config{};
    bool g_configured = false;
    bool g_enrolled = false;
    char g_device_credential[129]{};
    char g_access_token[257]{};
    u32 g_cycles_since_auth = AuthRefreshCycles;
    char g_response[ResponseCapacity];

    class Guard {
    public:
        Guard() { while (g_lock.test_and_set(std::memory_order_acquire)) {} }
        ~Guard() { g_lock.clear(std::memory_order_release); }
    };

    void Wipe(void* data, std::size_t size) {
        volatile u8* p = static_cast<volatile u8*>(data);
        while (size--) *p++ = 0;
    }

    bool ExtractString(const char* json, std::size_t length, const char* key,
                       char* output, std::size_t capacity) {
        char pattern[80];
        const int n = std::snprintf(pattern, sizeof(pattern), "\"%s\":\"", key);
        if (n <= 0 || static_cast<std::size_t>(n) >= sizeof(pattern)) return false;
        const std::size_t pattern_size = static_cast<std::size_t>(n);
        for (std::size_t i = 0; i + pattern_size <= length; ++i) {
            if (std::memcmp(json + i, pattern, pattern_size) != 0) continue;
            const std::size_t start = i + pattern_size;
            std::size_t end = start;
            while (end < length && json[end] != '"') {
                const unsigned char c = static_cast<unsigned char>(json[end]);
                if (c < 0x21 || c > 0x7e || c == '\\') return false;
                ++end;
            }
            if (end >= length || end - start == 0 || end - start >= capacity) return false;
            std::memcpy(output, json + start, end - start);
            output[end - start] = '\0';
            return true;
        }
        return false;
    }

    bool ExtractUnsigned(const char* json, std::size_t length, const char* key,
                         u64* output) {
        char pattern[80];
        const int n = std::snprintf(pattern, sizeof(pattern), "\"%s\":", key);
        if (n <= 0 || static_cast<std::size_t>(n) >= sizeof(pattern)) return false;
        const std::size_t pattern_size = static_cast<std::size_t>(n);
        for (std::size_t i = 0; i + pattern_size <= length; ++i) {
            if (std::memcmp(json + i, pattern, pattern_size) != 0) continue;
            std::size_t p = i + pattern_size;
            while (p < length && (json[p] == ' ' || json[p] == '\n')) ++p;
            if (p >= length || json[p] < '0' || json[p] > '9') return false;
            u64 value = 0;
            do {
                const u32 digit = static_cast<u32>(json[p++] - '0');
                if (value > (UINT64_MAX - digit) / 10) return false;
                value = value * 10 + digit;
            } while (p < length && json[p] >= '0' && json[p] <= '9');
            *output = value;
            return true;
        }
        return false;
    }

    bool Post(const char* operation, const char* secret, const char* purpose,
              const char* payload, std::size_t payload_size, u16* status,
              std::size_t* response_size) {
        return secure_http::Post(
            g_config.api_host, ApiPort, g_config.device_id, operation,
            secret, std::strlen(secret), purpose, payload, payload_size,
            g_response, sizeof(g_response), response_size, status) ==
            secure_http::Error::None;
    }

    bool Enroll() {
        char payload[256];
        const int n = std::snprintf(payload, sizeof(payload),
            "{\"enrollment_code\":\"%s\"}", g_config.enrollment_code);
        if (n <= 0 || static_cast<std::size_t>(n) >= sizeof(payload)) return false;
        u16 status = 0;
        std::size_t response_size = 0;
        if (!Post("enroll", g_config.enrollment_code, "enrollment",
                  payload, static_cast<std::size_t>(n), &status, &response_size) ||
            status != 200) return false;

        char credential[129]{};
        if (!ExtractString(g_response, response_size, "device_credential",
                           credential, sizeof(credential))) return false;
        std::memcpy(g_device_credential, credential, std::strlen(credential) + 1);
        Wipe(credential, sizeof(credential));
        Wipe(g_config.enrollment_code, sizeof(g_config.enrollment_code));
        g_enrolled = true;
        g_cycles_since_auth = AuthRefreshCycles;
        return true;
    }

    bool Authenticate() {
        if (!g_enrolled || g_device_credential[0] == '\0') return false;
        char payload[256];
        const int n = std::snprintf(payload, sizeof(payload),
            "{\"device_credential\":\"%s\"}", g_device_credential);
        if (n <= 0 || static_cast<std::size_t>(n) >= sizeof(payload)) return false;
        u16 status = 0;
        std::size_t response_size = 0;
        if (!Post("auth", g_device_credential, "device",
                  payload, static_cast<std::size_t>(n), &status, &response_size) ||
            status != 200) return false;
        char token[257]{};
        if (!ExtractString(g_response, response_size, "access_token",
                           token, sizeof(token))) return false;
        std::memcpy(g_access_token, token, std::strlen(token) + 1);
        Wipe(token, sizeof(token));
        g_cycles_since_auth = 0;
        return true;
    }

    bool SendHeartbeat(u64 uptime_seconds, u64 policy_revision) {
        if (!g_access_token[0]) return false;
        char payload[512];
        const int n = std::snprintf(payload, sizeof(payload),
            "{\"access_token\":\"%s\",\"protocol_version\":1,"
            "\"uptime_seconds\":%llu,\"policy_revision\":%llu}",
            g_access_token,
            static_cast<unsigned long long>(uptime_seconds),
            static_cast<unsigned long long>(policy_revision));
        if (n <= 0 || static_cast<std::size_t>(n) >= sizeof(payload)) return false;
        u16 status = 0;
        std::size_t response_size = 0;
        return Post("heartbeat", g_device_credential, "device",
                    payload, static_cast<std::size_t>(n), &status, &response_size) &&
               status == 200;
    }

    bool FetchPolicy(policy::PolicyStore& store) {
        if (!g_access_token[0]) return false;
        char payload[384];
        const int n = std::snprintf(payload, sizeof(payload),
            "{\"access_token\":\"%s\"}", g_access_token);
        if (n <= 0 || static_cast<std::size_t>(n) >= sizeof(payload)) return false;
        u16 status = 0;
        std::size_t response_size = 0;
        if (!Post("policy", g_device_credential, "device",
                  payload, static_cast<std::size_t>(n), &status, &response_size)) return false;
        if (status != 200) return false;

        u64 server_time = 0;
        if (!ExtractUnsigned(g_response, response_size, "server_time", &server_time) ||
            server_time == 0) return false;
        policy::ValidationContext context{};
        context.expected_device_id = g_config.device_id;
        context.trusted_server_time = static_cast<std::int64_t>(server_time);
        context.minimum_revision = store.GetRevision();
        policy::Policy candidate{};
        if (policy::json::DecodeAndValidate(
                g_response, response_size, context, &candidate) != policy::json::Error::None)
            return false;
        return store.TryReplace(candidate, context) == policy::ValidationError::None;
    }

    void AcknowledgePolicy(u64 revision) {
        if (!g_access_token[0] || revision == 0) return;
        char payload[512];
        const int n = std::snprintf(payload, sizeof(payload),
            "{\"access_token\":\"%s\",\"revision\":%llu,"
            "\"status\":\"accepted\",\"message\":\"validated\"}",
            g_access_token, static_cast<unsigned long long>(revision));
        if (n <= 0 || static_cast<std::size_t>(n) >= sizeof(payload)) return;
        u16 status = 0;
        std::size_t response_size = 0;
        (void)Post("policy-ack", g_device_credential, "device",
                   payload, static_cast<std::size_t>(n), &status, &response_size);
    }
}

void Configure(const protocol::ProvisioningRequest& request) {
    Guard guard;
    Wipe(&g_config, sizeof(g_config));
    Wipe(g_device_credential, sizeof(g_device_credential));
    Wipe(g_access_token, sizeof(g_access_token));
    g_config = request;
    g_configured = true;
    g_enrolled = false;
    g_cycles_since_auth = AuthRefreshCycles;
}

bool IsConfigured() {
    Guard guard;
    return g_configured;
}

bool IsEnrolled() {
    Guard guard;
    return g_enrolled;
}

void RunCycle(policy::PolicyStore& store, u64 uptime_seconds) {
    Guard guard;
    if (!g_configured) return;

    if (!g_enrolled && !Enroll()) return;
    if (g_access_token[0] == '\0' || g_cycles_since_auth >= AuthRefreshCycles) {
        Wipe(g_access_token, sizeof(g_access_token));
        if (!Authenticate()) return;
    }

    const u64 revision = store.GetRevision();
    (void)SendHeartbeat(uptime_seconds, revision);
    if (FetchPolicy(store)) {
        AcknowledgePolicy(store.GetRevision());
    }
    ++g_cycles_since_auth;
}

void Clear() {
    Guard guard;
    Wipe(&g_config, sizeof(g_config));
    Wipe(g_device_credential, sizeof(g_device_credential));
    Wipe(g_access_token, sizeof(g_access_token));
    g_configured = false;
    g_enrolled = false;
    g_cycles_since_auth = AuthRefreshCycles;
}

}
