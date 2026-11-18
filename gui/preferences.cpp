#include "preferences.h"

#include "apo.h"
#include "apo_internal.h"

#include <windows.h>

#include <cwchar>
#include <string>

namespace gui {
namespace {

constexpr wchar_t kPreferencesFile[] =
    L"C:\\ProgramData\\better_micro_apo\\gui.ini";
constexpr wchar_t kShowAllKey[] = L"show_all_devices";
constexpr wchar_t kLastDeviceKey[] = L"last_device";

struct Preferences {
    bool showAllDevices = false;
    std::wstring lastDevice;
};

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

std::wstring ReadValue(const std::wstring& text, const wchar_t* key)
{
    size_t position = 0;
    while (position <= text.size()) {
        const size_t newline = text.find(L'\n', position);
        const std::wstring line =
            Trim(text.substr(position, newline == std::wstring::npos
                                           ? std::wstring::npos
                                           : newline - position));
        position = (newline == std::wstring::npos) ? text.size() + 1
                                                  : newline + 1;

        const size_t equals = line.find(L'=');
        if (equals == std::wstring::npos) {
            continue;
        }
        if (_wcsicmp(Trim(line.substr(0, equals)).c_str(), key) == 0) {
            return Trim(line.substr(equals + 1));
        }
    }
    return {};
}

bool ParseBool(const std::wstring& value)
{
    return value == L"1" || _wcsicmp(value.c_str(), L"true") == 0 ||
           _wcsicmp(value.c_str(), L"yes") == 0;
}

Preferences LoadPreferences()
{
    const std::wstring text = apo::detail::ReadTextFile(kPreferencesFile);
    Preferences preferences;
    preferences.showAllDevices = ParseBool(ReadValue(text, kShowAllKey));
    preferences.lastDevice = ReadValue(text, kLastDeviceKey);
    return preferences;
}

void SavePreferences(const Preferences& preferences)
{
    CreateDirectoryW(apo::kDataDir, nullptr);

    std::wstring text;
    text += L"[gui]\n";
    text += kShowAllKey;
    text += L"=";
    text += preferences.showAllDevices ? L"1" : L"0";
    text += L"\n";
    text += kLastDeviceKey;
    text += L"=";
    text += preferences.lastDevice;
    text += L"\n";
    apo::detail::WriteTextFile(kPreferencesFile, text);
}

} // namespace

bool LoadShowAllDevices()
{
    return LoadPreferences().showAllDevices;
}

void SaveShowAllDevices(bool showAll)
{
    Preferences preferences = LoadPreferences();
    preferences.showAllDevices = showAll;
    SavePreferences(preferences);
}

std::wstring LoadLastDevice()
{
    return LoadPreferences().lastDevice;
}

void SaveLastDevice(const std::wstring& id)
{
    Preferences preferences = LoadPreferences();
    preferences.lastDevice = id;
    SavePreferences(preferences);
}

} // namespace gui
