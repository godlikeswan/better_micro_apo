#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace apo {

// Spectral noise reduction driven by a profile captured from the device.
// `reductionDb` is the maximum attenuation applied to a frequency bin.
struct NoiseSettings
{
    bool enabled = false;
    double reductionDb = 12.0;
};

// Analyzed noise profile. `magnitudes` holds one value per bin from 0 through
// kNoiseFftSize/2 at `sampleRate`, averaged across the captured channels.
struct NoiseProfile
{
    bool valid = false;
    std::uint32_t sampleRate = 0;
    std::vector<double> magnitudes;
};

struct GateSettings
{
    bool enabled = false;
    double thresholdDb = -50.0;

    // Maximum attenuation when the gate is closed.
    double rangeDb = 60.0;
    double attackMs = 5.0;
    double holdMs = 50.0;
    double releaseMs = 100.0;

    // Detector lookahead. The audio is delayed by this much so the gate can
    // open before a transient reaches the output. Adds the same latency to the
    // stream while the gate is enabled.
    double lookaheadMs = 0.0;
};

struct CompressorSettings
{
    bool enabled = true;
    double thresholdDb = -58.0;
    double ratio = 3.0;
    double attackMs = 5.0;
    double releaseMs = 100.0;
};

// Frequency-selective dynamics. The detector watches a band around
// `frequencyHz`; `expand` selects the extend mode (boost quiet lows, which
// strengthens a distant proximity effect) instead of the reduce mode (cut loud
// lows, which tames a close proximity effect). A broad Q makes it behave like
// a dynamic low shelf.
struct DynamicSettings
{
    bool enabled = false;
    bool expand = false;
    double frequencyHz = 120.0;
    double q = 0.7;
    double thresholdDb = -45.0;
    double ratio = 4.0;

    // Maximum cut (compress) or boost (expand) applied to the band.
    double rangeDb = 18.0;
    double attackMs = 10.0;
    double releaseMs = 150.0;
};

// Per-endpoint DSP settings. Defaults match the tuned sound that used to be
// compiled into the APO: the old +28 dB input gain now lives in the compressor
// threshold and makeup gain.
struct ApoSettings
{
    NoiseSettings noise;
    GateSettings gate;
    CompressorSettings compressor;
    DynamicSettings dynamics;

    // Flat makeup gain. `makeupEnabled` lets the stage be bypassed without
    // losing the configured value.
    bool makeupEnabled = true;
    double makeupDb = 28.0;

    // Copy the first (left) channel to every other channel after processing.
    // Useful for a mono source on a stereo interface.
    bool mono = false;

    // GUI binding option for devices that publish composite FX entries: replace
    // the driver's composite stream chain with only this APO instead of
    // appending to it. The APO does not use this value while processing; it is
    // read by the GUI from the same INI file.
    bool isolateEffects = false;
};

// Clamp every field into a range the DSP can handle safely. Non-finite values
// fall back to the defaults.
void ClampSettings(ApoSettings& settings);

// Filename-safe form of an endpoint GUID. Shared by the per-device settings,
// noise profile and runtime files.
std::wstring SafeEndpointId(const std::wstring& endpointId);

// C:\ProgramData\better_micro_apo\devices
std::wstring SettingsDirectory();

// C:\ProgramData\better_micro_apo\devices\{endpoint-guid}.ini
std::wstring SettingsPath(const std::wstring& endpointId);

// Loads the endpoint's settings. Missing or unreadable files leave the
// defaults in place and return false.
bool LoadSettings(const std::wstring& endpointId, ApoSettings& settings);

// Writes the endpoint's settings, creating the directory if needed.
bool SaveSettings(const std::wstring& endpointId, const ApoSettings& settings);

// C:\ProgramData\better_micro_apo\devices\{endpoint-guid}.noise
std::wstring NoiseProfilePath(const std::wstring& endpointId);

// Writes a raw capture (BMNP header plus interleaved float32 samples).
bool SaveNoiseCapture(const std::wstring& endpointId, std::uint32_t sampleRate,
                      std::uint32_t channels,
                      const std::vector<float>& samples);

// Reads and analyzes the endpoint's noise profile. Missing or invalid files
// leave `profile` invalid and return false.
bool LoadNoiseProfile(const std::wstring& endpointId, NoiseProfile& profile);

} // namespace apo
