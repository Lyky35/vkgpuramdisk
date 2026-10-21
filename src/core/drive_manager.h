#pragma once

#include <map>
#include <memory>
#include <string>
#include <vector>

#include "core/config.h"
#include "core/drive.h"

namespace grd {

class DriveManager {
public:
    DriveManager();
    ~DriveManager();

    DriveManager(const DriveManager&) = delete;
    DriveManager& operator=(const DriveManager&) = delete;

    bool load(std::wstring& error);

    const ConfigStore& configs() const { return configs_; }
    ConfigStore& configs() { return configs_; }

    bool mount(wchar_t letter, std::wstring& error);
    void mountAutoStart();
    bool unmount(wchar_t letter, bool force, std::wstring& error);
    bool unmountAll(bool force, std::wstring& error);

    bool addOrUpdate(const DriveConfig& config, std::wstring& error);
    bool remove(wchar_t letter, std::wstring& error);
    bool save(std::wstring& error);

    bool setAutoStart(bool enabled, std::wstring& error);
    bool autoStartEnabled();

    Drive* find(wchar_t letter);
    const Drive* find(wchar_t letter) const;

    std::vector<DriveStatus> statuses() const;
    bool mounted(wchar_t letter) const;

private:
    ConfigStore configs_;
    std::map<wchar_t, std::unique_ptr<Drive>> drives_;
};

}  // namespace grd
