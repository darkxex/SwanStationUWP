/*
 * SwanStation Xbox - the UWP application shell: a CoreApplication view whose Run() hosts the frontend's main loop.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <winrt/Windows.ApplicationModel.h>
#include <winrt/Windows.ApplicationModel.Activation.h>
#include <winrt/Windows.ApplicationModel.Core.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Graphics.Display.Core.h>
#include <winrt/Windows.Storage.h>
#include <winrt/Windows.System.Profile.h>
#include <winrt/Windows.UI.Core.h>
#include <winrt/Windows.UI.ViewManagement.h>

#include <windows.h>
#include <string>

#include "uwp_app.h"

using namespace winrt;
using namespace winrt::Windows::ApplicationModel;
using namespace winrt::Windows::ApplicationModel::Core;
using namespace winrt::Windows::ApplicationModel::Activation;
using namespace winrt::Windows::Foundation;
using namespace winrt::Windows::Graphics::Display::Core;
using namespace winrt::Windows::UI::Core;
using namespace winrt::Windows::UI::ViewManagement;

static volatile bool quit_requested;
static void (*suspend_handler)(void);
static std::string install_folder, local_folder;

static std::string utf8_forward(winrt::hstring const &path)
{
    std::string out = winrt::to_string(path);
    for (char &c : out)
        if (c == '\\')
            c = '/';
    while (!out.empty() && out.back() == '/')
        out.pop_back();
    return out;
}

extern "C" const char *uwp_install_folder(void) { return install_folder.c_str(); }
extern "C" const char *uwp_local_folder(void) { return local_folder.c_str(); }
extern "C" bool uwp_quit_requested(void) { return quit_requested; }
extern "C" void uwp_set_suspend_handler(void (*handler)(void)) { suspend_handler = handler; }

extern "C" void uwp_pump_events(void)
{
    auto window = CoreWindow::GetForCurrentThread();
    if (window)
        window.Dispatcher().ProcessEvents(CoreProcessEventsOption::ProcessAllIfPresent);
}

extern "C" bool uwp_current_display_mode(int *width, int *height, double *hz)
{
    try
    {
        auto mode = HdmiDisplayInformation::GetForCurrentView().GetCurrentDisplayMode();
        *width = (int)mode.ResolutionWidthInRawPixels();
        *height = (int)mode.ResolutionHeightInRawPixels();
        *hz = mode.RefreshRate();
        return true;
    }
    catch (winrt::hresult_error const &)
    {
        return false; // not an Xbox (a PC has no HDMI display information)
    }
}

extern "C" bool uwp_request_display_mode(int width, int height, double hz)
{
    try
    {
        auto info = HdmiDisplayInformation::GetForCurrentView();
        HdmiDisplayMode wanted{nullptr};
        for (auto const &mode : info.GetSupportedDisplayModes())
        {
            if ((int)mode.ResolutionWidthInRawPixels() != width || (int)mode.ResolutionHeightInRawPixels() != height)
                continue;
            if (hz > 0 && (mode.RefreshRate() < hz - 0.5 || mode.RefreshRate() > hz + 0.5))
                continue;
            if (mode.ColorSpace() != HdmiDisplayColorSpace::BgrFullRange || mode.PixelEncoding() != HdmiDisplayPixelEncoding::Rgb444)
                if (wanted) // keep a plain RGB mode if there is one
                    continue;
            wanted = mode;
        }
        if (!wanted)
            return false;
        // The call completes when the TV has switched. The UI thread cannot block on it: pump events until it ends.
        auto request = info.RequestSetCurrentDisplayModeAsync(wanted, HdmiDisplayHdrOption::None);
        for (int waited = 0; request.Status() == AsyncStatus::Started && waited < 100; ++waited)
        {
            uwp_pump_events();
            Sleep(100);
        }
        return request.Status() == AsyncStatus::Completed && request.GetResults();
    }
    catch (winrt::hresult_error const &)
    {
        return false;
    }
}

struct App : implements<App, IFrameworkViewSource, IFrameworkView>
{
    IFrameworkView CreateView() { return *this; }

    void Initialize(CoreApplicationView const &view)
    {
        view.Activated([](CoreApplicationView const &, IActivatedEventArgs const &) {
            CoreWindow::GetForCurrentThread().Activate();
        });
        CoreApplication::Suspending([](IInspectable const &, SuspendingEventArgs const &args) {
            auto deferral = args.SuspendingOperation().GetDeferral();
            if (suspend_handler)
                suspend_handler();
            deferral.Complete();
        });
    }

    void SetWindow(CoreWindow const &window)
    {
        window.Closed([](CoreWindow const &, CoreWindowEventArgs const &) { quit_requested = true; });
        // The whole screen, not the TV's safe area
        auto view = ApplicationView::GetForCurrentView();
        view.SetDesiredBoundsMode(ApplicationViewBoundsMode::UseCoreWindow);
        view.TryEnterFullScreenMode();
    }

    void Load(hstring const &) {}

    void Run()
    {
        install_folder = utf8_forward(Package::Current().InstalledLocation().Path());
        local_folder = utf8_forward(Windows::Storage::ApplicationData::Current().LocalFolder().Path());
        auto window = CoreWindow::GetForCurrentThread();
        window.Activate();
        uwp_pump_events();
        swanstation_main(0, nullptr);
        // The frontend returns when the player quits
        CoreApplication::Exit();
    }

    void Uninitialize() {}
};

int __stdcall wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
    winrt::init_apartment();
    CoreApplication::Run(make<App>());
    return 0;
}
