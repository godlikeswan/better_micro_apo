#pragma once

#include <string>

namespace gui {

// C:\ProgramData\better_micro_apo\gui.ini
bool LoadShowAllDevices();
void SaveShowAllDevices(bool showAll);

// Endpoint id to select on the next launch. Empty when none has been chosen.
std::wstring LoadLastDevice();
void SaveLastDevice(const std::wstring& id);

} // namespace gui
