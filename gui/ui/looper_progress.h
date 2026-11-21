#pragma once

#include <windows.h>

#include "stage_monitor.h"

namespace ui {

// One row of the Looper progress control. Lengths and positions are in audio
// frames, exactly as published by the APO's stage monitor.
struct LooperProgressTrack
{
    apo::LooperState state = apo::LooperState::Empty;
    ULONG length = 0;
    ULONG position = 0;
    bool overdub = false;
};

HWND LooperProgress(HWND parent, int id);
void SetLooperProgressTrack(HWND progress, int track,
                            const LooperProgressTrack& value);
void ClearLooperProgress(HWND progress);

} // namespace ui
