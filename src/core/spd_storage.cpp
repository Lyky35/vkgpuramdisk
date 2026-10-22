#include "core/spd_storage.h"

#include <objbase.h>
#include <winioctl.h>

#include <algorithm>
#include <cstring>
#include <format>
#include <iterator>
#include <limits>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "core/log.h"
#include "core/util.h"
#include "gpu/vulkan_drive.h"

namespace grd {
namespace {

constexpr uint64_t kMiB = 1024ull * 1024ull;
constexpr uint64_t kSectorSize = 512;
constexpr uint64_t kPartitionStartBlock = 2048;
constexpr uint64_t kPartitionOffsetBytes = kPartitionStartBlock * kSectorSize;
constexpr uint64_t kRestoreBufferSize = 4ull * kMiB;
constexpr uint32_t kMaxTransferLength = 4u * 1024u * 1024u;
constexpr ULONG kDispatcherThreads = 8;

std::wstring diskpartPath() {
    return joinPath(systemDirectory(), L"diskpart.exe");
}

bool writeAll(HANDLE file, const void* data, size_t length) {
    const auto* bytes = static_cast<const uint8_t*>(data);
    while (length > 0) {
        DWORD written = 0;
        const DWORD chunk = static_cast<DWORD>(
            std::min<size_t>(length, std::numeric_limits<DWORD>::max()));
        if (WriteFile(file, bytes, chunk, &written, nullptr) == FALSE ||
            written == 0) {
            return false;
        }
        bytes += written;
        length -= written;
    }
    return true;
}

std::string readFileText(HANDLE file) {
    LARGE_INTEGER size = {};
    if (GetFileSizeEx(file, &size) == FALSE || size.QuadPart <= 0) {
        return {};
    }

    LARGE_INTEGER zero = {};
    SetFilePointerEx(file, zero, nullptr, FILE_BEGIN);

    const size_t limit = static_cast<size_t>(
        std::min<int64_t>(size.QuadPart, 64 * 1024));
    std::string text(limit, '\0');
    DWORD read = 0;
    if (ReadFile(file, text.data(), static_cast<DWORD>(limit), &read, nullptr) ==
        FALSE) {
        return {};
    }
    text.resize(read);
    return text;
}

std::wstring trimProduct(const std::string& text) {
    size_t end = text.size();
    while (end > 0 && (text[end - 1] == ' ' || text[end - 1] == '\0')) {
        --end;
    }
    return widen(text.substr(0, end), CP_ACP);
}

}  // namespace

SpdStorage::SpdStorage(VulkanDrive& gpu, DriveConfig config, uint64_t sizeBytes,
                       std::wstring imagePath)
    : gpu_(gpu),
      config_(std::move(config)),
      sizeBytes_(sizeBytes),
      imagePath_(std::move(imagePath)) {
    blockCount_ = sizeBytes_ / kSectorSize;
    productId_ = "GpuRamDrive-" +
                 std::string(1, static_cast<char>(config_.letter));
}

SpdStorage::~SpdStorage() {
    stopUnit();
}

bool SpdStorage::isAvailable() {
    spd::Api api;
    std::wstring error;
    return api.load(error);
}

bool SpdStorage::validRange(uint64_t blockAddress, uint32_t blockCount,
                            uint64_t totalBlocks) {
    if (blockCount == 0) {
        return true;
    }
    if (blockAddress >= totalBlocks) {
        return false;
    }
    return blockCount <= totalBlocks - blockAddress;
}

void SpdStorage::setMediumError(spd::StorageUnitStatus* status) {
    *status = {};
    status->ScsiStatus = 0x02;
    status->SenseKey = 0x03;
    status->ASC = 0x11;
    status->ASCQ = 0x00;
}

void SpdStorage::setWriteProtected(spd::StorageUnitStatus* status) {
    *status = {};
    status->ScsiStatus = 0x02;
    status->SenseKey = 0x07;
    status->ASC = 0x27;
    status->ASCQ = 0x00;
}

BOOLEAN SpdStorage::readCallback(spd::StorageUnit* unit, PVOID buffer,
                                 UINT64 blockAddress, UINT32 blockCount,
                                 BOOLEAN flush,
                                 spd::StorageUnitStatus* status) {
    (void)flush;
    auto* self = unit == nullptr ? nullptr
                                 : static_cast<SpdStorage*>(unit->UserContext);
    if (self == nullptr ||
        !validRange(blockAddress, blockCount, self->blockCount_)) {
        setMediumError(status);
        return FALSE;
    }

    try {
        self->gpu_.read(blockAddress * kSectorSize, buffer,
                        static_cast<size_t>(blockCount * kSectorSize));
    } catch (const std::exception& ex) {
        Log::error(L"WinSpd read failed: " + widen(ex.what()));
        setMediumError(status);
        return FALSE;
    } catch (...) {
        Log::error(L"WinSpd read failed with an unknown error.");
        setMediumError(status);
        return FALSE;
    }
    return TRUE;
}

BOOLEAN SpdStorage::writeCallback(spd::StorageUnit* unit, PVOID buffer,
                                  UINT64 blockAddress, UINT32 blockCount,
                                  BOOLEAN flush,
                                  spd::StorageUnitStatus* status) {
    (void)flush;
    auto* self = unit == nullptr ? nullptr
                                 : static_cast<SpdStorage*>(unit->UserContext);
    if (self == nullptr ||
        !validRange(blockAddress, blockCount, self->blockCount_)) {
        setMediumError(status);
        return FALSE;
    }
    if (self->config_.readOnly) {
        setWriteProtected(status);
        return FALSE;
    }

    try {
        self->gpu_.write(blockAddress * kSectorSize, buffer,
                         static_cast<size_t>(blockCount * kSectorSize));
    } catch (const std::exception& ex) {
        Log::error(L"WinSpd write failed: " + widen(ex.what()));
        setMediumError(status);
        return FALSE;
    } catch (...) {
        Log::error(L"WinSpd write failed with an unknown error.");
        setMediumError(status);
        return FALSE;
    }
    return TRUE;
}

BOOLEAN SpdStorage::flushCallback(spd::StorageUnit* unit, UINT64 blockAddress,
                                  UINT32 blockCount,
                                  spd::StorageUnitStatus* status) {
    (void)unit;
    (void)blockAddress;
    (void)blockCount;
    (void)status;
    return TRUE;
}

BOOLEAN SpdStorage::unmapCallback(spd::StorageUnit* unit,
                                  spd::UnmapDescriptor descriptors[],
                                  UINT32 count,
                                  spd::StorageUnitStatus* status) {
    (void)unit;
    (void)descriptors;
    (void)count;
    (void)status;
    return TRUE;
}

bool SpdStorage::writePartitionTable(std::wstring& error) {
    if (blockCount_ <= kPartitionStartBlock) {
        error = L"The drive is too small for a WinSpd partition.";
        return false;
    }

    spd::Partition partition = {};
    partition.Type = config_.filesystem == FileSystem::Fat32 ? 0x0C : 0x07;
    partition.Active = 0;
    partition.BlockAddress = kPartitionStartBlock;
    partition.BlockCount = blockCount_ - kPartitionStartBlock;

    UINT8 mbr[512] = {};
    const DWORD result = api_.definePartitionTable(&partition, 1, mbr);
    if (result != ERROR_SUCCESS) {
        error = L"WinSpd could not create a partition table: " +
                win32Error(result);
        return false;
    }

    try {
        gpu_.write(0, mbr, sizeof(mbr));
    } catch (const std::exception& ex) {
        error = L"Unable to write the partition table to GPU memory: " +
                widen(ex.what());
        return false;
    }
    return true;
}

bool SpdStorage::restoreImageBeforeStart(std::wstring& error) {
    if (imagePath_.empty()) {
        return true;
    }
    if (!fileExists(imagePath_)) {
        error = L"The image does not exist: " + imagePath_;
        return false;
    }

    UniqueHandle input(CreateFileW(
        imagePath_.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
        OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
    if (!input) {
        error = L"Unable to open the image " + imagePath_ + L": " + lastError();
        return false;
    }

    LARGE_INTEGER imageSize = {};
    if (GetFileSizeEx(input.get(), &imageSize) == FALSE ||
        imageSize.QuadPart <= 0) {
        error = L"The image is empty or unreadable.";
        return false;
    }

    const uint64_t available =
        (blockCount_ - kPartitionStartBlock) * kSectorSize;
    if (static_cast<uint64_t>(imageSize.QuadPart) > available) {
        Log::warn(std::format(
            L"Image is {} MiB but the WinSpd partition is {} MiB; restoring "
            L"the first {} MiB.",
            static_cast<uint64_t>(imageSize.QuadPart) / kMiB,
            available / kMiB, available / kMiB));
    }

    const uint64_t transfer = std::min<uint64_t>(
        static_cast<uint64_t>(imageSize.QuadPart), available);
    std::vector<uint8_t> buffer(kRestoreBufferSize);
    uint64_t offset = 0;

    while (offset < transfer) {
        const DWORD want = static_cast<DWORD>(
            std::min<uint64_t>(buffer.size(), transfer - offset));
        DWORD read = 0;
        while (read < want) {
            DWORD got = 0;
            if (ReadFile(input.get(), buffer.data() + read, want - read, &got,
                         nullptr) == FALSE ||
                got == 0) {
                error = std::format(
                    L"Unable to read the image at offset {}: {}", offset,
                    lastError());
                return false;
            }
            read += got;
        }

        const uint64_t aligned = (static_cast<uint64_t>(read) + 511) & ~511ull;
        if (aligned > read) {
            std::memset(buffer.data() + read, 0,
                        static_cast<size_t>(aligned - read));
        }
        const uint64_t writeSize =
            std::min<uint64_t>(aligned, available - offset);

        try {
            gpu_.write(kPartitionOffsetBytes + offset, buffer.data(),
                       static_cast<size_t>(writeSize));
        } catch (const std::exception& ex) {
            error = L"Unable to restore the image into GPU memory: " +
                    widen(ex.what());
            return false;
        }
        offset += read;
    }

    restoredImage_ = true;
    Log::info(std::format(L"Restored {} MiB from {} into the WinSpd partition.",
                          transfer / kMiB, imagePath_));
    return true;
}

bool SpdStorage::createUnit(std::wstring& error) {
    spd::StorageUnitParams params = {};
    if (FAILED(CoCreateGuid(&params.Guid))) {
        error = L"Unable to create a WinSpd storage-unit identity.";
        return false;
    }

    params.BlockCount = blockCount_;
    params.BlockLength = static_cast<UINT32>(kSectorSize);
    params.DeviceType = 0;
    params.WriteProtected = config_.readOnly ? 1 : 0;
    params.CacheSupported = 0;
    params.UnmapSupported = 0;
    params.EjectDisabled = 1;
    params.MaxTransferLength = static_cast<UINT32>(
        std::min<uint64_t>(kMaxTransferLength, sizeBytes_));

    std::string product = productId_;
    product.resize(16, ' ');
    std::memcpy(params.ProductId, product.data(), 16);
    std::memcpy(params.ProductRevisionLevel, "1.0 ", 4);

    spd::StorageUnitInterface storageInterface = {};
    storageInterface.Read = &SpdStorage::readCallback;
    storageInterface.Write = &SpdStorage::writeCallback;
    storageInterface.Flush = &SpdStorage::flushCallback;
    storageInterface.Unmap = &SpdStorage::unmapCallback;

    const DWORD result =
        api_.create(nullptr, params, storageInterface, &unit_);
    if (result != ERROR_SUCCESS) {
        unit_ = nullptr;
        error = L"WinSpd could not create the storage unit: " +
                win32Error(result) +
                L". Make sure the WinSpd driver is installed and running.";
        return false;
    }

    unit_->UserContext = this;
    Log::debug(std::format(L"WinSpd storage unit {} created for {}.",
                           widen(productId_),
                           displayLetter(config_.letter)));
    return true;
}

bool SpdStorage::startDispatcher(std::wstring& error) {
    const DWORD result = api_.startDispatcher(unit_, kDispatcherThreads);
    if (result != ERROR_SUCCESS) {
        error = L"WinSpd could not start its dispatcher: " +
                win32Error(result);
        api_.shutdown(unit_);
        api_.waitDispatcher(unit_);
        return false;
    }
    dispatcherStarted_ = true;
    return true;
}

bool SpdStorage::findDisk(DWORD& diskNumber, std::wstring& error) const {
    for (DWORD number = 0; number < 128; ++number) {
        const std::wstring path =
            L"\\\\.\\PhysicalDrive" + std::to_wstring(number);
        UniqueHandle disk(CreateFileW(
            path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
            nullptr, OPEN_EXISTING, 0, nullptr));
        if (!disk) {
            continue;
        }

        STORAGE_PROPERTY_QUERY query = {};
        query.PropertyId = StorageDeviceProperty;
        query.QueryType = PropertyStandardQuery;

        std::vector<uint8_t> buffer(1024);
        DWORD returned = 0;
        if (DeviceIoControl(disk.get(), IOCTL_STORAGE_QUERY_PROPERTY, &query,
                            sizeof(query), buffer.data(),
                            static_cast<DWORD>(buffer.size()), &returned,
                            nullptr) == FALSE) {
            continue;
        }

        const auto* descriptor =
            reinterpret_cast<const STORAGE_DEVICE_DESCRIPTOR*>(buffer.data());
        if (descriptor->ProductIdOffset == 0 ||
            descriptor->ProductIdOffset >= returned) {
            continue;
        }

        const char* product = reinterpret_cast<const char*>(
            buffer.data() + descriptor->ProductIdOffset);
        size_t productLength = 0;
        const size_t available = returned - descriptor->ProductIdOffset;
        while (productLength < available && product[productLength] != '\0') {
            ++productLength;
        }
        const std::wstring productName =
            trimProduct(std::string(product, productLength));
        if (productName != widen(productId_)) {
            continue;
        }

        GET_LENGTH_INFORMATION length = {};
        if (DeviceIoControl(disk.get(), IOCTL_DISK_GET_LENGTH_INFO, nullptr, 0,
                            &length, sizeof(length), &returned, nullptr) !=
            FALSE) {
            if (static_cast<uint64_t>(length.Length.QuadPart) != sizeBytes_) {
                continue;
            }
        }

        diskNumber = number;
        return true;
    }

    error = L"The WinSpd disk did not appear. Verify that the WinSpd driver "
            L"is installed and that no other program is using it.";
    return false;
}

bool SpdStorage::waitForDisk(std::wstring& error) {
    std::wstring lastErrorText;
    for (int attempt = 0; attempt < 100; ++attempt) {
        if (findDisk(diskNumber_, lastErrorText)) {
            return true;
        }
        Sleep(100);
    }
    error = lastErrorText.empty()
                ? L"The WinSpd disk did not appear."
                : lastErrorText;
    return false;
}

bool SpdStorage::runDiskpart(const std::wstring& script, std::wstring& error,
                             bool ignoreFailure) {
    wchar_t tempPath[MAX_PATH] = {};
    if (GetTempPathW(MAX_PATH, tempPath) == 0) {
        error = L"Unable to locate the temporary directory: " + lastError();
        return false;
    }

    wchar_t scriptFile[MAX_PATH] = {};
    if (GetTempFileNameW(tempPath, L"grd", 0, scriptFile) == 0) {
        error = L"Unable to create a diskpart script: " + lastError();
        return false;
    }

    wchar_t outputFile[MAX_PATH] = {};
    if (GetTempFileNameW(tempPath, L"grd", 0, outputFile) == 0) {
        DeleteFileW(scriptFile);
        error = L"Unable to create a diskpart output file: " + lastError();
        return false;
    }

    SECURITY_ATTRIBUTES attributes = {};
    attributes.nLength = sizeof(attributes);
    attributes.bInheritHandle = TRUE;

    UniqueHandle scriptHandle(CreateFileW(
        scriptFile, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!scriptHandle) {
        DeleteFileW(scriptFile);
        DeleteFileW(outputFile);
        error = L"Unable to write the diskpart script: " + lastError();
        return false;
    }

    const std::string narrowScript = narrow(script, CP_ACP);
    const bool wrote =
        writeAll(scriptHandle.get(), narrowScript.data(), narrowScript.size());
    scriptHandle.reset();
    if (!wrote) {
        DeleteFileW(scriptFile);
        DeleteFileW(outputFile);
        error = L"Unable to write the diskpart script.";
        return false;
    }

    UniqueHandle output(CreateFileW(
        outputFile, GENERIC_WRITE | GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE, &attributes, CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!output) {
        DeleteFileW(scriptFile);
        DeleteFileW(outputFile);
        error = L"Unable to create the diskpart output file: " + lastError();
        return false;
    }

    UniqueHandle nul(CreateFileW(L"NUL", GENERIC_READ | GENERIC_WRITE,
                                 FILE_SHARE_READ | FILE_SHARE_WRITE,
                                 &attributes, OPEN_EXISTING, 0, nullptr));
    if (!nul) {
        DeleteFileW(scriptFile);
        DeleteFileW(outputFile);
        error = L"Unable to open NUL for diskpart: " + lastError();
        return false;
    }

    const std::wstring command =
        quoteArgument(diskpartPath()) + L" /s " + quoteArgument(scriptFile);
    std::vector<wchar_t> commandLine(command.begin(), command.end());
    commandLine.push_back(L'\0');

    STARTUPINFOW startup = {};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    startup.wShowWindow = SW_HIDE;
    startup.hStdInput = nul.get();
    startup.hStdOutput = output.get();
    startup.hStdError = output.get();

    PROCESS_INFORMATION processInfo = {};
    if (CreateProcessW(nullptr, commandLine.data(), nullptr, nullptr, TRUE,
                       CREATE_NO_WINDOW, nullptr, nullptr, &startup,
                       &processInfo) == FALSE) {
        DeleteFileW(scriptFile);
        DeleteFileW(outputFile);
        error = L"Unable to start diskpart: " + lastError();
        return false;
    }

    UniqueHandle process(processInfo.hProcess);
    UniqueHandle thread(processInfo.hThread);

    const DWORD wait = WaitForSingleObject(process.get(), 5 * 60 * 1000);
    if (wait == WAIT_TIMEOUT || wait == WAIT_FAILED) {
        TerminateProcess(process.get(), 1);
        WaitForSingleObject(process.get(), 5000);
    }

    DWORD exitCode = 1;
    GetExitCodeProcess(process.get(), &exitCode);
    output.reset();

    UniqueHandle reader(CreateFileW(
        outputFile, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    const std::wstring text =
        reader ? trim(widen(readFileText(reader.get()), CP_OEMCP))
               : std::wstring();

    DeleteFileW(scriptFile);
    DeleteFileW(outputFile);

    if (wait == WAIT_TIMEOUT) {
        error = L"diskpart timed out.";
        return false;
    }
    if (wait == WAIT_FAILED) {
        error = L"Unable to wait for diskpart: " + lastError();
        return false;
    }
    if (exitCode != 0) {
        if (ignoreFailure) {
            Log::debug(L"diskpart ignored an error: " + text);
            return true;
        }
        error = std::format(L"diskpart failed with exit code {}", exitCode);
        if (!text.empty()) {
            error += L": " + text;
        }
        return false;
    }

    if (!text.empty()) {
        Log::debug(L"diskpart: " + text);
    }
    return true;
}

bool SpdStorage::removeCurrentLetter() {
    std::wstring error;
    const std::wstring script = std::format(
        L"select disk {}\r\nonline disk noerr\r\nselect partition 1\r\n"
        L"remove\r\nexit\r\n",
        diskNumber_);
    return runDiskpart(script, error, true);
}

bool SpdStorage::waitForLetter(std::wstring& error) const {
    for (int attempt = 0; attempt < 100; ++attempt) {
        if (isDriveLetterInUse(config_.letter)) {
            return true;
        }
        Sleep(100);
    }
    error = std::format(L"WinSpd created the disk but drive {} was not assigned.",
                        displayLetter(config_.letter));
    return false;
}

bool SpdStorage::assignLetter(std::wstring& error) {
    if (!findDisk(diskNumber_, error)) {
        return false;
    }

    removeCurrentLetter();
    const std::wstring script = std::format(
        L"select disk {}\r\nonline disk noerr\r\nselect partition 1\r\n"
        L"assign letter={}\r\nexit\r\n",
        diskNumber_, config_.letter);
    if (!runDiskpart(script, error)) {
        Sleep(500);
        if (!runDiskpart(script, error)) {
            return false;
        }
    }
    return waitForLetter(error);
}

bool SpdStorage::formatVolume(FileSystem filesystem,
                              const std::wstring& label,
                              std::wstring& error) {
    if (filesystem == FileSystem::Fat32 && sizeBytes_ > 32ull * 1024ull * kMiB) {
        error = L"Windows cannot format a FAT32 volume larger than 32 GiB. "
                L"Choose exFAT or NTFS.";
        return false;
    }

    if (!findDisk(diskNumber_, error)) {
        return false;
    }
    removeCurrentLetter();

    const wchar_t* fileSystem = filesystem == FileSystem::Fat32
                                    ? L"fat32"
                                    : (filesystem == FileSystem::ExFat ? L"exfat"
                                                                      : L"ntfs");
    const std::wstring script = std::format(
        L"select disk {}\r\nonline disk noerr\r\nselect partition 1\r\n"
        L"format fs={} quick\r\nassign letter={}\r\nexit\r\n",
        diskNumber_, fileSystem, config_.letter);
    if (!runDiskpart(script, error)) {
        Sleep(500);
        if (!runDiskpart(script, error)) {
            return false;
        }
    }
    if (!waitForLetter(error)) {
        return false;
    }

    if (!label.empty()) {
        const std::wstring root = driveRoot(config_.letter);
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

    restoredImage_ = false;
    Log::info(std::format(L"Formatted {}: as {}.", displayLetter(config_.letter),
                          fileSystemName(filesystem)));
    return true;
}

bool SpdStorage::prepareVolume(std::wstring& error) {
    if (restoredImage_) {
        return assignLetter(error);
    }
    return formatVolume(config_.filesystem, config_.label, error);
}

bool SpdStorage::mount(std::wstring& error) {
    error.clear();
    if (!api_.load(error)) {
        return false;
    }
    if (!writePartitionTable(error)) {
        return false;
    }
    if (!restoreImageBeforeStart(error)) {
        return false;
    }
    if (!createUnit(error)) {
        return false;
    }
    if (!startDispatcher(error)) {
        stopUnit();
        return false;
    }
    if (!waitForDisk(error)) {
        stopUnit();
        return false;
    }
    if (!prepareVolume(error)) {
        stopUnit();
        return false;
    }

    mounted_ = true;
    Log::info(std::format(L"WinSpd mounted {}: ({} MiB).",
                          displayLetter(config_.letter), sizeBytes_ / kMiB));
    return true;
}

bool SpdStorage::unmount(bool force, std::wstring& error) {
    (void)force;
    error.clear();
    if (unit_ == nullptr && !mounted_) {
        return true;
    }

    stopUnit();
    mounted_ = false;

    for (int attempt = 0; attempt < 100; ++attempt) {
        if (!isDriveLetterInUse(config_.letter)) {
            return true;
        }
        Sleep(100);
    }

    Log::warn(std::format(L"Drive {}: is still reported after WinSpd stopped.",
                          displayLetter(config_.letter)));
    return true;
}

bool SpdStorage::format(FileSystem filesystem, const std::wstring& label,
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

void SpdStorage::stopUnit() {
    if (unit_ == nullptr) {
        return;
    }

    if (dispatcherStarted_) {
        api_.shutdown(unit_);
        api_.waitDispatcher(unit_);
        dispatcherStarted_ = false;
    }
    api_.destroy(unit_);
    unit_ = nullptr;
}

}  // namespace grd
