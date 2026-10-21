#include "core/util.h"

#include <algorithm>
#include <cwctype>
#include <limits>

namespace grd {
namespace {

std::wstring trimTrailingSeparators(std::wstring path) {
    while (path.size() > 1 && (path.back() == L'\\' || path.back() == L'/')) {
        path.pop_back();
    }
    return path;
}

}  // namespace

std::wstring widen(std::string_view text, UINT codePage) {
    if (text.empty()) {
        return {};
    }

    const int length = MultiByteToWideChar(
        codePage, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (length <= 0) {
        return {};
    }

    std::wstring result(static_cast<size_t>(length), L'\0');
    MultiByteToWideChar(codePage, 0, text.data(), static_cast<int>(text.size()),
                        result.data(), length);
    return result;
}

std::string narrow(std::wstring_view text, UINT codePage) {
    if (text.empty()) {
        return {};
    }

    const int length = WideCharToMultiByte(codePage, 0, text.data(),
                                           static_cast<int>(text.size()),
                                           nullptr, 0, nullptr, nullptr);
    if (length <= 0) {
        return {};
    }

    std::string result(static_cast<size_t>(length), '\0');
    WideCharToMultiByte(codePage, 0, text.data(), static_cast<int>(text.size()),
                        result.data(), length, nullptr, nullptr);
    return result;
}

std::wstring win32Error(DWORD code) {
    wchar_t* buffer = nullptr;
    const DWORD length = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
            FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, code, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        reinterpret_cast<wchar_t*>(&buffer), 0, nullptr);

    if (length == 0 || buffer == nullptr) {
        return L"error " + std::to_wstring(code);
    }

    std::wstring message(buffer, length);
    LocalFree(buffer);
    return trim(message);
}

std::wstring lastError() {
    return win32Error(GetLastError());
}

std::wstring modulePath() {
    std::wstring buffer(MAX_PATH, L'\0');
    for (;;) {
        const DWORD length =
            GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (length == 0) {
            return {};
        }
        if (length < buffer.size()) {
            buffer.resize(length);
            return buffer;
        }
        buffer.resize(buffer.size() * 2);
    }
}

std::wstring executableDir() {
    std::wstring path = modulePath();
    const size_t separator = path.find_last_of(L"\\/");
    return separator == std::wstring::npos ? std::wstring() : path.substr(0, separator);
}

std::wstring localAppDataDir() {
    wchar_t buffer[MAX_PATH] = {};
    DWORD length = GetEnvironmentVariableW(L"LOCALAPPDATA", buffer, MAX_PATH);
    if (length > 0 && length < MAX_PATH) {
        return buffer;
    }

    length = GetEnvironmentVariableW(L"APPDATA", buffer, MAX_PATH);
    if (length > 0 && length < MAX_PATH) {
        return buffer;
    }

    return executableDir();
}

std::wstring configDir() {
    return joinPath(localAppDataDir(), L"GpuRamDriveVk");
}

std::wstring joinPath(std::wstring_view left, std::wstring_view right) {
    std::wstring result(left);
    if (!result.empty() && result.back() != L'\\' && result.back() != L'/') {
        result.push_back(L'\\');
    }
    result.append(right);
    return result;
}

bool ensureDirectory(const std::wstring& path) {
    if (path.empty()) {
        return false;
    }

    const std::wstring trimmed = trimTrailingSeparators(path);
    if (CreateDirectoryW(trimmed.c_str(), nullptr) != FALSE) {
        return true;
    }
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        const DWORD attributes = GetFileAttributesW(trimmed.c_str());
        return attributes != INVALID_FILE_ATTRIBUTES &&
               (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    }
    return false;
}

bool fileExists(const std::wstring& path) {
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES &&
           (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

std::wstring trim(std::wstring_view text) {
    size_t begin = 0;
    while (begin < text.size() && std::iswspace(text[begin])) {
        ++begin;
    }
    size_t end = text.size();
    while (end > begin && std::iswspace(text[end - 1])) {
        --end;
    }
    return std::wstring(text.substr(begin, end - begin));
}

std::wstring lower(std::wstring_view text) {
    std::wstring result(text);
    std::transform(result.begin(), result.end(), result.begin(),
                   [](wchar_t ch) { return static_cast<wchar_t>(std::towlower(ch)); });
    return result;
}

std::optional<uint64_t> parseUInt64(std::wstring_view text) {
    if (text.empty()) {
        return std::nullopt;
    }

    uint64_t value = 0;
    for (const wchar_t ch : text) {
        if (ch < L'0' || ch > L'9') {
            return std::nullopt;
        }
        const uint64_t digit = static_cast<uint64_t>(ch - L'0');
        if (value > (std::numeric_limits<uint64_t>::max() - digit) / 10) {
            return std::nullopt;
        }
        value = value * 10 + digit;
    }
    return value;
}

std::wstring driveRoot(wchar_t letter) {
    std::wstring root;
    root.push_back(letter);
    root += L":\\";
    return root;
}

std::wstring displayLetter(wchar_t letter) {
    std::wstring text;
    text.push_back(letter);
    text.push_back(L':');
    return text;
}

bool isDriveLetter(wchar_t letter) {
    return letter >= L'A' && letter <= L'Z';
}

bool isDriveLetterInUse(wchar_t letter) {
    if (!isDriveLetter(letter)) {
        return false;
    }
    const DWORD mask = GetLogicalDrives();
    return (mask & (1u << (letter - L'A'))) != 0;
}


bool isElevated() {
    HANDLE token = nullptr;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token) == FALSE) {
        return false;
    }

    TOKEN_ELEVATION elevation = {};
    DWORD size = sizeof(elevation);
    const BOOL ok = GetTokenInformation(token, TokenElevation, &elevation,
                                        sizeof(elevation), &size);
    CloseHandle(token);
    return ok != FALSE && elevation.TokenIsElevated != 0;
}

std::wstring systemDirectory() {
    wchar_t buffer[MAX_PATH] = {};
    const UINT length = GetSystemDirectoryW(buffer, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) {
        return L"C:\\Windows\\System32";
    }
    return buffer;
}

std::wstring quoteArgument(const std::wstring& argument) {
    if (argument.find_first_of(L" \t\"") == std::wstring::npos) {
        return argument;
    }

    std::wstring result = L"\"";
    size_t backslashes = 0;
    for (wchar_t ch : argument) {
        if (ch == L'\\') {
            ++backslashes;
            continue;
        }
        if (ch == L'"') {
            result.append(backslashes * 2 + 1, L'\\');
            result.push_back(L'"');
        } else {
            result.append(backslashes, L'\\');
            result.push_back(ch);
        }
        backslashes = 0;
    }
    result.append(backslashes * 2, L'\\');
    result.push_back(L'"');
    return result;
}

}  // namespace grd
