#include "core/drive.h"

#include <windows.h>

#include <cwctype>
#include <format>
#include <utility>

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

}  // namespace

Drive::Drive(DriveConfig config) : config_(std::move(config)) {
    config_.letter = static_cast<wchar_t>(
        towupper(static_cast<wint_t>(config_.letter)));
}

Drive::~Drive() {
    std::wstring ignored;
    if (!unmount(true, ignored)) {
        Log::error(L"Leaking GPU storage because the WinSpd unit could not be "
                   L"stopped: " + ignored);
        if (storage_) {
            storage_.release();
        }
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
    if (!SpdStorage::isAvailable()) {
        error = L"WinSpd is not installed. Install WinSpd 1.0B1 from "
                L"https://github.com/winfsp/winspd/releases/tag/v1.0B1 and run "
                L"GpuRamDriveVk as administrator.";
        return false;
    }

    if (config_.kind != DriveKind::HardDisk || config_.removable) {
        Log::warn(L"WinSpd presents a fixed SCSI disk; the configured floppy "
                  L"type or removable flag is ignored.");
    }

    const std::wstring image = resolvedImagePath();
    const bool restore = !image.empty() && fileExists(image);
    if (config_.readOnly && !restore) {
        error = image.empty()
                    ? L"A read-only drive requires an existing image."
                    : L"The read-only image does not exist: " + image;
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

    storage_ = std::make_unique<SpdStorage>(
        *gpu_, config_, sizeBytes_, restore ? image : std::wstring());
    if (!storage_->mount(error)) {
        storage_.reset();
        resetStorage();
        return false;
    }

    mounted_ = true;

    if (config_.useTempFolder) {
        if (!applyTempFolder(config_.letter, error)) {
            std::wstring ignored;
            if (storage_) {
                storage_->unmount(true, ignored);
            }
            resetStorage();
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
    if (!mounted_ && !gpu_ && !storage_) {
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

    if (storage_) {
        std::wstring stopError;
        if (!storage_->unmount(force, stopError)) {
            error = stopError + L" The drive was left mounted.";
            return false;
        }
    }

    resetStorage();
    return true;
}

void Drive::resetStorage() {
    if (storage_ && storage_->mounted()) {
        Log::error(L"Leaking GPU storage because a WinSpd unit is still "
                   L"attached.");
        storage_.release();
        gpu_.release();
    } else {
        storage_.reset();
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
    if (!mounted_ || !storage_) {
        error = L"The drive is not mounted.";
        return false;
    }
    if (config_.readOnly) {
        error = L"A read-only drive cannot be formatted.";
        return false;
    }
    if (!storage_->format(filesystem, label, error)) {
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
        result.backend = L"WinSpd / " + gpu_->info();
    }
    return result;
}

}  // namespace grd
