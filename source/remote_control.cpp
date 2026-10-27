#include "remote_control.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <fstream>
#include <sstream>
#include <algorithm>

#ifdef NX_PCTL_REMOTE_CONTROL
#include <curl/curl.h>
#include "util/pctl_ops_c.hpp"

namespace
{
struct Config
{
    std::string base_url, device_id, token, ca_file = "/switch/nx_pctl_ca.pem";
    bool apply_policy = false;
};

bool read_config(Config& c)
{
    std::ifstream in("/switch/nx_pctl_server.conf");
    if (!in) return false;
    std::string line;
    while (std::getline(in, line)) {
        auto pos = line.find('=');
        if (pos == std::string::npos || line.empty() || line[0] == '#') continue;
        std::string key = line.substr(0, pos), value = line.substr(pos + 1);
        while (!value.empty() && (value.back() == '\r' || value.back() == ' ')) value.pop_back();
        if (key == "base_url") c.base_url = value;
        else if (key == "device_id") c.device_id = value;
        else if (key == "device_token") c.token = value;
        else if (key == "ca_file") c.ca_file = value;
        else if (key == "apply_policy") c.apply_policy = (value == "1");
    }
    while (!c.base_url.empty() && c.base_url.back() == '/') c.base_url.pop_back();
    std::ifstream ca(c.ca_file);
    return c.base_url.rfind("https://", 0) == 0 &&
           c.base_url.find_first_of("?#") == std::string::npos &&
           !c.device_id.empty() && !c.token.empty() && ca.good();
}

size_t write_body(char* ptr, size_t size, size_t nmemb, void* userdata)
{
    auto* out = static_cast<std::string*>(userdata);
    const size_t bytes = size * nmemb;
    if (out->size() + bytes > 65536) return 0;
    out->append(ptr, bytes);
    return bytes;
}

bool request(const Config& c, const char* path, const char* method,
             const std::string& body, std::string& response, long& status)
{
    CURL* curl = curl_easy_init();
    if (!curl) return false;
    const std::string url = c.base_url + path;
    const std::string auth = "Authorization: Bearer " + c.token;
    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, auth.c_str());
    headers = curl_slist_append(headers, "Content-Type: application/json");
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_CAINFO, c.ca_file.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_body);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 5L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    // libcurl verifies the certificate chain and hostname by default. Never disable either.
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
    if (std::strcmp(method, "POST") == 0) {
        curl_easy_setopt(curl, CURLOPT_POST, 1L);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)body.size());
    }
    const CURLcode rc = curl_easy_perform(curl);
    if (rc == CURLE_OK) curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    return rc == CURLE_OK;
}

bool json_bool(const std::string& json, const char* key, bool& value)
{
    const std::string needle = std::string("\"") + key + "\":";
    auto p = json.find(needle);
    if (p == std::string::npos) return false;
    p += needle.size();
    while (p < json.size() && json[p] == ' ') ++p;
    if (json.compare(p, 4, "true") == 0) { value = true; return true; }
    if (json.compare(p, 5, "false") == 0) { value = false; return true; }
    return false;
}

bool json_optional_int(const std::string& json, const char* key, int& value)
{
    const std::string needle = std::string("\"") + key + "\":";
    auto p = json.find(needle);
    if (p == std::string::npos) return false;
    p += needle.size();
    while (p < json.size() && json[p] == ' ') ++p;
    if (json.compare(p, 4, "null") == 0) return false;
    if (p >= json.size() || json[p] < '0' || json[p] > '9') return false;
    value = 0;
    while (p < json.size() && json[p] >= '0' && json[p] <= '9') {
        value = value * 10 + (json[p++] - '0');
        if (value > 1440) return false;
    }
    return true;
}
}
#endif

namespace remote_control
{
bool authenticate(std::string& message)
{
#ifndef NX_PCTL_REMOTE_CONTROL
    message = "Central control is disabled in this build; local console-PIN check only.";
    return true;
#else
    Config c;
    if (!read_config(c)) {
        message = "Configure the HTTPS server URL and device credentials in /switch/nx_pctl_server.conf.";
        return false;
    }
    if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) {
        message = "Could not initialize HTTPS transport.";
        return false;
    }
    std::string response;
    long status = 0;
    const bool ok = request(c, "/api/v1/auth/check", "GET", "", response, status) && status == 200;
    curl_global_cleanup();
    if (!ok) {
        message = "Central server authentication failed or the server is unreachable. Check HTTPS, credentials, and network.";
        return false;
    }
    message = "Central server authenticated.";
    return true;
#endif
}

SyncResult synchronize()
{
    SyncResult result;
#ifndef NX_PCTL_REMOTE_CONTROL
    result.message = "Remote control is disabled in this build. Rebuild with NX_PCTL_REMOTE_CONTROL=ON.";
    return result;
#else
    Config c;
    if (!read_config(c)) {
        result.message = "Configure /switch/nx_pctl_server.conf with the HTTPS URL and device credentials.";
        return result;
    }
    result.configured = true;
    if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) {
        result.message = "Could not initialize HTTPS transport.";
        return result;
    }

    std::string policy_json;
    long http_status = 0;
    const bool fetched = request(c, "/api/v1/policy", "GET", "", policy_json, http_status);
    if (!fetched || http_status != 200) {
        result.message = "Server unavailable or authentication failed; local settings were left unchanged.";
        curl_global_cleanup();
        return result;
    }
    result.connected = true;
    result.policy_received = true;

    bool enabled = true;
    int limit = -1;
    json_bool(policy_json, "enabled", enabled);
    const bool has_limit = json_optional_int(policy_json, "daily_limit_minutes", limit);
    std::string applied = "[]";
    if (c.apply_policy && enabled && has_limit) {
        // Do not write remote policy unless the console has a PIN and this firmware
        // supports the documented play-timer structure.
        PctlStatus state{};
        pctl_status_fetch(&state);
        if (pctl_fw_play_timer_supported() && state.pin_length_ok && state.pin_length > 0) {
            const Result rc = pctl_play_timer_set_uniform((u16)limit);
            if (R_SUCCEEDED(rc)) {
                result.policy_applied = true;
                applied = "[\"daily_limit_minutes\"]";
            } else {
                result.message = "Policy received, but the daily limit write failed; inspect the local diagnostic log.";
            }
        } else {
            result.message = "Policy received, but applying it requires supported firmware and a registered console PIN.";
        }
    } else if (c.apply_policy && !enabled) {
        result.message = "Policy received; disabled policy does not modify local settings.";
    } else if (c.apply_policy && !has_limit) {
        result.message = "Policy received; no daily limit is configured, so local settings were left unchanged.";
    } else {
        result.message = "Connected. Policy received; automatic application is disabled in the local config.";
    }

    std::ostringstream status_json;
    status_json << "{\"client_version\":\"1.0\",\"app_version\":\"3.1.0\","
                << "\"online\":true,\"last_sync_unix\":" << (long long)time(nullptr)
                << ",\"policy_version\":";
    const auto version_pos = policy_json.find("\"version\":");
    if (version_pos != std::string::npos) {
        auto p = version_pos + 10;
        while (p < policy_json.size() && policy_json[p] == ' ') ++p;
        while (p < policy_json.size() && policy_json[p] >= '0' && policy_json[p] <= '9')
            status_json << policy_json[p++];
    } else status_json << "0";
    status_json << ",\"supported_features\":[\"daily_limit_minutes\"],"
                << "\"applied_features\":" << applied << ",\"warnings\":[]}";

    std::string ignored;
    long status_code = 0;
    request(c, "/api/v1/status", "POST", status_json.str(), ignored, status_code);
    curl_global_cleanup();
    if (result.message.empty()) result.message = result.policy_applied
        ? "Policy synchronized and daily limit applied." : "Policy synchronized; local settings unchanged.";
    return result;
#endif
}
}
