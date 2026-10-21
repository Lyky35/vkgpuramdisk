#include "core/config.h"

#include <windows.h>

#include <cwctype>
#include <format>
#include <map>
#include <sstream>

#include "core/log.h"
#include "core/util.h"

namespace grd {
namespace {

std::string readFile(const std::wstring& path, std::wstring& error) {
    UniqueHandle file(CreateFileW(path.c_str(), GENERIC_READ,
                                  FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                  OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!file) {
        const DWORD code = GetLastError();
        if (code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND) {
            return {};
        }
        error = L"Unable to open " + path + L": " + win32Error(code);
        return {};
    }

    LARGE_INTEGER size = {};
    if (GetFileSizeEx(file.get(), &size) == FALSE) {
        error = L"Unable to read the size of " + path + L": " + lastError();
        return {};
    }
    if (size.QuadPart < 0 || size.QuadPart > 16 * 1024 * 1024) {
        error = L"Config file is too large: " + path;
        return {};
    }

    std::string data(static_cast<size_t>(size.QuadPart), '\0');
    DWORD read = 0;
    if (!data.empty() &&
        ReadFile(file.get(), data.data(), static_cast<DWORD>(data.size()), &read,
                 nullptr) == FALSE) {
        error = L"Unable to read " + path + L": " + lastError();
        return {};
    }
    data.resize(read);
    if (data.size() >= 3 && static_cast<unsigned char>(data[0]) == 0xEF &&
        static_cast<unsigned char>(data[1]) == 0xBB &&
        static_cast<unsigned char>(data[2]) == 0xBF) {
        data.erase(0, 3);
    }
    return data;
}

bool writeFile(const std::wstring& path, const std::string& data,
               std::wstring& error) {
    const std::wstring temporary = path + L".tmp";
    UniqueHandle file(CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr,
                                  CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!file) {
        error = L"Unable to create " + temporary + L": " + lastError();
        return false;
    }

    DWORD written = 0;
    if (!data.empty() &&
        WriteFile(file.get(), data.data(), static_cast<DWORD>(data.size()),
                  &written, nullptr) == FALSE) {
        error = L"Unable to write " + temporary + L": " + lastError();
        return false;
    }
    if (written != data.size()) {
        error = L"Short write to " + temporary;
        return false;
    }
    FlushFileBuffers(file.get());
    file.reset();

    if (MoveFileExW(temporary.c_str(), path.c_str(),
                    MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) == FALSE) {
        error = L"Unable to replace " + path + L": " + lastError();
        DeleteFileW(temporary.c_str());
        return false;
    }
    return true;
}

bool parseBool(std::wstring_view text, bool& value) {
    const std::wstring normalized = lower(trim(text));
    if (normalized == L"1" || normalized == L"true" || normalized == L"yes" ||
        normalized == L"on") {
        value = true;
        return true;
    }
    if (normalized == L"0" || normalized == L"false" || normalized == L"no" ||
        normalized == L"off") {
        value = false;
        return true;
    }
    return false;
}

std::wstring singleLine(std::wstring text) {
    for (wchar_t& ch : text) {
        if (ch == L'\r' || ch == L'\n') {
            ch = L' ';
        }
    }
    return text;
}

bool parseGpu(std::wstring_view text, uint32_t& value) {
    const auto parsed = parseUInt64(text);
    if (!parsed || *parsed > UINT32_MAX) {
        return false;
    }
    value = static_cast<uint32_t>(*parsed);
    return true;
}

}  // namespace

const wchar_t* fileSystemName(FileSystem value) {
    switch (value) {
        case FileSystem::Fat32:
            return L"FAT32";
        case FileSystem::ExFat:
            return L"exFAT";
        case FileSystem::Ntfs:
            return L"NTFS";
    }
    return L"NTFS";
}

std::optional<FileSystem> parseFileSystem(std::wstring_view text) {
    const std::wstring normalized = lower(trim(text));
    if (normalized == L"fat32" || normalized == L"fat") {
        return FileSystem::Fat32;
    }
    if (normalized == L"exfat") {
        return FileSystem::ExFat;
    }
    if (normalized == L"ntfs") {
        return FileSystem::Ntfs;
    }
    return std::nullopt;
}

const wchar_t* driveKindName(DriveKind value) {
    switch (value) {
        case DriveKind::HardDisk:
            return L"HD";
        case DriveKind::Floppy:
            return L"FD";
    }
    return L"HD";
}

std::optional<DriveKind> parseDriveKind(std::wstring_view text) {
    const std::wstring normalized = lower(trim(text));
    if (normalized == L"hd" || normalized == L"hdd" || normalized == L"harddisk") {
        return DriveKind::HardDisk;
    }
    if (normalized == L"fd" || normalized == L"floppy") {
        return DriveKind::Floppy;
    }
    return std::nullopt;
}

ConfigStore::ConfigStore(std::wstring path) : path_(std::move(path)) {}

std::wstring ConfigStore::defaultPath() {
    return joinPath(configDir(), L"drives.ini");
}

bool ConfigStore::load(std::wstring& error) {
    drives_.clear();
    error.clear();

    const std::string data = readFile(path_, error);
    if (!error.empty()) {
        return false;
    }

    std::istringstream stream(data);
    std::string line;
    std::optional<wchar_t> current;
    size_t lineNumber = 0;

    while (std::getline(stream, line)) {
        ++lineNumber;
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        const std::wstring wide = trim(widen(line));
        if (wide.empty() || wide[0] == L';' || wide[0] == L'#') {
            continue;
        }

        if (wide.front() == L'[' && wide.back() == L']') {
            const std::wstring section = lower(trim(wide.substr(1, wide.size() - 2)));
            current.reset();
            if (section.size() == 7 && section.starts_with(L"drive.")) {
                const wchar_t letter = static_cast<wchar_t>(std::towupper(section[6]));
                if (isDriveLetter(letter)) {
                    current = letter;
                    DriveConfig config;
                    config.letter = letter;
                    drives_[letter] = config;
                }
            }
            continue;
        }

        const size_t equals = wide.find(L'=');
        if (equals == std::wstring::npos || !current) {
            Log::warn(std::format(L"{}:{}: ignoring malformed line", path_,
                                  lineNumber));
            continue;
        }

        const std::wstring key = lower(trim(wide.substr(0, equals)));
        const std::wstring value = trim(wide.substr(equals + 1));
        DriveConfig& config = drives_[*current];

        if (key == L"gpu") {
            if (!parseGpu(value, config.gpuIndex)) {
                Log::warn(std::format(L"{}:{}: invalid gpu value", path_, lineNumber));
            }
        } else if (key == L"size_mb") {
            if (const auto parsed = parseUInt64(value);
                parsed && *parsed > 0 && *parsed <= kMaxDriveMiB) {
                config.sizeMiB = *parsed;
            } else {
                Log::warn(std::format(L"{}:{}: invalid size_mb value", path_,
                                      lineNumber));
            }
        } else if (key == L"filesystem" || key == L"format") {
            if (const auto parsed = parseFileSystem(value)) {
                config.filesystem = *parsed;
            } else {
                Log::warn(std::format(L"{}:{}: invalid filesystem value", path_,
                                      lineNumber));
            }
        } else if (key == L"label") {
            config.label = singleLine(value);
        } else if (key == L"image") {
            config.imagePath = singleLine(value);
        } else if (key == L"type") {
            if (const auto parsed = parseDriveKind(value)) {
                config.kind = *parsed;
            } else {
                Log::warn(std::format(L"{}:{}: invalid type value", path_, lineNumber));
            }
        } else if (key == L"removable") {
            parseBool(value, config.removable);
        } else if (key == L"read_only" || key == L"readonly") {
            parseBool(value, config.readOnly);
        } else if (key == L"temp" || key == L"temp_folder") {
            parseBool(value, config.useTempFolder);
        } else if (key == L"autostart" || key == L"start_on_windows") {
            parseBool(value, config.autoStart);
        }
    }

    for (auto it = drives_.begin(); it != drives_.end();) {
        if (it->second.sizeMiB == 0) {
            it = drives_.erase(it);
        } else {
            ++it;
        }
    }
    return true;
}

bool ConfigStore::save(std::wstring& error) const {
    error.clear();
    if (!ensureDirectory(configDir())) {
        error = L"Unable to create " + configDir() + L": " + lastError();
        return false;
    }

    std::ostringstream output;
    output << "; GpuRamDriveVk configuration. Edit with the GUI or the CLI.\r\n";
    for (const auto& [letter, config] : drives_) {
        output << "\r\n[drive." << static_cast<char>(letter) << "]\r\n";
        output << "gpu=" << config.gpuIndex << "\r\n";
        output << "size_mb=" << config.sizeMiB << "\r\n";
        output << "filesystem=" << narrow(fileSystemName(config.filesystem))
               << "\r\n";
        output << "type=" << narrow(driveKindName(config.kind)) << "\r\n";
        output << "label=" << narrow(singleLine(config.label)) << "\r\n";
        output << "image=" << narrow(singleLine(config.imagePath)) << "\r\n";
        output << "removable=" << (config.removable ? 1 : 0) << "\r\n";
        output << "read_only=" << (config.readOnly ? 1 : 0) << "\r\n";
        output << "temp=" << (config.useTempFolder ? 1 : 0) << "\r\n";
        output << "autostart=" << (config.autoStart ? 1 : 0) << "\r\n";
    }
    return writeFile(path_, output.str(), error);
}

std::optional<DriveConfig> ConfigStore::find(wchar_t letter) const {
    const auto it = drives_.find(letter);
    if (it == drives_.end()) {
        return std::nullopt;
    }
    return it->second;
}

bool ConfigStore::contains(wchar_t letter) const {
    return drives_.contains(letter);
}

void ConfigStore::put(const DriveConfig& config) {
    DriveConfig normalized = config;
    normalized.letter = static_cast<wchar_t>(
        std::towupper(static_cast<wint_t>(normalized.letter)));
    normalized.label = singleLine(normalized.label);
    normalized.imagePath = singleLine(normalized.imagePath);
    drives_[normalized.letter] = normalized;
}

bool ConfigStore::erase(wchar_t letter) {
    return drives_.erase(letter) > 0;
}

std::vector<wchar_t> ConfigStore::letters() const {
    std::vector<wchar_t> result;
    result.reserve(drives_.size());
    for (const auto& [letter, config] : drives_) {
        (void)config;
        result.push_back(letter);
    }
    return result;
}

}  // namespace grd
