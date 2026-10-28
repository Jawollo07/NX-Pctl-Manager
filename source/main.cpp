// NX-Pctl-Manager — Plutonium UI entry point.
// Copyright (C) 2026 Taylor. GPLv3-or-later; see LICENSE.
#include <pu/Plutonium>
#include <string>

#include "app.hpp"
#include "remote_control.hpp"
#include "util/pctl_ops_c.hpp"

namespace
{
class ManagerLayout final : public pu::ui::Layout
{
public:
    pu::ui::elm::TextBlock::Ref title;
    pu::ui::elm::TextBlock::Ref status;
    pu::ui::elm::TextBlock::Ref help;

    ManagerLayout()
    {
        title = pu::ui::elm::TextBlock::New(40, 30, "NX-Pctl-Manager");
        status = pu::ui::elm::TextBlock::New(40, 105, "Initializing parental-control service...");
        help = pu::ui::elm::TextBlock::New(
            40, 400,
            "A: Authenticate / retry   X: Sync server   Y: Refresh status   +: Exit");
        this->Add(title);
        this->Add(status);
        this->Add(help);
    }

    PU_SMART_CTOR(ManagerLayout)
};

class ManagerApplication final : public pu::ui::Application
{
    ManagerLayout::Ref layout;
    bool service_ready = false;
    bool authenticated = false;

public:
    using Application::Application;
    PU_SMART_CTOR(ManagerApplication)

    void OnLoad() override
    {
        layout = ManagerLayout::New();
        this->LoadLayout(layout);
        this->SetOnInput([this](const u64 down, const u64, const u64, const pu::ui::TouchPoint) {
            if (down & HidNpadButton_Plus) {
                if (service_ready) app::shutdown();
                this->Close();
            } else if (down & HidNpadButton_A) {
                this->Authenticate();
            } else if (down & HidNpadButton_X) {
                this->Synchronize();
            } else if (down & HidNpadButton_Y) {
                this->RefreshStatus();
            }
        });

        service_ready = app::init();
        if (!service_ready) {
            layout->status->SetText("Could not initialize pctl service. Press A to retry.");
            return;
        }
        layout->status->SetText("pctl ready. Press A to authenticate with central server.");
    }

private:
    void Authenticate()
    {
        if (!service_ready) {
            service_ready = app::init();
            if (!service_ready) {
                layout->status->SetText("pctl initialization failed. Check nx_pctl_manager.log.");
                return;
            }
        }
        std::string message;
        authenticated = remote_control::authenticate(message);
        layout->status->SetText(message);
        if (authenticated)
            this->CreateShowDialog("Authentication", message, {"OK"}, true);
    }

    void Synchronize()
    {
        if (!authenticated) {
            layout->status->SetText("Authenticate first with A.");
            return;
        }
        const auto result = remote_control::synchronize();
        layout->status->SetText(result.message);
        this->CreateShowDialog("Central control", result.message, {"OK"}, true);
    }

    void RefreshStatus()
    {
        if (!service_ready) {
            layout->status->SetText("pctl service is not available.");
            return;
        }
        PctlStatus st{};
        pctl_status_fetch(&st);
        std::string text = "Parental-control status: ";
        text += st.restriction_enabled_ok
            ? (st.restriction_enabled ? "restrictions enabled" : "restrictions disabled")
            : "restriction state unavailable";
        text += " | Safety: ";
        text += st.safety_level_ok ? pctl_safety_level_name(st.safety_level) : "unavailable";
        text += " | PIN digits: ";
        text += st.pin_length_ok ? std::to_string(st.pin_length) : "unavailable";
        layout->status->SetText(text);
    }
};
}

int main()
{
    auto options = pu::ui::render::RendererInitOptions(
        SDL_INIT_EVERYTHING, pu::ui::render::RendererHardwareFlags);
    options.UseImage(pu::ui::render::ImgAllFlags);
    options.SetPlServiceType(PlServiceType_User);
    options.AddDefaultAllSharedFonts();
    options.SetInputPlayerCount(1);
    options.AddInputNpadStyleTag(HidNpadStyleSet_NpadStandard);
    options.AddInputNpadIdType(HidNpadIdType_Handheld);
    options.AddInputNpadIdType(HidNpadIdType_No1);

    auto renderer = pu::ui::render::Renderer::New(options);
    auto app = ManagerApplication::New(renderer);
    const auto rc = app->Load();
    if (R_FAILED(rc))
        diagAbortWithResult(rc);
    app->Show();
    return 0;
}
