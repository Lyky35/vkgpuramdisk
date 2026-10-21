#pragma once

#include <windows.h>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace grd {

std::wstring widen(std::string_view text, UINT codePage = CP_UTF8);
std::string narrow(std::wstring_view text, UINT codePage = CP_UTF8);

std::wstring win32Error(DWORD code);
std::wstring lastError();

std::wstring modulePath();
std::wstring executableDir();
std::wstring localAppDataDir();
std::wstring configDir();

std::wstring joinPath(std::wstring_view left, std::wstring_view right);
bool ensureDirectory(const std::wstring& path);
bool fileExists(const std::wstring& path);

std::wstring trim(std::wstring_view text);
std::wstring lower(std::wstring_view text);
std::optional<uint64_t> parseUInt64(std::wstring_view text);

std::wstring driveRoot(wchar_t letter);
std::wstring displayLetter(wchar_t letter);
bool isDriveLetter(wchar_t letter);
bool isDriveLetterInUse(wchar_t letter);

bool isElevated();
std::wstring systemDirectory();

std::wstring quoteArgument(const std::wstring& argument);

class UniqueHandle {
public:
    UniqueHandle() = default;
    explicit UniqueHandle(HANDLE handle) : handle_(handle) {}

    ~UniqueHandle() { reset(); }

    UniqueHandle(const UniqueHandle&) = delete;
    UniqueHandle& operator=(const UniqueHandle&) = delete;

    UniqueHandle(UniqueHandle&& other) noexcept : handle_(other.handle_) {
        other.handle_ = nullptr;
    }

    UniqueHandle& operator=(UniqueHandle&& other) noexcept {
        if (this != &other) {
            reset();
            handle_ = other.handle_;
            other.handle_ = nullptr;
        }
        return *this;
    }

    void reset(HANDLE handle = nullptr) {
        if (handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE) {
            CloseHandle(handle_);
        }
        handle_ = handle;
    }

    HANDLE get() const { return handle_; }
    HANDLE release() {
        HANDLE handle = handle_;
        handle_ = nullptr;
        return handle;
    }

    explicit operator bool() const {
        return handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE;
    }

private:
    HANDLE handle_ = nullptr;
};

class UniqueLibrary {
public:
    UniqueLibrary() = default;
    explicit UniqueLibrary(HMODULE module) : module_(module) {}

    ~UniqueLibrary() {
        if (module_ != nullptr) {
            FreeLibrary(module_);
        }
    }

    UniqueLibrary(const UniqueLibrary&) = delete;
    UniqueLibrary& operator=(const UniqueLibrary&) = delete;

    UniqueLibrary(UniqueLibrary&& other) noexcept : module_(other.module_) {
        other.module_ = nullptr;
    }

    UniqueLibrary& operator=(UniqueLibrary&& other) noexcept {
        if (this != &other) {
            if (module_ != nullptr) {
                FreeLibrary(module_);
            }
            module_ = other.module_;
            other.module_ = nullptr;
        }
        return *this;
    }

    HMODULE get() const { return module_; }
    explicit operator bool() const { return module_ != nullptr; }

private:
    HMODULE module_ = nullptr;
};

}  // namespace grd
