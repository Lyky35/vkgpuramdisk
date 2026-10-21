#include "core/drive_manager.h"

#include <cwctype>
#include <format>

#include "core/environment.h"
#include "core/log.h"
#include "core/task.h"
#include "core/util.h"

namespace grd {

DriveManager::DriveManager() = default;

DriveManager::~DriveManager() {
    std::wstring ignored;
    unmountAll(true, ignored);
}

bool DriveManager::load(std::wstring& error) {
    if (!configs_.load(error)) {
        return false;
    }

    if (hasTempFolderBackup()) {
        bool currentlyMounted = false;
        if (const auto letter = tempFolderBackupDrive()) {
            currentlyMounted = isDriveLetterInUse(*letter);
        }
        if (!currentlyMounted) {
            std::wstring tempError;
            if (restoreTempFolder(tempError)) {
                Log::info(
                    L"Restored a stale TEMP redirection from a previous run.");
            } else {
                Log::warn(L"Unable to restore a stale TEMP redirection: " +
                          tempError);
            }
        }
    }
    return true;
}

Drive* DriveManager::find(wchar_t letter) {
    const auto it = drives_.find(static_cast<wchar_t>(
        towupper(static_cast<wint_t>(letter))));
    return it == drives_.end() ? nullptr : it->second.get();
}

const Drive* DriveManager::find(wchar_t letter) const {
    const auto it = drives_.find(static_cast<wchar_t>(
        towupper(static_cast<wint_t>(letter))));
    return it == drives_.end() ? nullptr : it->second.get();
}

bool DriveManager::mounted(wchar_t letter) const {
    const Drive* drive = find(letter);
    return drive != nullptr && drive->mounted();
}

bool DriveManager::mount(wchar_t letter, std::wstring& error) {
    error.clear();
    const wchar_t normalized =
        static_cast<wchar_t>(towupper(static_cast<wint_t>(letter)));
    if (drives_.contains(normalized)) {
        error = std::format(L"Drive {} is already mounted by this process.",
                            displayLetter(normalized));
        return false;
    }

    const auto config = configs_.find(normalized);
    if (!config) {
        error = std::format(L"No configuration exists for drive {}.",
                            displayLetter(normalized));
        return false;
    }

    if (config->useTempFolder) {
        for (const auto& [otherLetter, otherDrive] : drives_) {
            if (otherDrive && otherDrive->mounted() &&
                otherDrive->config().useTempFolder) {
                error = std::format(
                    L"TEMP is already provided by drive {}.",
                    displayLetter(otherLetter));
                return false;
            }
        }
    }

    auto drive = std::make_unique<Drive>(*config);
    if (!drive->mount(error)) {
        return false;
    }

    drives_[normalized] = std::move(drive);
    return true;
}

void DriveManager::mountAutoStart() {
    for (const wchar_t letter : configs_.letters()) {
        const auto config = configs_.find(letter);
        if (!config || !config->autoStart) {
            continue;
        }
        if (drives_.contains(letter)) {
            continue;
        }
        std::wstring error;
        if (!mount(letter, error)) {
            Log::error(std::format(L"Autostart could not mount {}: {}",
                                   displayLetter(letter), error));
        }
    }
}

bool DriveManager::unmount(wchar_t letter, bool force, std::wstring& error) {
    error.clear();
    const wchar_t normalized =
        static_cast<wchar_t>(towupper(static_cast<wint_t>(letter)));
    const auto it = drives_.find(normalized);
    if (it == drives_.end()) {
        error = std::format(L"Drive {} is not mounted by this process.",
                            displayLetter(normalized));
        return false;
    }

    if (!it->second->unmount(force, error)) {
        return false;
    }
    drives_.erase(it);
    return true;
}

bool DriveManager::unmountAll(bool force, std::wstring& error) {
    error.clear();
    std::vector<wchar_t> letters;
    letters.reserve(drives_.size());
    for (const auto& [letter, drive] : drives_) {
        if (drive) {
            letters.push_back(letter);
        }
    }

    bool all = true;
    for (const wchar_t letter : letters) {
        std::wstring currentError;
        if (!unmount(letter, force, currentError)) {
            all = false;
            if (!error.empty()) {
                error += L" ";
            }
            error += currentError;
        }
    }
    return all;
}

bool DriveManager::addOrUpdate(const DriveConfig& config, std::wstring& error) {
    error.clear();
    if (!isDriveLetter(config.letter)) {
        error = L"The drive letter must be between A and Z.";
        return false;
    }
    if (config.sizeMiB == 0 || config.sizeMiB > kMaxDriveMiB) {
        error = L"The drive size is out of range.";
        return false;
    }
    if (config.letter == L'C') {
        error = L"C: is reserved for the system drive.";
        return false;
    }

    const wchar_t letter = static_cast<wchar_t>(
        towupper(static_cast<wint_t>(config.letter)));

    if (const auto it = drives_.find(letter);
        it != drives_.end() && it->second && it->second->mounted()) {
        const DriveConfig& current = it->second->config();
        if (config.gpuIndex != current.gpuIndex ||
            config.sizeMiB != current.sizeMiB ||
            config.kind != current.kind ||
            config.imagePath != current.imagePath ||
            config.removable != current.removable ||
            config.readOnly != current.readOnly ||
            config.useTempFolder != current.useTempFolder) {
            error = L"Unmount the drive before changing its size, GPU, device "
                    L"type, image, removable flag, read-only flag, or TEMP "
                    L"setting.";
            return false;
        }
    }

    if (config.useTempFolder) {
        for (const wchar_t other : configs_.letters()) {
            if (other == letter) {
                continue;
            }
            const auto otherConfig = configs_.find(other);
            if (otherConfig && otherConfig->useTempFolder &&
                mounted(other)) {
                error = std::format(
                    L"TEMP is already provided by drive {}. Disable it first.",
                    displayLetter(other));
                return false;
            }
        }
    }

    DriveConfig normalized = config;
    normalized.letter = letter;
    configs_.put(normalized);
    if (const auto stored = configs_.find(letter); stored) {
        if (const auto it = drives_.find(letter);
            it != drives_.end() && it->second && it->second->mounted()) {
            it->second->updateConfig(*stored);
        }
    }
    return save(error);
}

bool DriveManager::remove(wchar_t letter, std::wstring& error) {
    error.clear();
    const wchar_t normalized =
        static_cast<wchar_t>(towupper(static_cast<wint_t>(letter)));
    const bool mountedHere = drives_.contains(normalized);
    if (mountedHere) {
        if (!unmount(normalized, false, error)) {
            return false;
        }
    }
    if (!mountedHere) {
        if (const auto config = configs_.find(normalized);
            config && config->useTempFolder && isDriveLetterInUse(normalized)) {
            error = std::format(
                L"Drive {} still provides TEMP. Unmount it before removing its "
                L"configuration.",
                displayLetter(normalized));
            return false;
        }
    }
    if (!configs_.erase(normalized)) {
        error = std::format(L"No configuration exists for drive {}.",
                            displayLetter(normalized));
        return false;
    }
    return save(error);
}

bool DriveManager::save(std::wstring& error) {
    return configs_.save(error);
}

bool DriveManager::setAutoStart(bool enabled, std::wstring& error) {
    return setAutoStartTask(enabled, error);
}

bool DriveManager::autoStartEnabled() {
    return autoStartTaskExists();
}

std::vector<DriveStatus> DriveManager::statuses() const {
    std::vector<DriveStatus> result;
    result.reserve(configs_.drives().size());
    for (const wchar_t letter : configs_.letters()) {
        const auto config = configs_.find(letter);
        if (!config) {
            continue;
        }
        const Drive* drive = find(letter);
        if (drive != nullptr) {
            result.push_back(drive->status());
        } else {
            DriveStatus status;
            status.config = *config;
            result.push_back(status);
        }
    }
    return result;
}

}  // namespace grd
