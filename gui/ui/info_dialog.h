#pragma once

#include <windows.h>

#include "apo.h"

namespace ui {

// Modal About/Info window. It reports file and registry state from a snapshot
// taken by the caller so the dialog never blocks a background operation.
void ShowInfoDialog(HWND owner, const apo::FilesStatus& files,
                    int boundDevices);

} // namespace ui
