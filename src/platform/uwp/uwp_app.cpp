/*
 * SwanStationUWP - the UWP application shell: a CoreApplication view whose Run() hosts the frontend's main loop.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <winrt/Windows.ApplicationModel.h>
#include <winrt/Windows.ApplicationModel.Activation.h>
#include <winrt/Windows.ApplicationModel.Core.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Graphics.Display.h>
#include <winrt/Windows.Graphics.Display.Core.h>
#include <winrt/Windows.Storage.h>
#include <winrt/Windows.System.h>
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
static volatile LONG window_resized;
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
extern "C" bool uwp_take_window_resized(void) { return InterlockedExchange(&window_resized, 0) != 0; }

extern "C" bool uwp_open_file_access_settings(void)
{
    try
    {
        auto op = Windows::System::Launcher::LaunchUriAsync(Uri(L"ms-settings:privacy-broadfilesystemaccess"));
        for (int waited = 0; op.Status() == AsyncStatus::Started && waited < 40; ++waited)
        {
            uwp_pump_events();
            Sleep(50);
        }
        return op.Status() == AsyncStatus::Completed && op.GetResults();
    }
    catch (winrt::hresult_error const &)
    {
        return false;
    }
}

extern "C" bool uwp_window_pixels(int *width, int *height)
{
    try
    {
        // On the console the swap chain has the TV's mode (as RetroArch-UWP does): the window is 1080p in its own units
        // whatever the TV is at, and its scale does not say otherwise. A PC has no HDMI information (a null object).
        auto hdmi = HdmiDisplayInformation::GetForCurrentView();
        if (hdmi)
        {
            auto mode = hdmi.GetCurrentDisplayMode();
            *width = (int)mode.ResolutionWidthInRawPixels();
            *height = (int)mode.ResolutionHeightInRawPixels();
            if (*width > 0 && *height > 0)
                return true;
        }
        auto window = CoreWindow::GetForCurrentThread();
        if (!window)
            return false;
        auto bounds = window.Bounds();
        double scale = Windows::Graphics::Display::DisplayInformation::GetForCurrentView().RawPixelsPerViewPixel();
        if (scale < 0.5)
            scale = 1.0;
        *width = (int)(bounds.Width * scale + 0.5);
        *height = (int)(bounds.Height * scale + 0.5);
        return *width > 0 && *height > 0;
    }
    catch (winrt::hresult_error const &)
    {
        return false;
    }
}
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
        // a PC gives a null object here, not an exception, and C++/WinRT does not check for null
        auto info = HdmiDisplayInformation::GetForCurrentView();
        if (!info)
            return false;
        auto mode = info.GetCurrentDisplayMode();
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
        if (!info)
            return false;
        HdmiDisplayMode wanted{nullptr};
        for (auto const &mode : info.GetSupportedDisplayModes())
        {
            if ((int)mode.ResolutionWidthInRawPixels() != width || (int)mode.ResolutionHeightInRawPixels() != height)
                continue;
            if (hz > 0 && (mode.RefreshRate() < hz - 0.5 || mode.RefreshRate() > hz + 0.5))
                continue;
            if (mode.ColorSpace() != HdmiDisplayColorSpace::RgbFull || mode.PixelEncoding() != HdmiDisplayPixelEncoding::Rgb444)
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
        window.SizeChanged([](CoreWindow const &, WindowSizeChangedEventArgs const &) {
            InterlockedExchange(&window_resized, 1);
        });
        // The B button of an Xbox controller (and the Back key) is a "back" request to the system, which closes an app that
        // does not take it: the app reads B itself (XInput), so the request is taken and nothing else happens.
        SystemNavigationManager::GetForCurrentView().BackRequested(
            [](IInspectable const &, BackRequestedEventArgs const &args) { args.Handled(true); });
        // F11 toggles full screen (the swap chain follows the new size through SizeChanged)
        window.KeyDown([](CoreWindow const &, KeyEventArgs const &args) {
            if (args.VirtualKey() != Windows::System::VirtualKey::F11 || args.KeyStatus().WasKeyDown)
                return;
            auto view = ApplicationView::GetForCurrentView();
            if (view.IsFullScreenMode())
                view.ExitFullScreenMode();
            else
                view.TryEnterFullScreenMode();
            args.Handled(true);
        });
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
