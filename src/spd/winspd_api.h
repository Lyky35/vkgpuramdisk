#pragma once

#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <string>

namespace grd::spd {

// WinSpd - Windows Storage Proxy Driver, Copyright (C) Bill Zissimopoulos.
// https://github.com/winfsp/winspd
// This is a minimal ABI binding for the public WinSpd user-mode API.

struct StorageUnit;

struct StorageUnitParams {
    GUID Guid;
    UINT64 BlockCount;
    UINT32 BlockLength;
    UCHAR ProductId[16];
    UCHAR ProductRevisionLevel[4];
    UINT8 DeviceType;
    UINT32 WriteProtected : 1;
    UINT32 CacheSupported : 1;
    UINT32 UnmapSupported : 1;
    UINT32 EjectDisabled : 1;
    UINT32 MaxTransferLength;
    UINT64 Reserved[8];
};

struct StorageUnitStatus {
    UINT8 ScsiStatus;
    UINT8 SenseKey;
    UINT8 ASC;
    UINT8 ASCQ;
    UINT64 Information;
    UINT64 ReservedCSI;
    UINT32 ReservedSKS;
    UINT32 ReservedFRU : 8;
    UINT32 InformationValid : 1;
};

struct UnmapDescriptor {
    UINT64 BlockAddress;
    UINT32 BlockCount;
    UINT32 Reserved;
};

struct Partition {
    UINT8 Type;
    UINT8 Active;
    UINT64 BlockAddress;
    UINT64 BlockCount;
};

struct StorageUnitInterface {
    BOOLEAN (*Read)(StorageUnit* unit, PVOID buffer, UINT64 blockAddress,
                    UINT32 blockCount, BOOLEAN flush,
                    StorageUnitStatus* status);
    BOOLEAN (*Write)(StorageUnit* unit, PVOID buffer, UINT64 blockAddress,
                     UINT32 blockCount, BOOLEAN flush,
                     StorageUnitStatus* status);
    BOOLEAN (*Flush)(StorageUnit* unit, UINT64 blockAddress, UINT32 blockCount,
                     StorageUnitStatus* status);
    BOOLEAN (*Unmap)(StorageUnit* unit, UnmapDescriptor descriptors[],
                     UINT32 count, StorageUnitStatus* status);
    BOOLEAN (*Reserved[12])();
};

struct StorageUnit {
    UINT16 Version;
    PVOID UserContext;
    StorageUnitParams StorageUnitParams;
    const StorageUnitInterface* Interface;
    PVOID (*BufferAlloc)(size_t);
    VOID (*BufferFree)(PVOID);
    HANDLE Handle;
    UINT32 Btl;
    DWORD DispatcherThreadId;
    HANDLE DispatcherThread;
    ULONG DispatcherThreadCount;
    DWORD DispatcherError;
    UINT32 DebugLog;
};

static_assert(sizeof(StorageUnitParams) == 128);
static_assert(offsetof(StorageUnitParams, MaxTransferLength) == 56);
static_assert(sizeof(StorageUnitStatus) == 32);
static_assert(sizeof(UnmapDescriptor) == 16);
static_assert(sizeof(Partition) == 24);
static_assert(offsetof(StorageUnit, UserContext) == 8);

class Api {
public:
    Api() = default;
    ~Api();

    Api(const Api&) = delete;
    Api& operator=(const Api&) = delete;

    bool load(std::wstring& error);
    bool loaded() const { return module_ != nullptr; }

    DWORD create(PWSTR deviceName, const StorageUnitParams& params,
                 const StorageUnitInterface& interface,
                 StorageUnit** storageUnit) const;
    void destroy(StorageUnit* storageUnit) const;
    void shutdown(StorageUnit* storageUnit) const;
    DWORD startDispatcher(StorageUnit* storageUnit, ULONG threadCount) const;
    void waitDispatcher(StorageUnit* storageUnit) const;
    DWORD definePartitionTable(Partition partitions[4], ULONG count,
                               UINT8 buffer[512]) const;

private:
    using CreateFn = DWORD(WINAPI*)(PWSTR, const StorageUnitParams*,
                                    const StorageUnitInterface*,
                                    StorageUnit**);
    using DeleteFn = VOID(WINAPI*)(StorageUnit*);
    using ShutdownFn = VOID(WINAPI*)(StorageUnit*);
    using StartDispatcherFn = DWORD(WINAPI*)(StorageUnit*, ULONG);
    using WaitDispatcherFn = VOID(WINAPI*)(StorageUnit*);
    using DefinePartitionTableFn = DWORD(WINAPI*)(Partition[4], ULONG, UINT8[512]);

    HMODULE module_ = nullptr;
    CreateFn create_ = nullptr;
    DeleteFn delete_ = nullptr;
    ShutdownFn shutdown_ = nullptr;
    StartDispatcherFn startDispatcher_ = nullptr;
    WaitDispatcherFn waitDispatcher_ = nullptr;
    DefinePartitionTableFn definePartitionTable_ = nullptr;
};

}  // namespace grd::spd
