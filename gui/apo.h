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
inline constexpr wchar_t kEndpointProp[] =
    L"{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},7";
inline constexpr wchar_t kCompositeStreamProp[] =
    L"{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},13";
inline constexpr wchar_t kCompositeEndpointProp[] =
    L"{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},15";
inline constexpr int kCompositeFirstProp = 13;
inline constexpr int kCompositeLastProp = 20;
inline constexpr int kCompositeEndpointIndex = 15;
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
    bool endpointOurs = false;
    bool disableSysFx = false;

    // Composite effect packs replace the single FX values on some devices.
    // When present, Windows ignores the legacy PreMix slot.
    bool compositePresent = false;
    bool compositeOurs = false;
    bool compositeEndpointOurs = false;

    // Effect-pack devices use the composite endpoint chain because Windows
    // applies it even to raw streams. Legacy devices without an effect pack
    // use PreMix: Windows instantiates an APO in the single endpoint slot on
    // those endpoints but never locks it for streaming, even when the EFX
    // mode list contains only DEFAULT.
    bool BoundAtPreferredSlot() const
    {
        return compositePresent ? compositeEndpointOurs : premixOurs;
    }

    // Bindings written by older versions, including the single endpoint slot,
    // still count as enabled so Apply can move them to the preferred slot.
    bool Enabled() const
    {
        return BoundAtPreferredSlot() || compositeOurs || premixOurs ||
               streamOurs || endpointOurs;
    }

    // Per-device settings, loaded from ProgramData and edited by the GUI.
    ApoSettings settings;

    // Effect edits pending Apply.
    bool settingsDirty = false;
};

struct FilesStatus {
    bool dllPresent = false;
    bool dllUpToDate = false;
    bool comRegistered = false;
    bool apoKeyPresent = false;
    std::wstring registeredPath;

    // FNV-1a 64-bit hashes used by the hash comparison and shown in About.
    unsigned long long embeddedHash = 0;
    unsigned long long installedHash = 0;

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

    // Settings normally reach a running APO through the shared runtime file.
    // Reserved for operations that must reload state from disk; noise profiles
    // now reach the running stages through the stage capture channel.
    bool forceRestart = false;
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

// True when applying these changes requires the audio engine to reload the
// APO (binding/isolation edits, a forced reload, or a device whose live
// settings channel is not ready). Settings-only changes return false.
bool ChangesNeedRestart(const std::vector<Change>& changes);

// Blocking operations: run them on a worker thread.
Result InstallFiles();
Result UninstallFiles();
Result ApplyChanges(const std::vector<Change>& changes);

} // namespace apo
