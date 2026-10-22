#include <windows.h>

#include <algorithm>
#include <cwctype>
#include <format>
#include <iostream>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "core/config.h"
#include "core/drive_manager.h"
#include "core/image.h"
#include "core/util.h"
#include "gpu/vulkan_runtime.h"

namespace {

using grd::DriveConfig;
using grd::DriveManager;

const wchar_t* kUsage =
    L"GpuRamDriveVk - Vulkan-backed RAM disk for Windows\n"
    L"\n"
    L"Usage: gpuvramdrive-cli <command> [options]\n"
    L"\n"
    L"Commands:\n"
    L"  gpus                         List Vulkan devices\n"
    L"  status                       Show configured drives\n"
    L"  mount <letter>               Mount and keep running until Ctrl+C\n"
    L"  unmount <letter>             Stop a foreground mount process\n"
    L"  autostart                    Mount auto-start drives and stay running\n"
    L"  add <letter> [options]       Add or update a drive\n"
    L"  remove <letter>              Remove a drive configuration\n"
    L"  save <letter> <path>         Save a mounted drive to a raw image\n"
    L"  restore <letter> <path>      Restore a raw image to a mounted drive\n"
    L"  autostart-task on|off        Enable or disable the logon task\n"
    L"  where                        Print the configuration path\n"
    L"\n"
    L"Options for add:\n"
    L"  --size <MiB>       Size in MiB (default 1024)\n"
    L"  --gpu <index>      Vulkan device index (default 0)\n"
    L"  --fs <fs>          NTFS, exFAT, or FAT32 (default NTFS)\n"
    L"  --label <text>     Volume label (default GpuDisk)\n"
    L"  --image <path>     Raw image to load on mount and save on unmount\n"
    L"  --type <hd|fd>     Device type metadata (WinSpd presents a fixed disk)\n"
    L"  --removable        Removable metadata (WinSpd presents a fixed disk)\n"
    L"  --read-only        Mount read-only (requires an existing image)\n"
    L"  --temp             Point TEMP and TMP at <letter>:\\Temp\n"
    L"  --autostart        Mount this drive at logon\n"
    L"\n"
    L"Storage driver: WinSpd - Windows Storage Proxy Driver, "
    L"Copyright (C) Bill Zissimopoulos\n"
    L"https://github.com/winfsp/winspd\n";

HANDLE g_interruptEvent = nullptr;

BOOL WINAPI consoleControlHandler(DWORD type) {
    if (g_interruptEvent == nullptr) {
        return FALSE;
    }
    if (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT ||
        type == CTRL_CLOSE_EVENT || type == CTRL_LOGOFF_EVENT ||
        type == CTRL_SHUTDOWN_EVENT) {
        SetEvent(g_interruptEvent);
        return TRUE;
    }
    return FALSE;
}

std::wstring stopEventName(wchar_t letter) {
    return L"Global\\GpuRamDriveVk_Stop_" + std::wstring(1, letter);
}

std::wstring trimLetter(std::wstring text) {
    if (text.size() == 2 && text[1] == L':') {
        text.pop_back();
    }
    if (text.size() == 1) {
        text[0] = static_cast<wchar_t>(towupper(static_cast<wint_t>(text[0])));
    }
    return text;
}

std::optional<wchar_t> parseLetter(const std::wstring& text) {
    const std::wstring normalized = trimLetter(text);
    if (normalized.size() != 1 || !grd::isDriveLetter(normalized[0])) {
        return std::nullopt;
    }
    return normalized[0];
}

bool requireElevated() {
    if (grd::isElevated()) {
        return true;
    }
    std::wcerr << L"This command needs administrator rights. Run it from an "
                  L"elevated terminal.\n";
    return false;
}

void printError(const std::wstring& error) {
    std::wcerr << L"Error: " << error << L"\n";
}

int listGpus() {
    std::wstring error;
    auto& runtime = grd::VulkanRuntime::instance();
    if (!runtime.initialize(error)) {
        printError(error);
        return 1;
    }

    const auto& devices = runtime.devices();
    if (devices.empty()) {
        std::wcout << L"No Vulkan devices found.\n";
        return 0;
    }

    for (const auto& device : devices) {
        std::wcout << std::format(
            L"[{}] {} ({}, Vulkan {}.{}.{}, {:.1f} GiB, {})\n", device.index,
            device.name, device.typeName(), VK_API_VERSION_MAJOR(device.apiVersion),
            VK_API_VERSION_MINOR(device.apiVersion),
            VK_API_VERSION_PATCH(device.apiVersion),
            static_cast<double>(device.deviceLocalBytes) / (1024.0 * 1024.0 * 1024.0),
            device.hostVisibleDeviceLocal ? L"direct mapping" : L"staged");
    }
    return 0;
}

int showStatus() {
    DriveManager manager;
    std::wstring error;
    if (!manager.load(error)) {
        printError(error);
        return 1;
    }

    const auto statuses = manager.statuses();
    if (statuses.empty()) {
        std::wcout << L"No drives configured.\n";
        return 0;
    }

    std::wcout << std::format(L"{:<4} {:<8} {:>10} {:<4} {:<7} {:<16} {} {}\n",
                              L"DRV", L"STATE", L"SIZE(MiB)", L"GPU", L"FS",
                              L"LABEL", L"TEMP", L"IMAGE");
    for (const auto& status : statuses) {
        std::wcout << std::format(
            L"{:<4} {:<8} {:>10} {:<4} {:<7} {:<16} {:<4} {}\n",
            grd::displayLetter(status.config.letter),
            status.mounted ? L"mounted" : L"stopped",
            (status.mounted ? status.sizeBytes
                            : status.config.sizeMiB * 1024ull * 1024ull) /
                (1024 * 1024),
            status.config.gpuIndex,
            grd::fileSystemName(status.config.filesystem),
            status.config.label,
            status.config.useTempFolder ? L"yes" : L"no",
            status.config.imagePath.empty() ? L"-" : status.config.imagePath);
    }
    return 0;
}

int mountDrive(const std::wstring& letterText) {
    if (!requireElevated()) {
        return 1;
    }
    const auto letter = parseLetter(letterText);
    if (!letter) {
        printError(L"Invalid drive letter.");
        return 2;
    }

    DriveManager manager;
    std::wstring error;
    if (!manager.load(error)) {
        printError(error);
        return 1;
    }
    if (!manager.mount(*letter, error)) {
        printError(error);
        return 1;
    }

    const std::wstring eventName = stopEventName(*letter);
    grd::UniqueHandle stopEvent(
        CreateEventW(nullptr, FALSE, FALSE, eventName.c_str()));
    if (!stopEvent) {
        printError(L"Unable to create the stop event: " + grd::lastError());
        manager.unmount(*letter, true, error);
        return 1;
    }
    ResetEvent(stopEvent.get());

    grd::UniqueHandle interruptEvent(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (!interruptEvent) {
        printError(L"Unable to create the interrupt event: " + grd::lastError());
        manager.unmount(*letter, true, error);
        return 1;
    }

    g_interruptEvent = interruptEvent.get();
    SetConsoleCtrlHandler(consoleControlHandler, TRUE);
    std::wcout << L"Mounted " << grd::displayLetter(*letter)
               << L". Press Ctrl+C or run \"unmount "
               << grd::displayLetter(*letter)
               << L"\" from another terminal to stop.\n";

    const HANDLE handles[2] = {stopEvent.get(), interruptEvent.get()};
    WaitForMultipleObjects(2, handles, FALSE, INFINITE);

    SetConsoleCtrlHandler(consoleControlHandler, FALSE);
    g_interruptEvent = nullptr;

    std::wstring unmountError;
    if (!manager.unmount(*letter, false, unmountError)) {
        printError(unmountError);
        manager.unmount(*letter, true, unmountError);
    }
    std::wcout << L"Unmounted " << grd::displayLetter(*letter) << L"\n";
    return 0;
}

int unmountDrive(const std::wstring& letterText, bool force) {
    (void)force;
    if (!requireElevated()) {
        return 1;
    }
    const auto letter = parseLetter(letterText);
    if (!letter) {
        printError(L"Invalid drive letter.");
        return 2;
    }

    grd::UniqueHandle stopEvent(
        OpenEventW(EVENT_MODIFY_STATE, FALSE, stopEventName(*letter).c_str()));
    if (!stopEvent) {
        printError(L"That drive is not mounted by a foreground "
                   L"gpuvramdrive-cli mount process.");
        return 1;
    }

    if (SetEvent(stopEvent.get()) == FALSE) {
        printError(L"Unable to signal the mount process: " + grd::lastError());
        return 1;
    }
    std::wcout << L"Stop requested for " << grd::displayLetter(*letter)
               << L".\n";
    return 0;
}

int mountAutoStart() {
    if (!requireElevated()) {
        return 1;
    }
    DriveManager manager;
    std::wstring error;
    if (!manager.load(error)) {
        printError(error);
        return 1;
    }
    manager.mountAutoStart();

    std::vector<grd::UniqueHandle> stopEvents;
    std::vector<HANDLE> handles;
    for (const wchar_t letter : manager.configs().letters()) {
        const auto config = manager.configs().find(letter);
        if (!config || !config->autoStart || !manager.mounted(letter)) {
            continue;
        }
        grd::UniqueHandle event(
            CreateEventW(nullptr, FALSE, FALSE, stopEventName(letter).c_str()));
        if (!event) {
            printError(L"Unable to create the stop event for " +
                       grd::displayLetter(letter) + L": " + grd::lastError());
            manager.unmountAll(true, error);
            return 1;
        }
        ResetEvent(event.get());
        handles.push_back(event.get());
        stopEvents.push_back(std::move(event));
    }

    if (stopEvents.empty()) {
        std::wcout << L"No autostart drives were mounted.\n";
        return 1;
    }

    grd::UniqueHandle interruptEvent(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (!interruptEvent) {
        printError(L"Unable to create the interrupt event: " + grd::lastError());
        manager.unmountAll(true, error);
        return 1;
    }
    g_interruptEvent = interruptEvent.get();
    SetConsoleCtrlHandler(consoleControlHandler, TRUE);

    std::vector<HANDLE> waitHandles = handles;
    waitHandles.push_back(interruptEvent.get());
    std::wcout << L"Mounted " << stopEvents.size()
               << L" autostart drive(s). Press Ctrl+C to stop all.\n";
    WaitForMultipleObjects(static_cast<DWORD>(waitHandles.size()),
                           waitHandles.data(), FALSE, INFINITE);

    SetConsoleCtrlHandler(consoleControlHandler, FALSE);
    g_interruptEvent = nullptr;

    if (!manager.unmountAll(false, error)) {
        printError(error);
        manager.unmountAll(true, error);
    }
    std::wcout << L"All drives unmounted.\n";
    return 0;
}

int addDrive(const std::wstring& letterText,
             const std::map<std::wstring, std::wstring>& values,
             const std::set<std::wstring>& flags) {
    const auto letter = parseLetter(letterText);
    if (!letter) {
        printError(L"Invalid drive letter.");
        return 2;
    }

    DriveConfig config;
    config.letter = *letter;

    if (values.contains(L"size")) {
        const auto size = grd::parseUInt64(values.at(L"size"));
        if (!size || *size == 0 || *size > grd::kMaxDriveMiB) {
            printError(L"Invalid --size value.");
            return 2;
        }
        config.sizeMiB = *size;
    }
    if (values.contains(L"gpu")) {
        const auto gpu = grd::parseUInt64(values.at(L"gpu"));
        if (!gpu || *gpu > UINT32_MAX) {
            printError(L"Invalid --gpu value.");
            return 2;
        }
        config.gpuIndex = static_cast<uint32_t>(*gpu);
    }
    if (values.contains(L"fs")) {
        const auto fs = grd::parseFileSystem(values.at(L"fs"));
        if (!fs) {
            printError(L"Invalid --fs value; use NTFS, exFAT, or FAT32.");
            return 2;
        }
        config.filesystem = *fs;
    }
    if (values.contains(L"label")) {
        config.label = values.at(L"label");
    }
    if (values.contains(L"image")) {
        config.imagePath = values.at(L"image");
    }
    if (values.contains(L"type")) {
        const auto kind = grd::parseDriveKind(values.at(L"type"));
        if (!kind) {
            printError(L"Invalid --type value; use hd or fd.");
            return 2;
        }
        config.kind = *kind;
    }
    config.removable = flags.contains(L"removable");
    config.readOnly = flags.contains(L"read-only");
    config.useTempFolder = flags.contains(L"temp");
    config.autoStart = flags.contains(L"autostart");

    if (config.readOnly && config.useTempFolder) {
        printError(L"A read-only drive cannot provide TEMP.");
        return 2;
    }
    if (config.readOnly && config.imagePath.empty()) {
        printError(L"A read-only drive requires an image.");
        return 2;
    }

    DriveManager manager;
    std::wstring error;
    if (!manager.load(error)) {
        printError(error);
        return 1;
    }
    if (!manager.addOrUpdate(config, error)) {
        printError(error);
        return 1;
    }
    std::wcout << L"Saved " << grd::displayLetter(*letter) << L"\n";
    return 0;
}

int removeDrive(const std::wstring& letterText) {
    const auto letter = parseLetter(letterText);
    if (!letter) {
        printError(L"Invalid drive letter.");
        return 2;
    }

    DriveManager manager;
    std::wstring error;
    if (!manager.load(error)) {
        printError(error);
        return 1;
    }
    if (!manager.remove(*letter, error)) {
        printError(error);
        return 1;
    }
    std::wcout << L"Removed " << grd::displayLetter(*letter) << L"\n";
    return 0;
}

int saveDrive(const std::wstring& letterText, const std::wstring& path) {
    if (!requireElevated()) {
        return 1;
    }
    const auto letter = parseLetter(letterText);
    if (!letter) {
        printError(L"Invalid drive letter.");
        return 2;
    }
    std::wstring error;
    if (!grd::saveImage(*letter, path, error)) {
        printError(error);
        return 1;
    }
    std::wcout << L"Saved " << grd::displayLetter(*letter) << L" to " << path
               << L"\n";
    return 0;
}

int restoreDrive(const std::wstring& letterText, const std::wstring& path) {
    if (!requireElevated()) {
        return 1;
    }
    const auto letter = parseLetter(letterText);
    if (!letter) {
        printError(L"Invalid drive letter.");
        return 2;
    }
    std::wstring error;
    if (!grd::restoreImage(path, *letter, error)) {
        printError(error);
        return 1;
    }
    std::wcout << L"Restored " << path << L" to " << grd::displayLetter(*letter)
               << L"\n";
    return 0;
}

int taskCommand(const std::wstring& value) {
    if (!requireElevated()) {
        return 1;
    }
    DriveManager manager;
    std::wstring error;
    if (value == L"on") {
        if (!manager.setAutoStart(true, error)) {
            printError(error);
            return 1;
        }
        std::wcout << L"Autostart task enabled.\n";
        return 0;
    }
    if (value == L"off") {
        if (!manager.setAutoStart(false, error)) {
            printError(error);
            return 1;
        }
        std::wcout << L"Autostart task disabled.\n";
        return 0;
    }
    if (value == L"status") {
        std::wcout << (manager.autoStartEnabled() ? L"enabled\n" : L"disabled\n");
        return 0;
    }
    printError(L"Use on, off, or status.");
    return 2;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);

    if (argc < 2) {
        std::wcout << kUsage;
        return 2;
    }

    const std::wstring command = grd::lower(argv[1]);
    if (command == L"help" || command == L"--help" || command == L"-h") {
        std::wcout << kUsage;
        return 0;
    }
    if (command == L"gpus" || command == L"list") {
        return listGpus();
    }
    if (command == L"status" || command == L"drives") {
        return showStatus();
    }
    if (command == L"where") {
        std::wcout << grd::ConfigStore::defaultPath() << L"\n";
        return 0;
    }
    if (command == L"autostart" || command == L"--autostart" ||
        command == L"/autostart") {
        return mountAutoStart();
    }
    if (command == L"autostart-task") {
        if (argc < 3) {
            printError(L"Use autostart-task on|off|status.");
            return 2;
        }
        return taskCommand(grd::lower(argv[2]));
    }

    if (command == L"mount" || command == L"unmount" || command == L"remove" ||
        command == L"add" || command == L"save" || command == L"restore") {
        if (argc < 3) {
            printError(L"Missing drive letter.");
            return 2;
        }

        if (command == L"mount") {
            return mountDrive(argv[2]);
        }
        if (command == L"unmount") {
            const bool force = argc >= 4 && grd::lower(argv[3]) == L"--force";
            return unmountDrive(argv[2], force);
        }
        if (command == L"remove") {
            return removeDrive(argv[2]);
        }
        if (command == L"save" || command == L"restore") {
            if (argc < 4) {
                printError(L"Missing image path.");
                return 2;
            }
            return command == L"save" ? saveDrive(argv[2], argv[3])
                                      : restoreDrive(argv[2], argv[3]);
        }

        std::map<std::wstring, std::wstring> values;
        std::set<std::wstring> flags;
        for (int i = 3; i < argc; ++i) {
            std::wstring option = argv[i];
            if (option.starts_with(L"--")) {
                option.erase(0, 2);
                if (option == L"removable" || option == L"read-only" ||
                    option == L"temp" || option == L"autostart") {
                    flags.insert(option);
                } else if (option == L"size" || option == L"gpu" ||
                           option == L"fs" || option == L"label" ||
                           option == L"image" || option == L"type") {
                    if (i + 1 >= argc) {
                        printError(L"Missing value for --" + option + L".");
                        return 2;
                    }
                    values[option] = argv[++i];
                } else {
                    printError(L"Unknown option --" + option + L".");
                    return 2;
                }
            } else {
                printError(L"Unexpected argument: " + option);
                return 2;
            }
        }
        return addDrive(argv[2], values, flags);
    }

    printError(L"Unknown command: " + command);
    std::wcout << kUsage;
    return 2;
}
