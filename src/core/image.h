#pragma once

#include <string>

namespace grd {

// Saves the raw contents of the mounted volume to `path`.
bool saveImage(wchar_t letter, const std::wstring& path, std::wstring& error);

// Restores a raw image file onto the mounted volume, truncating to the smaller
// of the image and the device.
bool restoreImage(const std::wstring& path, wchar_t letter, std::wstring& error);

}  // namespace grd
