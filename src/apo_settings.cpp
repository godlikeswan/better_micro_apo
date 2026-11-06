#include "apo_settings.h"

#include <windows.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <string>

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
std::wstring SanitizeId(const std::wstring& id)
{
    std::wstring safe;
    safe.reserve(id.size());
    for (wchar_t ch : id) {
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

namespace {

void ClampFinite(double& value, double fallback, double low, double high)
{
    if (!std::isfinite(value)) {
        value = fallback;
    }
    value = Clamp(value, low, high);
}

} // namespace

void ClampSettings(ApoSettings& settings)
{
    const ApoSettings defaults;

    ClampFinite(settings.gate.thresholdDb, defaults.gate.thresholdDb, -80.0,
                0.0);
    ClampFinite(settings.gate.rangeDb, defaults.gate.rangeDb, 0.0, 96.0);
    ClampFinite(settings.gate.attackMs, defaults.gate.attackMs, 0.1, 1000.0);
    ClampFinite(settings.gate.holdMs, defaults.gate.holdMs, 0.0, 2000.0);
    ClampFinite(settings.gate.releaseMs, defaults.gate.releaseMs, 1.0, 5000.0);
    ClampFinite(settings.gate.lookaheadMs, defaults.gate.lookaheadMs, 0.0,
                300.0);

    ClampFinite(settings.compressor.thresholdDb,
                defaults.compressor.thresholdDb, -60.0, 0.0);
    ClampFinite(settings.compressor.ratio, defaults.compressor.ratio, 1.0,
                20.0);
    ClampFinite(settings.compressor.attackMs, defaults.compressor.attackMs,
                0.1, 1000.0);
    ClampFinite(settings.compressor.releaseMs, defaults.compressor.releaseMs,
                1.0, 5000.0);

    ClampFinite(settings.makeupDb, defaults.makeupDb, -24.0, 48.0);
}

std::wstring SettingsDirectory()
{
    return kDevicesDirectory;
}

std::wstring SettingsPath(const std::wstring& endpointId)
{
    return SettingsDirectory() + L"\\" + SanitizeId(endpointId) + L".ini";
}

bool LoadSettings(const std::wstring& endpointId, ApoSettings& settings)
{
    settings = ApoSettings{};

    std::string data;
    if (!ReadFileText(SettingsPath(endpointId), data)) {
        return false;
    }

    // Older files used a separate input gain stage. If we see it, fold its
    // value into the compressor threshold and makeup gain so the sound is
    // preserved after the stage was removed.
    bool hasLegacyInputGain = false;
    double legacyInputGainDb = 0.0;

    const std::wstring text = Utf8ToWide(data);
    std::wstring section;
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
        const std::wstring key = Trim(line.substr(0, equals));
        const std::wstring value = Trim(line.substr(equals + 1));

        if (EqualsIgnoreCase(key, L"input_gain_db")) {
            if (ParseDouble(value, legacyInputGainDb)) {
                hasLegacyInputGain = true;
            }
        } else if (EqualsIgnoreCase(key, L"gate_enabled")) {
            ParseBool(value, settings.gate.enabled);
        } else if (EqualsIgnoreCase(key, L"gate_threshold_db")) {
            ParseDouble(value, settings.gate.thresholdDb);
        } else if (EqualsIgnoreCase(key, L"gate_range_db")) {
            ParseDouble(value, settings.gate.rangeDb);
        } else if (EqualsIgnoreCase(key, L"gate_attack_ms")) {
            ParseDouble(value, settings.gate.attackMs);
        } else if (EqualsIgnoreCase(key, L"gate_hold_ms")) {
            ParseDouble(value, settings.gate.holdMs);
        } else if (EqualsIgnoreCase(key, L"gate_release_ms")) {
            ParseDouble(value, settings.gate.releaseMs);
        } else if (EqualsIgnoreCase(key, L"gate_lookahead_ms")) {
            ParseDouble(value, settings.gate.lookaheadMs);
        } else if (EqualsIgnoreCase(key, L"compressor_enabled")) {
            ParseBool(value, settings.compressor.enabled);
        } else if (EqualsIgnoreCase(key, L"compressor_threshold_db") ||
                   EqualsIgnoreCase(key, L"threshold_db")) {
            ParseDouble(value, settings.compressor.thresholdDb);
        } else if (EqualsIgnoreCase(key, L"compressor_ratio") ||
                   EqualsIgnoreCase(key, L"ratio")) {
            ParseDouble(value, settings.compressor.ratio);
        } else if (EqualsIgnoreCase(key, L"compressor_attack_ms") ||
                   EqualsIgnoreCase(key, L"attack_ms")) {
            ParseDouble(value, settings.compressor.attackMs);
        } else if (EqualsIgnoreCase(key, L"compressor_release_ms") ||
                   EqualsIgnoreCase(key, L"release_ms")) {
            ParseDouble(value, settings.compressor.releaseMs);
        } else if (EqualsIgnoreCase(key, L"makeup_db")) {
            ParseDouble(value, settings.makeupDb);
        } else if (EqualsIgnoreCase(key, L"mono")) {
            ParseBool(value, settings.mono);
        }
    }

    if (hasLegacyInputGain) {
        settings.compressor.thresholdDb -= legacyInputGainDb;
        settings.makeupDb += legacyInputGainDb;
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
    text += std::string("gate_enabled=") +
            (settings.gate.enabled ? "1" : "0") + "\n";
    text += "gate_threshold_db=" + FormatDouble(settings.gate.thresholdDb) +
            "\n";
    text += "gate_range_db=" + FormatDouble(settings.gate.rangeDb) + "\n";
    text += "gate_attack_ms=" + FormatDouble(settings.gate.attackMs) + "\n";
    text += "gate_hold_ms=" + FormatDouble(settings.gate.holdMs) + "\n";
    text += "gate_release_ms=" + FormatDouble(settings.gate.releaseMs) + "\n";
    text += "gate_lookahead_ms=" + FormatDouble(settings.gate.lookaheadMs) +
            "\n";
    text += std::string("compressor_enabled=") +
            (settings.compressor.enabled ? "1" : "0") + "\n";
    text += "compressor_threshold_db=" +
            FormatDouble(settings.compressor.thresholdDb) + "\n";
    text += "compressor_ratio=" + FormatDouble(settings.compressor.ratio) +
            "\n";
    text += "compressor_attack_ms=" +
            FormatDouble(settings.compressor.attackMs) + "\n";
    text += "compressor_release_ms=" +
            FormatDouble(settings.compressor.releaseMs) + "\n";
    text += "makeup_db=" + FormatDouble(settings.makeupDb) + "\n";
    text += std::string("mono=") + (settings.mono ? "1" : "0") + "\n";

    return WriteFileText(SettingsPath(endpointId), text);
}

} // namespace apo
