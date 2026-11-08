#include <switch.h>

#include <cstdio>
#include <cstring>

#include "../../../protocol/provisioning.hpp"

namespace {

constexpr const char *ProvisioningPath = "sdmc:/nxpctl.conf";
constexpr const char *StatusPath = "sdmc:/nxpctl-provision.status";

void Wipe(void *data, std::size_t size) {
    volatile unsigned char *p = static_cast<volatile unsigned char *>(data);
    while (size-- != 0) *p++ = 0;
}

void WriteStatus(const char *state, Result result) {
    if (FILE *file = std::fopen(StatusPath, "w")) {
        std::fprintf(file, "state=%s\nresult=0x%08X\n", state,
                     static_cast<unsigned int>(result));
        std::fclose(file);
    }
}

} // namespace

int main() {
    /* libnx mounts sdmc during process initialization; do not mount it twice. */
    if (fsdevGetDeviceFileSystem("sdmc") == nullptr) return 10;

    Result result = 0;
    char config[::nxpctl::protocol::ProvisioningConfigMaxSize + 1]{};
    ::nxpctl::protocol::ProvisioningRequest request{};
    FILE *file = std::fopen(ProvisioningPath, "rb");
    if (file == nullptr) {
        WriteStatus("missing-config-file", MAKERESULT(Module_Libnx, LibnxError_NotFound));
        return 11;
    }

    const std::size_t bytes_read = std::fread(config, 1, sizeof(config), file);
    const bool read_error = std::ferror(file) != 0;
    std::fclose(file);

    if (read_error || bytes_read == 0 ||
        bytes_read > ::nxpctl::protocol::ProvisioningConfigMaxSize ||
        !::nxpctl::protocol::ParseProvisioningConfig(config, bytes_read, request)) {
        Wipe(config, sizeof(config));
        Wipe(&request, sizeof(request));
        WriteStatus("invalid-config-file", MAKERESULT(Module_Libnx, LibnxError_BadInput));
        return 12;
    }
    Wipe(config, sizeof(config));

    Service service{};
    result = smGetService(&service, ::nxpctl::protocol::ServiceName);
    if (R_FAILED(result)) {
        Wipe(&request, sizeof(request));
        WriteStatus("sysmodule-service-unavailable", result);
        return 13;
    }

    result = serviceDispatch(
        &service,
        ::nxpctl::protocol::ProvisioningCommandId,
        .buffer_attrs = { SfBufferAttr_HipcMapAlias | SfBufferAttr_In },
        .buffers = { { &request, sizeof(request) } }
    );
    serviceClose(&service);
    Wipe(&request, sizeof(request));

    if (R_FAILED(result)) {
        WriteStatus("sysmodule-rejected-config", result);
        return 14;
    }

    /* The enrollment code is one-time. Remove the config after HIPC accepts it. */
    if (std::remove(ProvisioningPath) != 0) {
        WriteStatus("accepted-but-config-removal-failed", result);
        return 15;
    }
    (void)fsdevCommitDevice("sdmc");
    WriteStatus("accepted-by-sysmodule", result);
    return 0;
}
