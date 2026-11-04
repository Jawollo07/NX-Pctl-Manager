#include "secure_sync.hpp"
#include "secure_http_client.hpp"
#include "credential_store.hpp"
#include "../../protocol/policy_json.hpp"
#include "../../protocol/trusted_clock.hpp"

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
    protocol::ProvisioningRequest g_pending_config{};
    bool g_pending_configured = false;
    std::atomic<bool> g_configured{false};
    std::atomic<bool> g_enrolled{false};
    std::atomic<bool> g_enrollment_in_progress{false};
    char g_device_credential[129]{};
    char g_access_token[257]{};
    u32 g_cycles_since_auth = AuthRefreshCycles;
    bool g_persistence_pending = false;
    std::atomic<bool> g_clock_anchor_valid{false};
    std::atomic<u32> g_clock_anchor_sequence{0};
    std::atomic<s64> g_clock_server_time_anchor{0};
    std::atomic<s64> g_clock_not_after{0};
    std::atomic<u64> g_clock_monotonic_anchor{0};
    std::atomic<s16> g_clock_timezone_offset_minutes{0};
    char g_response[ResponseCapacity];

    u64 GetMonotonicSeconds() {
        const s64 frequency = ams::os::GetSystemTickFrequency();
        const s64 ticks = ams::os::GetSystemTick().GetInt64Value();
        if (frequency <= 0 || ticks < 0) return 0;
        return static_cast<u64>(ticks / frequency);
    }

    class Guard {
    public:
        Guard() { while (g_lock.test_and_set(std::memory_order_acquire)) {} }
        ~Guard() { g_lock.clear(std::memory_order_release); }
    };

    void Wipe(void* data, std::size_t size) {
        volatile u8* p = static_cast<volatile u8*>(data);
        while (size--) *p++ = 0;
    }

    class ResponseWiper {
    public:
        ~ResponseWiper() { Wipe(g_response, sizeof(g_response)); }
        ResponseWiper() = default;
        ResponseWiper(const ResponseWiper&) = delete;
        ResponseWiper& operator=(const ResponseWiper&) = delete;
    };

    class ScopedWipe {
    public:
        ScopedWipe(void* data, std::size_t size) : m_data(data), m_size(size) {}
        ~ScopedWipe() { Wipe(m_data, m_size); }
        ScopedWipe(const ScopedWipe&) = delete;
        ScopedWipe& operator=(const ScopedWipe&) = delete;
    private:
        void* m_data;
        std::size_t m_size;
    };

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
        ResponseWiper response_wiper;
        char payload[256];
        ScopedWipe payload_wiper(payload, sizeof(payload));
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

        credential_store::CredentialData stored{};
        std::memcpy(stored.api_host, g_config.api_host, sizeof(stored.api_host));
        std::memcpy(stored.device_id, g_config.device_id, sizeof(stored.device_id));
        std::memcpy(stored.device_credential, g_device_credential, sizeof(stored.device_credential));
        g_persistence_pending = !credential_store::Save(stored);
        Wipe(&stored, sizeof(stored));

        Wipe(credential, sizeof(credential));
        Wipe(g_config.enrollment_code, sizeof(g_config.enrollment_code));
        g_enrolled.store(true, std::memory_order_release);
        g_cycles_since_auth = AuthRefreshCycles;
        return true;
    }

    bool Authenticate() {
        ResponseWiper response_wiper;
        if (!g_enrolled || g_device_credential[0] == '\0') return false;
        char payload[256];
        ScopedWipe payload_wiper(payload, sizeof(payload));
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
        ResponseWiper response_wiper;
        if (!g_access_token[0]) return false;
        char payload[512];
        ScopedWipe payload_wiper(payload, sizeof(payload));
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
        ResponseWiper response_wiper;
        if (!g_access_token[0]) return false;
        char payload[384];
        ScopedWipe payload_wiper(payload, sizeof(payload));
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
        if (store.TryReplace(candidate, context) != policy::ValidationError::None)
            return false;

        // Anchor the backend-authenticated timestamp to the monotonic clock.
        // Device wall-clock changes cannot move the usage day backwards.
        const u64 monotonic_anchor = GetMonotonicSeconds();
        if (monotonic_anchor == 0) {
            g_clock_anchor_valid.store(false, std::memory_order_release);
            return true;
        }
        g_clock_anchor_sequence.fetch_add(1, std::memory_order_acq_rel); // writer lock: odd
        g_clock_anchor_valid.store(false, std::memory_order_relaxed);
        g_clock_server_time_anchor.store(candidate.server_time, std::memory_order_relaxed);
        g_clock_not_after.store(candidate.not_after, std::memory_order_relaxed);
        g_clock_timezone_offset_minutes.store(candidate.timezone_offset_minutes, std::memory_order_relaxed);
        g_clock_monotonic_anchor.store(monotonic_anchor, std::memory_order_relaxed);
        g_clock_anchor_valid.store(true, std::memory_order_relaxed);
        g_clock_anchor_sequence.fetch_add(1, std::memory_order_release); // publish: even
        return true;
    }

    void AcknowledgePolicy(u64 revision) {
        ResponseWiper response_wiper;
        if (!g_access_token[0] || revision == 0) return;
        char payload[512];
        ScopedWipe payload_wiper(payload, sizeof(payload));
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

bool Initialize() {
    g_clock_anchor_valid.store(false, std::memory_order_release);
    g_clock_server_time_anchor.store(0, std::memory_order_relaxed);
    g_clock_not_after.store(0, std::memory_order_relaxed);
    g_clock_monotonic_anchor.store(0, std::memory_order_relaxed);
    g_clock_timezone_offset_minutes.store(0, std::memory_order_relaxed);
    const bool storage_available = credential_store::Initialize();
    credential_store::CredentialData stored{};
    if (storage_available && credential_store::Load(&stored)) {
        Wipe(&g_config, sizeof(g_config));
        std::memcpy(g_config.api_host, stored.api_host, sizeof(g_config.api_host));
        std::memcpy(g_config.device_id, stored.device_id, sizeof(g_config.device_id));
        std::memcpy(g_device_credential, stored.device_credential, sizeof(g_device_credential));
        g_configured.store(true, std::memory_order_release);
        g_enrolled.store(true, std::memory_order_release);
        g_cycles_since_auth = AuthRefreshCycles;
    }
    Wipe(&stored, sizeof(stored));
    return storage_available;
}

void Finalize() {
    credential_store::Finalize();
}

void Configure(const protocol::ProvisioningRequest& request) {
    /*
     * HIPC must never wait for DNS/TCP/HTTP while the worker is synchronizing.
     * Queue a bounded copy here; the worker adopts it at the start of its next
     * cycle. The lock is held only for this fixed-size memory operation.
     */
    Guard guard;
    if (g_enrolled.load(std::memory_order_acquire) ||
        g_enrollment_in_progress.load(std::memory_order_acquire)) return;
    Wipe(&g_pending_config, sizeof(g_pending_config));
    g_pending_config = request;
    g_pending_configured = true;
    g_configured.store(true, std::memory_order_release);
}

bool IsConfigured() {
    return g_configured.load(std::memory_order_acquire);
}

bool IsEnrolled() {
    return g_enrolled.load(std::memory_order_acquire) ||
           g_enrollment_in_progress.load(std::memory_order_acquire);
}

bool GetTrustedLocalDayKey(u64 monotonic_seconds, s64* out_day_key) {
    if (out_day_key == nullptr) return false;

    trusted_clock::Anchor anchor{};
    for (unsigned attempt = 0; attempt < 3; ++attempt) {
        const u32 before = g_clock_anchor_sequence.load(std::memory_order_acquire);
        if ((before & 1u) != 0) continue;

        anchor.valid = g_clock_anchor_valid.load(std::memory_order_relaxed);
        anchor.server_time = g_clock_server_time_anchor.load(std::memory_order_relaxed);
        anchor.not_after = g_clock_not_after.load(std::memory_order_relaxed);
        anchor.monotonic_seconds = g_clock_monotonic_anchor.load(std::memory_order_relaxed);
        anchor.timezone_offset_minutes =
            g_clock_timezone_offset_minutes.load(std::memory_order_relaxed);

        const u32 after = g_clock_anchor_sequence.load(std::memory_order_acquire);
        if (before == after && (after & 1u) == 0) {
            return trusted_clock::GetLocalDayKey(anchor, monotonic_seconds, out_day_key);
        }
    }
    return false;
}

void RunCycle(policy::PolicyStore& store, u64 uptime_seconds) {
    /*
     * Adopt provisioning and reserve the enrollment transition atomically.
     * The lock is released before DNS/TCP/HTTP, so HIPC remains responsive.
     */
    bool should_enroll = false;
    bool configured = false;
    {
        Guard guard;
        if (g_pending_configured) {
            Wipe(&g_config, sizeof(g_config));
            Wipe(g_device_credential, sizeof(g_device_credential));
            Wipe(g_access_token, sizeof(g_access_token));
            g_config = g_pending_config;
            Wipe(&g_pending_config, sizeof(g_pending_config));
            g_pending_configured = false;
            g_enrolled.store(false, std::memory_order_release);
            g_cycles_since_auth = AuthRefreshCycles;
        }
        configured = g_configured.load(std::memory_order_acquire);
        if (configured && !g_enrolled.load(std::memory_order_acquire)) {
            g_enrollment_in_progress.store(true, std::memory_order_release);
            should_enroll = true;
        }
    }
    if (!configured) return;

    if (should_enroll) {
        const bool enrolled = Enroll();
        g_enrollment_in_progress.store(false, std::memory_order_release);
        if (!enrolled) return;
    }

    if (g_persistence_pending && g_enrolled.load(std::memory_order_acquire)) {
        credential_store::CredentialData stored{};
        std::memcpy(stored.api_host, g_config.api_host, sizeof(stored.api_host));
        std::memcpy(stored.device_id, g_config.device_id, sizeof(stored.device_id));
        std::memcpy(stored.device_credential, g_device_credential, sizeof(stored.device_credential));
        g_persistence_pending = !credential_store::Save(stored);
        Wipe(&stored, sizeof(stored));
    }
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
    Wipe(&g_pending_config, sizeof(g_pending_config));
    Wipe(g_device_credential, sizeof(g_device_credential));
    Wipe(g_access_token, sizeof(g_access_token));
    g_pending_configured = false;
    g_configured.store(false, std::memory_order_release);
    g_enrolled.store(false, std::memory_order_release);
    g_enrollment_in_progress.store(false, std::memory_order_release);
    g_persistence_pending = false;
    g_cycles_since_auth = AuthRefreshCycles;
    g_clock_anchor_valid.store(false, std::memory_order_release);
    g_clock_server_time_anchor.store(0, std::memory_order_relaxed);
    g_clock_not_after.store(0, std::memory_order_relaxed);
    g_clock_monotonic_anchor.store(0, std::memory_order_relaxed);
    g_clock_timezone_offset_minutes.store(0, std::memory_order_relaxed);
}

}
