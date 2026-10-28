// NX-Pctl-Manager — borealis entry point. Registers the custom StatusPanel
// XML view, opens the pctl service, and pushes MainActivity onto the stack.
// Copyright (C) 2026 Taylor.  This program is free software under the GNU
// General Public License v3 or later; it comes with NO WARRANTY. See the
// LICENSE file or <https://www.gnu.org/licenses/gpl-3.0.html> for details.
#include <borealis.hpp>
#include <cstdlib>

#include "app.hpp"
#include "activity/init_error_activity.hpp"
#include "activity/main_activity.hpp"
#include "activity/pin_gate_activity.hpp"
#include "util/pctl_ops_c.hpp"
#include "view/pt_state_header.hpp"
#include "view/status_panel.hpp"

using namespace brls::literals;   // enables the "key"_i18n suffix

namespace
{
bool g_window_created = false;

int run()
{
    pctl_logf("BOOT 01: entering run(); libnx process start");
    pctl_logf("BOOT 02: calling brls::Application::init()");
    if (!brls::Application::init()) {
        pctl_logf("BOOT FATAL: brls::Application::init() returned false");
        brls::Logger::error("Unable to init borealis Application");
        return EXIT_FAILURE;
    }

    pctl_logf("BOOT 03: Application::init succeeded; creating window");
    brls::Application::createWindow("nx_pctl/title"_i18n);
    g_window_created = true;
    pctl_logf("BOOT 04: window created");

    // Auto-follow system theme; user can override in dev settings if they want.
    pctl_logf("BOOT 05: setting dark theme");
    brls::Application::getPlatform()->setThemeVariant(brls::ThemeVariant::DARK);
    pctl_logf("BOOT 06: theme set");

    // We own our own quit path (B on the root activity); no global Start-quit.
    brls::Application::setGlobalQuit(false);

    // Register custom views referenced from XML by their unprefixed tag name.
    pctl_logf("BOOT 07: registering XML view StatusPanel");
    brls::Application::registerXMLView("StatusPanel", StatusPanel::create);
    pctl_logf("BOOT 08: registering XML view PtStateHeader");
    brls::Application::registerXMLView("PtStateHeader", PtStateHeader::create);
    pctl_logf("BOOT 09: custom XML views registered");

    // Open the pctl service. On a non-CFW console this returns an error and
    // we show a dedicated InitErrorActivity (the only thing the user can do
    // is press B to exit).
    pctl_logf("BOOT 10: calling app::init() / pctl_ops_init()");
    if (app::init()) {
        pctl_logf("BOOT 11: app::init succeeded");
        // A registered PIN guards the whole tool (it can lift every restriction).
        PctlStatus st{};
        pctl_logf("BOOT 12: fetching initial parental-control status");
        pctl_status_fetch(&st);
        pctl_logf("BOOT 13: initial status: pin_ok=%d pin_length=%u restriction_ok=%d restriction_enabled=%d safety_ok=%d",
                  st.pin_length_ok, st.pin_length, st.restriction_enabled_ok,
                  st.restriction_enabled, st.safety_level_ok);
#ifdef NX_PCTL_REMOTE_CONTROL
        // With central control enabled, every launch must authenticate the device.
        pctl_logf("BOOT: pushing PinGateActivity");
        brls::Application::pushActivity(new PinGateActivity());
        pctl_logf("BOOT: pushActivity(PinGateActivity) returned");
#else
        // Local-only builds keep the original console-PIN gate.
        if (st.pin_length_ok && st.pin_length == 0) {
            pctl_logf("BOOT: pushing MainActivity (local build)");
            brls::Application::pushActivity(new MainActivity());
            pctl_logf("BOOT: pushActivity(MainActivity) returned");
        } else {
            pctl_logf("BOOT: pushing PinGateActivity (local build)");
            brls::Application::pushActivity(new PinGateActivity());
            pctl_logf("BOOT: pushActivity(PinGateActivity) returned");
        }
#endif
    } else {
        brls::Logger::error("pctl_ops_init failed (0x{:08X}) — pushing InitErrorActivity",
                            app::pctl_init_result());
        pctl_logf("BOOT ERROR: pushing InitErrorActivity");
        brls::Application::pushActivity(new InitErrorActivity());
        pctl_logf("BOOT ERROR: InitErrorActivity push returned");
    }

    pctl_logf("Entering application main loop");
    unsigned completed_frames = 0;
    pctl_logf("BOOT 16: entering main loop");
    while (true) {
        if (completed_frames < 3)
            pctl_logf("Application frame %u starting", completed_frames + 1);
        const bool running = brls::Application::mainLoop();
        if (completed_frames < 3)
            pctl_logf("Application frame %u returned (running=%d)",
                      completed_frames + 1, running);
        ++completed_frames;
        if (!running) break;
    }

    pctl_logf("BOOT 17: main loop ended; shutting down pctl service");
    app::shutdown();
    pctl_logf("BOOT 18: app::shutdown returned");
    return EXIT_SUCCESS;
}
}   // namespace

int main(int argc, char* argv[])
{
    (void)argc; (void)argv;

    // borealis only logs to stdout (nxlink); keep a copy on the SD card so
    // start-up failures can be diagnosed without a debugger.
    std::FILE* log = std::fopen("/switch/nx_pctl_borealis.log", "w");
    if (log) {
        std::setvbuf(log, nullptr, _IONBF, 0);   // unbuffered: survive a hard crash
        brls::Logger::setLogOutput(log);
        // INFO, not DEBUG: DEBUG logs every focus change, each one a synchronous SD write.
        brls::Logger::setLogLevel(brls::LogLevel::LOG_INFO);
    }

    pctl_logf("PROCESS: main() entered; borealis log opened=%d", log != nullptr);
    pctl_logf("PROCESS: manager diagnostic log path=/switch/nx_pctl_manager.log");
    int rc = EXIT_FAILURE;
    try {
        rc = run();
    } catch (const std::exception& e) {
        pctl_logf("EXCEPTION: std::exception caught in main(): %s", e.what());
        // brls::fatal() throws. Left uncaught it reaches abort(), which under
        // hbloader returns straight to the loader while borealis's task thread
        // is still alive — that thread then runs unmapped code (2168-0001).
        brls::Logger::error("Uncaught exception: {}", e.what());
        if (g_window_created) {
            // One more loop iteration with quit requested runs Application::exit():
            // stops the task thread and tears down the window / GPU context, so
            // hbmenu can start cleanly afterwards.
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

    pctl_logf("PROCESS: main() exiting rc=%d", rc);
    if (log) {
        brls::Logger::setLogOutput(stdout);
        std::fclose(log);
    }
    return rc;
}
