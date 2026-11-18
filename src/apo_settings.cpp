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
    // Infinity is only meaningful for the dynamics ratio. Other fields accept
    // it syntactically and ClampFinite replaces it with their default.
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
    case EffectType::Gain:       return L"Gain";
    case EffectType::Noise:      return L"Noise reduction";
    case EffectType::Dynamics:   return L"Gate/Compressor/Limiter/Expander";
    case EffectType::Equalizer:  return L"Equalizer";
    case EffectType::Mono:       return L"Mono";
    case EffectType::Saturation: return L"Saturation/Overdrive/Distortion";
    case EffectType::Reverb:     return L"Reverb";
    default:                     return L"";
    }
}

const wchar_t* DynamicsModeName(DynamicsMode mode)
{
    switch (mode)
    {
    case DynamicsMode::CompressorLimiter: return L"Compressor/Limiter";
    case DynamicsMode::ExpanderGate:      return L"Expander/Gate";
    default:                              return L"";
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

const char* EffectTypeKey(EffectType type)
{
    switch (type)
    {
    case EffectType::Gain:       return "gain";
    case EffectType::Noise:      return "noise";
    case EffectType::Dynamics:   return "dynamics";
    case EffectType::Equalizer:  return "equalizer";
    case EffectType::Mono:       return "mono";
    case EffectType::Saturation: return "saturation";
    case EffectType::Reverb:     return "reverb";
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
    case EffectType::Noise:
    case EffectType::Dynamics:
    case EffectType::Reverb:
        return kMaxHeavyEffects;
    default:
        return kMaxEffects;
    }
}

bool EffectEnabled(const EffectInstance& effect)
{
    switch (effect.type)
    {
    case EffectType::Gain:       return effect.gain.enabled;
    case EffectType::Noise:      return effect.noise.enabled;
    case EffectType::Dynamics:   return effect.dynamics.enabled;
    case EffectType::Equalizer:  return effect.equalizer.enabled;
    case EffectType::Mono:       return effect.mono.enabled;
    case EffectType::Saturation: return effect.distortion.enabled;
    case EffectType::Reverb:     return effect.reverb.enabled;
    default:                     return false;
    }
}

void SetEffectEnabled(EffectInstance& effect, bool enabled)
{
    switch (effect.type)
    {
    case EffectType::Gain:       effect.gain.enabled = enabled; break;
    case EffectType::Noise:      effect.noise.enabled = enabled; break;
    case EffectType::Dynamics:   effect.dynamics.enabled = enabled; break;
    case EffectType::Equalizer:  effect.equalizer.enabled = enabled; break;
    case EffectType::Mono:       effect.mono.enabled = enabled; break;
    case EffectType::Saturation: effect.distortion.enabled = enabled; break;
    case EffectType::Reverb:     effect.reverb.enabled = enabled; break;
    default:                     break;
    }
}

EffectInstance MakeEffect(EffectType type, int id)
{
    EffectInstance effect;
    effect.id = id;
    effect.type = type;
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

void ClampEffect(EffectInstance& effect)
{
    switch (effect.type)
    {
    case EffectType::Gain:
        ClampFinite(effect.gain.gainDb, 12.0, -24.0, 50.0);
        break;

    case EffectType::Noise:
        ClampFinite(effect.noise.reductionDb, 12.0, 0.0, 50.0);
        effect.noise.fftSize = ValidNoiseFftSize(effect.noise.fftSize);
        break;

    case EffectType::Dynamics:
        if (effect.dynamics.mode < DynamicsMode::CompressorLimiter ||
            effect.dynamics.mode >= DynamicsMode::Count) {
            effect.dynamics.mode = DynamicsMode::CompressorLimiter;
        }
        ClampFinite(effect.dynamics.thresholdDb, -15.0, -80.0, 0.0);
        ClampRatio(effect.dynamics.ratio);
        ClampFinite(effect.dynamics.inputDb, 0.0, -24.0, 30.0);
        ClampFinite(effect.dynamics.outputDb, 0.0, -24.0, 24.0);
        ClampFinite(effect.dynamics.attackMs, 5.0, 0.1, 1000.0);
        ClampFinite(effect.dynamics.releaseMs, 100.0, 1.0, 5000.0);
        ClampFinite(effect.dynamics.holdMs, 50.0, 0.0, 2000.0);
        ClampFinite(effect.dynamics.lookaheadMs, 0.0, 0.0,
                    kDynamicsMaxLookaheadMs);
        break;

    case EffectType::Equalizer:
        ClampFinite(effect.equalizer.frequencyHz, 120.0, 20.0, 2000.0);
        ClampFinite(effect.equalizer.q, 0.7, 0.1, 10.0);
        ClampFinite(effect.equalizer.thresholdDb, -45.0, -80.0, 0.0);
        ClampFinite(effect.equalizer.ratio, 4.0, 1.0, 20.0);
        ClampFinite(effect.equalizer.rangeDb, 18.0, 0.0, 36.0);
        ClampFinite(effect.equalizer.attackMs, 10.0, 0.1, 1000.0);
        ClampFinite(effect.equalizer.releaseMs, 150.0, 1.0, 5000.0);
        break;

    case EffectType::Mono:
        break;

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

        for (int i = 0; i < preset.effectCount; ++i) {
            if (static_cast<int>(preset.effects[i].type) < 0 ||
                preset.effects[i].type >= EffectType::Count) {
                preset.effects[i].type = EffectType::Gain;
            }
            ClampEffect(preset.effects[i]);
        }

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

    getBool(L"enabled", effect.gain.enabled);
    getBool(L"enabled", effect.noise.enabled);
    getBool(L"enabled", effect.dynamics.enabled);
    getBool(L"enabled", effect.equalizer.enabled);
    getBool(L"enabled", effect.mono.enabled);
    getBool(L"enabled", effect.distortion.enabled);
    getBool(L"enabled", effect.reverb.enabled);

    switch (effect.type)
    {
    case EffectType::Gain:
        getDouble(L"gain_db", effect.gain.gainDb);
        break;

    case EffectType::Noise:
        getDouble(L"noise_reduction_db", effect.noise.reductionDb);
        getInt(L"noise_fft_size", effect.noise.fftSize);
        break;

    case EffectType::Dynamics: {
        int mode = static_cast<int>(DynamicsMode::CompressorLimiter);
        getInt(L"dynamics_mode", mode);
        effect.dynamics.mode = static_cast<DynamicsMode>(mode);
        getDouble(L"dynamics_threshold_db", effect.dynamics.thresholdDb);
        getDouble(L"dynamics_ratio", effect.dynamics.ratio);
        getDouble(L"dynamics_input_db", effect.dynamics.inputDb);
        getDouble(L"dynamics_output_db", effect.dynamics.outputDb);
        getDouble(L"dynamics_attack_ms", effect.dynamics.attackMs);
        getDouble(L"dynamics_release_ms", effect.dynamics.releaseMs);
        getDouble(L"dynamics_hold_ms", effect.dynamics.holdMs);
        getDouble(L"dynamics_lookahead_ms", effect.dynamics.lookaheadMs);
        break;
    }

    case EffectType::Equalizer:
        getBool(L"equalizer_expand", effect.equalizer.expand);
        getDouble(L"equalizer_frequency_hz", effect.equalizer.frequencyHz);
        getDouble(L"equalizer_q", effect.equalizer.q);
        getDouble(L"equalizer_threshold_db", effect.equalizer.thresholdDb);
        getDouble(L"equalizer_ratio", effect.equalizer.ratio);
        getDouble(L"equalizer_range_db", effect.equalizer.rangeDb);
        getDouble(L"equalizer_attack_ms", effect.equalizer.attackMs);
        getDouble(L"equalizer_release_ms", effect.equalizer.releaseMs);
        break;

    case EffectType::Mono:
        break;

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

    default:
        break;
    }
}

} // namespace

bool LoadSettings(const std::wstring& endpointId, ApoSettings& settings)
{
    settings = ApoSettings{};

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

            case EffectType::Dynamics:
                text += prefix + "dynamics_mode=" +
                        std::to_string(static_cast<int>(effect.dynamics.mode)) +
                        "\n";
                text += prefix + "dynamics_threshold_db=" +
                        FormatDouble(effect.dynamics.thresholdDb) + "\n";
                text += prefix + "dynamics_ratio=" +
                        FormatDouble(effect.dynamics.ratio) + "\n";
                text += prefix + "dynamics_input_db=" +
                        FormatDouble(effect.dynamics.inputDb) + "\n";
                text += prefix + "dynamics_output_db=" +
                        FormatDouble(effect.dynamics.outputDb) + "\n";
                text += prefix + "dynamics_attack_ms=" +
                        FormatDouble(effect.dynamics.attackMs) + "\n";
                text += prefix + "dynamics_release_ms=" +
                        FormatDouble(effect.dynamics.releaseMs) + "\n";
                text += prefix + "dynamics_hold_ms=" +
                        FormatDouble(effect.dynamics.holdMs) + "\n";
                text += prefix + "dynamics_lookahead_ms=" +
                        FormatDouble(effect.dynamics.lookaheadMs) + "\n";
                break;

            case EffectType::Equalizer:
                text += prefix + "equalizer_expand=" +
                        (effect.equalizer.expand ? "1" : "0") + "\n";
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
                text += prefix + "equalizer_attack_ms=" +
                        FormatDouble(effect.equalizer.attackMs) + "\n";
                text += prefix + "equalizer_release_ms=" +
                        FormatDouble(effect.equalizer.releaseMs) + "\n";
                break;

            case EffectType::Mono:
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

} // namespace apo
