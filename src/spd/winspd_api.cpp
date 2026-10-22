#include "spd/winspd_api.h"

#include <iterator>
#include <vector>

#include "core/util.h"

namespace grd::spd {
namespace {

std::wstring moduleDirectory() {
    std::wstring path(MAX_PATH, L'\0');
    for (;;) {
        const DWORD length = GetModuleFileNameW(
            nullptr, path.data(), static_cast<DWORD>(path.size()));
        if (length == 0) {
            return {};
        }
        if (length < path.size()) {
            path.resize(length);
            break;
        }
        path.resize(path.size() * 2);
    }

    const size_t separator = path.find_last_of(L"\\/");
    return separator == std::wstring::npos ? std::wstring()
                                           : path.substr(0, separator);
}

std::wstring environmentPath(const wchar_t* name) {
    wchar_t buffer[MAX_PATH] = {};
    const DWORD length =
        GetEnvironmentVariableW(name, buffer, static_cast<DWORD>(std::size(buffer)));
    return length > 0 && length < std::size(buffer) ? std::wstring(buffer)
                                                    : std::wstring();
}

std::vector<std::wstring> candidatePaths() {
    std::vector<std::wstring> paths;
    paths.push_back(joinPath(moduleDirectory(), L"winspd-x64.dll"));

    const std::wstring programFiles = environmentPath(L"ProgramFiles");
    if (!programFiles.empty()) {
        paths.push_back(joinPath(programFiles, L"WinSpd\\sys\\winspd-x64.dll"));
    }

    const std::wstring programFilesX86 = environmentPath(L"ProgramFiles(x86)");
    if (!programFilesX86.empty()) {
        paths.push_back(
            joinPath(programFilesX86, L"WinSpd\\sys\\winspd-x64.dll"));
    }

    paths.push_back(L"C:\\Program Files\\WinSpd\\sys\\winspd-x64.dll");
    return paths;
}

template <typename T>
bool resolve(HMODULE module, T& target, const char* name, std::wstring& error) {
    target = reinterpret_cast<T>(GetProcAddress(module, name));
    if (target == nullptr) {
        error = L"winspd-x64.dll does not export " + widen(name) + L".";
        return false;
    }
    return true;
}

}  // namespace

Api::~Api() {
    if (module_ != nullptr) {
        FreeLibrary(module_);
        module_ = nullptr;
    }
}

bool Api::load(std::wstring& error) {
    if (module_ != nullptr) {
        error.clear();
        return true;
    }

    std::wstring lastErrorText;
    for (const std::wstring& path : candidatePaths()) {
        if (!fileExists(path)) {
            continue;
        }
        module_ = LoadLibraryW(path.c_str());
        if (module_ == nullptr) {
            lastErrorText = lastError();
            continue;
        }

        if (!resolve(module_, create_, "SpdStorageUnitCreate", error) ||
            !resolve(module_, delete_, "SpdStorageUnitDelete", error) ||
            !resolve(module_, shutdown_, "SpdStorageUnitShutdown", error) ||
            !resolve(module_, startDispatcher_,
                     "SpdStorageUnitStartDispatcher", error) ||
            !resolve(module_, waitDispatcher_, "SpdStorageUnitWaitDispatcher",
                     error) ||
            !resolve(module_, definePartitionTable_, "SpdDefinePartitionTable",
                     error)) {
            FreeLibrary(module_);
            module_ = nullptr;
            return false;
        }

        error.clear();
        return true;
    }

    error = L"WinSpd is not installed. Install WinSpd 1.0B1 from "
            L"https://github.com/winfsp/winspd/releases/tag/v1.0B1 and run "
            L"GpuRamDriveVk as administrator.";
    if (!lastErrorText.empty()) {
        error += L" " + lastErrorText;
    }
    return false;
}

DWORD Api::create(PWSTR deviceName, const StorageUnitParams& params,
                  const StorageUnitInterface& interface,
                  StorageUnit** storageUnit) const {
    return create_ == nullptr
               ? ERROR_PROC_NOT_FOUND
               : create_(deviceName, &params, &interface, storageUnit);
}

void Api::destroy(StorageUnit* storageUnit) const {
    if (delete_ != nullptr && storageUnit != nullptr) {
        delete_(storageUnit);
    }
}

void Api::shutdown(StorageUnit* storageUnit) const {
    if (shutdown_ != nullptr && storageUnit != nullptr) {
        shutdown_(storageUnit);
    }
}

DWORD Api::startDispatcher(StorageUnit* storageUnit, ULONG threadCount) const {
    return startDispatcher_ == nullptr
               ? ERROR_PROC_NOT_FOUND
               : startDispatcher_(storageUnit, threadCount);
}

void Api::waitDispatcher(StorageUnit* storageUnit) const {
    if (waitDispatcher_ != nullptr && storageUnit != nullptr) {
        waitDispatcher_(storageUnit);
    }
}

DWORD Api::definePartitionTable(Partition partitions[4], ULONG count,
                                UINT8 buffer[512]) const {
    return definePartitionTable_ == nullptr
               ? ERROR_PROC_NOT_FOUND
               : definePartitionTable_(partitions, count, buffer);
}

}  // namespace grd::spd
