#pragma once

#include <string>

namespace apo {

// Plays the selected endpoint's post-DSP APO audio ring through the default
// render device so the applied DSP settings can be checked by ear. No second
// capture client is opened, so audition never restarts the endpoint's APO. The
// tap and render device keep their native rates; audition converts between them
// in software. Returns an empty string on success, or a user-facing error.
std::wstring StartMonitor(const std::wstring& endpointId);

// Stops the monitor and waits for its thread to finish. Safe to call when no
// monitor is running.
void StopMonitor();

// Appends a message to C:\ProgramData\better_micro_apo\listen.log for manual
// diagnosis of capture/render setup failures.
void LogListenEvent(const std::wstring& text);

// True while the monitor thread is still running.
bool IsMonitorActive();

struct MonitorLevel {
    double peak = 0.0;          // Linear peak since the previous call, 1.0 = full scale.
    double sumSquares = 0.0;    // Sum of squared samples since the previous call.
    unsigned long long samples = 0;  // Sample count since the previous call.
    bool clipped = false;
    // Real capture gaps (excluding the expected first-packet flag) or audio
    // dropped from the monitor's playback queue.
    unsigned drops = 0;
};

// Returns the peak/RMS accumulators, clip flag, and drop counter accumulated
// since the previous call and resets them. Safe to call while the monitor is
// running.
MonitorLevel TakeMonitorLevel();


} // namespace apo
