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

// Flat makeup gain. New instances start at unity so adding a Gain effect is
// safe; the legacy chain explicitly starts at +28 dB.
struct GainSettings
{
    bool enabled = false;
    double gainDb = 0.0;
};

// Copies the first (left) channel to every other channel after processing.
struct MonoSettings
{
    bool enabled = false;
};

// Soft saturation curves. Each shape has unity slope at zero, so the makeup
// gain stage sets how hard the curve is driven and there is no separate drive
// control.
enum DistortionFunction : int
{
    kDistortionTanh = 0,
    kDistortionArctan,
    kDistortionCubic,
    kDistortionLog,
    kDistortionExp,
    kDistortionErf,
    kDistortionFunctionCount,
};

struct DistortionSettings
{
    bool enabled = false;
    int function = kDistortionTanh;
    // Input gain into the curve, in dB.
    double driveDb = 0.0;
    // How hard the curve bends, 0 = almost linear, 100 = fully saturated.
    double curvature = 50.0;
    // Level after the curve, in dB.
    double outputDb = 0.0;
};

// Room/hall reverb. `mix` is the wet percentage, `size` sets the decay length,
// `damping` absorbs high frequencies in the tail, and `predelayMs` adds a gap
// before the reverb starts.
struct ReverbSettings
{
    bool enabled = false;
    double mix = 20.0;
    double size = 50.0;
    double damping = 50.0;
    double predelayMs = 20.0;
};

// Effect types the list can hold. The list order is the DSP order, and a type
// may appear more than once.
enum class EffectType : int
{
    Noise = 0,
    Gate,
    Dynamic,
    Compressor,
    Gain,
    Mono,
    Distortion,
    Reverb,
    Count,
};

inline constexpr int kMaxEffects = 16;

// Stages that allocate large buffers get a smaller pool so a pathological list
// cannot exhaust memory. Other types can fill the whole list.
inline constexpr int kMaxHeavyEffects = 4;

// User-facing label, e.g. "Noise reduction".
const wchar_t* EffectTypeName(EffectType type);

// User-facing curve name, e.g. "Tanh" or "Error function".
const wchar_t* DistortionFunctionName(int function);

// Stable token used in the INI file, e.g. "noise".
const char* EffectTypeKey(EffectType type);

bool ParseEffectType(const std::wstring& text, EffectType& type);

// Maximum number of instances of `type` the DSP pool supports.
int EffectTypeCapacity(EffectType type);

// One effect in the per-endpoint list. Only the block matching `type` is read;
// the others stay at their defaults. Keeping every block here makes the whole
// snapshot trivially copyable and safe to publish through shared memory.
struct EffectInstance
{
    int id = 0;
    EffectType type = EffectType::Gain;

    NoiseSettings noise;
    GateSettings gate;
    CompressorSettings compressor;
    DynamicSettings dynamics;
    GainSettings gain;
    MonoSettings mono;
    DistortionSettings distortion;
    ReverbSettings reverb;
};

bool EffectEnabled(const EffectInstance& effect);
void SetEffectEnabled(EffectInstance& effect, bool enabled);

// Builds an instance with the type's defaults and enables it.
EffectInstance MakeEffect(EffectType type, int id);

// Per-endpoint DSP settings. The default constructor builds the legacy fixed
// chain (noise, gate, dynamic, compressor, gain, mono, distortion, reverb) so
// existing installations keep their sound and old INI files migrate cleanly.
struct ApoSettings
{
    ApoSettings();

    int effectCount = 0;
    int nextEffectId = 1;
    EffectInstance effects[kMaxEffects];

    // GUI binding option for devices that publish composite FX entries: replace
    // the driver's composite stream chain with only this APO instead of
    // appending to it. The APO does not use this value while processing; it is
    // read by the GUI from the same INI file.
    bool isolateEffects = false;
};

// Clamp every field into a range the DSP can handle safely. Non-finite values
// fall back to the defaults. Also normalizes effect ids so each instance has a
// unique positive id.
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
