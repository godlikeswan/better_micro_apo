#include "apo.h"

#include "apo_internal.h"

#include <algorithm>
#include <iterator>

namespace apo {
namespace {

bool HasCompositeFx(const std::wstring& fx)
{
    // Any composite effect position means the driver uses effect packs. The
    // composite keys use the same property GUID as the single FX keys.
    for (int index = kCompositeFirstProp; index <= kCompositeLastProp;
         ++index) {
        const std::wstring name =
            std::wstring(kFxPropBase) + L"," + std::to_wstring(index);
        std::vector<std::wstring> values;
        if (detail::RegReadMultiString(HKEY_LOCAL_MACHINE, fx, name.c_str(),
                                       values) &&
            !values.empty()) {
            return true;
        }
    }
    return false;
}

bool HasCompositeExtraFx(const std::wstring& fx)
{
    // Everything after the stream chain is an effect that is not this APO.
    for (int index = kCompositeFirstProp + 1; index <= kCompositeLastProp;
         ++index) {
        const std::wstring name =
            std::wstring(kFxPropBase) + L"," + std::to_wstring(index);
        std::vector<std::wstring> values;
        if (detail::RegReadMultiString(HKEY_LOCAL_MACHINE, fx, name.c_str(),
                                       values) &&
            !values.empty()) {
            return true;
        }
    }
    return false;
}

bool ContainsClsid(const std::vector<std::wstring>& values,
                   const wchar_t* clsid)
{
    return std::any_of(values.begin(), values.end(),
                       [&](const std::wstring& value) {
                           return _wcsicmp(value.c_str(), clsid) == 0;
                       });
}

} // namespace

bool IsDeviceActive(DWORD state)
{
    return (state & 1) != 0;
}

std::wstring DeviceStateText(DWORD state)
{
    if (state & 1) {
        return L"Active";
    }
    if (state & 2) {
        return L"Disabled";
    }
    if (state & 8) {
        return L"Unplugged";
    }
    if (state & 4) {
        return L"Not present";
    }
    return L"Unknown";
}

std::wstring FormatWin32Error(DWORD error)
{
    wchar_t* buffer = nullptr;
    const DWORD length = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
            FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, error, 0, reinterpret_cast<LPWSTR>(&buffer), 0, nullptr);

    std::wstring text;
    if (length != 0 && buffer != nullptr) {
        text.assign(buffer, length);
    }
    if (buffer != nullptr) {
        LocalFree(buffer);
    }

    while (!text.empty() &&
           (text.back() == L'\r' || text.back() == L'\n' ||
            text.back() == L' ')) {
        text.pop_back();
    }
    if (text.empty()) {
        text = L"error " + std::to_wstring(error);
    }
    return text;
}

bool FileExists(const std::wstring& path)
{
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES &&
           (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

bool IsElevated()
{
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
        return false;
    }

    TOKEN_ELEVATION elevation{};
    DWORD size = sizeof(elevation);
    const BOOL ok = GetTokenInformation(token, TokenElevation, &elevation,
                                        size, &size);
    CloseHandle(token);
    return ok && elevation.TokenIsElevated != 0;
}

std::vector<Endpoint> EnumEndpoints()
{
    std::vector<Endpoint> endpoints;

    HKEY root = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, kCaptureRoot, 0,
                      KEY_READ | KEY_WOW64_64KEY,
                      &root) != ERROR_SUCCESS) {
        return endpoints;
    }

    for (DWORD index = 0;; ++index) {
        wchar_t id[256]{};
        DWORD length = static_cast<DWORD>(std::size(id) - 1);
        const LSTATUS status =
            RegEnumKeyExW(root, index, id, &length, nullptr, nullptr, nullptr,
                          nullptr);
        if (status == ERROR_NO_MORE_ITEMS) {
            break;
        }
        if (status != ERROR_SUCCESS) {
            continue;
        }

        Endpoint endpoint;
        endpoint.id = id;
        LoadSettings(endpoint.id, endpoint.settings);
        const std::wstring base =
            std::wstring(kCaptureRoot) + L"\\" + endpoint.id;

        detail::RegReadDword(HKEY_LOCAL_MACHINE, base, L"DeviceState",
                             endpoint.state);

        const std::wstring properties = base + L"\\Properties";
        if (!detail::RegReadString(HKEY_LOCAL_MACHINE, properties,
                                   kDeviceNameProp, endpoint.name) ||
            endpoint.name.empty()) {
            if (!detail::RegReadString(HKEY_LOCAL_MACHINE, properties,
                                       kFriendlyNameProp, endpoint.name) ||
                endpoint.name.empty()) {
                endpoint.name = endpoint.id;
            }
        }

        const std::wstring fx = base + L"\\FxProperties";
        std::wstring value;
        if (detail::RegReadString(HKEY_LOCAL_MACHINE, fx, kPreMixProp,
                                  value)) {
            endpoint.premixOurs = _wcsicmp(value.c_str(), kClsid) == 0;
        }
        if (detail::RegReadString(HKEY_LOCAL_MACHINE, fx, kStreamProp,
                                  value)) {
            endpoint.streamOurs = _wcsicmp(value.c_str(), kClsid) == 0;
        }

        std::vector<std::wstring> composite;
        if (detail::RegReadMultiString(HKEY_LOCAL_MACHINE, fx,
                                       kCompositeStreamProp, composite)) {
            endpoint.compositeOurs = ContainsClsid(composite, kClsid);
        }
        endpoint.compositePresent = HasCompositeFx(fx);
        endpoint.isolatePendingCleanup =
            endpoint.Enabled() && endpoint.compositePresent &&
            endpoint.settings.isolateEffects && HasCompositeExtraFx(fx);

        DWORD disable = 0;
        if (detail::RegReadDword(HKEY_LOCAL_MACHINE, fx, kDisableSysFxProp,
                                 disable)) {
            endpoint.disableSysFx = disable != 0;
        }

        endpoints.push_back(std::move(endpoint));
    }

    RegCloseKey(root);

    std::sort(endpoints.begin(), endpoints.end(),
              [](const Endpoint& a, const Endpoint& b) {
                  const int cmp = _wcsicmp(a.name.c_str(), b.name.c_str());
                  return cmp < 0;
              });
    return endpoints;
}

FilesStatus GetFilesStatus()
{
    FilesStatus status;
    status.dllPresent = FileExists(kInstalledDll);

    const std::wstring inproc = std::wstring(L"SOFTWARE\\Classes\\CLSID\\") +
                                kClsid + L"\\InprocServer32";
    if (detail::RegReadString(HKEY_LOCAL_MACHINE, inproc, nullptr,
                              status.registeredPath)) {
        status.comRegistered =
            _wcsicmp(status.registeredPath.c_str(), kInstalledDll) == 0;
    }

    const std::wstring apoKey =
        std::wstring(L"SOFTWARE\\Classes\\AudioEngine\\"
                     L"AudioProcessingObjects\\") +
        kClsid;
    status.apoKeyPresent =
        detail::RegKeyExists(HKEY_LOCAL_MACHINE, apoKey);
    return status;
}

} // namespace apo
