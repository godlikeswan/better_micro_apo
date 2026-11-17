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

} // namespace

bool LoadShowAllDevices()
{
    const std::wstring text = apo::detail::ReadTextFile(kPreferencesFile);

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
        if (_wcsicmp(Trim(line.substr(0, equals)).c_str(),
                     L"show_all_devices") != 0) {
            continue;
        }

        const std::wstring value = Trim(line.substr(equals + 1));
        return value == L"1" || _wcsicmp(value.c_str(), L"true") == 0 ||
               _wcsicmp(value.c_str(), L"yes") == 0;
    }

    return false;
}

void SaveShowAllDevices(bool showAll)
{
    CreateDirectoryW(apo::kDataDir, nullptr);

    std::wstring text;
    text += L"[gui]\n";
    text += L"show_all_devices=";
    text += showAll ? L"1" : L"0";
    text += L"\n";
    apo::detail::WriteTextFile(kPreferencesFile, text);
}

} // namespace gui
