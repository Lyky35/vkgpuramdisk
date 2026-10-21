#include "core/environment.h"

#include <windows.h>

#include <cwctype>
#include <format>

#include "core/log.h"
#include "core/util.h"

namespace grd {
namespace {

constexpr wchar_t kAppKey[] = L"Software\\GpuRamDriveVk";
constexpr wchar_t kEnvironmentKey[] = L"Environment";

struct RegistryString {
    bool exists = false;
    DWORD type = REG_SZ;
    std::wstring value;
};

class RegKey {
public:
    RegKey() = default;
    explicit RegKey(HKEY key) : key_(key) {}
    ~RegKey() {
        if (key_ != nullptr) {
            RegCloseKey(key_);
        }
    }
    RegKey(const RegKey&) = delete;
    RegKey& operator=(const RegKey&) = delete;
    RegKey(RegKey&& other) noexcept : key_(other.key_) { other.key_ = nullptr; }
    RegKey& operator=(RegKey&& other) noexcept {
        if (this != &other) {
            if (key_ != nullptr) {
                RegCloseKey(key_);
            }
            key_ = other.key_;
            other.key_ = nullptr;
        }
        return *this;
    }
    HKEY get() const { return key_; }
    explicit operator bool() const { return key_ != nullptr; }

private:
    HKEY key_ = nullptr;
};

RegKey openKey(HKEY root, const wchar_t* subKey, REGSAM access, bool create,
               std::wstring& error) {
    HKEY key = nullptr;
    DWORD disposition = 0;
    const LSTATUS status =
        create ? RegCreateKeyExW(root, subKey, 0, nullptr, 0, access, nullptr,
                                 &key, &disposition)
               : RegOpenKeyExW(root, subKey, 0, access, &key);
    if (status != ERROR_SUCCESS) {
        error = L"Unable to open registry key " + std::wstring(subKey) + L": " +
                win32Error(static_cast<DWORD>(status));
        return RegKey();
    }
    return RegKey(key);
}

RegistryString readString(HKEY key, const wchar_t* name) {
    RegistryString result;
    DWORD type = 0;
    DWORD bytes = 0;
    LSTATUS status = RegQueryValueExW(key, name, nullptr, &type, nullptr, &bytes);
    if (status == ERROR_FILE_NOT_FOUND || status == ERROR_PATH_NOT_FOUND) {
        return result;
    }
    if (status != ERROR_SUCCESS) {
        return result;
    }
    if (type != REG_SZ && type != REG_EXPAND_SZ) {
        return result;
    }

    std::wstring buffer(bytes / sizeof(wchar_t) + 1, L'\0');
    status = RegQueryValueExW(key, name, nullptr, &type,
                              reinterpret_cast<BYTE*>(buffer.data()), &bytes);
    if (status != ERROR_SUCCESS) {
        return result;
    }

    buffer.resize(bytes / sizeof(wchar_t));
    while (!buffer.empty() && buffer.back() == L'\0') {
        buffer.pop_back();
    }

    result.exists = true;
    result.type = type;
    result.value = std::move(buffer);
    return result;
}

bool readDword(HKEY key, const wchar_t* name, DWORD& value) {
    DWORD type = 0;
    DWORD bytes = sizeof(value);
    const LSTATUS status = RegQueryValueExW(
        key, name, nullptr, &type, reinterpret_cast<BYTE*>(&value), &bytes);
    return status == ERROR_SUCCESS && type == REG_DWORD;
}

bool writeString(HKEY key, const wchar_t* name, const std::wstring& value,
                 DWORD type, std::wstring& error) {
    const LSTATUS status = RegSetValueExW(
        key, name, 0, type, reinterpret_cast<const BYTE*>(value.c_str()),
        static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
    if (status != ERROR_SUCCESS) {
        error = L"Unable to write registry value " + std::wstring(name) +
                L": " + win32Error(static_cast<DWORD>(status));
        return false;
    }
    return true;
}

bool writeDword(HKEY key, const wchar_t* name, DWORD value,
                std::wstring& error) {
    const LSTATUS status =
        RegSetValueExW(key, name, 0, REG_DWORD,
                       reinterpret_cast<const BYTE*>(&value), sizeof(value));
    if (status != ERROR_SUCCESS) {
        error = L"Unable to write registry value " + std::wstring(name) +
                L": " + win32Error(static_cast<DWORD>(status));
        return false;
    }
    return true;
}

void deleteValue(HKEY key, const wchar_t* name) {
    RegDeleteValueW(key, name);
}

void broadcastEnvironmentChange() {
    DWORD_PTR result = 0;
    SendMessageTimeoutW(HWND_BROADCAST, WM_SETTINGCHANGE, 0,
                        reinterpret_cast<LPARAM>(L"Environment"),
                        SMTO_ABORTIFHUNG, 1000, &result);
}

bool anyBackupValue(HKEY key) {
    DWORD active = 0;
    return readDword(key, L"BackupActive", active) && active != 0;
}

}  // namespace

bool hasTempFolderBackup() {
    std::wstring error;
    RegKey key = openKey(HKEY_CURRENT_USER, kAppKey, KEY_READ, false, error);
    if (!key) {
        return false;
    }
    return anyBackupValue(key.get());
}

std::optional<wchar_t> tempFolderBackupDrive() {
    std::wstring error;
    RegKey key = openKey(HKEY_CURRENT_USER, kAppKey, KEY_READ, false, error);
    if (!key || !anyBackupValue(key.get())) {
        return std::nullopt;
    }

    DWORD drive = 0;
    if (!readDword(key.get(), L"BackupDrive", drive)) {
        return std::nullopt;
    }

    const wchar_t letter =
        static_cast<wchar_t>(towupper(static_cast<wint_t>(drive)));
    if (!isDriveLetter(letter)) {
        return std::nullopt;
    }
    return letter;
}

bool applyTempFolder(wchar_t letter, std::wstring& error) {
    error.clear();
    const std::wstring tempPath = driveRoot(letter) + L"Temp";

    if (CreateDirectoryW(tempPath.c_str(), nullptr) == FALSE) {
        const DWORD code = GetLastError();
        if (code != ERROR_ALREADY_EXISTS) {
            error = L"Unable to create " + tempPath + L": " +
                    win32Error(code);
            return false;
        }
        const DWORD attributes = GetFileAttributesW(tempPath.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES ||
            (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
            error = tempPath + L" exists but is not a directory.";
            return false;
        }
    }

    RegKey appKey = openKey(HKEY_CURRENT_USER, kAppKey,
                              KEY_READ | KEY_SET_VALUE, true, error);
    if (!appKey) {
        return false;
    }
    HKEY app = appKey.get();

    DWORD active = 0;
    const bool isActive = readDword(app, L"BackupActive", active) && active != 0;
    DWORD backupDrive = 0;
    const bool hasDrive = readDword(app, L"BackupDrive", backupDrive);

    if (isActive) {
        if (hasDrive &&
            towupper(static_cast<wint_t>(backupDrive)) != towupper(letter)) {
            error = L"TEMP is already redirected to another GpuRamDrive.";
            return false;
        }
    } else {
        RegKey envKey = openKey(HKEY_CURRENT_USER, kEnvironmentKey, KEY_READ,
                                true, error);
        if (!envKey) {
            return false;
        }
        HKEY env = envKey.get();

        const RegistryString oldTemp = readString(env, L"TEMP");
        const RegistryString oldTmp = readString(env, L"TMP");

        deleteValue(app, L"TempBackup");
        deleteValue(app, L"TempBackupType");
        deleteValue(app, L"TmpBackup");
        deleteValue(app, L"TmpBackupType");

        if (oldTemp.exists) {
            if (!writeString(app, L"TempBackup", oldTemp.value, REG_SZ, error) ||
                !writeDword(app, L"TempBackupType", oldTemp.type, error)) {
                return false;
            }
        }
        if (oldTmp.exists) {
            if (!writeString(app, L"TmpBackup", oldTmp.value, REG_SZ, error) ||
                !writeDword(app, L"TmpBackupType", oldTmp.type, error)) {
                return false;
            }
        }

        if (!writeDword(app, L"BackupDrive", letter, error) ||
            !writeDword(app, L"BackupActive", 1, error)) {
            return false;
        }
    }

    RegKey envKey = openKey(HKEY_CURRENT_USER, kEnvironmentKey, KEY_SET_VALUE,
                            true, error);
    if (!envKey) {
        std::wstring restoreError;
        restoreTempFolder(restoreError);
        if (!restoreError.empty()) {
            error += L" " + restoreError;
        }
        return false;
    }
    HKEY env = envKey.get();

    if (!writeString(env, L"TEMP", tempPath, REG_SZ, error) ||
        !writeString(env, L"TMP", tempPath, REG_SZ, error)) {
        std::wstring restoreError;
        restoreTempFolder(restoreError);
        if (!restoreError.empty()) {
            error += L" " + restoreError;
        }
        return false;
    }

    SetEnvironmentVariableW(L"TEMP", tempPath.c_str());
    SetEnvironmentVariableW(L"TMP", tempPath.c_str());
    broadcastEnvironmentChange();
    Log::info(L"TEMP and TMP now point to " + tempPath);
    return true;
}

bool restoreTempFolder(std::wstring& error) {
    error.clear();
    RegKey appKey = openKey(HKEY_CURRENT_USER, kAppKey,
                              KEY_READ | KEY_SET_VALUE, false, error);
    if (!appKey) {
        return true;
    }
    HKEY app = appKey.get();

    if (!anyBackupValue(app)) {
        return true;
    }

    RegKey envKey = openKey(HKEY_CURRENT_USER, kEnvironmentKey,
                            KEY_READ | KEY_SET_VALUE, true, error);
    if (!envKey) {
        return false;
    }
    HKEY env = envKey.get();

    const RegistryString temp = readString(app, L"TempBackup");
    const RegistryString tmp = readString(app, L"TmpBackup");
    DWORD tempType = REG_SZ;
    DWORD tmpType = REG_SZ;
    readDword(app, L"TempBackupType", tempType);
    readDword(app, L"TmpBackupType", tmpType);

    if (temp.exists) {
        if (!writeString(env, L"TEMP", temp.value, tempType, error)) {
            return false;
        }
        SetEnvironmentVariableW(L"TEMP", temp.value.c_str());
    } else {
        deleteValue(env, L"TEMP");
        SetEnvironmentVariableW(L"TEMP", nullptr);
    }

    if (tmp.exists) {
        if (!writeString(env, L"TMP", tmp.value, tmpType, error)) {
            return false;
        }
        SetEnvironmentVariableW(L"TMP", tmp.value.c_str());
    } else {
        deleteValue(env, L"TMP");
        SetEnvironmentVariableW(L"TMP", nullptr);
    }

    deleteValue(app, L"TempBackup");
    deleteValue(app, L"TmpBackup");
    deleteValue(app, L"TempBackupType");
    deleteValue(app, L"TmpBackupType");
    deleteValue(app, L"BackupDrive");
    deleteValue(app, L"BackupActive");
    broadcastEnvironmentChange();
    Log::info(L"Restored the previous TEMP and TMP values.");
    return true;
}

}  // namespace grd
