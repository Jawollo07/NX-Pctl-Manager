// NX-Pctl-Manager — borealis entry point. Registers the custom StatusPanel
// XML view, opens the pctl service, and pushes MainActivity onto the stack.
// Copyright (C) 2026 Taylor.  This program is free software under the GNU
// General Public License v3 or later; it comes with NO WARRANTY. See LICENSE.
#include <borealis.hpp>
#include <cstdlib>

#include "app.hpp"
#include "activity/init_error_activity.hpp"
#include "activity/main_activity.hpp"
#include "activity/pin_gate_activity.hpp"
#include "util/pctl_ops_c.hpp"
#include "util/totp.hpp"
#include "util/sysmodule_client.hpp"
#include "view/pt_state_header.hpp"
#include "view/status_panel.hpp"

using namespace brls::literals;

namespace
{
bool g_window_created = false;

int run()
{
    if (!brls::Application::init()) {
        brls::Logger::error("Unable to init borealis Application");
        return EXIT_FAILURE;
    }

    brls::Application::createWindow("nx_pctl/title"_i18n);
    g_window_created = true;

    brls::Application::getPlatform()->setThemeVariant(brls::ThemeVariant::DARK);
    brls::Application::setGlobalQuit(false);

    brls::Application::registerXMLView("StatusPanel", StatusPanel::create);
    brls::Application::registerXMLView("PtStateHeader", PtStateHeader::create);

    if (app::init()) {
        // Optional read-only diagnostics probe. The NRO remains usable if the
        // resident sysmodule is absent or its service is unavailable.
        nxpctl::RuntimeStatus runtime_status{};
        const Result sysmodule_rc = nxpctl::get_runtime_status(&runtime_status);
        if (R_SUCCEEDED(sysmodule_rc)) {
            if (nxpctl::protocol::IsCompatibleRuntimeStatus(runtime_status)) {
                brls::Logger::info(
                    "nxpctl sysmodule: protocol={}, worker_state={}, heartbeat={}, policy_revision={}",
                    runtime_status.protocol_version,
                    runtime_status.worker_state,
                    runtime_status.worker_heartbeat,
                    runtime_status.policy_revision);
            } else {
                brls::Logger::warning(
                    "nxpctl sysmodule returned an incompatible or malformed status; ignoring diagnostics");
            }
        } else {
            brls::Logger::info("nxpctl sysmodule unavailable (0x{:08X}); continuing in standalone mode",
                               static_cast<uint32_t>(sysmodule_rc));
        }

        PctlStatus st;
        pctl_status_fetch(&st);
        if (st.pin_length_ok && st.pin_length == 0 && !totp::secret_file_exists())
            brls::Application::pushActivity(new MainActivity());
        else
            brls::Application::pushActivity(new PinGateActivity());
    } else {
        brls::Logger::error("pctl_ops_init failed (0x{:08X}) — pushing InitErrorActivity",
                            app::pctl_init_result());
        brls::Application::pushActivity(new InitErrorActivity());
    }

    while (brls::Application::mainLoop())
        ;

    app::shutdown();
    return EXIT_SUCCESS;
}
} // namespace

int main(int argc, char* argv[])
{
    (void)argc;
    (void)argv;

    std::FILE* log = std::fopen("/switch/nx_pctl_borealis.log", "w");
    if (log) {
        std::setvbuf(log, nullptr, _IONBF, 0);
        brls::Logger::setLogOutput(log);
        brls::Logger::setLogLevel(brls::LogLevel::LOG_INFO);
    }

    int rc = EXIT_FAILURE;
    try {
        rc = run();
    } catch (const std::exception& e) {
        brls::Logger::error("Uncaught exception: {}", e.what());
        if (g_window_created) {
            brls::Application::quit();
            try {
                brls::Application::mainLoop();
            } catch (const std::exception& e2) {
                brls::Logger::error("Exception during shutdown: {}", e2.what());
                brls::Threading::stop();
            }
        } else {
            brls::Threading::stop();
        }
        app::shutdown();
    }

    if (log) {
        brls::Logger::setLogOutput(stdout);
        std::fclose(log);
    }
    return rc;
}
