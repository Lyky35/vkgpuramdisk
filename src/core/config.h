#pragma once

#include <cstdint>
#include <limits>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace grd {

enum class FileSystem {
    Fat32,
    ExFat,
    Ntfs,
};

enum class DriveKind {
    HardDisk,
    Floppy,
};

struct DriveConfig {
    wchar_t letter = L'R';
    uint32_t gpuIndex = 0;
    uint64_t sizeMiB = 1024;
    FileSystem filesystem = FileSystem::Ntfs;
    std::wstring label = L"GpuDisk";
    std::wstring imagePath;
    DriveKind kind = DriveKind::HardDisk;
    bool removable = false;
    bool readOnly = false;
    bool useTempFolder = false;
    bool autoStart = false;
};

constexpr uint64_t kMaxDriveMiB =
    std::numeric_limits<uint64_t>::max() / (1024ull * 1024ull);

const wchar_t* fileSystemName(FileSystem value);
std::optional<FileSystem> parseFileSystem(std::wstring_view text);

const wchar_t* driveKindName(DriveKind value);
std::optional<DriveKind> parseDriveKind(std::wstring_view text);

class ConfigStore {
public:
    explicit ConfigStore(std::wstring path = defaultPath());

    bool load(std::wstring& error);
    bool save(std::wstring& error) const;

    const std::wstring& path() const { return path_; }

    const std::map<wchar_t, DriveConfig>& drives() const { return drives_; }
    std::optional<DriveConfig> find(wchar_t letter) const;
    bool contains(wchar_t letter) const;
    void put(const DriveConfig& config);
    bool erase(wchar_t letter);

    std::vector<wchar_t> letters() const;

    static std::wstring defaultPath();

private:
    std::map<wchar_t, DriveConfig> drives_;
    std::wstring path_;
};

}  // namespace grd
