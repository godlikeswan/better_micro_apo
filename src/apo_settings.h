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
inline constexpr int kSettingsVersion = 11;

// The fixed first stage has its own gain range; it is stored in GainSettings
// but is separate from the standalone makeup Gain effect.
inline constexpr double kInputDefaultGainDb = 0.0;
inline constexpr double kInputMinGainDb = -24.0;
inline constexpr double kInputMaxGainDb = 24.0;

// Equalizer band limits. The frequency reaches the top of the graph on normal
// sample rates; the DSP still clamps to 49% of the active rate.
inline constexpr double kEqMinFrequencyHz = 20.0;
inline constexpr double kEqMaxFrequencyHz = 20000.0;
inline constexpr double kEqMinQ = 0.1;
inline constexpr double kEqMaxQ = 30.0;
// Level spans the same ±96 dB window used by the response graph; a boost can
// still run off the older +24 dB spectrum scale rather than being limited.
inline constexpr double kEqMinLevelDb = -96.0;
inline constexpr double kEqMaxLevelDb = 96.0;
inline constexpr double kEqMaxRangeDb = 96.0;
inline constexpr double kEqMaxHoldMs = 2000.0;

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

// Internal DSP mode for the two standalone dynamics effects. It is selected by
// the effect type, never stored in settings or shown in the GUI.
enum class DynamicsMode : int
{
    CompressorLimiter = 0,
    ExpanderGate = 1,
};

// Dynamics applied to the Equalizer's isolated band. `None` leaves only the
// static Level shift; the other modes cut loud lows or lift quiet lows.
enum class EqualizerDynamicsMode : int
{
    None = 0,
    Compressor = 1,
    Expander = 2,
    Count,
};

const wchar_t* EqualizerDynamicsModeName(EqualizerDynamicsMode mode);

// Threshold bounds shared by the standalone dynamics and Equalizer detectors.
inline constexpr double kMinThresholdDb = -96.0;
inline constexpr double kMaxThresholdDb = 24.0;

// Ratio bounds. `kDynamicsMaxFiniteRatio` is the last finite slider position;
// the end stop means true infinity (hard limiter / hard gate).
inline constexpr double kDynamicsMinRatio = 0.01;
inline constexpr double kDynamicsMaxFiniteRatio = 1000.0;
inline constexpr double kDynamicsMaxLookaheadMs = 300.0;

inline double InfiniteRatio()
{
    return std::numeric_limits<double>::infinity();
}

// Shared parameter block for the standalone Gate/Expander and
// Compressor/Limiter effects. Which half of the transfer curve it uses comes
// from the effect type, so no mode is stored here.
struct DynamicsSettings
{
    bool enabled = false;
    double thresholdDb = -15.0;
    double ratio = 5.0;
    double inputDb = 0.0;
    double outputDb = 0.0;
    double attackMs = 5.0;
    double releaseMs = 100.0;

    // Keeps the last computed gain change active after the signal stops
    // demanding it, giving attack time to catch short peaks before release.
    // The hold includes the lookahead, so it is measured at the output.
    double holdMs = 50.0;

    // Detector lookahead. The audio is delayed by this much so the gain change
    // can start before a transient reaches the output. Adds the same latency to
    // the stream while the effect is enabled.
    double lookaheadMs = 0.0;
};

// Frequency-selective EQ for the proximity effect. The detector watches a band
// around `frequencyHz`; `mode` chooses no dynamics, a compressor cut of loud
// lows, or an expander lift of quiet lows. A broad Q behaves like a dynamic low
// shelf. `levelDb` is a static signed shift of that band, independent of mode.
struct EqualizerSettings
{
    bool enabled = false;
    EqualizerDynamicsMode mode = EqualizerDynamicsMode::None;
    double frequencyHz = 120.0;
    double q = 0.7;
    // Static band shift; negative cuts and positive boosts the band.
    double levelDb = -12.0;
    double thresholdDb = -45.0;
    double ratio = 4.0;

    // Maximum cut (compress) or boost (expand) applied by the dynamics.
    double rangeDb = 18.0;
    double attackMs = 10.0;
    double releaseMs = 150.0;

    // Keeps the last computed band gain active briefly so short peaks are not
    // missed. Includes lookahead, so it is measured at the output.
    double holdMs = 50.0;
    // Delays the audio so the detector can act before a transient reaches the
    // output. Adds the same latency while the effect is enabled.
    double lookaheadMs = 0.0;
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

// Digital reverb engines. Comb/allpass is the classic parallel-comb and
// series-allpass network. The feedback delay network uses coupled delay lines
// with a mixing matrix.
enum class ReverbAlgorithm : int
{
    CombAllpass = 0,
    FeedbackDelayNetwork = 1,
    Count,
};

const wchar_t* ReverbAlgorithmName(ReverbAlgorithm algorithm);

inline constexpr double kReverbMinMaxLengthMs = 200.0;
inline constexpr double kReverbMaxMaxLengthMs = 8000.0;

// Room/hall reverb. `level` is the wet return level; dry stays at unity.
// `maxLengthMs` sets the decay ceiling, `size` adds room weight and low-end
// body, `damping` absorbs highs in the loop, `width` controls stereo
// decorrelation, `density` controls diffusion, and `predelayMs` delays the
// excitation.
struct ReverbSettings
{
    bool enabled = false;
    ReverbAlgorithm algorithm = ReverbAlgorithm::CombAllpass;
    double level = 20.0;
    double maxLengthMs = 2000.0;
    double size = 50.0;
    double damping = 50.0;
    double width = 50.0;
    double density = 50.0;
    double predelayMs = 20.0;
};

// Pitch detection engines offered by the monitoring-only Tuner. Autocorrelation
// is the default because it tracks the fundamental of monophonic sources well;
// FFT peak is a faster alternative that can follow harmonics more readily.
enum class TunerAlgorithm : int
{
    Autocorrelation = 0,
    FftPeak = 1,
    Count,
};

const wchar_t* TunerAlgorithmName(TunerAlgorithm algorithm);

// Analysis sizes in samples. Buffers and FFT plans for every size are created
// when the stream binds, so a live Sample size change never allocates.
inline constexpr int kTunerDefaultSampleSize = 8192;
inline constexpr std::array<int, 5> kTunerSampleSizes{{
    2048, 4096, 8192, 16384, 32768,
}};
inline constexpr double kTunerMinSmoothing = 0.0;
inline constexpr double kTunerMaxSmoothing = 100.0;
inline constexpr double kTunerMinReferenceHz = 430.0;
inline constexpr double kTunerMaxReferenceHz = 450.0;
inline constexpr double kTunerDefaultSmoothing = 50.0;
inline constexpr double kTunerDefaultReferenceHz = 440.0;

inline int ValidTunerSampleSize(int value)
{
    for (int candidate : kTunerSampleSizes) {
        if (candidate == value) {
            return value;
        }
    }
    return kTunerDefaultSampleSize;
}

inline int TunerSampleSizeAt(int index)
{
    if (index < 0 || index >= static_cast<int>(kTunerSampleSizes.size())) {
        return kTunerDefaultSampleSize;
    }
    return kTunerSampleSizes[static_cast<std::size_t>(index)];
}

inline int TunerSampleSizeIndex(int value)
{
    for (std::size_t index = 0; index < kTunerSampleSizes.size(); ++index) {
        if (kTunerSampleSizes[index] == value) {
            return static_cast<int>(index);
        }
    }
    for (std::size_t index = 0; index < kTunerSampleSizes.size(); ++index) {
        if (kTunerSampleSizes[index] == kTunerDefaultSampleSize) {
            return static_cast<int>(index);
        }
    }
    return 0;
}

// Tuner is a pass-through: it publishes detected pitch for the graph but never
// changes audio. `referenceHz` sets the A4 calibration used by note/cents math.
struct TunerSettings
{
    bool enabled = false;
    TunerAlgorithm algorithm = TunerAlgorithm::Autocorrelation;
    int sampleSize = kTunerDefaultSampleSize;
    double smoothing = kTunerDefaultSmoothing;
    double referenceHz = kTunerDefaultReferenceHz;
};

// Effect types the list can hold. The list order is the DSP order, and a type
// may appear more than once. Input is generated automatically as row 0 and is
// never addable or deletable; Gain is the first optional effect after it.
enum class EffectType : int
{
    Input = 0,
    Gain,
    Noise,
    Gate,
    Compressor,
    Equalizer,
    Mono,
    Saturation,
    Reverb,
    Tuner,
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
    DynamicsSettings gate;
    DynamicsSettings compressor;
    EqualizerSettings equalizer;
    GainSettings gain;
    MonoSettings mono;
    DistortionSettings distortion;
    ReverbSettings reverb;
    TunerSettings tuner;
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
// fall back to the defaults except the dynamics ratios, where infinity is the
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
