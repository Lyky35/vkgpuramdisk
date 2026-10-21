#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>

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

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    SetProcessDPIAware();

    INITCOMMONCONTROLSEX controls = {};
    controls.dwSize = sizeof(controls);
    controls.dwICC = ICC_LISTVIEW_CLASSES | ICC_STANDARD_CLASSES | ICC_BAR_CLASSES;
    InitCommonControlsEx(&controls);

    if (!grd::isElevated()) {
        MessageBoxW(nullptr,
                    L"GpuRamDriveVk must run as administrator. ImDisk and GPU "
                    L"drives require elevated access.",
                    L"GpuRamDriveVk", MB_OK | MB_ICONERROR);
        return 1;
    }

    HANDLE mutex = CreateMutexW(nullptr, TRUE, L"Global\\GpuRamDriveVk_UI");
    if (mutex == nullptr) {
        MessageBoxW(nullptr, L"Unable to create the single-instance mutex.",
                    L"GpuRamDriveVk", MB_OK | MB_ICONERROR);
        return 1;
    }

    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        HWND existing = FindWindowW(grd::MainWindow::kClassName, nullptr);
        if (existing != nullptr) {
            SendMessageW(existing, WM_APP + 2, 0, 0);
        }
        CloseHandle(mutex);
        return 0;
    }

    grd::MainWindow window(instance);
    if (!window.create(hasAutostartArgument())) {
        CloseHandle(mutex);
        return 1;
    }

    const int result = window.runMessageLoop();
    CloseHandle(mutex);
    return result;
}
