#include <switch.h>

#include <cstdio>
#include <cstring>

#include "../../../protocol/provisioning.hpp"
#include "../../../protocol/runtime_status.hpp"

namespace {

constexpr const char *ProvisioningPath = "sdmc:/nxpctl-provision.bin";
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
    ::nxpctl::protocol::ProvisioningRequest request{};
    FILE *file = std::fopen(ProvisioningPath, "rb");
    if (file == nullptr) {
        WriteStatus("missing-provisioning-file", MAKERESULT(Module_Libnx, LibnxError_NotFound));
        return 11;
    }

    const std::size_t bytes_read = std::fread(&request, 1, sizeof(request), file);
    const int trailing_byte = std::fgetc(file);
    const bool read_error = std::ferror(file) != 0;
    std::fclose(file);

    if (read_error || bytes_read != sizeof(request) || trailing_byte != EOF ||
        !::nxpctl::protocol::IsValidProvisioningRequest(request)) {
        Wipe(&request, sizeof(request));
        WriteStatus("invalid-provisioning-file", MAKERESULT(Module_Libnx, LibnxError_BadInput));
        return 12;
    }

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
        WriteStatus("sysmodule-rejected-provisioning", result);
        return 14;
    }

    /* The enrollment code is one-time. Remove the bootstrap file after HIPC accepts it. */
    if (std::remove(ProvisioningPath) != 0) {
        WriteStatus("accepted-but-file-removal-failed", result);
        return 15;
    }
    (void)fsdevCommitDevice("sdmc");
    WriteStatus("accepted-by-sysmodule", result);
    return 0;
}
