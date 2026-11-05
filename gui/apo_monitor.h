#pragma once

#include <string>

namespace apo {

// Plays the selected capture endpoint through the default render device so
// the applied DSP settings can be checked by ear. Returns an empty string on
// success, or a user-facing error message.
std::wstring StartMonitor(const std::wstring& endpointId);

// Stops the monitor and waits for its thread to finish. Safe to call when no
// monitor is running.
void StopMonitor();

// True while the monitor thread is still running.
bool IsMonitorActive();

struct MonitorLevel {
    double peak = 0.0;   // Linear peak since the previous call, 1.0 = full scale.
    bool clipped = false;
    unsigned drops = 0;  // Capture gaps or queued audio dropped by the monitor.
};

// Returns the peak/clip/drop counters accumulated since the previous call and
// resets them. Safe to call while the monitor is running.
MonitorLevel TakeMonitorLevel();

} // namespace apo
