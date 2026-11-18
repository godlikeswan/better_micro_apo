#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

namespace apo {

// Settings files with any other version are discarded on load. There is no
// migration path: a new schema starts from defaults.
inline constexpr int kSettingsVersion = 5;

// Analysis sizes offered for spectral noise reduction. The buffers and FFT
// plans for every size are created when the stream binds, so changing
// `fftSize` is a live retune rather than a reload.
inline constexpr int kNoiseDefaultFftSize = 512;
inline constexpr std::array<int, 5> kNoiseFftSizes{{256, 512, 1024, 2048,
                                                    4096}};

inline int ValidNoiseFftSize(int value)
{
    for (int candidate : kNoiseFftSizes) {
        if (candidate == value) {
            return value;
        }
    }
    return kNoiseDefaultFftSize;
}

inline int NoiseFftSizeAt(int index)
{
    if (index < 0 || index >= static_cast<int>(kNoiseFftSizes.size())) {
        return kNoiseDefaultFftSize;
    }
    return kNoiseFftSizes[static_cast<std::size_t>(index)];
}

inline int NoiseFftSizeIndex(int value)
{
    for (std::size_t index = 0; index < kNoiseFftSizes.size(); ++index) {
        if (kNoiseFftSizes[index] == value) {
            return static_cast<int>(index);
        }
    }
    for (std::size_t index = 0; index < kNoiseFftSizes.size(); ++index) {
        if (kNoiseFftSizes[index] == kNoiseDefaultFftSize) {
            return static_cast<int>(index);
        }
    }
    return 0;
}

// Spectral noise reduction driven by a profile captured from the device.
// `reductionDb` is the maximum attenuation applied to a frequency bin.
struct NoiseSettings
{
    bool enabled = false;
    double reductionDb = 12.0;
    int fftSize = kNoiseDefaultFftSize;
};

// Analyzed noise profile. `magnitudes` is always sized for the largest FFT;
// `binCount` says how many leading entries are real data, from bin 0 through
// binCount - 1 at `sampleRate` with an FFT size of 2 * (binCount - 1).
struct NoiseProfile
{
    bool valid = false;
    std::uint32_t sampleRate = 0;
    std::size_t binCount = 0;
    std::vector<double> magnitudes;
};

// The two sides of the unified dynamics effect. Compressor/Limiter acts on
// signal above the threshold; Expander/Gate acts on signal below it.
enum class DynamicsMode : int
{
    CompressorLimiter = 0,
    ExpanderGate = 1,
    Count,
};

const wchar_t* DynamicsModeName(DynamicsMode mode);

// Ratio bounds. `kDynamicsMaxFiniteRatio` is the last finite slider position;
// the end stop means true infinity (hard limiter / hard gate).
inline constexpr double kDynamicsMinRatio = 0.01;
inline constexpr double kDynamicsMaxFiniteRatio = 1000.0;
inline constexpr double kDynamicsMaxLookaheadMs = 300.0;

inline double InfiniteRatio()
{
    return std::numeric_limits<double>::infinity();
}

struct DynamicsSettings
{
    bool enabled = false;
    DynamicsMode mode = DynamicsMode::CompressorLimiter;
    double thresholdDb = -15.0;
    double ratio = 5.0;
    double inputDb = 0.0;
    double outputDb = 0.0;
    double attackMs = 5.0;
    double releaseMs = 100.0;

    // Detector lookahead. The audio is delayed by this much so the gain change
    // can start before a transient reaches the output. Adds the same latency to
    // the stream while the effect is enabled.
    double lookaheadMs = 0.0;
};

// Frequency-selective dynamics for the proximity effect. The detector watches
// a band around `frequencyHz`; `expand` selects the extend mode (boost quiet
// lows, which strengthens a distant proximity effect) instead of the reduce
// mode (cut loud lows, which tames a close proximity effect). A broad Q makes
// it behave like a dynamic low shelf.
struct EqualizerSettings
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

// Flat gain. A newly added effect starts at +12 dB, which is the usual makeup
// level after dynamics processing.
struct GainSettings
{
    bool enabled = false;
    double gainDb = 12.0;
};

// Copies the first (left) channel to every other channel after processing.
struct MonoSettings
{
    bool enabled = false;
};

// Saturation curves. Each function is odd and maps full scale to full scale.
// `curvature` sets how hard it bends and `driveDb` how far the signal is
// pushed into it.
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
// may appear more than once. Gain is first so the usual makeup step is easy to
// find and sits at the top of the chain.
enum class EffectType : int
{
    Gain = 0,
    Noise,
    Dynamics,
    Equalizer,
    Mono,
    Saturation,
    Reverb,
    Count,
};

inline constexpr int kMaxEffects = 16;

// Each device keeps this many independent effect lists. The selected preset is
// the one the APO processes and the GUI edits.
inline constexpr int kPresetCount = 4;

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
    DynamicsSettings dynamics;
    EqualizerSettings equalizer;
    GainSettings gain;
    MonoSettings mono;
    DistortionSettings distortion;
    ReverbSettings reverb;
};

bool EffectEnabled(const EffectInstance& effect);
void SetEffectEnabled(EffectInstance& effect, bool enabled);

// Builds an instance with the type's defaults and enables it.
EffectInstance MakeEffect(EffectType type, int id);

// One independent effects list. `nextEffectId` only needs to be unique inside
// this preset because only one preset is active at a time.
struct EffectPreset
{
    int effectCount = 0;
    int nextEffectId = 1;
    EffectInstance effects[kMaxEffects];
};

// Per-endpoint DSP settings. A new device starts with four empty presets; the
// first one is selected. Effects added to a preset stay with it when another
// preset is selected.
struct ApoSettings
{
    int presetIndex = 0;
    EffectPreset presets[kPresetCount];

    // Isolation is always on when this APO is enabled: devices that publish
    // composite FX entries have the driver's other chains cleared and restored
    // from the backup when the APO is disabled. The field stays in the file and
    // runtime layouts for compatibility, but ClampSettings forces it on.
    bool isolateEffects = true;
};

EffectPreset& ActivePreset(ApoSettings& settings);
const EffectPreset& ActivePreset(const ApoSettings& settings);

// Clamp every field into a range the DSP can handle safely. Non-finite values
// fall back to the defaults except the dynamics ratio, where infinity is the
// end of the slider. Also normalizes effect ids so each instance has a unique
// positive id.
void ClampSettings(ApoSettings& settings);

// Filename-safe form of an endpoint GUID. Shared by the per-device settings,
// noise profile and runtime files.
std::wstring SafeEndpointId(const std::wstring& endpointId);

// C:\ProgramData\better_micro_apo\devices
std::wstring SettingsDirectory();

// C:\ProgramData\better_micro_apo\devices\{endpoint-guid}.ini
std::wstring SettingsPath(const std::wstring& endpointId);

// Loads the endpoint's settings. Missing, unreadable, or version-mismatched
// files leave the defaults in place and return false. A mismatched file is
// deleted so stale schema never comes back.
bool LoadSettings(const std::wstring& endpointId, ApoSettings& settings);

// Writes the endpoint's settings, creating the directory if needed.
bool SaveSettings(const std::wstring& endpointId, const ApoSettings& settings);

// C:\ProgramData\better_micro_apo\devices\{endpoint-guid}.noise
std::wstring NoiseProfilePath(const std::wstring& endpointId);

// Writes a raw capture (BMNP header plus interleaved float32 samples).
bool SaveNoiseCapture(const std::wstring& endpointId, std::uint32_t sampleRate,
                      std::uint32_t channels,
                      const std::vector<float>& samples);

// Writes a stage-local capture (BMNP v2 header plus double magnitude bins).
bool SaveNoiseProfileBins(const std::wstring& endpointId,
                          std::uint32_t sampleRate,
                          const std::vector<double>& magnitudes);

// Reads and analyzes the endpoint's noise profile. Missing or invalid files
// leave `profile` invalid and return false.
bool LoadNoiseProfile(const std::wstring& endpointId, NoiseProfile& profile);

} // namespace apo
