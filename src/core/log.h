#pragma once

#include <atomic>
#include <mutex>
#include <string>

namespace grd {

class Log {
public:
    static void setVerbose(bool verbose);
    static bool verbose();

    static void info(const std::wstring& message);
    static void warn(const std::wstring& message);
    static void error(const std::wstring& message);
    static void debug(const std::wstring& message);

private:
    static void write(const wchar_t* level, const std::wstring& message);
    static std::wstring filePath();

    static std::mutex mutex_;
    static std::atomic<bool> verbose_;
};

}  // namespace grd
