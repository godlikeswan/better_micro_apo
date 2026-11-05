#pragma once

#include <string>

namespace apo {

// Per-endpoint DSP settings. Defaults match the tuned sound that used to be
// compiled into the APO.
struct ApoSettings
{
    double inputGainDb = 28.0;
    bool compressorEnabled = true;
    double thresholdDb = -30.0;
    double ratio = 3.0;
    double attackMs = 5.0;
    double releaseMs = 100.0;
    double makeupDb = 0.0;

    // Copy the first (left) channel to every other channel after processing.
    // Useful for a mono source on a stereo interface.
    bool mono = false;
};

// Clamp every field into a range the DSP can handle safely. Non-finite values
// fall back to the defaults.
void ClampSettings(ApoSettings& settings);

// C:\ProgramData\better_micro_apo\devices
std::wstring SettingsDirectory();

// C:\ProgramData\better_micro_apo\devices\{endpoint-guid}.ini
std::wstring SettingsPath(const std::wstring& endpointId);

// Loads the endpoint's settings. Missing or unreadable files leave the
// defaults in place and return false.
bool LoadSettings(const std::wstring& endpointId, ApoSettings& settings);

// Writes the endpoint's settings, creating the directory if needed.
bool SaveSettings(const std::wstring& endpointId, const ApoSettings& settings);

} // namespace apo
