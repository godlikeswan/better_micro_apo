#pragma once

#include <windows.h>
#include <string>
#include <vector>

#include "apo_settings.h"

namespace apo {

inline constexpr wchar_t kClsid[] = L"{6A8BE200-CD89-4E77-97F5-D51E8F16EF90}";
inline constexpr wchar_t kPreMixProp[] = L"{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},1";
inline constexpr wchar_t kStreamProp[] = L"{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},5";
inline constexpr wchar_t kDisableSysFxProp[] = L"{1da5d803-d492-4edd-8c23-e0c0ffee7f0e},5";
inline constexpr wchar_t kDeviceNameProp[] = L"{a45c254e-df1c-4efd-8020-67d146a850e0},2";
inline constexpr wchar_t kFriendlyNameProp[] = L"{b3f8fa53-0004-438e-9003-51a46e139bfc},6";

inline constexpr wchar_t kCaptureRoot[] =
    L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\MMDevices\\Audio\\Capture";
inline constexpr wchar_t kInstallDir[] = L"C:\\Program Files\\better_micro_apo";
inline constexpr wchar_t kInstalledDll[] =
    L"C:\\Program Files\\better_micro_apo\\better_micro_apo.dll";
inline constexpr wchar_t kDataDir[] = L"C:\\ProgramData\\better_micro_apo";
inline constexpr wchar_t kLogFile[] =
    L"C:\\ProgramData\\better_micro_apo\\better_micro_apo.log";
inline constexpr wchar_t kBackupDir[] = L"C:\\ProgramData\\better_micro_apo\\backup";

struct Endpoint {
    std::wstring id;    // {GUID}, as used in the MMDevices key
    std::wstring name;
    DWORD state = 0;
    bool premixOurs = false;
    bool streamOurs = false;
    bool disableSysFx = false;

    // Per-device settings, loaded from ProgramData and edited by the GUI.
    ApoSettings settings;
    bool settingsDirty = false;
};

struct FilesStatus {
    bool dllPresent = false;
    bool comRegistered = false;
    bool apoKeyPresent = false;
    std::wstring registeredPath;

    bool FullyInstalled() const
    {
        return dllPresent && comRegistered && apoKeyPresent;
    }

    bool AnyPresent() const
    {
        return dllPresent || comRegistered || apoKeyPresent ||
               !registeredPath.empty();
    }
};

struct Change {
    std::wstring id;
    bool enable = false;
    bool settingsChanged = false;
    ApoSettings settings;
};

struct Result {
    bool ok = true;
    std::wstring summary;
    std::wstring details;
};

bool IsDeviceActive(DWORD state);
std::wstring DeviceStateText(DWORD state);
std::wstring FormatWin32Error(DWORD error);
bool FileExists(const std::wstring& path);
std::wstring DefaultSourceDll();
bool IsElevated();

std::vector<Endpoint> EnumEndpoints();
FilesStatus GetFilesStatus();

// Blocking operations: run them on a worker thread.
Result InstallFiles(const std::wstring& sourceDll);
Result UninstallFiles();
Result ApplyChanges(const std::vector<Change>& changes);

} // namespace apo
