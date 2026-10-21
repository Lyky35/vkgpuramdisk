#pragma once

#include <string>

namespace grd {

bool autoStartTaskExists();
bool setAutoStartTask(bool enabled, std::wstring& error);

}  // namespace grd
