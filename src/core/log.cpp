#include "core/log.h"

#include <windows.h>

#include <format>

#include "core/util.h"

namespace grd {

std::mutex Log::mutex_;
std::atomic<bool> Log::verbose_ = false;

void Log::setVerbose(bool verbose) {
    verbose_.store(verbose, std::memory_order_relaxed);
}

bool Log::verbose() {
    return verbose_.load(std::memory_order_relaxed);
}

void Log::info(const std::wstring& message) {
    write(L"INFO", message);
}

void Log::warn(const std::wstring& message) {
    write(L"WARN", message);
}

void Log::error(const std::wstring& message) {
    write(L"ERROR", message);
}

void Log::debug(const std::wstring& message) {
    if (verbose_.load(std::memory_order_relaxed)) {
        write(L"DEBUG", message);
    }
}

std::wstring Log::filePath() {
    return joinPath(configDir(), L"gpuvramdrive.log");
}

void Log::write(const wchar_t* level, const std::wstring& message) {
    SYSTEMTIME time = {};
    GetLocalTime(&time);

    const std::wstring line = std::format(
        L"{:04}-{:02}-{:02} {:02}:{:02}:{:02}.{:03} [{:5}] [{}] {}\r\n",
        time.wYear, time.wMonth, time.wDay, time.wHour, time.wMinute,
        time.wSecond, time.wMilliseconds, level, GetCurrentThreadId(), message);

    OutputDebugStringW(line.c_str());

    std::lock_guard lock(mutex_);
    ensureDirectory(configDir());

    UniqueHandle file(CreateFileW(filePath().c_str(), FILE_APPEND_DATA,
                                  FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                  OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!file) {
        return;
    }

    const std::string utf8 = narrow(line);
    DWORD written = 0;
    WriteFile(file.get(), utf8.data(), static_cast<DWORD>(utf8.size()), &written,
              nullptr);
}

}  // namespace grd
