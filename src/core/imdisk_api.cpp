#include "core/imdisk_api.h"

#include <algorithm>
#include <cwctype>
#include <format>

#include "core/log.h"
#include "core/util.h"

namespace grd {

ImDiskApi::~ImDiskApi() {
    if (module_ != nullptr) {
        FreeLibrary(module_);
    }
}

bool ImDiskApi::isAvailable() {
    const std::wstring path = joinPath(systemDirectory(), L"imdisk.cpl");
    HMODULE module = LoadLibraryW(path.c_str());
    if (module == nullptr) {
        return false;
    }
    FreeLibrary(module);
    return true;
}

template <typename T>
bool ImDiskApi::resolve(T& target, const char* name, std::wstring& error) {
    target = reinterpret_cast<T>(GetProcAddress(module_, name));
    if (target == nullptr) {
        error = L"imdisk.cpl does not export " + widen(name) +
                L". Install a current ImDisk version.";
        return false;
    }
    return true;
}

bool ImDiskApi::load(std::wstring& error) {
    error.clear();
    if (module_ != nullptr) {
        return true;
    }

    const std::wstring path = joinPath(systemDirectory(), L"imdisk.cpl");
    module_ = LoadLibraryW(path.c_str());
    if (module_ == nullptr) {
        error = L"Unable to load " + path + L": " + lastError() +
                L". Install the ImDisk Virtual Disk Driver first.";
        return false;
    }

    if (!resolve(setApiFlags_, "ImDiskSetAPIFlags", error) ||
        !resolve(createDeviceEx_, "ImDiskCreateDeviceEx", error) ||
        !resolve(removeDevice_, "ImDiskRemoveDevice", error) ||
        !resolve(forceRemoveDevice_, "ImDiskForceRemoveDevice", error) ||
        !resolve(getDeviceListEx_, "ImDiskGetDeviceListEx", error) ||
        !resolve(queryDevice_, "ImDiskQueryDevice", error)) {
        FreeLibrary(module_);
        module_ = nullptr;
        return false;
    }

    setApiFlags_(0);
    return true;
}

bool ImDiskApi::createProxyDevice(const std::wstring& serviceName,
                                  wchar_t letter, bool floppy, bool removable,
                                  bool readOnly, DWORD& deviceNumber,
                                  std::wstring& error) {
    if (!load(error)) {
        return false;
    }

    DISK_GEOMETRY geometry = {};
    DWORD flags = kImDiskTypeProxy | kImDiskProxyTypeShm;
    flags |= floppy ? kImDiskDeviceTypeFd : kImDiskDeviceTypeHd;
    if (removable) {
        flags |= kImDiskOptionRemovable;
    }
    if (readOnly) {
        flags |= kImDiskOptionReadOnly;
    }

    wchar_t mountPoint[4] = {letter, L':', L'\0', L'\0'};
    DWORD number = kImDiskAutoDeviceNumber;

    if (createDeviceEx_(nullptr, &number, &geometry, nullptr, flags,
                        serviceName.c_str(), FALSE, mountPoint) == FALSE) {
        const DWORD code = GetLastError();
        error = std::format(
            L"ImDisk could not create the virtual drive at {}: {}", mountPoint,
            win32Error(code));
        return false;
    }

    deviceNumber = number;
    Log::debug(std::format(L"ImDisk created device {} at {}", number,
                           mountPoint));
    return true;
}

bool ImDiskApi::findDeviceByLetter(wchar_t letter,
                                   DWORD& deviceNumber) const {
    if (module_ == nullptr || getDeviceListEx_ == nullptr ||
        queryDevice_ == nullptr) {
        return false;
    }

    ULONG list[256] = {};
    if (getDeviceListEx_(256, list) == FALSE &&
        GetLastError() != ERROR_MORE_DATA) {
        return false;
    }
    if (list[0] == 0) {
        return false;
    }

    const ULONG count = std::min<ULONG>(list[0], 255);
    for (ULONG i = 0; i < count; ++i) {
        alignas(8) unsigned char buffer[4096] = {};
        if (queryDevice_(list[i + 1], buffer, sizeof(buffer)) != FALSE) {
            const auto* data = reinterpret_cast<const ImDiskCreateData*>(buffer);
            if (data->DriveLetter != L'\0' &&
                std::towupper(data->DriveLetter) == letter) {
                deviceNumber = list[i + 1];
                return true;
            }
        }
    }
    return false;
}

bool ImDiskApi::removeByLetter(wchar_t letter, bool force,
                               std::wstring& error) {
    if (!load(error)) {
        return false;
    }

    const std::wstring mountPoint = displayLetter(letter);
    if (removeDevice_(nullptr, 0, mountPoint.c_str()) != FALSE) {
        Log::debug(L"ImDisk removed " + mountPoint);
        return true;
    }

    const DWORD firstCode = GetLastError();
    if (!force) {
        error = std::format(
            L"ImDisk could not remove {}: {}. Close programs using the drive "
            L"or force-unmount it.",
            mountPoint, win32Error(firstCode));
        return false;
    }

    DWORD deviceNumber = 0;
    if (!findDeviceByLetter(letter, deviceNumber)) {
        error = std::format(L"ImDisk could not remove {}: {}", mountPoint,
                            win32Error(firstCode));
        return false;
    }

    if (forceRemoveDevice_(nullptr, deviceNumber) == FALSE) {
        error = std::format(L"ImDisk could not force-remove {} (device {}): {}",
                            mountPoint, deviceNumber, lastError());
        return false;
    }

    Log::debug(std::format(L"ImDisk force-removed {} (device {})", mountPoint,
                           deviceNumber));
    return true;
}

bool ImDiskApi::forceRemove(DWORD deviceNumber, std::wstring& error) {
    if (!load(error)) {
        return false;
    }

    if (forceRemoveDevice_(nullptr, deviceNumber) == FALSE) {
        error = std::format(L"ImDisk could not force-remove device {}: {}",
                            deviceNumber, lastError());
        return false;
    }
    return true;
}

}  // namespace grd
