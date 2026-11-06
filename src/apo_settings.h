#pragma once

#include <string>

namespace apo {

struct GateSettings
{
    bool enabled = false;
    double thresholdDb = -50.0;

    // Maximum attenuation when the gate is closed.
    double rangeDb = 60.0;
    double attackMs = 5.0;
    double holdMs = 50.0;
    double releaseMs = 100.0;
};

struct CompressorSettings
{
    bool enabled = true;
    double thresholdDb = -58.0;
    double ratio = 3.0;
    double attackMs = 5.0;
    double releaseMs = 100.0;
};

// Per-endpoint DSP settings. Defaults match the tuned sound that used to be
// compiled into the APO: the old +28 dB input gain now lives in the compressor
// threshold and makeup gain.
struct ApoSettings
{
    GateSettings gate;
    CompressorSettings compressor;
    double makeupDb = 28.0;

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
