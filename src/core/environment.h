#pragma once

#include <optional>
#include <string>

namespace grd {

// Points TEMP/TMP at <letter>:\Temp for newly started processes. The previous
// values are backed up in HKCU\Software\GpuRamDriveVk.
bool applyTempFolder(wchar_t letter, std::wstring& error);

// Restores the backed-up TEMP/TMP values, if a backup is active.
bool restoreTempFolder(std::wstring& error);

bool hasTempFolderBackup();
std::optional<wchar_t> tempFolderBackupDrive();

}  // namespace grd
