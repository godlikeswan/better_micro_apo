#include "apo_settings.h"

#include "dsp/dsp.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <limits>
#include <string>
#include <vector>

namespace apo {
namespace {

constexpr wchar_t kDataDirectory[] = L"C:\\ProgramData\\better_micro_apo";
constexpr wchar_t kDevicesDirectory[] =
    L"C:\\ProgramData\\better_micro_apo\\devices";

std::wstring Trim(const std::wstring& text)
{
    size_t begin = 0;
    while (begin < text.size() &&
           (text[begin] == L' ' || text[begin] == L'\t' ||
            text[begin] == L'\r' || text[begin] == L'\n')) {
        ++begin;
    }

    size_t end = text.size();
    while (end > begin &&
           (text[end - 1] == L' ' || text[end - 1] == L'\t' ||
            text[end - 1] == L'\r' || text[end - 1] == L'\n')) {
        --end;
    }
    return text.substr(begin, end - begin);
}

bool EqualsIgnoreCase(const std::wstring& text, const wchar_t* expected)
{
    return _wcsicmp(text.c_str(), expected) == 0;
}

// Endpoint IDs are GUIDs, but keep the filename safe even if the engine ever
// hands us something unexpected.

bool ReadFileText(const std::wstring& path, std::string& text)
{
    HANDLE file = CreateFileW(
        path.c_str(), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }

    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file, &size) || size.QuadPart > 64 * 1024) {
        CloseHandle(file);
        return false;
    }

    text.resize(static_cast<size_t>(size.QuadPart));
    DWORD read = 0;
    const bool ok =
        text.empty() ||
        (ReadFile(file, text.data(), static_cast<DWORD>(text.size()), &read,
                  nullptr) &&
         read == text.size());
    CloseHandle(file);
    return ok;
}

bool WriteFileText(const std::wstring& path, const std::string& text)
{
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ,
                              nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL,
                              nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }

    DWORD written = 0;
    const bool ok =
        text.empty() ||
        (WriteFile(file, text.data(), static_cast<DWORD>(text.size()),
                   &written, nullptr) &&
         written == text.size());
    CloseHandle(file);
    return ok;
}

std::wstring Utf8ToWide(const std::string& data)
{
    size_t offset = 0;
    if (data.size() >= 3 &&
        static_cast<unsigned char>(data[0]) == 0xEF &&
        static_cast<unsigned char>(data[1]) == 0xBB &&
        static_cast<unsigned char>(data[2]) == 0xBF) {
        offset = 3;
    }
    if (data.size() <= offset) {
        return {};
    }

    const int length = MultiByteToWideChar(
        CP_UTF8, 0, data.data() + offset,
        static_cast<int>(data.size() - offset), nullptr, 0);
    std::wstring text(length, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, data.data() + offset,
                        static_cast<int>(data.size() - offset), text.data(),
                        length);
    return text;
}

bool ParseDouble(const std::wstring& text, double& value)
{
    // Infinity is meaningful for the dynamics and equalizer ratios. Other
    // fields accept it syntactically and ClampFinite replaces it with their
    // default.
    if (EqualsIgnoreCase(text, L"inf") || EqualsIgnoreCase(text, L"+inf") ||
        EqualsIgnoreCase(text, L"infinity") ||
        EqualsIgnoreCase(text, L"+infinity")) {
        value = std::numeric_limits<double>::infinity();
        return true;
    }
    if (EqualsIgnoreCase(text, L"-inf") ||
        EqualsIgnoreCase(text, L"-infinity")) {
        value = -std::numeric_limits<double>::infinity();
        return true;
    }

    wchar_t* end = nullptr;
    const double parsed = std::wcstod(text.c_str(), &end);
    if (end == text.c_str()) {
        return false;
    }
    while (*end == L' ' || *end == L'\t') {
        ++end;
    }
    if (*end != L'\0' || !std::isfinite(parsed)) {
        return false;
    }
    value = parsed;
    return true;
}

bool ParseBool(const std::wstring& text, bool& value)
{
    if (EqualsIgnoreCase(text, L"1") || EqualsIgnoreCase(text, L"true") ||
        EqualsIgnoreCase(text, L"yes") || EqualsIgnoreCase(text, L"on")) {
        value = true;
        return true;
    }
    if (EqualsIgnoreCase(text, L"0") || EqualsIgnoreCase(text, L"false") ||
        EqualsIgnoreCase(text, L"no") || EqualsIgnoreCase(text, L"off")) {
        value = false;
        return true;
    }
    return false;
}

bool ParseInt(const std::wstring& text, int& value)
{
    wchar_t* end = nullptr;
    const long parsed = std::wcstol(text.c_str(), &end, 10);
    if (end == text.c_str()) {
        return false;
    }
    while (*end == L' ' || *end == L'\t') {
        ++end;
    }
    if (*end != L'\0') {
        return false;
    }
    value = static_cast<int>(parsed);
    return true;
}

std::string FormatDouble(double value)
{
    if (std::isinf(value)) {
        return (value > 0.0) ? "inf" : "-inf";
    }
    char text[64]{};
    sprintf_s(text, "%.6f", value);
    return text;
}

double Clamp(double value, double low, double high)
{
    if (value < low) {
        return low;
    }
    if (value > high) {
        return high;
    }
    return value;
}

} // namespace

const wchar_t* EffectTypeName(EffectType type)
{
    switch (type)
    {
    case EffectType::Input:      return L"Input";
    case EffectType::Gain:       return L"Gain";
    case EffectType::Noise:      return L"Noise reduction";
    case EffectType::Gate:       return L"Gate/Expander";
    case EffectType::Compressor: return L"Compressor/Limiter";
    case EffectType::Equalizer:  return L"Equalizer";
    case EffectType::Saturation: return L"Saturation/Overdrive/Distortion";
    case EffectType::Reverb:     return L"Reverb";
    case EffectType::Echo:       return L"Echo";
    case EffectType::Chorus:     return L"Chorus";
    case EffectType::Tremolo:    return L"Tremolo";
    case EffectType::Vibrato:    return L"Vibrato";
    case EffectType::PitchShifter: return L"Pitch Shifter";
    case EffectType::Tuner:      return L"Tuner";
    case EffectType::Looper:     return L"Looper";
    default:                     return L"";
    }
}

const wchar_t* EqualizerDynamicsModeName(EqualizerDynamicsMode mode)
{
    switch (mode)
    {
    case EqualizerDynamicsMode::None:       return L"No";
    case EqualizerDynamicsMode::Compressor: return L"Compressor";
    case EqualizerDynamicsMode::Expander:   return L"Expander";
    default:                                return L"";
    }
}

const wchar_t* ReverbAlgorithmName(ReverbAlgorithm algorithm)
{
    switch (algorithm)
    {
    case ReverbAlgorithm::CombAllpass:        return L"Comb/allpass";
    case ReverbAlgorithm::FeedbackDelayNetwork:
        return L"Feedback delay network (FDN)";
    default:                                  return L"";
    }
}

const wchar_t* EchoModeName(EchoMode mode)
{
    switch (mode)
    {
    case EchoMode::Stereo:    return L"Stereo";
    case EchoMode::PingPong:  return L"Ping-pong";
    default:                  return L"";
    }
}

const wchar_t* ModulationWaveformName(ModulationWaveform waveform)
{
    switch (waveform)
    {
    case ModulationWaveform::Sine:     return L"Sine";
    case ModulationWaveform::Triangle: return L"Triangle";
    case ModulationWaveform::Square:   return L"Square";
    default:                           return L"";
    }
}

const wchar_t* PitchAlgorithmName(PitchAlgorithm algorithm)
{
    switch (algorithm)
    {
    case PitchAlgorithm::Granular:      return L"Granular";
    case PitchAlgorithm::Wsola:         return L"WSOLA";
    case PitchAlgorithm::PhaseVocoder:  return L"Phase vocoder";
    default:                            return L"";
    }
}

const wchar_t* TunerAlgorithmName(TunerAlgorithm algorithm)
{
    switch (algorithm)
    {
    case TunerAlgorithm::Autocorrelation: return L"Autocorrelation";
    case TunerAlgorithm::FftPeak:         return L"FFT peak";
    default:                              return L"";
    }
}

const wchar_t* DistortionFunctionName(int function)
{
    switch (function)
    {
    case kDistortionTanh:   return L"Tanh";
    case kDistortionArctan: return L"Arctan";
    case kDistortionCubic:  return L"Cubic polynomial";
    case kDistortionLog:    return L"Logarithmic";
    case kDistortionExp:    return L"Exponential";
    case kDistortionErf:    return L"Error function";
    default:                return L"";
    }
}

const wchar_t* LooperStateName(LooperState state)
{
    switch (state)
    {
    case LooperState::Empty:      return L"Empty";
    case LooperState::Recording:  return L"Recording";
    case LooperState::Playback:   return L"Playback";
    case LooperState::Stopped:    return L"Stopped";
    default:                      return L"";
    }
}

const char* EffectTypeKey(EffectType type)
{
    switch (type)
    {
    case EffectType::Input:      return "input";
    case EffectType::Gain:       return "gain";
    case EffectType::Noise:      return "noise";
    case EffectType::Gate:       return "gate";
    case EffectType::Compressor: return "compressor";
    case EffectType::Equalizer:  return "equalizer";
    case EffectType::Saturation: return "saturation";
    case EffectType::Reverb:     return "reverb";
    case EffectType::Echo:       return "echo";
    case EffectType::Chorus:     return "chorus";
    case EffectType::Tremolo:    return "tremolo";
    case EffectType::Vibrato:    return "vibrato";
    case EffectType::PitchShifter: return "pitch_shifter";
    case EffectType::Tuner:      return "tuner";
    case EffectType::Looper:     return "looper";
    default:                     return "gain";
    }
}

bool ParseEffectType(const std::wstring& text, EffectType& type)
{
    for (int i = 0; i < static_cast<int>(EffectType::Count); ++i) {
        const auto candidate = static_cast<EffectType>(i);
        const char* key = EffectTypeKey(candidate);
        const std::wstring wide(key, key + std::strlen(key));
        if (EqualsIgnoreCase(text, wide.c_str())) {
            type = candidate;
            return true;
        }
    }
    return false;
}

int EffectTypeCapacity(EffectType type)
{
    switch (type)
    {
    case EffectType::Input:
    case EffectType::Looper:
        return 1;
    case EffectType::Noise:
    case EffectType::Gate:
    case EffectType::Compressor:
    case EffectType::Reverb:
    case EffectType::Echo:
    case EffectType::Chorus:
    case EffectType::PitchShifter:
    case EffectType::Tuner:
        return kMaxHeavyEffects;
    default:
        return kMaxEffects;
    }
}

bool EffectEnabled(const EffectInstance& effect)
{
    switch (effect.type)
    {
    case EffectType::Input:      return true;
    case EffectType::Gain:       return effect.gain.enabled;
    case EffectType::Noise:      return effect.noise.enabled;
    case EffectType::Gate:       return effect.gate.enabled;
    case EffectType::Compressor: return effect.compressor.enabled;
    case EffectType::Equalizer:  return effect.equalizer.enabled;
    case EffectType::Saturation: return effect.distortion.enabled;
    case EffectType::Reverb:     return effect.reverb.enabled;
    case EffectType::Echo:       return effect.echo.enabled;
    case EffectType::Chorus:     return effect.chorus.enabled;
    case EffectType::Tremolo:    return effect.tremolo.enabled;
    case EffectType::Vibrato:    return effect.vibrato.enabled;
    case EffectType::PitchShifter: return effect.pitchShifter.enabled;
    case EffectType::Tuner:      return effect.tuner.enabled;
    case EffectType::Looper:     return effect.looper.enabled;
    default:                     return false;
    }
}

void SetEffectEnabled(EffectInstance& effect, bool enabled)
{
    switch (effect.type)
    {
    case EffectType::Input:      effect.gain.enabled = true; break;
    case EffectType::Gain:       effect.gain.enabled = enabled; break;
    case EffectType::Noise:      effect.noise.enabled = enabled; break;
    case EffectType::Gate:       effect.gate.enabled = enabled; break;
    case EffectType::Compressor: effect.compressor.enabled = enabled; break;
    case EffectType::Equalizer:  effect.equalizer.enabled = enabled; break;
    case EffectType::Saturation: effect.distortion.enabled = enabled; break;
    case EffectType::Reverb:     effect.reverb.enabled = enabled; break;
    case EffectType::Echo:       effect.echo.enabled = enabled; break;
    case EffectType::Chorus:     effect.chorus.enabled = enabled; break;
    case EffectType::Tremolo:    effect.tremolo.enabled = enabled; break;
    case EffectType::Vibrato:    effect.vibrato.enabled = enabled; break;
    case EffectType::PitchShifter: effect.pitchShifter.enabled = enabled; break;
    case EffectType::Tuner:      effect.tuner.enabled = enabled; break;
    case EffectType::Looper:     effect.looper.enabled = enabled; break;
    default:                     break;
    }
}

EffectInstance MakeEffect(EffectType type, int id)
{
    EffectInstance effect;
    effect.id = id;
    effect.type = type;
    if (type == EffectType::Input) {
        effect.gain.gainDb = kInputDefaultGainDb;
    }
    SetEffectEnabled(effect, true);
    return effect;
}

EffectPreset& ActivePreset(ApoSettings& settings)
{
    const int index =
        (settings.presetIndex >= 0 && settings.presetIndex < kPresetCount)
            ? settings.presetIndex
            : 0;
    return settings.presets[index];
}

const EffectPreset& ActivePreset(const ApoSettings& settings)
{
    const int index =
        (settings.presetIndex >= 0 && settings.presetIndex < kPresetCount)
            ? settings.presetIndex
            : 0;
    return settings.presets[index];
}

std::wstring SafeEndpointId(const std::wstring& endpointId)
{
    std::wstring safe;
    safe.reserve(endpointId.size());
    for (wchar_t ch : endpointId) {
        const bool allowed =
            (ch >= L'a' && ch <= L'z') || (ch >= L'A' && ch <= L'Z') ||
            (ch >= L'0' && ch <= L'9') || ch == L'-' || ch == L'_' ||
            ch == L'{' || ch == L'}' || ch == L'.';
        safe.push_back(allowed ? ch : L'_');
    }
    if (safe.empty()) {
        safe = L"default";
    }
    return safe;
}

namespace {

void ClampFinite(double& value, double fallback, double low, double high)
{
    if (!std::isfinite(value)) {
        value = fallback;
    }
    value = Clamp(value, low, high);
}

void ClampRatio(double& value)
{
    if (std::isinf(value) && value > 0.0) {
        return;
    }
    if (!std::isfinite(value)) {
        value = 5.0;
    }
    value = Clamp(value, kDynamicsMinRatio, kDynamicsMaxFiniteRatio);
}

namespace {

void ClampDynamics(DynamicsSettings& dynamics)
{
    ClampFinite(dynamics.thresholdDb, -15.0, kMinThresholdDb,
                kMaxThresholdDb);
    ClampRatio(dynamics.ratio);
    ClampFinite(dynamics.inputDb, 0.0, -24.0, 30.0);
    ClampFinite(dynamics.outputDb, 0.0, -24.0, 24.0);
    ClampFinite(dynamics.attackMs, 5.0, 0.1, 1000.0);
    ClampFinite(dynamics.releaseMs, 100.0, 1.0, 5000.0);
    ClampFinite(dynamics.holdMs, 50.0, 0.0, 2000.0);
    ClampFinite(dynamics.lookaheadMs, 0.0, 0.0, kDynamicsMaxLookaheadMs);
}

} // namespace

void ClampEffect(EffectInstance& effect)
{
    switch (effect.type)
    {
    case EffectType::Input:
        ClampFinite(effect.gain.gainDb, kInputDefaultGainDb,
                    kInputMinGainDb, kInputMaxGainDb);
        SetEffectEnabled(effect, true);
        break;

    case EffectType::Gain:
        ClampFinite(effect.gain.gainDb, 12.0, -24.0, 50.0);
        effect.gain.treatAsMono = false;
        break;

    case EffectType::Noise:
        ClampFinite(effect.noise.reductionDb, 12.0, 0.0, 50.0);
        effect.noise.fftSize = ValidNoiseFftSize(effect.noise.fftSize);
        break;

    case EffectType::Gate:
        ClampDynamics(effect.gate);
        break;

    case EffectType::Compressor:
        ClampDynamics(effect.compressor);
        break;

    case EffectType::Equalizer: {
        if (effect.equalizer.mode < EqualizerDynamicsMode::None ||
            effect.equalizer.mode >= EqualizerDynamicsMode::Count) {
            effect.equalizer.mode = EqualizerDynamicsMode::None;
        }
        ClampFinite(effect.equalizer.frequencyHz, 120.0,
                    kEqMinFrequencyHz, kEqMaxFrequencyHz);
        ClampFinite(effect.equalizer.q, 0.7, kEqMinQ, kEqMaxQ);
        ClampFinite(effect.equalizer.levelDb, -12.0, kEqMinLevelDb,
                    kEqMaxLevelDb);
        ClampFinite(effect.equalizer.thresholdDb, -45.0, kMinThresholdDb,
                    kMaxThresholdDb);
        // Positive infinity is allowed here too: the right end stop is a hard
        // limiter/expander, matching the standalone dynamics ratio sliders.
        double& equalizerRatio = effect.equalizer.ratio;
        if (std::isinf(equalizerRatio) && equalizerRatio > 0.0) {
            // Keep true infinity.
        } else if (!std::isfinite(equalizerRatio) || equalizerRatio < 1.0) {
            equalizerRatio = 4.0;
        } else {
            equalizerRatio = std::min(equalizerRatio, kDynamicsMaxFiniteRatio);
        }
        ClampFinite(effect.equalizer.rangeDb, 18.0, 0.0, kEqMaxRangeDb);
        ClampFinite(effect.equalizer.attackMs, 10.0, 0.1, 1000.0);
        ClampFinite(effect.equalizer.releaseMs, 150.0, 1.0, 5000.0);
        ClampFinite(effect.equalizer.holdMs, 50.0, 0.0, kEqMaxHoldMs);
        ClampFinite(effect.equalizer.lookaheadMs, 0.0, 0.0,
                    kDynamicsMaxLookaheadMs);
        break;
    }

    case EffectType::Saturation:
        if (effect.distortion.function < 0 ||
            effect.distortion.function >= kDistortionFunctionCount) {
            effect.distortion.function = kDistortionTanh;
        }
        ClampFinite(effect.distortion.driveDb, 0.0, 0.0, 36.0);
        ClampFinite(effect.distortion.curvature, 50.0, 0.0, 100.0);
        ClampFinite(effect.distortion.outputDb, 0.0, -24.0, 12.0);
        break;

    case EffectType::Reverb:
        if (effect.reverb.algorithm < ReverbAlgorithm::CombAllpass ||
            effect.reverb.algorithm >= ReverbAlgorithm::Count) {
            effect.reverb.algorithm = ReverbAlgorithm::CombAllpass;
        }
        ClampFinite(effect.reverb.level, 20.0, 0.0, 100.0);
        ClampFinite(effect.reverb.maxLengthMs, 2000.0,
                    kReverbMinMaxLengthMs, kReverbMaxMaxLengthMs);
        ClampFinite(effect.reverb.size, 50.0, 0.0, 100.0);
        ClampFinite(effect.reverb.damping, 50.0, 0.0, 100.0);
        ClampFinite(effect.reverb.width, 50.0, 0.0, 100.0);
        ClampFinite(effect.reverb.density, 50.0, 0.0, 100.0);
        ClampFinite(effect.reverb.predelayMs, 20.0, 0.0, 200.0);
        break;

    case EffectType::Echo:
        if (effect.echo.mode < EchoMode::Stereo ||
            effect.echo.mode >= EchoMode::Count) {
            effect.echo.mode = EchoMode::Stereo;
        }
        ClampFinite(effect.echo.timeMs, kEchoDefaultTimeMs,
                    kEchoMinTimeMs, kEchoMaxTimeMs);
        ClampFinite(effect.echo.feedback, kEchoDefaultFeedback, 0.0,
                    kEchoMaxFeedback);
        ClampFinite(effect.echo.damping, kEchoDefaultDamping, 0.0, 100.0);
        ClampFinite(effect.echo.level, kEchoDefaultLevel, 0.0, 100.0);
        break;

    case EffectType::Chorus:
        if (effect.chorus.voices < kChorusMinVoices ||
            effect.chorus.voices > kChorusMaxVoices) {
            effect.chorus.voices = kChorusDefaultVoices;
        }
        ClampFinite(effect.chorus.rateHz, kChorusDefaultRateHz,
                    kChorusMinRateHz, kChorusMaxRateHz);
        ClampFinite(effect.chorus.depthMs, kChorusDefaultDepthMs, 0.0,
                    kChorusMaxDepthMs);
        ClampFinite(effect.chorus.width, kChorusDefaultWidth, 0.0, 100.0);
        ClampFinite(effect.chorus.level, kChorusDefaultLevel, 0.0, 100.0);
        break;

    case EffectType::Tremolo:
        ClampFinite(effect.tremolo.rateHz, kModulationDefaultRateHz,
                    kModulationMinRateHz, kModulationMaxRateHz);
        ClampFinite(effect.tremolo.depth, kTremoloDefaultDepth, 0.0, 100.0);
        if (effect.tremolo.waveform < ModulationWaveform::Sine ||
            effect.tremolo.waveform >= ModulationWaveform::Count) {
            effect.tremolo.waveform = ModulationWaveform::Sine;
        }
        ClampFinite(effect.tremolo.stereoSpread, kModulationDefaultSpread, 0.0,
                    kModulationMaxSpread);
        break;

    case EffectType::Vibrato:
        ClampFinite(effect.vibrato.rateHz, kModulationDefaultRateHz,
                    kModulationMinRateHz, kModulationMaxRateHz);
        ClampFinite(effect.vibrato.depthCents, kVibratoDefaultDepthCents, 0.0,
                    kVibratoMaxDepthCents);
        if (effect.vibrato.waveform < ModulationWaveform::Sine ||
            effect.vibrato.waveform >= ModulationWaveform::Count) {
            effect.vibrato.waveform = ModulationWaveform::Sine;
        }
        ClampFinite(effect.vibrato.stereoSpread, kModulationDefaultSpread, 0.0,
                    kModulationMaxSpread);
        break;

    case EffectType::PitchShifter:
        if (effect.pitchShifter.algorithm < PitchAlgorithm::Granular ||
            effect.pitchShifter.algorithm >= PitchAlgorithm::Count) {
            effect.pitchShifter.algorithm = PitchAlgorithm::Granular;
        }
        ClampFinite(effect.pitchShifter.shiftSemitones,
                    kPitchDefaultSemitones, kPitchMinSemitones,
                    kPitchMaxSemitones);
        effect.pitchShifter.windowSamples =
            ValidPitchWindow(effect.pitchShifter.windowSamples);
        ClampFinite(effect.pitchShifter.level, kPitchDefaultLevel, 0.0, 100.0);
        ClampFinite(effect.pitchShifter.dryLevel, kPitchDefaultDryLevel, 0.0,
                    100.0);
        break;

    case EffectType::Tuner:
        if (effect.tuner.algorithm < TunerAlgorithm::Autocorrelation ||
            effect.tuner.algorithm >= TunerAlgorithm::Count) {
            effect.tuner.algorithm = TunerAlgorithm::Autocorrelation;
        }
        effect.tuner.sampleSize = ValidTunerSampleSize(effect.tuner.sampleSize);
        ClampFinite(effect.tuner.smoothing, kTunerDefaultSmoothing,
                    kTunerMinSmoothing, kTunerMaxSmoothing);
        ClampFinite(effect.tuner.referenceHz, kTunerDefaultReferenceHz,
                    kTunerMinReferenceHz, kTunerMaxReferenceHz);
        break;

    case EffectType::Looper:
        for (LooperTrackSettings& track : effect.looper.tracks) {
            ClampFinite(track.volume, kLooperDefaultVolume, 0.0, 100.0);
        }
        // Transport commands are normalized but never cleared here: they must
        // survive every settings publish that follows the gesture.
        if (effect.looper.command < LooperCommand::None ||
            effect.looper.command > LooperCommand::DiscardProvisional) {
            effect.looper.command = LooperCommand::None;
        }
        effect.looper.commandTrack = static_cast<int>(
            Clamp(effect.looper.commandTrack, 0, kLooperTrackCount - 1));
        break;

    default:
        effect.type = EffectType::Gain;
        break;
    }
}

} // namespace

void ClampSettings(ApoSettings& settings)
{
    if (settings.presetIndex < 0 || settings.presetIndex >= kPresetCount) {
        settings.presetIndex = 0;
    }

    for (EffectPreset& preset : settings.presets) {
        preset.effectCount =
            static_cast<int>(Clamp(preset.effectCount, 0, kMaxEffects));

        // Input is structural: every preset has exactly one, at row 0. Older
        // or hand-edited snapshots are repaired here rather than migrated.
        if (preset.effectCount == 0) {
            preset.effects[0] = MakeEffect(EffectType::Input, 0);
            preset.effectCount = 1;
        } else if (preset.effects[0].type != EffectType::Input) {
            if (preset.effectCount < kMaxEffects) {
                for (int i = preset.effectCount; i > 0; --i) {
                    preset.effects[i] = preset.effects[i - 1];
                }
                ++preset.effectCount;
            } else {
                for (int i = kMaxEffects - 1; i > 0; --i) {
                    preset.effects[i] = preset.effects[i - 1];
                }
            }
            preset.effects[0] = MakeEffect(EffectType::Input, 0);
        }

        for (int i = 0; i < preset.effectCount; ++i) {
            if (static_cast<int>(preset.effects[i].type) < 0 ||
                preset.effects[i].type >= EffectType::Count) {
                preset.effects[i].type = EffectType::Gain;
            }
            ClampEffect(preset.effects[i]);
        }
        SetEffectEnabled(preset.effects[0], true);

        // Give every instance a unique positive id inside this preset. This
        // also repairs hand-edited files without changing well-formed ids.
        int next = (preset.nextEffectId > 0) ? preset.nextEffectId : 1;
        for (int i = 0; i < preset.effectCount; ++i) {
            if (preset.effects[i].id <= 0) {
                preset.effects[i].id = next++;
            }
        }
        for (int i = 0; i < preset.effectCount; ++i) {
            for (int j = 0; j < i; ++j) {
                if (preset.effects[j].id == preset.effects[i].id) {
                    preset.effects[i].id = next++;
                    break;
                }
            }
        }

        int maxId = 0;
        for (int i = 0; i < preset.effectCount; ++i) {
            maxId = std::max(maxId, preset.effects[i].id);
        }
        if (next <= maxId) {
            next = maxId + 1;
        }
        preset.nextEffectId = next;
    }

    // Isolation is not a user option any more. Forcing it here covers every
    // path that loads, saves, commits, or publishes settings.
    settings.isolateEffects = true;
}

std::wstring SettingsDirectory()
{
    return kDevicesDirectory;
}

std::wstring SettingsPath(const std::wstring& endpointId)
{
    return SettingsDirectory() + L"\\" + SafeEndpointId(endpointId) + L".ini";
}

namespace {

struct KeyValue
{
    std::wstring key;
    std::wstring value;
};

const std::wstring* FindValue(const std::vector<KeyValue>& entries,
                              const std::wstring& key)
{
    for (const KeyValue& entry : entries) {
        if (EqualsIgnoreCase(entry.key, key.c_str())) {
            return &entry.value;
        }
    }
    return nullptr;
}

void LoadEffectFields(const std::vector<KeyValue>& entries,
                      const std::wstring& prefix, EffectInstance& effect)
{
    auto getDouble = [&](const wchar_t* field, double& value) {
        const std::wstring* text = FindValue(entries, prefix + field);
        if (text != nullptr) {
            ParseDouble(*text, value);
        }
    };
    auto getBool = [&](const wchar_t* field, bool& value) {
        const std::wstring* text = FindValue(entries, prefix + field);
        if (text != nullptr) {
            ParseBool(*text, value);
        }
    };
    auto getInt = [&](const wchar_t* field, int& value) {
        const std::wstring* text = FindValue(entries, prefix + field);
        if (text != nullptr) {
            ParseInt(*text, value);
        }
    };
    auto loadDynamics = [&](const wchar_t* key, DynamicsSettings& dynamics) {
        const std::wstring fieldPrefix = std::wstring(key) + L"_";
        auto read = [&](const wchar_t* field, double& value) {
            getDouble((fieldPrefix + field).c_str(), value);
        };
        read(L"threshold_db", dynamics.thresholdDb);
        read(L"ratio", dynamics.ratio);
        read(L"input_db", dynamics.inputDb);
        read(L"output_db", dynamics.outputDb);
        read(L"attack_ms", dynamics.attackMs);
        read(L"release_ms", dynamics.releaseMs);
        read(L"hold_ms", dynamics.holdMs);
        read(L"lookahead_ms", dynamics.lookaheadMs);
    };

    getBool(L"enabled", effect.gain.enabled);
    getBool(L"enabled", effect.noise.enabled);
    getBool(L"enabled", effect.gate.enabled);
    getBool(L"enabled", effect.compressor.enabled);
    getBool(L"enabled", effect.equalizer.enabled);
    getBool(L"enabled", effect.distortion.enabled);
    getBool(L"enabled", effect.reverb.enabled);
    getBool(L"enabled", effect.echo.enabled);
    getBool(L"enabled", effect.chorus.enabled);
    getBool(L"enabled", effect.tremolo.enabled);
    getBool(L"enabled", effect.vibrato.enabled);
    getBool(L"enabled", effect.pitchShifter.enabled);
    getBool(L"enabled", effect.tuner.enabled);
    getBool(L"enabled", effect.looper.enabled);

    switch (effect.type)
    {
    case EffectType::Input:
        getDouble(L"gain_db", effect.gain.gainDb);
        getBool(L"input_treat_as_mono", effect.gain.treatAsMono);
        break;

    case EffectType::Gain:
        getDouble(L"gain_db", effect.gain.gainDb);
        break;

    case EffectType::Noise:
        getDouble(L"noise_reduction_db", effect.noise.reductionDb);
        getInt(L"noise_fft_size", effect.noise.fftSize);
        break;

    case EffectType::Gate:
        loadDynamics(L"gate", effect.gate);
        break;

    case EffectType::Compressor:
        loadDynamics(L"compressor", effect.compressor);
        break;

    case EffectType::Equalizer: {
        int mode = static_cast<int>(EqualizerDynamicsMode::None);
        getInt(L"equalizer_mode", mode);
        effect.equalizer.mode = static_cast<EqualizerDynamicsMode>(mode);
        getDouble(L"equalizer_frequency_hz", effect.equalizer.frequencyHz);
        getDouble(L"equalizer_q", effect.equalizer.q);
        getDouble(L"equalizer_level_db", effect.equalizer.levelDb);
        getDouble(L"equalizer_threshold_db", effect.equalizer.thresholdDb);
        getDouble(L"equalizer_ratio", effect.equalizer.ratio);
        getDouble(L"equalizer_range_db", effect.equalizer.rangeDb);
        getDouble(L"equalizer_attack_ms", effect.equalizer.attackMs);
        getDouble(L"equalizer_release_ms", effect.equalizer.releaseMs);
        getDouble(L"equalizer_hold_ms", effect.equalizer.holdMs);
        getDouble(L"equalizer_lookahead_ms", effect.equalizer.lookaheadMs);
        break;
    }

    case EffectType::Saturation:
        getInt(L"saturation_function", effect.distortion.function);
        getDouble(L"saturation_drive_db", effect.distortion.driveDb);
        getDouble(L"saturation_curvature", effect.distortion.curvature);
        getDouble(L"saturation_output_db", effect.distortion.outputDb);
        break;

    case EffectType::Reverb: {
        int algorithm = static_cast<int>(effect.reverb.algorithm);
        getInt(L"reverb_algorithm", algorithm);
        effect.reverb.algorithm = static_cast<ReverbAlgorithm>(algorithm);
        getDouble(L"reverb_level", effect.reverb.level);
        getDouble(L"reverb_max_length_ms", effect.reverb.maxLengthMs);
        getDouble(L"reverb_size", effect.reverb.size);
        getDouble(L"reverb_damping", effect.reverb.damping);
        getDouble(L"reverb_width", effect.reverb.width);
        getDouble(L"reverb_density", effect.reverb.density);
        getDouble(L"reverb_predelay_ms", effect.reverb.predelayMs);
        break;
    }

    case EffectType::Echo: {
        int mode = static_cast<int>(effect.echo.mode);
        getInt(L"echo_mode", mode);
        effect.echo.mode = static_cast<EchoMode>(mode);
        getDouble(L"echo_time_ms", effect.echo.timeMs);
        getDouble(L"echo_feedback", effect.echo.feedback);
        getDouble(L"echo_damping", effect.echo.damping);
        getDouble(L"echo_level", effect.echo.level);
        break;
    }

    case EffectType::Chorus:
        getInt(L"chorus_voices", effect.chorus.voices);
        getDouble(L"chorus_rate_hz", effect.chorus.rateHz);
        getDouble(L"chorus_depth_ms", effect.chorus.depthMs);
        getDouble(L"chorus_width", effect.chorus.width);
        getDouble(L"chorus_level", effect.chorus.level);
        break;

    case EffectType::Tremolo: {
        int waveform = static_cast<int>(effect.tremolo.waveform);
        getDouble(L"tremolo_rate_hz", effect.tremolo.rateHz);
        getDouble(L"tremolo_depth", effect.tremolo.depth);
        getInt(L"tremolo_waveform", waveform);
        effect.tremolo.waveform = static_cast<ModulationWaveform>(waveform);
        getDouble(L"tremolo_stereo_spread", effect.tremolo.stereoSpread);
        break;
    }

    case EffectType::Vibrato: {
        int waveform = static_cast<int>(effect.vibrato.waveform);
        getDouble(L"vibrato_rate_hz", effect.vibrato.rateHz);
        getDouble(L"vibrato_depth_cents", effect.vibrato.depthCents);
        getInt(L"vibrato_waveform", waveform);
        effect.vibrato.waveform = static_cast<ModulationWaveform>(waveform);
        getDouble(L"vibrato_stereo_spread", effect.vibrato.stereoSpread);
        break;
    }

    case EffectType::PitchShifter: {
        int algorithm = static_cast<int>(effect.pitchShifter.algorithm);
        getInt(L"pitch_algorithm", algorithm);
        effect.pitchShifter.algorithm = static_cast<PitchAlgorithm>(algorithm);
        getDouble(L"pitch_shift_semitones",
                  effect.pitchShifter.shiftSemitones);
        getInt(L"pitch_window_samples", effect.pitchShifter.windowSamples);
        getDouble(L"pitch_level", effect.pitchShifter.level);
        getDouble(L"pitch_dry_level", effect.pitchShifter.dryLevel);
        break;
    }

    case EffectType::Tuner: {
        int algorithm = static_cast<int>(effect.tuner.algorithm);
        getInt(L"tuner_algorithm", algorithm);
        effect.tuner.algorithm = static_cast<TunerAlgorithm>(algorithm);
        getInt(L"tuner_sample_size", effect.tuner.sampleSize);
        getDouble(L"tuner_smoothing", effect.tuner.smoothing);
        getDouble(L"tuner_reference_hz", effect.tuner.referenceHz);
        break;
    }

    case EffectType::Looper:
        for (int track = 0; track < kLooperTrackCount; ++track) {
            const std::wstring trackPrefix =
                L"looper_track_" + std::to_wstring(track) + L"_";
            auto trackValue = [&](const wchar_t* field, double& value) {
                getDouble((trackPrefix + field).c_str(), value);
            };
            auto trackBool = [&](const wchar_t* field, bool& value) {
                getBool((trackPrefix + field).c_str(), value);
            };
            trackValue(L"volume", effect.looper.tracks[track].volume);
            trackBool(L"free_length", effect.looper.tracks[track].freeLength);
            trackBool(L"stop_at_loop_end",
                      effect.looper.tracks[track].stopAtLoopEnd);
        }
        break;

    default:
        break;
    }
}

} // namespace

bool LoadSettings(const std::wstring& endpointId, ApoSettings& settings)
{
    settings = ApoSettings{};
    // Even when there is no durable file, expose the structural Input row.
    ClampSettings(settings);

    std::string data;
    if (!ReadFileText(SettingsPath(endpointId), data)) {
        return false;
    }

    const std::wstring text = Utf8ToWide(data);
    std::wstring section;
    std::vector<KeyValue> entries;
    size_t position = 0;
    while (position <= text.size()) {
        const size_t newline = text.find(L'\n', position);
        const std::wstring raw =
            text.substr(position, newline == std::wstring::npos
                                      ? std::wstring::npos
                                      : newline - position);
        position = (newline == std::wstring::npos) ? text.size() + 1
                                                   : newline + 1;

        const std::wstring line = Trim(raw);
        if (line.empty() || line[0] == L';' || line[0] == L'#') {
            continue;
        }

        if (line.front() == L'[' && line.back() == L']') {
            section = Trim(line.substr(1, line.size() - 2));
            continue;
        }
        if (!EqualsIgnoreCase(section, L"dsp")) {
            continue;
        }

        const size_t equals = line.find(L'=');
        if (equals == std::wstring::npos) {
            continue;
        }
        entries.push_back(
            KeyValue{Trim(line.substr(0, equals)),
                     Trim(line.substr(equals + 1))});
    }

    // A schema change invalidates the whole file. Delete it instead of trying
    // to reinterpret old fields; the endpoint starts with fresh defaults.
    int version = 0;
    const std::wstring* versionText = FindValue(entries, L"version");
    if (versionText == nullptr || !ParseInt(*versionText, version) ||
        version != kSettingsVersion)
    {
        DeleteFileW(SettingsPath(endpointId).c_str());
        ClampSettings(settings);
        return false;
    }

    if (const std::wstring* indexText =
            FindValue(entries, L"preset_index")) {
        ParseInt(*indexText, settings.presetIndex);
    }

    for (int p = 0; p < kPresetCount; ++p) {
        const std::wstring presetPrefix =
            L"preset_" + std::to_wstring(p) + L"_";
        EffectPreset& preset = settings.presets[p];

        const std::wstring* countText =
            FindValue(entries, presetPrefix + L"effect_count");
        if (countText != nullptr) {
            int count = 0;
            if (!ParseInt(*countText, count)) {
                count = 0;
            }
            preset.effectCount =
                static_cast<int>(Clamp(count, 0, kMaxEffects));
        }

        for (int i = 0; i < preset.effectCount; ++i) {
            const std::wstring prefix =
                presetPrefix + L"effect_" + std::to_wstring(i) + L"_";
            EffectType type = EffectType::Gain;
            const std::wstring* typeText =
                FindValue(entries, prefix + L"type");
            if (typeText != nullptr) {
                ParseEffectType(*typeText, type);
            }

            preset.effects[i] = MakeEffect(type, 0);
            const std::wstring* idText = FindValue(entries, prefix + L"id");
            if (idText != nullptr) {
                ParseInt(*idText, preset.effects[i].id);
            }
            LoadEffectFields(entries, prefix, preset.effects[i]);
        }

        const std::wstring* nextText =
            FindValue(entries, presetPrefix + L"next_effect_id");
        if (nextText != nullptr) {
            ParseInt(*nextText, preset.nextEffectId);
        }
    }

    ClampSettings(settings);
    return true;
}

namespace {

void AppendDynamics(std::string& text, const std::string& prefix,
                    const char* key, const DynamicsSettings& dynamics)
{
    const std::string fieldPrefix = prefix + key + "_";
    text += fieldPrefix + "threshold_db=" +
            FormatDouble(dynamics.thresholdDb) + "\n";
    text += fieldPrefix + "ratio=" + FormatDouble(dynamics.ratio) + "\n";
    text += fieldPrefix + "input_db=" + FormatDouble(dynamics.inputDb) + "\n";
    text += fieldPrefix + "output_db=" + FormatDouble(dynamics.outputDb) + "\n";
    text += fieldPrefix + "attack_ms=" + FormatDouble(dynamics.attackMs) + "\n";
    text += fieldPrefix + "release_ms=" +
            FormatDouble(dynamics.releaseMs) + "\n";
    text += fieldPrefix + "hold_ms=" + FormatDouble(dynamics.holdMs) + "\n";
    text += fieldPrefix + "lookahead_ms=" +
            FormatDouble(dynamics.lookaheadMs) + "\n";
}

} // namespace

bool SaveSettings(const std::wstring& endpointId, const ApoSettings& input)
{
    ApoSettings settings = input;
    ClampSettings(settings);

    CreateDirectoryW(kDataDirectory, nullptr);
    CreateDirectoryW(kDevicesDirectory, nullptr);

    std::string text;
    text += "[dsp]\n";
    text += "version=" + std::to_string(kSettingsVersion) + "\n";
    text += "preset_count=" + std::to_string(kPresetCount) + "\n";
    text += "preset_index=" + std::to_string(settings.presetIndex) + "\n";
    text += "isolate_effects=1\n";

    for (int p = 0; p < kPresetCount; ++p) {
        const EffectPreset& preset = settings.presets[p];
        const std::string presetPrefix =
            "preset_" + std::to_string(p) + "_";
        text += presetPrefix +
                "effect_count=" + std::to_string(preset.effectCount) + "\n";
        text += presetPrefix +
                "next_effect_id=" + std::to_string(preset.nextEffectId) +
                "\n";

        for (int i = 0; i < preset.effectCount; ++i) {
            const EffectInstance& effect = preset.effects[i];
            const std::string prefix =
                presetPrefix + "effect_" + std::to_string(i) + "_";
            text += prefix + "id=" + std::to_string(effect.id) + "\n";
            text += prefix + "type=" + EffectTypeKey(effect.type) + "\n";
            text += prefix + "enabled=" +
                    (EffectEnabled(effect) ? "1" : "0") + "\n";

            switch (effect.type)
            {
            case EffectType::Input:
                text += prefix + "gain_db=" +
                        FormatDouble(effect.gain.gainDb) + "\n";
                text += prefix + "input_treat_as_mono=" +
                        (effect.gain.treatAsMono ? "1" : "0") + "\n";
                break;

            case EffectType::Gain:
                text += prefix + "gain_db=" +
                        FormatDouble(effect.gain.gainDb) + "\n";
                break;

            case EffectType::Noise:
                text += prefix + "noise_reduction_db=" +
                        FormatDouble(effect.noise.reductionDb) + "\n";
                text += prefix + "noise_fft_size=" +
                        std::to_string(effect.noise.fftSize) + "\n";
                break;

            case EffectType::Gate:
                AppendDynamics(text, prefix, "gate", effect.gate);
                break;

            case EffectType::Compressor:
                AppendDynamics(text, prefix, "compressor", effect.compressor);
                break;

            case EffectType::Equalizer:
                text += prefix + "equalizer_mode=" +
                        std::to_string(static_cast<int>(effect.equalizer.mode)) +
                        "\n";
                text += prefix + "equalizer_frequency_hz=" +
                        FormatDouble(effect.equalizer.frequencyHz) + "\n";
                text += prefix + "equalizer_q=" +
                        FormatDouble(effect.equalizer.q) + "\n";
                text += prefix + "equalizer_threshold_db=" +
                        FormatDouble(effect.equalizer.thresholdDb) + "\n";
                text += prefix + "equalizer_ratio=" +
                        FormatDouble(effect.equalizer.ratio) + "\n";
                text += prefix + "equalizer_range_db=" +
                        FormatDouble(effect.equalizer.rangeDb) + "\n";
                text += prefix + "equalizer_level_db=" +
                        FormatDouble(effect.equalizer.levelDb) + "\n";
                text += prefix + "equalizer_attack_ms=" +
                        FormatDouble(effect.equalizer.attackMs) + "\n";
                text += prefix + "equalizer_release_ms=" +
                        FormatDouble(effect.equalizer.releaseMs) + "\n";
                text += prefix + "equalizer_hold_ms=" +
                        FormatDouble(effect.equalizer.holdMs) + "\n";
                text += prefix + "equalizer_lookahead_ms=" +
                        FormatDouble(effect.equalizer.lookaheadMs) + "\n";
                break;

            case EffectType::Saturation:
                text += prefix + "saturation_function=" +
                        std::to_string(effect.distortion.function) + "\n";
                text += prefix + "saturation_drive_db=" +
                        FormatDouble(effect.distortion.driveDb) + "\n";
                text += prefix + "saturation_curvature=" +
                        FormatDouble(effect.distortion.curvature) + "\n";
                text += prefix + "saturation_output_db=" +
                        FormatDouble(effect.distortion.outputDb) + "\n";
                break;

            case EffectType::Reverb:
                text += prefix + "reverb_algorithm=" +
                        std::to_string(
                            static_cast<int>(effect.reverb.algorithm)) +
                        "\n";
                text += prefix + "reverb_level=" +
                        FormatDouble(effect.reverb.level) + "\n";
                text += prefix + "reverb_max_length_ms=" +
                        FormatDouble(effect.reverb.maxLengthMs) + "\n";
                text += prefix + "reverb_size=" +
                        FormatDouble(effect.reverb.size) + "\n";
                text += prefix + "reverb_damping=" +
                        FormatDouble(effect.reverb.damping) + "\n";
                text += prefix + "reverb_width=" +
                        FormatDouble(effect.reverb.width) + "\n";
                text += prefix + "reverb_density=" +
                        FormatDouble(effect.reverb.density) + "\n";
                text += prefix + "reverb_predelay_ms=" +
                        FormatDouble(effect.reverb.predelayMs) + "\n";
                break;

            case EffectType::Echo:
                text += prefix + "echo_mode=" +
                        std::to_string(static_cast<int>(effect.echo.mode)) +
                        "\n";
                text += prefix + "echo_time_ms=" +
                        FormatDouble(effect.echo.timeMs) + "\n";
                text += prefix + "echo_feedback=" +
                        FormatDouble(effect.echo.feedback) + "\n";
                text += prefix + "echo_damping=" +
                        FormatDouble(effect.echo.damping) + "\n";
                text += prefix + "echo_level=" +
                        FormatDouble(effect.echo.level) + "\n";
                break;

            case EffectType::Chorus:
                text += prefix + "chorus_voices=" +
                        std::to_string(effect.chorus.voices) + "\n";
                text += prefix + "chorus_rate_hz=" +
                        FormatDouble(effect.chorus.rateHz) + "\n";
                text += prefix + "chorus_depth_ms=" +
                        FormatDouble(effect.chorus.depthMs) + "\n";
                text += prefix + "chorus_width=" +
                        FormatDouble(effect.chorus.width) + "\n";
                text += prefix + "chorus_level=" +
                        FormatDouble(effect.chorus.level) + "\n";
                break;

            case EffectType::Tremolo:
                text += prefix + "tremolo_rate_hz=" +
                        FormatDouble(effect.tremolo.rateHz) + "\n";
                text += prefix + "tremolo_depth=" +
                        FormatDouble(effect.tremolo.depth) + "\n";
                text += prefix + "tremolo_waveform=" +
                        std::to_string(
                            static_cast<int>(effect.tremolo.waveform)) +
                        "\n";
                text += prefix + "tremolo_stereo_spread=" +
                        FormatDouble(effect.tremolo.stereoSpread) + "\n";
                break;

            case EffectType::Vibrato:
                text += prefix + "vibrato_rate_hz=" +
                        FormatDouble(effect.vibrato.rateHz) + "\n";
                text += prefix + "vibrato_depth_cents=" +
                        FormatDouble(effect.vibrato.depthCents) + "\n";
                text += prefix + "vibrato_waveform=" +
                        std::to_string(
                            static_cast<int>(effect.vibrato.waveform)) +
                        "\n";
                text += prefix + "vibrato_stereo_spread=" +
                        FormatDouble(effect.vibrato.stereoSpread) + "\n";
                break;

            case EffectType::PitchShifter:
                text += prefix + "pitch_algorithm=" +
                        std::to_string(
                            static_cast<int>(effect.pitchShifter.algorithm)) +
                        "\n";
                text += prefix + "pitch_shift_semitones=" +
                        FormatDouble(effect.pitchShifter.shiftSemitones) +
                        "\n";
                text += prefix + "pitch_window_samples=" +
                        std::to_string(effect.pitchShifter.windowSamples) +
                        "\n";
                text += prefix + "pitch_level=" +
                        FormatDouble(effect.pitchShifter.level) + "\n";
                text += prefix + "pitch_dry_level=" +
                        FormatDouble(effect.pitchShifter.dryLevel) + "\n";
                break;

            case EffectType::Tuner:
                text += prefix + "tuner_algorithm=" +
                        std::to_string(
                            static_cast<int>(effect.tuner.algorithm)) +
                        "\n";
                text += prefix + "tuner_sample_size=" +
                        std::to_string(effect.tuner.sampleSize) + "\n";
                text += prefix + "tuner_smoothing=" +
                        FormatDouble(effect.tuner.smoothing) + "\n";
                text += prefix + "tuner_reference_hz=" +
                        FormatDouble(effect.tuner.referenceHz) + "\n";
                break;

            case EffectType::Looper:
                // Transport commands live only in the runtime channel. No
                // command is written to the durable INI file.
                for (int track = 0; track < kLooperTrackCount; ++track) {
                    const std::string trackPrefix =
                        prefix + "looper_track_" + std::to_string(track) + "_";
                    const LooperTrackSettings& trackSettings =
                        effect.looper.tracks[track];
                    text += trackPrefix + "volume=" +
                            FormatDouble(trackSettings.volume) + "\n";
                    text += trackPrefix + "free_length=" +
                            (trackSettings.freeLength ? "1" : "0") + "\n";
                    text += trackPrefix + "stop_at_loop_end=" +
                            (trackSettings.stopAtLoopEnd ? "1" : "0") + "\n";
                }
                break;

            default:
                break;
            }
        }
    }

    return WriteFileText(SettingsPath(endpointId), text);
}

namespace {

#pragma pack(push, 1)
struct NoiseFileHeader
{
    char magic[4];
    std::uint32_t version;
    std::uint32_t sampleRate;
    std::uint32_t channels;
    std::uint32_t frames;
};

struct NoiseBinsHeader
{
    char magic[4];
    std::uint32_t version;
    std::uint32_t sampleRate;
    std::uint32_t binCount;
};
#pragma pack(pop)

constexpr std::uint32_t kNoiseFileVersion = 1;
constexpr std::uint32_t kNoiseBinsVersion = 2;
constexpr std::uint64_t kNoiseFileLimit = 64ull * 1024 * 1024;

#pragma pack(push, 1)
struct LooperBankFileHeader
{
    char magic[4];
    std::uint32_t version;
    std::uint32_t sampleRate;
    std::uint32_t channels;
    std::uint32_t maxFrames;
    std::uint32_t trackCount;
    std::uint64_t globalFrames;
    std::int32_t referenceTrack;
    std::uint64_t masterFrames;
    std::uint64_t syncOrigin;
    std::uint32_t undoValid;
    std::int32_t undoTrack;
    std::uint64_t undoLength;
    std::int32_t undoState;
    std::int32_t undoReferenceTrack;
    std::uint64_t undoMasterFrames;
    std::uint64_t undoSyncOrigin;
};

struct LooperBankFileTrack
{
    std::int32_t state;
    std::uint64_t length;
    std::uint64_t position;
    std::uint64_t recordPosition;
    std::uint64_t recordFrames;
    std::uint32_t flags;
};
#pragma pack(pop)

constexpr std::uint32_t kLooperBankVersion = 1;
constexpr std::uint64_t kLooperBankFileLimit = 2ull * 1024 * 1024 * 1024;
constexpr std::uint32_t kLooperTrackSyncedFlag = 1u;
constexpr std::uint32_t kLooperTrackOverdubFlag = 2u;
constexpr std::uint32_t kLooperTrackPendingStopFlag = 4u;
constexpr std::uint32_t kLooperTrackReferenceFlag = 8u;

bool ValidNoiseHeader(const NoiseFileHeader& header)
{
    return std::memcmp(header.magic, "BMNP", 4) == 0 &&
           header.version == kNoiseFileVersion && header.sampleRate > 0 &&
           header.channels > 0 &&
           header.frames >= static_cast<std::uint32_t>(kNoiseDefaultFftSize);
}

} // namespace

std::wstring NoiseProfilePath(const std::wstring& endpointId)
{
    return SettingsDirectory() + L"\\" + SafeEndpointId(endpointId) + L".noise";
}

bool SaveNoiseCapture(const std::wstring& endpointId, std::uint32_t sampleRate,
                      std::uint32_t channels,
                      const std::vector<float>& samples)
{
    if (sampleRate == 0 || channels == 0 || samples.empty() ||
        samples.size() % channels != 0)
    {
        return false;
    }

    const std::uint64_t frames64 = samples.size() / channels;
    if (frames64 > 0xFFFFFFFFull)
    {
        return false;
    }

    CreateDirectoryW(kDataDirectory, nullptr);
    CreateDirectoryW(kDevicesDirectory, nullptr);

    NoiseFileHeader header{};
    std::memcpy(header.magic, "BMNP", 4);
    header.version = kNoiseFileVersion;
    header.sampleRate = sampleRate;
    header.channels = channels;
    header.frames = static_cast<std::uint32_t>(frames64);

    const std::wstring path = NoiseProfilePath(endpointId);
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE,
                              FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                              CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
    {
        return false;
    }

    DWORD written = 0;
    bool ok = WriteFile(file, &header, sizeof(header), &written, nullptr) &&
              written == sizeof(header);
    if (ok)
    {
        const DWORD bytes =
            static_cast<DWORD>(samples.size() * sizeof(float));
        ok = WriteFile(file, samples.data(), bytes, &written, nullptr) &&
             written == bytes;
    }
    CloseHandle(file);

    if (!ok)
    {
        DeleteFileW(path.c_str());
    }
    return ok;
}

bool SaveNoiseProfileBins(const std::wstring& endpointId,
                          std::uint32_t sampleRate,
                          const std::vector<double>& magnitudes)
{
    const int profileFft = (magnitudes.size() >= 2)
        ? static_cast<int>((magnitudes.size() - 1) * 2)
        : 0;
    if (sampleRate == 0 || magnitudes.empty() ||
        magnitudes.size() > dsp::kNoiseMaxBins || profileFft <= 0 ||
        ValidNoiseFftSize(profileFft) != profileFft)
    {
        return false;
    }

    CreateDirectoryW(kDataDirectory, nullptr);
    CreateDirectoryW(kDevicesDirectory, nullptr);

    NoiseBinsHeader header{};
    std::memcpy(header.magic, "BMNP", 4);
    header.version = kNoiseBinsVersion;
    header.sampleRate = sampleRate;
    header.binCount = static_cast<std::uint32_t>(magnitudes.size());

    const std::wstring path = NoiseProfilePath(endpointId);
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE,
                              FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                              CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
    {
        return false;
    }

    DWORD written = 0;
    bool ok = WriteFile(file, &header, sizeof(header), &written, nullptr) &&
              written == sizeof(header);
    if (ok)
    {
        const DWORD bytes = static_cast<DWORD>(magnitudes.size() *
                                               sizeof(double));
        ok = WriteFile(file, magnitudes.data(), bytes, &written, nullptr) &&
             written == bytes;
    }
    CloseHandle(file);

    if (!ok)
    {
        DeleteFileW(path.c_str());
    }
    return ok;
}

bool LoadNoiseProfile(const std::wstring& endpointId, NoiseProfile& profile)
{
    profile = NoiseProfile{};

    const std::wstring path = NoiseProfilePath(endpointId);
    HANDLE file = CreateFileW(
        path.c_str(), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
    {
        return false;
    }

    auto readExact = [file](void* data, DWORD bytes) {
        DWORD read = 0;
        return ReadFile(file, data, bytes, &read, nullptr) && read == bytes;
    };
    auto storeBins = [&](std::vector<double>& values) {
        profile.binCount = values.size();
        profile.magnitudes.assign(dsp::kNoiseMaxBins, 0.0);
        std::copy(values.begin(), values.end(), profile.magnitudes.begin());
        profile.valid = true;
    };

    BYTE prefix[8]{};
    bool ok = readExact(prefix, sizeof(prefix)) &&
              std::memcmp(prefix, "BMNP", 4) == 0;

    if (ok)
    {
        std::uint32_t version = 0;
        std::memcpy(&version, prefix + 4, sizeof(version));

        if (version == kNoiseBinsVersion)
        {
            NoiseBinsHeader bins{};
            std::memcpy(&bins, prefix, sizeof(prefix));
            ok = readExact(reinterpret_cast<BYTE*>(&bins) + sizeof(prefix),
                           sizeof(bins) - sizeof(prefix));
            const std::uint64_t bytes64 =
                static_cast<std::uint64_t>(bins.binCount) * sizeof(double);
            const int profileFft =
                (bins.binCount >= 2)
                    ? static_cast<int>((bins.binCount - 1) * 2)
                    : 0;
            ok = ok && bins.sampleRate != 0 && bins.binCount != 0 &&
                 bins.binCount <= dsp::kNoiseMaxBins &&
                 profileFft > 0 && ValidNoiseFftSize(profileFft) == profileFft &&
                 bytes64 <= kNoiseFileLimit;
            if (ok)
            {
                std::vector<double> magnitudes(bins.binCount);
                ok = readExact(magnitudes.data(),
                               static_cast<DWORD>(bytes64));
                if (ok)
                {
                    profile.sampleRate = bins.sampleRate;
                    storeBins(magnitudes);
                }
            }
        }
        else if (version == kNoiseFileVersion)
        {
            NoiseFileHeader header{};
            std::memcpy(&header, prefix, sizeof(prefix));
            ok = readExact(reinterpret_cast<BYTE*>(&header) + sizeof(prefix),
                           sizeof(header) - sizeof(prefix)) &&
                 ValidNoiseHeader(header);
            if (ok)
            {
                const std::uint64_t bytes64 =
                    static_cast<std::uint64_t>(header.frames) *
                    header.channels * sizeof(float);
                ok = bytes64 <= kNoiseFileLimit;
                if (ok)
                {
                    std::vector<float> samples(
                        static_cast<size_t>(header.frames) * header.channels);
                    ok = readExact(samples.data(),
                                   static_cast<DWORD>(bytes64));
                    if (ok)
                    {
                        std::vector<double> magnitudes =
                            apo::dsp::AverageMagnitudeSpectrum(
                                samples.data(), header.frames, header.channels,
                                kNoiseDefaultFftSize);
                        ok = !magnitudes.empty();
                        if (ok)
                        {
                            profile.sampleRate = header.sampleRate;
                            storeBins(magnitudes);
                        }
                    }
                }
            }
        }
        else
        {
            ok = false;
        }
    }

    CloseHandle(file);
    return ok;
}

std::wstring LooperBankPath(const std::wstring& endpointId)
{
    return SettingsDirectory() + L"\\" + SafeEndpointId(endpointId) +
           L".looper";
}

bool SaveLooperBank(const std::wstring& endpointId,
                    const LooperBankData& bank)
{
    if (!bank.valid || endpointId.empty() || bank.sampleRate == 0 ||
        bank.channels == 0 || bank.maxFrames == 0)
    {
        return false;
    }

    const std::uint64_t expectedMax = static_cast<std::uint64_t>(
        std::ceil(apo::kLooperMaxLengthSeconds * bank.sampleRate) + 1.0);
    if (bank.maxFrames > expectedMax)
    {
        return false;
    }

    CreateDirectoryW(kDataDirectory, nullptr);
    CreateDirectoryW(kDevicesDirectory, nullptr);

    const std::wstring finalPath = LooperBankPath(endpointId);
    const std::wstring tempPath = finalPath + L".tmp";
    HANDLE file = CreateFileW(
        tempPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL | FILE_ATTRIBUTE_TEMPORARY, nullptr);
    if (file == nullptr || file == INVALID_HANDLE_VALUE)
    {
        return false;
    }

    auto writeExact = [file](const void* data, DWORD bytes) {
        DWORD written = 0;
        return WriteFile(file, data, bytes, &written, nullptr) &&
               written == bytes;
    };

    std::array<std::uint64_t, apo::kLooperTrackCount> trackSamples{};
    std::uint64_t totalBytes = sizeof(LooperBankFileHeader) +
                               sizeof(LooperBankFileTrack) *
                                   apo::kLooperTrackCount;
    for (int index = 0; index < apo::kLooperTrackCount; ++index)
    {
        const LooperBankTrack& track = bank.tracks[static_cast<size_t>(index)];
        const std::uint64_t samples =
            static_cast<std::uint64_t>(track.length) * bank.channels;
        if (track.length > bank.maxFrames || track.position > bank.maxFrames ||
            track.recordPosition > bank.maxFrames)
        {
            CloseHandle(file);
            DeleteFileW(tempPath.c_str());
            return false;
        }
        const std::uint64_t bytes = samples * sizeof(float);
        if (bytes > std::numeric_limits<DWORD>::max() ||
            bank.buffers[static_cast<size_t>(index)].size() < samples)
        {
            CloseHandle(file);
            DeleteFileW(tempPath.c_str());
            return false;
        }
        trackSamples[static_cast<size_t>(index)] = samples;
        totalBytes += bytes;
    }

    const std::uint64_t undoSamples =
        static_cast<std::uint64_t>(bank.undoLength) * bank.channels;
    const std::uint64_t undoBytes = undoSamples * sizeof(float);
    if (bank.undoLength > bank.maxFrames ||
        undoBytes > std::numeric_limits<DWORD>::max() ||
        (bank.undoValid && bank.undoBuffer.size() < undoSamples))
    {
        CloseHandle(file);
        DeleteFileW(tempPath.c_str());
        return false;
    }
    totalBytes += sizeof(LooperBankFileTrack) + undoBytes;
    if (totalBytes > kLooperBankFileLimit)
    {
        CloseHandle(file);
        DeleteFileW(tempPath.c_str());
        return false;
    }

    LooperBankFileHeader header{};
    std::memcpy(header.magic, "BMLP", 4);
    header.version = kLooperBankVersion;
    header.sampleRate = bank.sampleRate;
    header.channels = bank.channels;
    header.maxFrames = bank.maxFrames;
    header.trackCount = apo::kLooperTrackCount;
    header.globalFrames = bank.globalFrames;
    header.referenceTrack = bank.referenceTrack;
    header.masterFrames = bank.masterFrames;
    header.syncOrigin = bank.syncOrigin;
    header.undoValid = bank.undoValid ? 1u : 0u;
    header.undoTrack = bank.undoTrack;
    header.undoLength = bank.undoLength;
    header.undoState = static_cast<std::int32_t>(bank.undoState);
    header.undoReferenceTrack = bank.undoReferenceTrack;
    header.undoMasterFrames = bank.undoMasterFrames;
    header.undoSyncOrigin = bank.undoSyncOrigin;

    bool ok = writeExact(&header, sizeof(header));
    for (int index = 0; index < apo::kLooperTrackCount && ok; ++index)
    {
        const LooperBankTrack& track = bank.tracks[static_cast<size_t>(index)];
        LooperBankFileTrack fileTrack{};
        fileTrack.state = static_cast<std::int32_t>(track.state);
        fileTrack.length = track.length;
        fileTrack.position = track.position;
        fileTrack.recordPosition = track.recordPosition;
        fileTrack.recordFrames = track.recordFrames;
        fileTrack.flags =
            (track.synced ? kLooperTrackSyncedFlag : 0u) |
            (track.overdub ? kLooperTrackOverdubFlag : 0u) |
            (track.pendingStop ? kLooperTrackPendingStopFlag : 0u) |
            (track.isReference ? kLooperTrackReferenceFlag : 0u);
        ok = writeExact(&fileTrack, sizeof(fileTrack));

        const std::uint64_t samples = trackSamples[static_cast<size_t>(index)];
        if (ok && samples != 0)
        {
            ok = writeExact(
                bank.buffers[static_cast<size_t>(index)].data(),
                static_cast<DWORD>(samples * sizeof(float)));
        }
    }

    if (ok)
    {
        LooperBankFileTrack undoTrack{};
        undoTrack.state = header.undoState;
        undoTrack.length = bank.undoLength;
        undoTrack.flags = 0;
        ok = writeExact(&undoTrack, sizeof(undoTrack));
        if (ok && bank.undoValid && undoSamples != 0)
        {
            ok = writeExact(
                bank.undoBuffer.data(),
                static_cast<DWORD>(undoSamples * sizeof(float)));
        }
    }

    CloseHandle(file);
    if (!ok)
    {
        DeleteFileW(tempPath.c_str());
        return false;
    }
    if (!MoveFileExW(tempPath.c_str(), finalPath.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
    {
        DeleteFileW(tempPath.c_str());
        return false;
    }
    return true;
}

bool LoadLooperBank(const std::wstring& endpointId, LooperBankData& bank)
{
    bank = LooperBankData{};
    if (endpointId.empty())
    {
        return false;
    }

    const std::wstring path = LooperBankPath(endpointId);
    HANDLE file = CreateFileW(
        path.c_str(), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == nullptr || file == INVALID_HANDLE_VALUE)
    {
        return false;
    }

    auto readExact = [file](void* data, DWORD bytes) {
        DWORD read = 0;
        return ReadFile(file, data, bytes, &read, nullptr) && read == bytes;
    };
    auto reject = [&]() {
        CloseHandle(file);
        DeleteFileW(path.c_str());
        bank = LooperBankData{};
        return false;
    };

    LooperBankFileHeader header{};
    if (!readExact(&header, sizeof(header)) ||
        std::memcmp(header.magic, "BMLP", 4) != 0 ||
        header.version != kLooperBankVersion ||
        header.trackCount != apo::kLooperTrackCount || header.sampleRate == 0 ||
        header.channels == 0 || header.maxFrames == 0)
    {
        return reject();
    }

    const std::uint64_t expectedMax = static_cast<std::uint64_t>(
        std::ceil(apo::kLooperMaxLengthSeconds * header.sampleRate) + 1.0);
    if (header.maxFrames > expectedMax ||
        (header.undoValid != 0u && header.undoValid != 1u) ||
        (header.referenceTrack != -1 &&
         (header.referenceTrack < 0 ||
          header.referenceTrack >= apo::kLooperTrackCount)) ||
        (header.undoReferenceTrack != -1 &&
         (header.undoReferenceTrack < 0 ||
          header.undoReferenceTrack >= apo::kLooperTrackCount)) ||
        (header.undoState < static_cast<std::int32_t>(LooperState::Empty) ||
         header.undoState > static_cast<std::int32_t>(LooperState::Stopped)))
    {
        return reject();
    }

    bank.sampleRate = header.sampleRate;
    bank.channels = header.channels;
    bank.maxFrames = header.maxFrames;
    bank.globalFrames = header.globalFrames;
    bank.referenceTrack = header.referenceTrack;
    bank.masterFrames = header.masterFrames;
    bank.syncOrigin = header.syncOrigin;
    bank.undoValid = header.undoValid != 0u;
    bank.undoTrack = header.undoTrack;
    bank.undoLength = header.undoLength;
    bank.undoState = static_cast<LooperState>(header.undoState);
    bank.undoReferenceTrack = header.undoReferenceTrack;
    bank.undoMasterFrames = header.undoMasterFrames;
    bank.undoSyncOrigin = header.undoSyncOrigin;

    std::array<std::uint64_t, apo::kLooperTrackCount> trackSamples{};
    std::uint64_t totalBytes = sizeof(header) +
                               sizeof(LooperBankFileTrack) *
                                   apo::kLooperTrackCount;
    for (int index = 0; index < apo::kLooperTrackCount; ++index)
    {
        LooperBankFileTrack fileTrack{};
        if (!readExact(&fileTrack, sizeof(fileTrack)) ||
            fileTrack.state < static_cast<std::int32_t>(LooperState::Empty) ||
            fileTrack.state > static_cast<std::int32_t>(LooperState::Stopped) ||
            (fileTrack.flags & ~(kLooperTrackSyncedFlag |
                                 kLooperTrackOverdubFlag |
                                 kLooperTrackPendingStopFlag |
                                 kLooperTrackReferenceFlag)) != 0u)
        {
            return reject();
        }

        LooperBankTrack& track = bank.tracks[static_cast<size_t>(index)];
        track.state = static_cast<LooperState>(fileTrack.state);
        track.length = fileTrack.length;
        track.position = fileTrack.position;
        track.recordPosition = fileTrack.recordPosition;
        track.recordFrames = fileTrack.recordFrames;
        track.synced = (fileTrack.flags & kLooperTrackSyncedFlag) != 0u;
        track.overdub = (fileTrack.flags & kLooperTrackOverdubFlag) != 0u;
        track.pendingStop =
            (fileTrack.flags & kLooperTrackPendingStopFlag) != 0u;
        track.isReference = (fileTrack.flags & kLooperTrackReferenceFlag) != 0u;

        if (track.length > header.maxFrames ||
            track.position > header.maxFrames ||
            track.recordPosition > header.maxFrames)
        {
            return reject();
        }
        const std::uint64_t samples =
            static_cast<std::uint64_t>(track.length) * header.channels;
        const std::uint64_t bytes = samples * sizeof(float);
        if (bytes > std::numeric_limits<DWORD>::max())
        {
            return reject();
        }
        trackSamples[static_cast<size_t>(index)] = samples;
        totalBytes += bytes;
    }

    if (bank.undoLength > header.maxFrames ||
        (bank.undoTrack != -1 &&
         (bank.undoTrack < 0 ||
          bank.undoTrack >= apo::kLooperTrackCount)))
    {
        return reject();
    }
    const std::uint64_t undoSamples =
        static_cast<std::uint64_t>(bank.undoLength) * header.channels;
    const std::uint64_t undoBytes = undoSamples * sizeof(float);
    if (undoBytes > std::numeric_limits<DWORD>::max())
    {
        return reject();
    }
    totalBytes += sizeof(LooperBankFileTrack) + undoBytes;
    if (totalBytes > kLooperBankFileLimit)
    {
        return reject();
    }

    for (int index = 0; index < apo::kLooperTrackCount; ++index)
    {
        const std::uint64_t samples = trackSamples[static_cast<size_t>(index)];
        std::vector<float>& buffer = bank.buffers[static_cast<size_t>(index)];
        if (samples == 0)
        {
            continue;
        }
        buffer.assign(static_cast<size_t>(samples), 0.0f);
        if (!readExact(buffer.data(),
                       static_cast<DWORD>(samples * sizeof(float))))
        {
            return reject();
        }
    }

    LooperBankFileTrack undoTrack{};
    if (!readExact(&undoTrack, sizeof(undoTrack)) ||
        undoTrack.state < static_cast<std::int32_t>(LooperState::Empty) ||
        undoTrack.state > static_cast<std::int32_t>(LooperState::Stopped) ||
        undoTrack.length != bank.undoLength)
    {
        return reject();
    }
    if (bank.undoValid && undoSamples != 0)
    {
        bank.undoBuffer.assign(static_cast<size_t>(undoSamples), 0.0f);
        if (!readExact(bank.undoBuffer.data(),
                       static_cast<DWORD>(undoSamples * sizeof(float))))
        {
            return reject();
        }
    }

    CloseHandle(file);
    bank.valid = true;
    return true;
}

} // namespace apo
