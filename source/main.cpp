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
    if (!brls::Application::init()) {
        brls::Logger::error("Unable to init borealis Application");
        return EXIT_FAILURE;
    }

    brls::Application::createWindow("nx_pctl/title"_i18n);
    g_window_created = true;

    // Auto-follow system theme; user can override in dev settings if they want.
    brls::Application::getPlatform()->setThemeVariant(brls::ThemeVariant::DARK);

    // We own our own quit path (B on the root activity); no global Start-quit.
    brls::Application::setGlobalQuit(false);

    // Register custom views referenced from XML by their unprefixed tag name.
    brls::Application::registerXMLView("StatusPanel",   StatusPanel::create);
    brls::Application::registerXMLView("PtStateHeader", PtStateHeader::create);

    // Open the pctl service. On a non-CFW console this returns an error and
    // we show a dedicated InitErrorActivity (the only thing the user can do
    // is press B to exit).
    if (app::init()) {
        // A registered PIN guards the whole tool (it can lift every restriction).
        PctlStatus st;
        pctl_status_fetch(&st);
        if (st.pin_length_ok && st.pin_length == 0)
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
        brls::Logger::setLogLevel(brls::LogLevel::LOG_DEBUG);
    }

    int rc = EXIT_FAILURE;
    try {
        rc = run();
    } catch (const std::exception& e) {
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

    if (log) {
        brls::Logger::setLogOutput(stdout);
        std::fclose(log);
    }
    return rc;
}
