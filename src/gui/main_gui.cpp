#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>

#include <exception>
#include <string>

#include "core/log.h"
#include "core/util.h"
#include "gui/main_window.h"

namespace {

bool hasAutostartArgument() {
    int argc = 0;
    LPWSTR* arguments = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (arguments == nullptr) {
        return false;
    }
    bool found = false;
    for (int i = 1; i < argc; ++i) {
        const std::wstring argument = grd::lower(arguments[i]);
        if (argument == L"--autostart" || argument == L"/autostart") {
            found = true;
            break;
        }
    }
    LocalFree(arguments);
    return found;
}

}  // namespace

namespace {

int runGui(HINSTANCE instance) {
    SetProcessDPIAware();

    INITCOMMONCONTROLSEX controls = {};
    controls.dwSize = sizeof(controls);
    controls.dwICC = ICC_LISTVIEW_CLASSES | ICC_STANDARD_CLASSES | ICC_BAR_CLASSES;
    InitCommonControlsEx(&controls);

    if (!grd::isElevated()) {
        MessageBoxW(nullptr,
                    L"GpuRamDriveVk must run as administrator. WinSpd and GPU "
                    L"drives require elevated access.",
                    L"GpuRamDriveVk", MB_OK | MB_ICONERROR);
        return 1;
    }

    SetLastError(ERROR_SUCCESS);
    HANDLE mutex = CreateMutexW(nullptr, TRUE, L"Global\\GpuRamDriveVk_UI");
    const DWORD mutexError = GetLastError();
    if (mutex == nullptr) {
        MessageBoxW(nullptr, L"Unable to create the single-instance mutex.",
                    L"GpuRamDriveVk", MB_OK | MB_ICONERROR);
        return 1;
    }

    if (mutexError == ERROR_ALREADY_EXISTS) {
        HWND existing = FindWindowW(grd::MainWindow::kClassName, nullptr);
        if (existing != nullptr) {
            SendMessageW(existing, WM_APP + 2, 0, 0);
        }
        CloseHandle(mutex);
        return 0;
    }

    grd::MainWindow window(instance);
    std::wstring startupError;
    if (!window.create(hasAutostartArgument(), startupError)) {
        grd::Log::error(L"Unable to start the GUI: " + startupError);
        const std::wstring message =
            L"GpuRamDriveVk could not start.\n\n" + startupError;
        MessageBoxW(nullptr, message.c_str(), L"GpuRamDriveVk",
                    MB_OK | MB_ICONERROR);
        CloseHandle(mutex);
        return 1;
    }

    const int result = window.runMessageLoop();
    CloseHandle(mutex);
    return result;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    try {
        return runGui(instance);
    } catch (const std::exception& error) {
        const std::wstring message =
            L"GpuRamDriveVk could not start.\n\n" + grd::widen(error.what());
        grd::Log::error(message);
        MessageBoxW(nullptr, message.c_str(), L"GpuRamDriveVk",
                    MB_OK | MB_ICONERROR);
        return 1;
    } catch (...) {
        MessageBoxW(nullptr, L"GpuRamDriveVk could not start.",
                    L"GpuRamDriveVk", MB_OK | MB_ICONERROR);
        return 1;
    }
}
