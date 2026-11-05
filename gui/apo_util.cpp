#include "apo_internal.h"

#include "apo.h"

#include <aclapi.h>
#include <sddl.h>

#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <vector>

namespace apo::detail {
namespace {

bool RegOpenForWrite(HKEY root, const std::wstring& sub, HKEY& key)
{
    return RegOpenKeyExW(root, sub.c_str(), 0,
                         KEY_SET_VALUE | KEY_WOW64_64KEY,
                         &key) == ERROR_SUCCESS;
}

} // namespace

bool RegReadString(HKEY root, const std::wstring& sub, const wchar_t* name,
                   std::wstring& out)
{
    DWORD size = 0;
    LSTATUS status = RegGetValueW(root, sub.c_str(), name, RRF_RT_REG_SZ,
                                  nullptr, nullptr, &size);
    if (status != ERROR_SUCCESS || size < sizeof(wchar_t)) {
        return false;
    }

    std::wstring value(size / sizeof(wchar_t), L'\0');
    status = RegGetValueW(root, sub.c_str(), name, RRF_RT_REG_SZ, nullptr,
                          value.data(), &size);
    if (status != ERROR_SUCCESS) {
        return false;
    }

    value.resize(wcslen(value.c_str()));
    out = std::move(value);
    return true;
}

bool RegReadDword(HKEY root, const std::wstring& sub, const wchar_t* name,
                  DWORD& out)
{
    DWORD value = 0;
    DWORD size = sizeof(value);
    const LSTATUS status = RegGetValueW(root, sub.c_str(), name,
                                        RRF_RT_REG_DWORD, nullptr, &value,
                                        &size);
    if (status != ERROR_SUCCESS) {
        return false;
    }
    out = value;
    return true;
}

bool RegKeyExists(HKEY root, const std::wstring& sub)
{
    HKEY key = nullptr;
    const LSTATUS status = RegOpenKeyExW(root, sub.c_str(), 0,
                                         KEY_READ | KEY_WOW64_64KEY, &key);
    if (status != ERROR_SUCCESS) {
        return false;
    }
    RegCloseKey(key);
    return true;
}

bool RegWriteString(HKEY root, const std::wstring& sub, const wchar_t* name,
                    const std::wstring& value)
{
    HKEY key = nullptr;
    if (!RegOpenForWrite(root, sub, key)) {
        return false;
    }

    const LSTATUS status = RegSetValueExW(
        key, name, 0, REG_SZ,
        reinterpret_cast<const BYTE*>(value.c_str()),
        static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(key);
    return status == ERROR_SUCCESS;
}

bool RegWriteDword(HKEY root, const std::wstring& sub, const wchar_t* name,
                   DWORD value)
{
    HKEY key = nullptr;
    if (!RegOpenForWrite(root, sub, key)) {
        return false;
    }

    const LSTATUS status = RegSetValueExW(
        key, name, 0, REG_DWORD, reinterpret_cast<const BYTE*>(&value),
        sizeof(value));
    RegCloseKey(key);
    return status == ERROR_SUCCESS;
}

bool RegDeleteValue(HKEY root, const std::wstring& sub, const wchar_t* name)
{
    HKEY key = nullptr;
    if (!RegOpenForWrite(root, sub, key)) {
        return false;
    }

    const LSTATUS status = RegDeleteValueW(key, name);
    RegCloseKey(key);
    return status == ERROR_SUCCESS || status == ERROR_FILE_NOT_FOUND;
}

void DeleteKeyTree(HKEY root, const std::wstring& sub)
{
    RegDeleteTreeW(root, sub.c_str());
    RegDeleteKeyExW(root, sub.c_str(), KEY_WOW64_64KEY, 0);
}

std::wstring FxSub(const std::wstring& id)
{
    return std::wstring(kCaptureRoot) + L"\\" + id + L"\\FxProperties";
}

std::wstring SafeId(const std::wstring& id)
{
    std::wstring safe = id;
    if (!safe.empty() && safe.front() == L'{') {
        safe.erase(safe.begin());
    }
    if (!safe.empty() && safe.back() == L'}') {
        safe.pop_back();
    }
    return safe;
}

std::wstring System32Path(const wchar_t* name)
{
    wchar_t dir[MAX_PATH]{};
    const UINT length = GetSystemDirectoryW(dir, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) {
        return name;
    }
    return std::wstring(dir, length) + L"\\" + name;
}

std::wstring Quote(const std::wstring& s)
{
    return L"\"" + s + L"\"";
}

std::wstring Timestamp()
{
    SYSTEMTIME st{};
    GetLocalTime(&st);
    wchar_t buffer[32]{};
    swprintf_s(buffer, L"%04u%02u%02u-%02u%02u%02u",
               static_cast<unsigned>(st.wYear),
               static_cast<unsigned>(st.wMonth),
               static_cast<unsigned>(st.wDay),
               static_cast<unsigned>(st.wHour),
               static_cast<unsigned>(st.wMinute),
               static_cast<unsigned>(st.wSecond));
    return buffer;
}

std::wstring ReadTextFile(const std::wstring& path)
{
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return {};
    }

    std::vector<char> data((std::istreambuf_iterator<char>(in)),
                           std::istreambuf_iterator<char>());
    if (data.size() >= 2 &&
        static_cast<unsigned char>(data[0]) == 0xFF &&
        static_cast<unsigned char>(data[1]) == 0xFE) {
        const size_t count = (data.size() - 2) / sizeof(wchar_t);
        std::wstring text(count, L'\0');
        memcpy(text.data(), data.data() + 2, count * sizeof(wchar_t));
        return text;
    }

    if (data.size() >= 2 &&
        static_cast<unsigned char>(data[0]) == 0xFE &&
        static_cast<unsigned char>(data[1]) == 0xFF) {
        std::wstring text;
        for (size_t i = 2; i + 1 < data.size(); i += 2) {
            text.push_back(static_cast<wchar_t>(
                (static_cast<unsigned char>(data[i]) << 8) |
                static_cast<unsigned char>(data[i + 1])));
        }
        return text;
    }

    size_t offset = 0;
    UINT codepage = CP_ACP;
    if (data.size() >= 3 &&
        static_cast<unsigned char>(data[0]) == 0xEF &&
        static_cast<unsigned char>(data[1]) == 0xBB &&
        static_cast<unsigned char>(data[2]) == 0xBF) {
        offset = 3;
        codepage = CP_UTF8;
    }
    if (data.size() <= offset) {
        return {};
    }

    const int length = MultiByteToWideChar(
        codepage, 0, data.data() + offset,
        static_cast<int>(data.size() - offset), nullptr, 0);
    std::wstring text(length, L'\0');
    MultiByteToWideChar(codepage, 0, data.data() + offset,
                        static_cast<int>(data.size() - offset), text.data(),
                        length);
    return text;
}

bool WriteTextFile(const std::wstring& path, const std::wstring& text)
{
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        return false;
    }

    const unsigned char bom[3] = {0xEF, 0xBB, 0xBF};
    out.write(reinterpret_cast<const char*>(bom), sizeof(bom));

    const int length = WideCharToMultiByte(
        CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0,
        nullptr, nullptr);
    std::string utf8(length, '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.c_str(),
                        static_cast<int>(text.size()), utf8.data(), length,
                        nullptr, nullptr);
    out.write(utf8.data(), utf8.size());
    return out.good();
}

bool RunProcess(const std::wstring& command, std::wstring& output,
                DWORD& exitCode)
{
    wchar_t tempDir[MAX_PATH]{};
    const DWORD tempLength = GetTempPathW(MAX_PATH, tempDir);
    if (tempLength == 0) {
        return false;
    }

    const std::wstring outPath =
        std::wstring(tempDir) + L"better_micro_apo_gui_" +
        std::to_wstring(GetCurrentProcessId()) + L".tmp";

    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    HANDLE out = CreateFileW(outPath.c_str(), GENERIC_WRITE, FILE_SHARE_READ,
                             &sa, CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY,
                             nullptr);
    if (out == INVALID_HANDLE_VALUE) {
        return false;
    }

    HANDLE in = CreateFileW(L"NUL", GENERIC_READ,
                            FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
                            OPEN_EXISTING, 0, nullptr);

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = out;
    si.hStdError = out;
    si.hStdInput = in;

    PROCESS_INFORMATION pi{};
    std::wstring mutableCommand = command;
    const BOOL started = CreateProcessW(
        nullptr, mutableCommand.data(), nullptr, nullptr, TRUE,
        CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);

    if (!started) {
        CloseHandle(out);
        if (in != INVALID_HANDLE_VALUE) {
            CloseHandle(in);
        }
        DeleteFileW(outPath.c_str());
        return false;
    }

    WaitForSingleObject(pi.hProcess, INFINITE);
    GetExitCodeProcess(pi.hProcess, &exitCode);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    CloseHandle(out);
    if (in != INVALID_HANDLE_VALUE) {
        CloseHandle(in);
    }

    output = ReadTextFile(outPath);
    DeleteFileW(outPath.c_str());
    return true;
}

bool GrantLocalServiceModify(const std::wstring& dir, std::wstring& log)
{
    PSID sid = nullptr;
    if (!ConvertStringSidToSidW(L"S-1-5-19", &sid)) {
        log += L"Warning: could not resolve the LocalService SID: " +
               apo::FormatWin32Error(GetLastError()) + L"\r\n";
        return false;
    }

    PACL oldAcl = nullptr;
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    DWORD status = GetNamedSecurityInfoW(
        const_cast<LPWSTR>(dir.c_str()), SE_FILE_OBJECT,
        DACL_SECURITY_INFORMATION, nullptr, nullptr, &oldAcl, nullptr,
        &descriptor);
    if (status != ERROR_SUCCESS) {
        LocalFree(sid);
        log += L"Warning: could not read the ACL of " + dir + L": " +
               apo::FormatWin32Error(status) + L"\r\n";
        return false;
    }

    EXPLICIT_ACCESSW access{};
    access.grfAccessPermissions =
        FILE_GENERIC_READ | FILE_GENERIC_WRITE | FILE_GENERIC_EXECUTE | DELETE;
    access.grfAccessMode = GRANT_ACCESS;
    access.grfInheritance = SUB_CONTAINERS_AND_OBJECTS_INHERIT;
    access.Trustee.TrusteeForm = TRUSTEE_IS_SID;
    access.Trustee.TrusteeType = TRUSTEE_IS_WELL_KNOWN_GROUP;
    access.Trustee.ptstrName = reinterpret_cast<LPWSTR>(sid);

    PACL newAcl = nullptr;
    status = SetEntriesInAclW(1, &access, oldAcl, &newAcl);
    if (status == ERROR_SUCCESS) {
        status = SetNamedSecurityInfoW(
            const_cast<LPWSTR>(dir.c_str()), SE_FILE_OBJECT,
            DACL_SECURITY_INFORMATION, nullptr, nullptr, newAcl, nullptr);
    }

    if (newAcl) {
        LocalFree(newAcl);
    }
    if (descriptor) {
        LocalFree(descriptor);
    }
    LocalFree(sid);

    if (status != ERROR_SUCCESS) {
        log += L"Warning: could not grant LocalService access to " + dir +
               L": " + apo::FormatWin32Error(status) + L"\r\n";
        return false;
    }
    return true;
}

bool ExtractRegString(const std::wstring& text, const std::wstring& name,
                      std::wstring& value)
{
    const std::wstring needle = L"\"" + name + L"\"=";
    size_t pos = text.find(needle);
    if (pos == std::wstring::npos) {
        return false;
    }

    pos += needle.size();
    if (pos >= text.size() || text[pos] != L'"') {
        return false;
    }

    const size_t end = text.find(L'"', pos + 1);
    if (end == std::wstring::npos) {
        return false;
    }

    value = text.substr(pos + 1, end - pos - 1);
    return true;
}

} // namespace apo::detail
