#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "core/config.h"
#include "core/imdisk_api.h"
#include "core/proxy_server.h"

namespace grd {

class VulkanDrive;

struct DriveStatus {
    DriveConfig config;
    bool mounted = false;
    uint64_t sizeBytes = 0;
    bool directMapped = false;
    std::wstring backend;
};

// Owns one Vulkan allocation, its ImDisk proxy server, and the mounted volume.
class Drive {
public:
    explicit Drive(DriveConfig config);
    ~Drive();

    Drive(const Drive&) = delete;
    Drive& operator=(const Drive&) = delete;

    bool mount(std::wstring& error);
    bool unmount(bool force, std::wstring& error);

    bool mounted() const { return mounted_; }
    const DriveConfig& config() const { return config_; }
    void updateConfig(const DriveConfig& config) { config_ = config; }
    DriveStatus status() const;

    bool saveNow(std::wstring& error);
    bool restoreNow(std::wstring& error);
    bool formatNow(FileSystem filesystem, const std::wstring& label,
                   std::wstring& error);

private:
    bool startProxy(std::wstring& error);
    bool createDevice(bool readOnly, std::wstring& error);
    bool removeDevice(bool force, std::wstring& error);
    bool remountReadOnly(std::wstring& error);
    bool formatVolume(FileSystem filesystem, const std::wstring& label,
                      std::wstring& error);
    void resetStorage();
    std::wstring resolvedImagePath() const;

    DriveConfig config_;
    std::unique_ptr<VulkanDrive> gpu_;
    DWORD deviceNumber_ = 0;
    bool deviceAttached_ = false;
    ProxyServer proxy_;
    ImDiskApi imdisk_;
    bool mounted_ = false;
    bool ownsTemp_ = false;
    uint64_t sizeBytes_ = 0;
};

}  // namespace grd
