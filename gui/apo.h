#pragma once

#include <windows.h>
#include <string>
#include <vector>

#include "apo_settings.h"

namespace apo {

inline constexpr wchar_t kClsid[] = L"{6A8BE200-CD89-4E77-97F5-D51E8F16EF90}";
inline constexpr wchar_t kFxPropBase[] =
    L"{d04e05a6-594b-4fb6-a80d-01af5eed7d1d}";
inline constexpr wchar_t kPreMixProp[] = L"{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},1";
inline constexpr wchar_t kStreamProp[] = L"{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},5";
inline constexpr wchar_t kCompositeStreamProp[] =
    L"{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},13";
inline constexpr int kCompositeFirstProp = 13;
inline constexpr int kCompositeLastProp = 20;
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

    // Composite effect packs replace the single FX values on some devices.
    // When present, Windows ignores the legacy PreMix slot.
    bool compositePresent = false;
    bool compositeOurs = false;

    // The saved isolate option is on, the device is bound, and some other
    // composite effect chain is still present. Apply can clean this up without
    // requiring the user to toggle the checkbox.
    bool isolatePendingCleanup = false;

    bool Enabled() const
    {
        return compositePresent ? compositeOurs : premixOurs;
    }

    // Per-device settings, loaded from ProgramData and edited by the GUI.
    ApoSettings settings;

    // Effect edits pending Apply. Kept separate from the binding/isolation
    // edits so the Devices and Effects views can track their own dirty state.
    bool settingsDirty = false;

    // Persisted isolation value, plus the GUI-only pending edit. The APO
    // itself only reads isolateEffects; the GUI uses these to decide whether
    // the Devices Apply button has work to do.
    bool isolateSaved = false;
    bool isolationDirty = false;
};

struct FilesStatus {
    bool dllPresent = false;
    bool dllUpToDate = false;
    bool comRegistered = false;
    bool apoKeyPresent = false;
    std::wstring registeredPath;

    // The installed DLL differs from the copy embedded in this executable, so
    // the user should run Install again to update it.
    bool DllStale() const { return dllPresent && !dllUpToDate; }

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
bool IsElevated();

std::vector<Endpoint> EnumEndpoints();
FilesStatus GetFilesStatus();

// Blocking operations: run them on a worker thread.
Result InstallFiles();
Result UninstallFiles();
Result ApplyChanges(const std::vector<Change>& changes);

} // namespace apo
