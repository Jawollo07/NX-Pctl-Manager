#include <switch.h>

#include <cstdio>
#include <cstring>

#include "../../../protocol/provisioning.hpp"
#include "../../../protocol/runtime_status.hpp"

namespace {

constexpr const char *ProvisioningPath = "sdmc:/nxpctl.conf";
constexpr const char *StatusPath = "sdmc:/nxpctl-provision.status";

void Wipe(void *data, std::size_t size) {
    volatile unsigned char *p = static_cast<volatile unsigned char *>(data);
    while (size-- != 0) *p++ = 0;
}

void Show(const char *message) {
    std::printf("%s\n", message);
    consoleUpdate(NULL);
}

void WaitForExit() {
    Show("\nDruecke +, um zum Homebrew-Menue zurueckzukehren.");
    while (appletMainLoop()) {
        hidScanInput();
        if (hidKeysDown(CONTROLLER_P1_AUTO) & KEY_PLUS) break;
        consoleUpdate(NULL);
        svcSleepThread(16'000'000);
    }
}

void WriteStatus(const char *state, Result result) {
    if (FILE *file = std::fopen(StatusPath, "w")) {
        std::fprintf(file, "state=%s\nresult=0x%08X\n", state,
                     static_cast<unsigned int>(result));
        std::fclose(file);
        (void)fsdevCommitDevice("sdmc");
    }
}

int Fail(const char *message, const char *state, Result result, int exit_code) {
    Show(message);
    if (fsdevGetDeviceFileSystem("sdmc") != nullptr) WriteStatus(state, result);
    WaitForExit();
    return exit_code;
}

} // namespace

int main() {
    consoleInit(NULL);
    Show("NX-Pctl-Manager - einmalige Geraeteregistrierung");
    Show("Pruefe SD-Karten-Zugriff ...");

    /* Normally mounted by libnx startup; mount explicitly if it is absent. */
    if (fsdevGetDeviceFileSystem("sdmc") == nullptr) {
        const Result mount_result = fsdevMountSdmc();
        if (R_FAILED(mount_result) || fsdevGetDeviceFileSystem("sdmc") == nullptr) {
            return Fail("FEHLER: SD-Karte konnte nicht eingebunden werden.",
                        "sdmc-mount-failed", mount_result, 10);
        }
    }
    Show("SD-Karte: OK");
    Show("Lese sdmc:/nxpctl.conf ...");

    Result result = 0;
    char config[::nxpctl::protocol::ProvisioningConfigMaxSize + 1]{};
    ::nxpctl::protocol::ProvisioningRequest request{};
    FILE *file = std::fopen(ProvisioningPath, "rb");
    if (file == nullptr) {
        return Fail("FEHLER: nxpctl.conf wurde im SD-Karten-Hauptverzeichnis nicht gefunden.",
                    "missing-config-file",
                    MAKERESULT(Module_Libnx, LibnxError_NotFound), 11);
    }

    const std::size_t bytes_read = std::fread(config, 1, sizeof(config), file);
    const bool read_error = std::ferror(file) != 0;
    std::fclose(file);

    if (read_error || bytes_read == 0 ||
        bytes_read > ::nxpctl::protocol::ProvisioningConfigMaxSize ||
        !::nxpctl::protocol::ParseProvisioningConfig(config, bytes_read, request)) {
        Wipe(config, sizeof(config));
        Wipe(&request, sizeof(request));
        return Fail("FEHLER: nxpctl.conf ist ungueltig oder konnte nicht gelesen werden.",
                    "invalid-config-file",
                    MAKERESULT(Module_Libnx, LibnxError_BadInput), 12);
    }
    Wipe(config, sizeof(config));
    Show("Konfiguration: OK");
    Show("Suche HIPC-Dienst 'nxpctl' ...");

    Service service{};
    result = smGetService(&service, ::nxpctl::protocol::ServiceName);
    if (R_FAILED(result)) {
        Wipe(&request, sizeof(request));
        char message[96]{};
        std::snprintf(message, sizeof(message),
                      "FEHLER: Sysmodule-Dienst nicht verfuegbar (0x%08X).",
                      static_cast<unsigned int>(result));
        return Fail(message, "sysmodule-service-unavailable", result, 13);
    }

    Show("Sysmodule gefunden. Uebergebe Registrierungsdaten ...");
    result = serviceDispatch(
        &service,
        ::nxpctl::protocol::ProvisioningCommandId,
        .buffer_attrs = { SfBufferAttr_HipcMapAlias | SfBufferAttr_In },
        .buffers = { { &request, sizeof(request) } }
    );
    serviceClose(&service);
    Wipe(&request, sizeof(request));

    if (R_FAILED(result)) {
        char message[96]{};
        std::snprintf(message, sizeof(message),
                      "FEHLER: Sysmodule hat die Daten abgelehnt (0x%08X).",
                      static_cast<unsigned int>(result));
        return Fail(message, "sysmodule-rejected-config", result, 14);
    }

    Show("Lokale Uebergabe erfolgreich. Entferne Einmal-Konfiguration ...");
    if (std::remove(ProvisioningPath) != 0) {
        return Fail("WARNUNG: Uebergabe erfolgreich, nxpctl.conf konnte aber nicht geloescht werden.",
                    "accepted-but-config-removal-failed", result, 15);
    }
    (void)fsdevCommitDevice("sdmc");
    WriteStatus("accepted-by-sysmodule", result);
    Show("ERFOLG: Das Sysmodule hat die Registrierungsdaten angenommen.");
    Show("Die Verbindung zum Backend erfolgt anschliessend im Hintergrund.");
    WaitForExit();
    return 0;
}
