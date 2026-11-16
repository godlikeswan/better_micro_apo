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
    case EffectType::Noise:      return L"Noise reduction";
    case EffectType::Gate:       return L"Gate";
    case EffectType::Dynamic:    return L"Dynamic";
    case EffectType::Compressor: return L"Compressor";
    case EffectType::Gain:       return L"Gain";
    case EffectType::Mono:       return L"Mono";
    case EffectType::Distortion: return L"Distortion";
    case EffectType::Reverb:     return L"Reverb";
    default:                     return L"";
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
    case EffectType::Noise:      return "noise";
    case EffectType::Gate:       return "gate";
    case EffectType::Dynamic:    return "dynamic";
    case EffectType::Compressor: return "compressor";
    case EffectType::Gain:       return "gain";
    case EffectType::Mono:       return "mono";
    case EffectType::Distortion: return "distortion";
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
    case EffectType::Gate:
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
    case EffectType::Noise:      return effect.noise.enabled;
    case EffectType::Gate:       return effect.gate.enabled;
    case EffectType::Dynamic:    return effect.dynamics.enabled;
    case EffectType::Compressor: return effect.compressor.enabled;
    case EffectType::Gain:       return effect.gain.enabled;
    case EffectType::Mono:       return effect.mono.enabled;
    case EffectType::Distortion: return effect.distortion.enabled;
    case EffectType::Reverb:     return effect.reverb.enabled;
    default:                     return false;
    }
}

void SetEffectEnabled(EffectInstance& effect, bool enabled)
{
    switch (effect.type)
    {
    case EffectType::Noise:      effect.noise.enabled = enabled; break;
    case EffectType::Gate:       effect.gate.enabled = enabled; break;
    case EffectType::Dynamic:    effect.dynamics.enabled = enabled; break;
    case EffectType::Compressor: effect.compressor.enabled = enabled; break;
    case EffectType::Gain:       effect.gain.enabled = enabled; break;
    case EffectType::Mono:       effect.mono.enabled = enabled; break;
    case EffectType::Distortion: effect.distortion.enabled = enabled; break;
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

void ClampEffect(EffectInstance& effect)
{
    switch (effect.type)
    {
    case EffectType::Noise:
        ClampFinite(effect.noise.reductionDb, 12.0, 0.0, 70.0);
        break;

    case EffectType::Gate:
        ClampFinite(effect.gate.thresholdDb, -50.0, -80.0, 0.0);
        ClampFinite(effect.gate.rangeDb, 60.0, 0.0, 96.0);
        ClampFinite(effect.gate.attackMs, 5.0, 0.1, 1000.0);
        ClampFinite(effect.gate.holdMs, 50.0, 0.0, 2000.0);
        ClampFinite(effect.gate.releaseMs, 100.0, 1.0, 5000.0);
        ClampFinite(effect.gate.lookaheadMs, 0.0, 0.0, 300.0);
        break;

    case EffectType::Dynamic:
        ClampFinite(effect.dynamics.frequencyHz, 120.0, 20.0, 2000.0);
        ClampFinite(effect.dynamics.q, 0.7, 0.1, 10.0);
        ClampFinite(effect.dynamics.thresholdDb, -45.0, -80.0, 0.0);
        ClampFinite(effect.dynamics.ratio, 4.0, 1.0, 20.0);
        ClampFinite(effect.dynamics.rangeDb, 18.0, 0.0, 36.0);
        ClampFinite(effect.dynamics.attackMs, 10.0, 0.1, 1000.0);
        ClampFinite(effect.dynamics.releaseMs, 150.0, 1.0, 5000.0);
        break;

    case EffectType::Compressor:
        ClampFinite(effect.compressor.thresholdDb, -58.0, -60.0, 0.0);
        ClampFinite(effect.compressor.ratio, 3.0, 1.0, 20.0);
        ClampFinite(effect.compressor.attackMs, 5.0, 0.1, 1000.0);
        ClampFinite(effect.compressor.releaseMs, 100.0, 1.0, 5000.0);
        break;

    case EffectType::Gain:
        ClampFinite(effect.gain.gainDb, 0.0, -24.0, 48.0);
        break;

    case EffectType::Mono:
        break;

    case EffectType::Distortion:
        if (effect.distortion.function < 0 ||
            effect.distortion.function >= kDistortionFunctionCount) {
            effect.distortion.function = kDistortionTanh;
        }
        ClampFinite(effect.distortion.driveDb, 0.0, 0.0, 36.0);
        ClampFinite(effect.distortion.curvature, 50.0, 0.0, 100.0);
        ClampFinite(effect.distortion.outputDb, 0.0, -24.0, 12.0);
        break;

    case EffectType::Reverb:
        ClampFinite(effect.reverb.mix, 20.0, 0.0, 100.0);
        ClampFinite(effect.reverb.size, 50.0, 0.0, 100.0);
        ClampFinite(effect.reverb.damping, 50.0, 0.0, 100.0);
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

    getBool(L"enabled", effect.noise.enabled);
    getBool(L"enabled", effect.gate.enabled);
    getBool(L"enabled", effect.dynamics.enabled);
    getBool(L"enabled", effect.compressor.enabled);
    getBool(L"enabled", effect.gain.enabled);
    getBool(L"enabled", effect.mono.enabled);
    getBool(L"enabled", effect.distortion.enabled);
    getBool(L"enabled", effect.reverb.enabled);

    switch (effect.type)
    {
    case EffectType::Noise:
        getDouble(L"noise_reduction_db", effect.noise.reductionDb);
        break;

    case EffectType::Gate:
        getDouble(L"gate_threshold_db", effect.gate.thresholdDb);
        getDouble(L"gate_range_db", effect.gate.rangeDb);
        getDouble(L"gate_attack_ms", effect.gate.attackMs);
        getDouble(L"gate_hold_ms", effect.gate.holdMs);
        getDouble(L"gate_release_ms", effect.gate.releaseMs);
        getDouble(L"gate_lookahead_ms", effect.gate.lookaheadMs);
        break;

    case EffectType::Dynamic:
        getBool(L"dynamic_expand", effect.dynamics.expand);
        getDouble(L"dynamic_frequency_hz", effect.dynamics.frequencyHz);
        getDouble(L"dynamic_q", effect.dynamics.q);
        getDouble(L"dynamic_threshold_db", effect.dynamics.thresholdDb);
        getDouble(L"dynamic_ratio", effect.dynamics.ratio);
        getDouble(L"dynamic_range_db", effect.dynamics.rangeDb);
        getDouble(L"dynamic_attack_ms", effect.dynamics.attackMs);
        getDouble(L"dynamic_release_ms", effect.dynamics.releaseMs);
        break;

    case EffectType::Compressor:
        getDouble(L"compressor_threshold_db", effect.compressor.thresholdDb);
        getDouble(L"compressor_ratio", effect.compressor.ratio);
        getDouble(L"compressor_attack_ms", effect.compressor.attackMs);
        getDouble(L"compressor_release_ms", effect.compressor.releaseMs);
        break;

    case EffectType::Gain:
        getDouble(L"gain_db", effect.gain.gainDb);
        break;

    case EffectType::Mono:
        break;

    case EffectType::Distortion:
        getInt(L"distortion_function", effect.distortion.function);
        getDouble(L"distortion_drive_db", effect.distortion.driveDb);
        getDouble(L"distortion_curvature", effect.distortion.curvature);
        getDouble(L"distortion_output_db", effect.distortion.outputDb);
        break;

    case EffectType::Reverb:
        getDouble(L"reverb_mix", effect.reverb.mix);
        getDouble(L"reverb_size", effect.reverb.size);
        getDouble(L"reverb_damping", effect.reverb.damping);
        getDouble(L"reverb_predelay_ms", effect.reverb.predelayMs);
        break;

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

    // Files older than version 4 kept a single effect list. They load as four
    // empty presets; only the isolation binding option is carried over.
    if (const std::wstring* isolateText =
            FindValue(entries, L"isolate_effects")) {
        ParseBool(*isolateText, settings.isolateEffects);
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
    text += "version=4\n";
    text += "preset_count=" + std::to_string(kPresetCount) + "\n";
    text += "preset_index=" + std::to_string(settings.presetIndex) + "\n";
    text += std::string("isolate_effects=") +
            (settings.isolateEffects ? "1" : "0") + "\n";

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
            case EffectType::Noise:
                text += prefix + "noise_reduction_db=" +
                        FormatDouble(effect.noise.reductionDb) + "\n";
                break;

            case EffectType::Gate:
                text += prefix + "gate_threshold_db=" +
                        FormatDouble(effect.gate.thresholdDb) + "\n";
                text += prefix + "gate_range_db=" +
                        FormatDouble(effect.gate.rangeDb) + "\n";
                text += prefix + "gate_attack_ms=" +
                        FormatDouble(effect.gate.attackMs) + "\n";
                text += prefix + "gate_hold_ms=" +
                        FormatDouble(effect.gate.holdMs) + "\n";
                text += prefix + "gate_release_ms=" +
                        FormatDouble(effect.gate.releaseMs) + "\n";
                text += prefix + "gate_lookahead_ms=" +
                        FormatDouble(effect.gate.lookaheadMs) + "\n";
                break;

            case EffectType::Dynamic:
                text += prefix + "dynamic_expand=" +
                        (effect.dynamics.expand ? "1" : "0") + "\n";
                text += prefix + "dynamic_frequency_hz=" +
                        FormatDouble(effect.dynamics.frequencyHz) + "\n";
                text += prefix + "dynamic_q=" +
                        FormatDouble(effect.dynamics.q) + "\n";
                text += prefix + "dynamic_threshold_db=" +
                        FormatDouble(effect.dynamics.thresholdDb) + "\n";
                text += prefix + "dynamic_ratio=" +
                        FormatDouble(effect.dynamics.ratio) + "\n";
                text += prefix + "dynamic_range_db=" +
                        FormatDouble(effect.dynamics.rangeDb) + "\n";
                text += prefix + "dynamic_attack_ms=" +
                        FormatDouble(effect.dynamics.attackMs) + "\n";
                text += prefix + "dynamic_release_ms=" +
                        FormatDouble(effect.dynamics.releaseMs) + "\n";
                break;

            case EffectType::Compressor:
                text += prefix + "compressor_threshold_db=" +
                        FormatDouble(effect.compressor.thresholdDb) + "\n";
                text += prefix + "compressor_ratio=" +
                        FormatDouble(effect.compressor.ratio) + "\n";
                text += prefix + "compressor_attack_ms=" +
                        FormatDouble(effect.compressor.attackMs) + "\n";
                text += prefix + "compressor_release_ms=" +
                        FormatDouble(effect.compressor.releaseMs) + "\n";
                break;

            case EffectType::Gain:
                text += prefix + "gain_db=" +
                        FormatDouble(effect.gain.gainDb) + "\n";
                break;

            case EffectType::Mono:
                break;

            case EffectType::Distortion:
                text += prefix + "distortion_function=" +
                        std::to_string(effect.distortion.function) + "\n";
                text += prefix + "distortion_drive_db=" +
                        FormatDouble(effect.distortion.driveDb) + "\n";
                text += prefix + "distortion_curvature=" +
                        FormatDouble(effect.distortion.curvature) + "\n";
                text += prefix + "distortion_output_db=" +
                        FormatDouble(effect.distortion.outputDb) + "\n";
                break;

            case EffectType::Reverb:
                text += prefix + "reverb_mix=" +
                        FormatDouble(effect.reverb.mix) + "\n";
                text += prefix + "reverb_size=" +
                        FormatDouble(effect.reverb.size) + "\n";
                text += prefix + "reverb_damping=" +
                        FormatDouble(effect.reverb.damping) + "\n";
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
#pragma pack(pop)

constexpr std::uint32_t kNoiseFileVersion = 1;
constexpr std::uint64_t kNoiseFileLimit = 64ull * 1024 * 1024;

bool ValidNoiseHeader(const NoiseFileHeader& header)
{
    return std::memcmp(header.magic, "BMNP", 4) == 0 &&
           header.version == kNoiseFileVersion && header.sampleRate > 0 &&
           header.channels > 0 &&
           header.frames >= apo::dsp::kNoiseFftSize;
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

    NoiseFileHeader header{};
    DWORD read = 0;
    bool ok = ReadFile(file, &header, sizeof(header), &read, nullptr) &&
              read == sizeof(header) && ValidNoiseHeader(header);

    if (ok)
    {
        const std::uint64_t bytes64 =
            static_cast<std::uint64_t>(header.frames) * header.channels *
            sizeof(float);
        ok = bytes64 <= kNoiseFileLimit;
        if (ok)
        {
            std::vector<float> samples(
                static_cast<size_t>(header.frames) * header.channels);
            const DWORD bytes = static_cast<DWORD>(bytes64);
            ok = ReadFile(file, samples.data(), bytes, &read, nullptr) &&
                 read == bytes;
            if (ok)
            {
                profile.magnitudes = apo::dsp::AverageMagnitudeSpectrum(
                    samples.data(), header.frames, header.channels);
                profile.sampleRate = header.sampleRate;
                profile.valid = !profile.magnitudes.empty();
                ok = profile.valid;
            }
        }
    }

    CloseHandle(file);
    return ok;
}

} // namespace apo
