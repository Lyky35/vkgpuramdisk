#pragma once

#include <windows.h>

#include <cstdint>
#include <string>

#include "core/config.h"
#include "spd/winspd_api.h"

namespace grd {

class VulkanDrive;

class SpdStorage {
public:
    SpdStorage(VulkanDrive& gpu, DriveConfig config, uint64_t sizeBytes,
               std::wstring imagePath);
    ~SpdStorage();

    SpdStorage(const SpdStorage&) = delete;
    SpdStorage& operator=(const SpdStorage&) = delete;

    bool mount(std::wstring& error);
    bool unmount(bool force, std::wstring& error);
    bool format(FileSystem filesystem, const std::wstring& label,
                std::wstring& error);

    bool mounted() const { return mounted_; }

    static bool isAvailable();

private:
    static BOOLEAN readCallback(spd::StorageUnit* unit, PVOID buffer,
                                UINT64 blockAddress, UINT32 blockCount,
                                BOOLEAN flush, spd::StorageUnitStatus* status);
    static BOOLEAN writeCallback(spd::StorageUnit* unit, PVOID buffer,
                                 UINT64 blockAddress, UINT32 blockCount,
                                 BOOLEAN flush, spd::StorageUnitStatus* status);
    static BOOLEAN flushCallback(spd::StorageUnit* unit, UINT64 blockAddress,
                                 UINT32 blockCount,
                                 spd::StorageUnitStatus* status);
    static BOOLEAN unmapCallback(spd::StorageUnit* unit,
                                 spd::UnmapDescriptor descriptors[],
                                 UINT32 count,
                                 spd::StorageUnitStatus* status);

    bool writePartitionTable(std::wstring& error);
    bool restoreImageBeforeStart(std::wstring& error);
    bool createUnit(std::wstring& error);
    bool startDispatcher(std::wstring& error);
    bool waitForDisk(std::wstring& error);
    bool findDisk(DWORD& diskNumber, std::wstring& error) const;
    bool prepareVolume(std::wstring& error);
    bool formatVolume(FileSystem filesystem, const std::wstring& label,
                      std::wstring& error);
    bool assignLetter(std::wstring& error);
    bool removeCurrentLetter();
    bool waitForLetter(std::wstring& error) const;
    bool runDiskpart(const std::wstring& script, std::wstring& error,
                     bool ignoreFailure = false);
    void stopUnit();

    static bool validRange(uint64_t blockAddress, uint32_t blockCount,
                           uint64_t totalBlocks);
    static void setMediumError(spd::StorageUnitStatus* status);
    static void setWriteProtected(spd::StorageUnitStatus* status);

    VulkanDrive& gpu_;
    DriveConfig config_;
    uint64_t sizeBytes_ = 0;
    uint64_t blockCount_ = 0;
    std::wstring imagePath_;
    std::string productId_;

    spd::Api api_;
    spd::StorageUnit* unit_ = nullptr;
    bool dispatcherStarted_ = false;
    bool mounted_ = false;
    bool restoredImage_ = false;
    DWORD diskNumber_ = 0;
};

}  // namespace grd
