#include "core/image.h"

#include <windows.h>
#include <winioctl.h>

#include <algorithm>
#include <format>
#include <utility>

#include "core/log.h"
#include "core/util.h"

namespace grd {
namespace {

constexpr DWORD kBufferSize = 1024 * 1024;
constexpr DWORD kSectorSize = 512;

std::wstring devicePath(wchar_t letter) {
    return L"\\\\.\\" + displayLetter(letter);
}

class PendingFile {
public:
    explicit PendingFile(std::wstring path) : path_(std::move(path)) {}
    ~PendingFile() {
        if (!committed_ && !path_.empty()) {
            DeleteFileW(path_.c_str());
        }
    }

    PendingFile(const PendingFile&) = delete;
    PendingFile& operator=(const PendingFile&) = delete;

    void commit() { committed_ = true; }

private:
    std::wstring path_;
    bool committed_ = false;
};

class VirtualBuffer {
public:
    VirtualBuffer() {
        buffer_ = static_cast<uint8_t*>(
            VirtualAlloc(nullptr, kBufferSize, MEM_COMMIT | MEM_RESERVE,
                         PAGE_READWRITE));
    }

    ~VirtualBuffer() {
        if (buffer_ != nullptr) {
            VirtualFree(buffer_, 0, MEM_RELEASE);
        }
    }

    VirtualBuffer(const VirtualBuffer&) = delete;
    VirtualBuffer& operator=(const VirtualBuffer&) = delete;

    uint8_t* get() const { return buffer_; }
    explicit operator bool() const { return buffer_ != nullptr; }

private:
    uint8_t* buffer_ = nullptr;
};

bool lockVolume(HANDLE volume, std::wstring& error) {
    DWORD returned = 0;
    if (DeviceIoControl(volume, FSCTL_LOCK_VOLUME, nullptr, 0, nullptr, 0,
                        &returned, nullptr) == FALSE) {
        error = L"Unable to lock the volume. Close every file and program "
                L"using the drive and try again: " +
                lastError();
        return false;
    }
    return true;
}

void unlockVolume(HANDLE volume) {
    DWORD returned = 0;
    DeviceIoControl(volume, FSCTL_UNLOCK_VOLUME, nullptr, 0, nullptr, 0,
                    &returned, nullptr);
}

bool deviceLength(HANDLE device, uint64_t& length, std::wstring& error) {
    GET_LENGTH_INFORMATION info = {};
    DWORD returned = 0;
    if (DeviceIoControl(device, IOCTL_DISK_GET_LENGTH_INFO, nullptr, 0, &info,
                        sizeof(info), &returned, nullptr) == FALSE) {
        error = L"Unable to query the drive size: " + lastError();
        return false;
    }
    length = static_cast<uint64_t>(info.Length.QuadPart);
    return true;
}

}  // namespace

bool saveImage(wchar_t letter, const std::wstring& path, std::wstring& error) {
    error.clear();
    if (path.empty()) {
        error = L"No image path was specified.";
        return false;
    }
    const std::wstring device = devicePath(letter);

    UniqueHandle volume(CreateFileW(
        device.c_str(), GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
        FILE_FLAG_NO_BUFFERING | FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
    if (!volume) {
        error = L"Unable to open " + device + L" for reading: " + lastError();
        return false;
    }

    if (!lockVolume(volume.get(), error)) {
        return false;
    }
    FlushFileBuffers(volume.get());

    uint64_t length = 0;
    if (!deviceLength(volume.get(), length, error)) {
        unlockVolume(volume.get());
        return false;
    }
    if (length % kSectorSize != 0) {
        error = L"The drive size is not a multiple of 512 bytes.";
        unlockVolume(volume.get());
        return false;
    }

    VirtualBuffer buffer;
    if (!buffer) {
        error = L"Unable to allocate the image copy buffer: " + lastError();
        unlockVolume(volume.get());
        return false;
    }

    const std::wstring temporary = path + L".tmp";
    PendingFile pending(temporary);
    UniqueHandle output(CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr,
                                    CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL,
                                    nullptr));
    if (!output) {
        error = L"Unable to create " + temporary + L": " + lastError();
        unlockVolume(volume.get());
        return false;
    }

    uint64_t offset = 0;
    while (offset < length) {
        const DWORD want = static_cast<DWORD>(
            std::min<uint64_t>(kBufferSize, length - offset));
        DWORD read = 0;
        if (ReadFile(volume.get(), buffer.get(), want, &read, nullptr) == FALSE ||
            read != want) {
            error = std::format(L"Unable to read the drive at offset {}: {}",
                                offset, lastError());
            unlockVolume(volume.get());
            return false;
        }

        DWORD written = 0;
        if (WriteFile(output.get(), buffer.get(), read, &written, nullptr) ==
                FALSE ||
            written != read) {
            error = L"Unable to write " + temporary + L": " + lastError();
            unlockVolume(volume.get());
            return false;
        }
        offset += read;
    }

    FlushFileBuffers(output.get());
    output.reset();
    unlockVolume(volume.get());

    if (MoveFileExW(temporary.c_str(), path.c_str(),
                    MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) == FALSE) {
        error = L"Unable to replace " + path + L": " + lastError();
        return false;
    }
    pending.commit();

    Log::info(std::format(L"Saved {} MiB from {}: to {}", length / (1024 * 1024),
                          displayLetter(letter), path));
    return true;
}

bool restoreImage(const std::wstring& path, wchar_t letter,
                  std::wstring& error) {
    error.clear();
    if (path.empty()) {
        error = L"No image path was specified.";
        return false;
    }
    const std::wstring device = devicePath(letter);

    UniqueHandle input(CreateFileW(path.c_str(), GENERIC_READ,
                                   FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                   FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
    if (!input) {
        error = L"Unable to open the image " + path + L": " + lastError();
        return false;
    }

    LARGE_INTEGER imageSize = {};
    if (GetFileSizeEx(input.get(), &imageSize) == FALSE || imageSize.QuadPart < 0) {
        error = L"Unable to read the image size: " + lastError();
        return false;
    }
    if (imageSize.QuadPart == 0) {
        error = L"The image file is empty.";
        return false;
    }
    if (imageSize.QuadPart % kSectorSize != 0) {
        error = L"The image size is not a multiple of 512 bytes.";
        return false;
    }

    UniqueHandle volume(CreateFileW(
        device.c_str(), GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
        FILE_FLAG_NO_BUFFERING | FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
    if (!volume) {
        error = L"Unable to open " + device + L" for writing: " + lastError();
        return false;
    }

    if (!lockVolume(volume.get(), error)) {
        return false;
    }

    uint64_t length = 0;
    if (!deviceLength(volume.get(), length, error)) {
        unlockVolume(volume.get());
        return false;
    }

    VirtualBuffer buffer;
    if (!buffer) {
        error = L"Unable to allocate the image copy buffer: " + lastError();
        unlockVolume(volume.get());
        return false;
    }

    const uint64_t transfer =
        std::min(static_cast<uint64_t>(imageSize.QuadPart), length);
    uint64_t offset = 0;

    while (offset < transfer) {
        const DWORD want = static_cast<DWORD>(
            std::min<uint64_t>(kBufferSize, transfer - offset));
        DWORD read = 0;
        if (ReadFile(input.get(), buffer.get(), want, &read, nullptr) == FALSE ||
            read != want) {
            error = std::format(L"Unable to read the image at offset {}: {}",
                                offset, lastError());
            unlockVolume(volume.get());
            return false;
        }

        DWORD written = 0;
        if (WriteFile(volume.get(), buffer.get(), read, &written, nullptr) ==
                FALSE ||
            written != read) {
            error = std::format(L"Unable to write the drive at offset {}: {}",
                                offset, lastError());
            unlockVolume(volume.get());
            return false;
        }
        offset += read;
    }

    FlushFileBuffers(volume.get());
    unlockVolume(volume.get());
    Log::info(std::format(L"Restored {} MiB from {} to {}:", transfer / (1024 * 1024),
                          path, displayLetter(letter)));
    return true;
}

}  // namespace grd
