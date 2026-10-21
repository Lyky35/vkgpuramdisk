#pragma once

#include <windows.h>
#include <winioctl.h>

#include <cstdint>
#include <string>

namespace grd {

// Values from imdisk.h. They are ABI, not configuration.
constexpr DWORD kImDiskOptionReadOnly = 0x00000001;
constexpr DWORD kImDiskOptionRemovable = 0x00000002;
constexpr DWORD kImDiskDeviceTypeHd = 0x00000010;
constexpr DWORD kImDiskDeviceTypeFd = 0x00000020;
constexpr DWORD kImDiskTypeProxy = 0x00000300;
constexpr DWORD kImDiskProxyTypeShm = 0x00003000;
constexpr DWORD kImDiskAutoDeviceNumber = 0xFFFFFFFF;

struct ImDiskCreateData {
    ULONG DeviceNumber;
    DISK_GEOMETRY DiskGeometry;
    LARGE_INTEGER ImageOffset;
    ULONG Flags;
    WCHAR DriveLetter;
    USHORT FileNameLength;
    WCHAR FileName[1];
};

class ImDiskApi {
public:
    ImDiskApi() = default;
    ~ImDiskApi();

    ImDiskApi(const ImDiskApi&) = delete;
    ImDiskApi& operator=(const ImDiskApi&) = delete;

    bool load(std::wstring& error);
    bool loaded() const { return module_ != nullptr; }

    bool createProxyDevice(const std::wstring& serviceName, wchar_t letter,
                           bool floppy, bool removable, bool readOnly,
                           DWORD& deviceNumber, std::wstring& error);

    bool removeByLetter(wchar_t letter, bool force,
                        std::wstring& error);
    bool forceRemove(DWORD deviceNumber, std::wstring& error);

    bool findDeviceByLetter(wchar_t letter, DWORD& deviceNumber) const;

    static bool isAvailable();

private:
    using SetApiFlagsFn = ULONGLONG(WINAPI*)(ULONGLONG);
    using CreateDeviceExFn = BOOL(WINAPI*)(HWND, LPDWORD, PDISK_GEOMETRY,
                                           PLARGE_INTEGER, DWORD, LPCWSTR, BOOL,
                                           LPWSTR);
    using RemoveDeviceFn = BOOL(WINAPI*)(HWND, DWORD, LPCWSTR);
    using ForceRemoveDeviceFn = BOOL(WINAPI*)(HANDLE, DWORD);
    using GetDeviceListExFn = BOOL(WINAPI*)(ULONG, PULONG);
    using QueryDeviceFn = BOOL(WINAPI*)(DWORD, void*, ULONG);

    template <typename T>
    bool resolve(T& target, const char* name, std::wstring& error);

    HMODULE module_ = nullptr;
    SetApiFlagsFn setApiFlags_ = nullptr;
    CreateDeviceExFn createDeviceEx_ = nullptr;
    RemoveDeviceFn removeDevice_ = nullptr;
    ForceRemoveDeviceFn forceRemoveDevice_ = nullptr;
    GetDeviceListExFn getDeviceListEx_ = nullptr;
    QueryDeviceFn queryDevice_ = nullptr;
};

}  // namespace grd
