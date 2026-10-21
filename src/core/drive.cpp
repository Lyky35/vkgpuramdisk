#include "core/drive.h"

#include <windows.h>

#include <algorithm>
#include <cwctype>
#include <format>
#include <utility>
#include <vector>

#include "core/environment.h"
#include "core/image.h"
#include "core/log.h"
#include "core/util.h"
#include "gpu/vulkan_drive.h"
#include "gpu/vulkan_runtime.h"

namespace grd {
namespace {

constexpr uint64_t kMiB = 1024ull * 1024ull;

bool isAbsolutePath(const std::wstring& path) {
    if (path.size() >= 2 && path[1] == L':') {
        return true;
    }
    if (path.size() >= 2 && path[0] == L'\\' && path[1] == L'\\') {
        return true;
    }
    return !path.empty() && (path[0] == L'\\' || path[0] == L'/');
}

std::wstring serviceName(wchar_t letter) {
    return L"GpuRamDriveVk_" + std::wstring(1, letter);
}

bool runFormat(wchar_t letter, FileSystem filesystem, const std::wstring& label,
               std::wstring& error) {
    const wchar_t* fileSystem = filesystem == FileSystem::Fat32
                                    ? L"FAT32"
                                    : (filesystem == FileSystem::ExFat ? L"exFAT"
                                                                      : L"NTFS");
    const std::wstring executable = joinPath(systemDirectory(), L"format.com");
    std::wstring command = quoteArgument(executable) + L" " +
                           displayLetter(letter) + L" /FS:" + fileSystem +
                           L" /Q /Y";
    if (!label.empty()) {
        command += L" /V:" + quoteArgument(label);
    }

    std::vector<wchar_t> commandLine(command.begin(), command.end());
    commandLine.push_back(L'\0');

    STARTUPINFOW startup = {};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION processInfo = {};
    if (CreateProcessW(executable.c_str(), commandLine.data(), nullptr, nullptr,
                       FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &startup,
                       &processInfo) == FALSE) {
        error = L"Unable to start format.com: " + lastError();
        return false;
    }

    UniqueHandle process(processInfo.hProcess);
    UniqueHandle thread(processInfo.hThread);

    const DWORD wait = WaitForSingleObject(process.get(), 5 * 60 * 1000);
    if (wait == WAIT_TIMEOUT) {
        TerminateProcess(process.get(), 1);
        WaitForSingleObject(process.get(), 5000);
        error = L"format.com timed out.";
        return false;
    }
    if (wait != WAIT_OBJECT_0) {
        error = L"Unable to wait for format.com: " + lastError();
        return false;
    }

    DWORD exitCode = 0;
    if (GetExitCodeProcess(process.get(), &exitCode) == FALSE) {
        error = L"Unable to read format.com's exit code: " + lastError();
        return false;
    }
    if (exitCode != 0) {
        error = std::format(L"format.com failed with exit code {}.", exitCode);
        return false;
    }

    if (!label.empty()) {
        const std::wstring root = driveRoot(letter);
        bool labeled = false;
        for (int attempt = 0; attempt < 3 && !labeled; ++attempt) {
            labeled = SetVolumeLabelW(root.c_str(), label.c_str()) != FALSE;
            if (!labeled) {
                Sleep(200);
            }
        }
        if (!labeled) {
            Log::warn(L"Unable to set the volume label after formatting: " +
                      lastError());
        }
    }
    return true;
}

}  // namespace

Drive::Drive(DriveConfig config) : config_(std::move(config)) {
    config_.letter = static_cast<wchar_t>(
        towupper(static_cast<wint_t>(config_.letter)));
}

Drive::~Drive() {
    std::wstring ignored;
    if (!unmount(true, ignored)) {
        // The driver still owns the shared-memory volume. Keep the GPU block
        // alive rather than freeing memory that ImDisk may still access.
        Log::error(L"Leaking GPU storage because the ImDisk device could not "
                   L"be removed: " + ignored);
        proxy_.stop();
        if (gpu_) {
            gpu_.release();
        }
    }
}

std::wstring Drive::resolvedImagePath() const {
    if (config_.imagePath.empty()) {
        return {};
    }
    if (isAbsolutePath(config_.imagePath)) {
        return config_.imagePath;
    }
    return joinPath(executableDir(), config_.imagePath);
}

bool Drive::startProxy(std::wstring& error) {
    if (!gpu_) {
        error = L"GPU storage is not allocated.";
        return false;
    }

    const std::wstring name = serviceName(config_.letter);
    return proxy_.start(
        name, sizeBytes_,
        [this](uint64_t offset, void* buffer, size_t length) {
            gpu_->read(offset, buffer, length);
            return true;
        },
        [this](uint64_t offset, const void* buffer, size_t length) {
            gpu_->write(offset, buffer, length);
            return true;
        },
        error);
}

bool Drive::createDevice(bool readOnly, std::wstring& error) {
    DWORD deviceNumber = 0;
    if (!imdisk_.createProxyDevice(
            serviceName(config_.letter), config_.letter,
            config_.kind == DriveKind::Floppy, config_.removable, readOnly,
            deviceNumber, error)) {
        return false;
    }
    deviceNumber_ = deviceNumber;
    deviceAttached_ = true;
    return true;
}

bool Drive::formatVolume(FileSystem filesystem, const std::wstring& label,
                         std::wstring& error) {
    if (filesystem == FileSystem::Fat32 && sizeBytes_ > 32ull * 1024ull * kMiB) {
        error = L"Windows cannot format a FAT32 volume larger than 32 GiB. "
                L"Choose exFAT or NTFS.";
        return false;
    }
    return runFormat(config_.letter, filesystem, label, error);
}

bool Drive::removeDevice(bool force, std::wstring& error) {
    if (!deviceAttached_) {
        return true;
    }

    std::wstring firstError;
    if (imdisk_.removeByLetter(config_.letter, force, firstError)) {
        deviceAttached_ = false;
        deviceNumber_ = 0;
        return true;
    }

    if (force && deviceNumber_ != 0) {
        std::wstring byNumberError;
        if (imdisk_.forceRemove(deviceNumber_, byNumberError)) {
            deviceAttached_ = false;
            deviceNumber_ = 0;
            return true;
        }
        firstError += L" " + byNumberError;
    }

    error = firstError;
    return false;
}

bool Drive::remountReadOnly(std::wstring& error) {
    if (!removeDevice(true, error)) {
        return false;
    }

    proxy_.stop();
    for (int attempt = 0; attempt < 20; ++attempt) {
        if (!isDriveLetterInUse(config_.letter)) {
            break;
        }
        Sleep(100);
    }
    Sleep(200);

    if (!startProxy(error)) {
        return false;
    }
    if (!createDevice(true, error)) {
        proxy_.stop();
        return false;
    }
    return true;
}

bool Drive::mount(std::wstring& error) {
    error.clear();
    if (mounted_) {
        error = L"This drive is already mounted.";
        return false;
    }

    if (!isDriveLetter(config_.letter)) {
        error = L"The drive letter must be between A and Z.";
        return false;
    }
    if (config_.letter == L'C') {
        error = L"C: is reserved for the system drive.";
        return false;
    }
    if (config_.sizeMiB == 0 || config_.sizeMiB > kMaxDriveMiB) {
        error = L"The drive size is out of range.";
        return false;
    }
    if (config_.readOnly && config_.useTempFolder) {
        error = L"A read-only drive cannot be used for TEMP.";
        return false;
    }
    if (isDriveLetterInUse(config_.letter)) {
        error = std::format(L"Drive {} is already in use.",
                            displayLetter(config_.letter));
        return false;
    }
    if (!ImDiskApi::isAvailable()) {
        error = L"imdisk.cpl is not installed. Install the ImDisk Virtual "
                L"Disk Driver first.";
        return false;
    }

    const std::wstring image = resolvedImagePath();
    if (config_.readOnly && !image.empty() && !fileExists(image)) {
        error = L"The read-only image does not exist: " + image;
        return false;
    }

    const uint64_t requestedBytes = config_.sizeMiB * kMiB;
    VulkanRuntime& runtime = VulkanRuntime::instance();
    gpu_ = runtime.createDrive(config_.gpuIndex, requestedBytes, error);
    if (!gpu_) {
        if (error.empty()) {
            error = runtime.lastError();
        }
        return false;
    }
    sizeBytes_ = gpu_->size();

    if (!startProxy(error)) {
        resetStorage();
        return false;
    }
    if (!createDevice(false, error)) {
        resetStorage();
        return false;
    }

    auto cleanupFailedMount = [this]() {
        if (deviceAttached_) {
            std::wstring ignored;
            if (!removeDevice(true, ignored)) {
                Log::error(L"Unable to remove the failed mount: " + ignored);
            }
        }
        if (!deviceAttached_) {
            resetStorage();
        }
    };

    if (!image.empty() && fileExists(image)) {
        if (!restoreImage(image, config_.letter, error)) {
            cleanupFailedMount();
            return false;
        }
    } else {
        if (!formatVolume(config_.filesystem, config_.label, error)) {
            cleanupFailedMount();
            return false;
        }
    }

    if (config_.readOnly && !remountReadOnly(error)) {
        cleanupFailedMount();
        return false;
    }

    mounted_ = true;

    if (config_.useTempFolder) {
        if (!applyTempFolder(config_.letter, error)) {
            std::wstring ignored;
            unmount(true, ignored);
            return false;
        }
        ownsTemp_ = true;
    }

    Log::info(std::format(L"Mounted {}: ({} MiB on GPU {}).",
                          displayLetter(config_.letter), sizeBytes_ / kMiB,
                          config_.gpuIndex));
    return true;
}

bool Drive::unmount(bool force, std::wstring& error) {
    error.clear();
    if (!mounted_ && !gpu_ && !deviceAttached_) {
        return true;
    }

    if (ownsTemp_) {
        std::wstring tempError;
        if (!restoreTempFolder(tempError)) {
            if (!force) {
                error = tempError;
                return false;
            }
            Log::warn(L"Unable to restore TEMP: " + tempError);
        }
        ownsTemp_ = false;
    }

    const std::wstring image = resolvedImagePath();
    if (mounted_ && !config_.readOnly && !image.empty()) {
        std::wstring saveError;
        if (!saveImage(config_.letter, image, saveError)) {
            if (!force) {
                error = saveError + L" The drive was left mounted.";
                return false;
            }
            Log::warn(L"Unable to save the image: " + saveError);
        }
    }

    if (deviceAttached_) {
        std::wstring removeError;
        if (!removeDevice(force, removeError)) {
            error = removeError + L" The drive was left mounted.";
            return false;
        }
    }

    resetStorage();
    return true;
}

void Drive::resetStorage() {
    proxy_.stop();
    if (deviceAttached_) {
        // The volume still points at this proxy. Never free the backing store
        // while the driver can still issue requests; leak it instead.
        Log::error(L"Leaking GPU storage because an ImDisk device is still "
                   L"attached.");
        gpu_.release();
    } else {
        gpu_.reset();
    }
    mounted_ = false;
    sizeBytes_ = 0;
}

bool Drive::saveNow(std::wstring& error) {
    error.clear();
    if (!mounted_) {
        error = L"The drive is not mounted.";
        return false;
    }
    if (config_.readOnly) {
        error = L"A read-only drive cannot be saved.";
        return false;
    }
    const std::wstring image = resolvedImagePath();
    if (image.empty()) {
        error = L"No image path is configured for this drive.";
        return false;
    }
    return saveImage(config_.letter, image, error);
}

bool Drive::restoreNow(std::wstring& error) {
    error.clear();
    if (!mounted_) {
        error = L"The drive is not mounted.";
        return false;
    }
    if (config_.readOnly) {
        error = L"A read-only drive cannot be restored.";
        return false;
    }
    const std::wstring image = resolvedImagePath();
    if (image.empty() || !fileExists(image)) {
        error = L"The configured image does not exist.";
        return false;
    }
    return restoreImage(image, config_.letter, error);
}

bool Drive::formatNow(FileSystem filesystem, const std::wstring& label,
                      std::wstring& error) {
    error.clear();
    if (!mounted_) {
        error = L"The drive is not mounted.";
        return false;
    }
    if (config_.readOnly) {
        error = L"A read-only drive cannot be formatted.";
        return false;
    }
    if (!formatVolume(filesystem, label, error)) {
        return false;
    }
    config_.filesystem = filesystem;
    config_.label = label;
    return true;
}

DriveStatus Drive::status() const {
    DriveStatus result;
    result.config = config_;
    result.mounted = mounted_;
    result.sizeBytes = sizeBytes_;
    if (gpu_) {
        result.directMapped = gpu_->directMapped();
        result.backend = gpu_->info();
    }
    return result;
}

}  // namespace grd
