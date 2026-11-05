#include "apo.h"

#include "apo_internal.h"

#include <algorithm>
#include <cstdio>
#include <cwchar>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace apo {
namespace {

struct AudioSession {
    bool wasRunning = false;
    std::vector<std::wstring> stopped; // stop order: dependents first
};

template <class F>
class ScopeExit {
public:
    explicit ScopeExit(F fn) : fn_(std::move(fn)) {}
    ~ScopeExit() { fn_(); }
    ScopeExit(const ScopeExit&) = delete;
    ScopeExit& operator=(const ScopeExit&) = delete;

private:
    F fn_;
};

template <class F>
ScopeExit<F> MakeScopeExit(F fn)
{
    return ScopeExit<F>(std::move(fn));
}

bool QueryServiceState(SC_HANDLE service, DWORD& state)
{
    SERVICE_STATUS_PROCESS status{};
    DWORD needed = 0;
    if (!QueryServiceStatusEx(service, SC_STATUS_PROCESS_INFO,
                              reinterpret_cast<LPBYTE>(&status),
                              sizeof(status), &needed)) {
        return false;
    }
    state = status.dwCurrentState;
    return true;
}

bool WaitServiceState(SC_HANDLE service, DWORD desired, DWORD timeoutMs)
{
    const ULONGLONG start = GetTickCount64();
    for (;;) {
        DWORD state = 0;
        if (!QueryServiceState(service, state)) {
            return false;
        }
        if (state == desired) {
            return true;
        }
        if (GetTickCount64() - start > timeoutMs) {
            return false;
        }
        Sleep(100);
    }
}

void CollectDependents(SC_HANDLE scm, const std::wstring& name,
                       std::vector<std::wstring>& out)
{
    SC_HANDLE service = OpenServiceW(
        scm, name.c_str(),
        SERVICE_ENUMERATE_DEPENDENTS | SERVICE_QUERY_STATUS | SERVICE_STOP);
    if (!service) {
        return;
    }

    DWORD needed = 0;
    DWORD returned = 0;
    EnumDependentServicesW(service, SERVICE_ACTIVE, nullptr, 0, &needed,
                           &returned);
    if (needed != 0) {
        std::vector<BYTE> buffer(needed);
        if (EnumDependentServicesW(
                service, SERVICE_ACTIVE,
                reinterpret_cast<LPENUM_SERVICE_STATUSW>(buffer.data()), needed,
                &needed, &returned)) {
            auto* entries =
                reinterpret_cast<LPENUM_SERVICE_STATUSW>(buffer.data());
            for (DWORD i = 0; i < returned; ++i) {
                if (!entries[i].lpServiceName) {
                    continue;
                }
                const std::wstring dependent = entries[i].lpServiceName;
                if (std::find(out.begin(), out.end(), dependent) != out.end()) {
                    continue;
                }
                CollectDependents(scm, dependent, out);
                out.push_back(dependent);
            }
        }
    }
    CloseServiceHandle(service);
}

bool StopAudio(AudioSession& session, std::wstring& log)
{
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!scm) {
        log += L"Could not open the service manager: " +
               FormatWin32Error(GetLastError()) + L"\r\n";
        return false;
    }

    SC_HANDLE service = OpenServiceW(
        scm, L"Audiosrv",
        SERVICE_STOP | SERVICE_QUERY_STATUS | SERVICE_START |
            SERVICE_ENUMERATE_DEPENDENTS);
    if (!service) {
        log += L"Could not open Audiosrv: " + FormatWin32Error(GetLastError()) +
               L"\r\n";
        CloseServiceHandle(scm);
        return false;
    }

    DWORD state = 0;
    if (!QueryServiceState(service, state)) {
        log += L"Could not query Audiosrv: " +
               FormatWin32Error(GetLastError()) + L"\r\n";
        CloseServiceHandle(service);
        CloseServiceHandle(scm);
        return false;
    }
    if (state != SERVICE_RUNNING) {
        log += L"The audio service is not running.\r\n";
        CloseServiceHandle(service);
        CloseServiceHandle(scm);
        return true;
    }

    session.wasRunning = true;
    CollectDependents(scm, L"Audiosrv", session.stopped);

    for (const auto& name : session.stopped) {
        SC_HANDLE dependent = OpenServiceW(
            scm, name.c_str(), SERVICE_STOP | SERVICE_QUERY_STATUS);
        if (!dependent) {
            continue;
        }
        SERVICE_STATUS status{};
        ControlService(dependent, SERVICE_CONTROL_STOP, &status);
        if (!WaitServiceState(dependent, SERVICE_STOPPED, 30000)) {
            log += L"Warning: " + name + L" did not stop in time.\r\n";
        }
        CloseServiceHandle(dependent);
    }

    SERVICE_STATUS status{};
    if (!ControlService(service, SERVICE_CONTROL_STOP, &status)) {
        const DWORD error = GetLastError();
        if (error != ERROR_SERVICE_NOT_ACTIVE) {
            log += L"Could not stop Audiosrv: " +
                   FormatWin32Error(error) + L"\r\n";
            CloseServiceHandle(service);
            CloseServiceHandle(scm);
            return false;
        }
    }

    if (!WaitServiceState(service, SERVICE_STOPPED, 30000)) {
        log += L"Audiosrv did not stop in time.\r\n";
        CloseServiceHandle(service);
        CloseServiceHandle(scm);
        return false;
    }

    log += L"Stopped the audio service.\r\n";
    CloseServiceHandle(service);
    CloseServiceHandle(scm);
    return true;
}

void StartAudio(const AudioSession& session, std::wstring& log)
{
    if (!session.wasRunning) {
        return;
    }

    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!scm) {
        log += L"Could not open the service manager to restart audio.\r\n";
        return;
    }

    SC_HANDLE service =
        OpenServiceW(scm, L"Audiosrv", SERVICE_START | SERVICE_QUERY_STATUS);
    if (!service) {
        log += L"Could not open Audiosrv to restart it: " +
               FormatWin32Error(GetLastError()) + L"\r\n";
        CloseServiceHandle(scm);
        return;
    }

    DWORD state = 0;
    if (QueryServiceState(service, state) && state != SERVICE_RUNNING) {
        if (!StartServiceW(service, 0, nullptr)) {
            const DWORD error = GetLastError();
            if (error != ERROR_SERVICE_ALREADY_RUNNING) {
                log += L"Could not start Audiosrv: " +
                       FormatWin32Error(error) + L"\r\n";
            }
        }
        if (WaitServiceState(service, SERVICE_RUNNING, 30000)) {
            log += L"Started the audio service.\r\n";
        } else {
            log += L"Audiosrv did not start in time.\r\n";
        }
    }
    CloseServiceHandle(service);

    for (auto it = session.stopped.rbegin(); it != session.stopped.rend();
         ++it) {
        SC_HANDLE dependent = OpenServiceW(
            scm, it->c_str(), SERVICE_START | SERVICE_QUERY_STATUS);
        if (!dependent) {
            continue;
        }

        DWORD dependentState = 0;
        if (QueryServiceState(dependent, dependentState) &&
            dependentState != SERVICE_RUNNING) {
            if (StartServiceW(dependent, 0, nullptr) ||
                GetLastError() == ERROR_SERVICE_ALREADY_RUNNING) {
                if (!WaitServiceState(dependent, SERVICE_RUNNING, 15000)) {
                    log += L"Warning: " + *it +
                           L" did not start in time.\r\n";
                }
            } else {
                log += L"Warning: could not start " + *it + L": " +
                       FormatWin32Error(GetLastError()) + L"\r\n";
            }
        }
        CloseServiceHandle(dependent);
    }

    CloseServiceHandle(scm);
}

bool CopyWithRetry(const std::wstring& source,
                   const std::wstring& destination, std::wstring& log)
{
    for (int attempt = 0; attempt < 20; ++attempt) {
        if (CopyFileW(source.c_str(), destination.c_str(), FALSE)) {
            return true;
        }
        if (attempt == 19) {
            log += L"Could not copy the DLL: " +
                   FormatWin32Error(GetLastError()) + L"\r\n";
            return false;
        }
        Sleep(250);
    }
    return false;
}

std::wstring FormatHresult(HRESULT hr)
{
    wchar_t buffer[32]{};
    swprintf_s(buffer, L"0x%08X", static_cast<unsigned>(hr));
    return buffer;
}

bool RegisterDll(const std::wstring& dll, std::wstring& log)
{
    HMODULE module =
        LoadLibraryExW(dll.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!module) {
        log += L"Could not load " + dll + L": " +
               FormatWin32Error(GetLastError()) + L"\r\n";
        return false;
    }

    using RegisterFn = HRESULT(WINAPI*)();
    auto fn = reinterpret_cast<RegisterFn>(
        GetProcAddress(module, "DllRegisterServer"));
    if (!fn) {
        log += L"DllRegisterServer is missing from the DLL.\r\n";
        FreeLibrary(module);
        return false;
    }

    const HRESULT hr = fn();
    FreeLibrary(module);
    if (FAILED(hr)) {
        log += L"DllRegisterServer failed: " + FormatHresult(hr) + L"\r\n";
        return false;
    }

    log += L"Registered the COM class.\r\n";
    return true;
}

bool UnregisterDll(const std::wstring& dll, std::wstring& log)
{
    HMODULE module =
        LoadLibraryExW(dll.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!module) {
        log += L"Could not load " + dll + L": " +
               FormatWin32Error(GetLastError()) + L"\r\n";
        return false;
    }

    using UnregisterFn = HRESULT(WINAPI*)();
    auto fn = reinterpret_cast<UnregisterFn>(
        GetProcAddress(module, "DllUnregisterServer"));
    if (!fn) {
        log += L"DllUnregisterServer is missing from the DLL.\r\n";
        FreeLibrary(module);
        return false;
    }

    const HRESULT hr = fn();
    FreeLibrary(module);
    if (FAILED(hr)) {
        log += L"DllUnregisterServer failed: " + FormatHresult(hr) + L"\r\n";
        return false;
    }

    log += L"Unregistered the COM class.\r\n";
    return true;
}

std::wstring BackupPrefix(const std::wstring& id)
{
    return std::wstring(kBackupDir) + L"\\FxProperties-" +
           detail::SafeId(id) + L"-";
}

std::vector<std::filesystem::path> FindBackups(const std::wstring& id)
{
    std::vector<std::filesystem::path> files;
    std::error_code ec;
    std::filesystem::directory_iterator it(kBackupDir, ec);
    if (ec) {
        return files;
    }

    const std::wstring prefix = BackupPrefix(id);
    for (const auto& entry : it) {
        if (!entry.is_regular_file(ec)) {
            continue;
        }
        const std::wstring name = entry.path().filename().wstring();
        if (name.size() <= prefix.size()) {
            continue;
        }
        if (_wcsnicmp(name.c_str(), prefix.c_str(), prefix.size()) != 0) {
            continue;
        }
        if (_wcsicmp(entry.path().extension().wstring().c_str(), L".reg") !=
            0) {
            continue;
        }
        files.push_back(entry.path());
    }

    std::sort(files.begin(), files.end(), [](const auto& a, const auto& b) {
        std::error_code ea;
        std::error_code eb;
        const auto ta = std::filesystem::last_write_time(a, ea);
        const auto tb = std::filesystem::last_write_time(b, eb);
        return ta > tb;
    });
    return files;
}

bool CreateBackup(const Endpoint& endpoint, std::wstring& file,
                  std::wstring& log)
{
    std::error_code ec;
    std::filesystem::create_directories(kBackupDir, ec);
    if (ec) {
        log += L"Could not create " + std::wstring(kBackupDir) + L": " +
               FormatWin32Error(static_cast<DWORD>(ec.value())) + L"\r\n";
        return false;
    }

    std::wstring oldValue;
    const bool existed = detail::RegReadString(
        HKEY_LOCAL_MACHINE, detail::FxSub(endpoint.id), kPreMixProp, oldValue);

    const std::wstring stamp = detail::Timestamp();
    file = BackupPrefix(endpoint.id) + stamp + L".reg";
    for (int suffix = 1;
         std::filesystem::exists(file, ec) && suffix < 1000; ++suffix) {
        file = BackupPrefix(endpoint.id) + stamp + L"-" +
               std::to_wstring(suffix) + L".reg";
    }

    const std::wstring command =
        detail::Quote(detail::System32Path(L"reg.exe")) + L" export " +
        detail::Quote(std::wstring(L"HKLM\\") +
                      detail::FxSub(endpoint.id)) +
        L" " + detail::Quote(file) + L" /y";

    std::wstring output;
    DWORD exitCode = 0;
    if (!detail::RunProcess(command, output, exitCode) || exitCode != 0) {
        log += L"Could not back up " + endpoint.name +
               L" (reg export exit " + std::to_wstring(exitCode) + L").\r\n";
        if (!output.empty()) {
            log += output + L"\r\n";
        }
        return false;
    }

    std::wstring sidecar = file;
    sidecar.resize(sidecar.size() - 4);
    sidecar += L".txt";
    const std::wstring text =
        L"endpoint   = " + endpoint.id + L"\r\n" +
        L"property   = " + std::wstring(kPreMixProp) + L"\r\n" +
        L"existed    = " + (existed ? L"True" : L"False") + L"\r\n" +
        L"oldValue   = " + oldValue + L"\r\n" +
        L"newValue   = " + std::wstring(kClsid) + L"\r\n";
    detail::WriteTextFile(sidecar, text);

    log += L"Backed up " + endpoint.name + L" to " + file + L"\r\n";
    return true;
}

bool RestoreBackup(const Endpoint& endpoint, std::wstring& log)
{
    const std::wstring fx = detail::FxSub(endpoint.id);
    const auto backups = FindBackups(endpoint.id);

    std::wstring chosen;
    std::wstring text;
    for (const auto& path : backups) {
        text = detail::ReadTextFile(path.wstring());
        if (text.find(kClsid) == std::wstring::npos) {
            chosen = path.wstring();
            break;
        }
    }
    if (chosen.empty() && !backups.empty()) {
        chosen = backups.front().wstring();
        text = detail::ReadTextFile(chosen);
        log += L"Every backup for " + endpoint.name +
               L" already contains the APO CLSID; using the newest.\r\n";
    }

    bool wrote = false;
    if (!chosen.empty()) {
        std::wstring value;
        if (detail::ExtractRegString(text, kPreMixProp, value)) {
            if (_wcsicmp(value.c_str(), kClsid) == 0) {
                log += L"The backup for " + endpoint.name +
                       L" points at this APO; removing the slot instead.\r\n";
                wrote = detail::RegDeleteValue(HKEY_LOCAL_MACHINE, fx,
                                               kPreMixProp);
            } else {
                wrote = detail::RegWriteString(HKEY_LOCAL_MACHINE, fx,
                                               kPreMixProp, value);
            }
        } else {
            wrote =
                detail::RegDeleteValue(HKEY_LOCAL_MACHINE, fx, kPreMixProp);
        }
    } else {
        log += L"No backup for " + endpoint.name +
               L"; removing the slot.\r\n";
        wrote = detail::RegDeleteValue(HKEY_LOCAL_MACHINE, fx, kPreMixProp);
    }

    std::wstring current;
    if (detail::RegReadString(HKEY_LOCAL_MACHINE, fx, kPreMixProp, current) &&
        _wcsicmp(current.c_str(), kClsid) == 0) {
        log += L"Could not remove the APO from " + endpoint.name + L".\r\n";
        return false;
    }

    log += wrote ? L"Disabled on " + endpoint.name + L".\r\n"
                 : L"Slot already clear on " + endpoint.name + L".\r\n";
    return true;
}

} // namespace

Result InstallFiles(const std::wstring& sourceDll)
{
    Result result;
    std::wstring log;

    if (!FileExists(sourceDll)) {
        result.ok = false;
        result.summary = L"Source DLL not found.";
        result.details = sourceDll;
        return result;
    }

    AudioSession session;
    if (!StopAudio(session, log)) {
        result.ok = false;
        result.summary = L"Could not stop the audio service.";
        result.details = log;
        return result;
    }
    auto resume = MakeScopeExit([&] { StartAudio(session, log); });

    std::error_code ec;
    std::filesystem::create_directories(kInstallDir, ec);
    if (ec) {
        log += L"Could not create " + std::wstring(kInstallDir) + L": " +
               FormatWin32Error(static_cast<DWORD>(ec.value())) + L"\r\n";
        result.ok = false;
        result.summary = L"Could not create the install directory.";
        result.details = log;
        return result;
    }

    if (_wcsicmp(sourceDll.c_str(), kInstalledDll) != 0) {
        if (!CopyWithRetry(sourceDll, kInstalledDll, log)) {
            result.ok = false;
            result.summary = L"Could not copy the DLL.";
            result.details = log;
            return result;
        }
        log += L"Copied the DLL to " + std::wstring(kInstalledDll) + L"\r\n";
    }

    std::filesystem::create_directories(kDataDir, ec);
    if (ec) {
        log += L"Warning: could not create " + std::wstring(kDataDir) +
               L"\r\n";
    } else {
        detail::GrantLocalServiceModify(kDataDir, log);

        // Create the settings directory after the ACL grant so it inherits
        // the LocalService access the APO needs to read its files.
        std::error_code settingsEc;
        std::filesystem::create_directories(SettingsDirectory(), settingsEc);
        if (settingsEc) {
            log += L"Warning: could not create " + SettingsDirectory() +
                   L"\r\n";
        }
    }

    if (!RegisterDll(kInstalledDll, log)) {
        result.ok = false;
        result.summary = L"Could not register the APO.";
        result.details = log;
        return result;
    }

    const FilesStatus status = GetFilesStatus();
    if (!status.FullyInstalled()) {
        log += L"Registration check failed after install.\r\n";
        result.ok = false;
        result.summary = L"Install did not complete.";
        result.details = log;
        return result;
    }

    result.ok = true;
    result.summary = L"Installed.";
    result.details = log;
    return result;
}

Result UninstallFiles()
{
    Result result;
    std::wstring log;

    AudioSession session;
    if (!StopAudio(session, log)) {
        result.ok = false;
        result.summary = L"Could not stop the audio service.";
        result.details = log;
        return result;
    }
    auto resume = MakeScopeExit([&] { StartAudio(session, log); });

    // Re-enumerate instead of trusting the caller's snapshot: a device may
    // have been bound or removed since the status was last refreshed.
    for (const auto& endpoint : EnumEndpoints()) {
        if (endpoint.premixOurs || endpoint.streamOurs) {
            if (!RestoreBackup(endpoint, log)) {
                result.ok = false;
            }
        }
        if (endpoint.streamOurs &&
            !detail::RegDeleteValue(HKEY_LOCAL_MACHINE,
                                    detail::FxSub(endpoint.id), kStreamProp)) {
            log += L"Could not remove the Stream slot from " + endpoint.name +
                   L".\r\n";
            result.ok = false;
        }
    }

    if (FileExists(kInstalledDll)) {
        if (!UnregisterDll(kInstalledDll, log)) {
            log += L"DllUnregisterServer failed; deleting the keys "
                   L"directly.\r\n";
        }
    }

    detail::DeleteKeyTree(
        HKEY_LOCAL_MACHINE,
        std::wstring(L"SOFTWARE\\Classes\\CLSID\\") + kClsid);
    detail::DeleteKeyTree(
        HKEY_LOCAL_MACHINE,
        std::wstring(L"SOFTWARE\\Classes\\AudioEngine\\"
                     L"AudioProcessingObjects\\") +
            kClsid);

    std::error_code ec;
    for (int attempt = 0; attempt < 20; ++attempt) {
        std::filesystem::remove_all(kInstallDir, ec);
        if (!ec && !std::filesystem::exists(kInstallDir, ec)) {
            break;
        }
        Sleep(250);
    }

    if (std::filesystem::exists(kInstallDir, ec)) {
        log += L"Could not delete " + std::wstring(kInstallDir) + L"\r\n";
        result.ok = false;
    } else {
        log += L"Deleted " + std::wstring(kInstallDir) + L"\r\n";
    }

    const FilesStatus status = GetFilesStatus();
    if (status.AnyPresent()) {
        log += L"Some APO registration is still present.\r\n";
        result.ok = false;
    }

    result.summary = result.ok ? L"Uninstalled." : L"Uninstall incomplete.";
    result.details = log;
    return result;
}

Result ApplyChanges(const std::vector<Change>& changes)
{
    Result result;
    std::wstring log;

    const auto endpoints = EnumEndpoints();
    AudioSession session;
    if (!StopAudio(session, log)) {
        result.ok = false;
        result.summary = L"Could not stop the audio service.";
        result.details = log;
        return result;
    }
    auto resume = MakeScopeExit([&] { StartAudio(session, log); });

    for (const auto& change : changes) {
        const auto it = std::find_if(
            endpoints.begin(), endpoints.end(), [&](const Endpoint& endpoint) {
                return _wcsicmp(endpoint.id.c_str(), change.id.c_str()) == 0;
            });
        if (it == endpoints.end()) {
            log += L"Device not found: " + change.id + L"\r\n";
            result.ok = false;
            continue;
        }

        if (change.settingsChanged) {
            if (SaveSettings(it->id, change.settings)) {
                log += L"Saved settings for " + it->name + L".\r\n";
            } else {
                log += L"Could not save settings for " + it->name + L".\r\n";
                result.ok = false;
            }
        }

        if (change.enable == it->premixOurs) {
            continue;
        }

        if (change.enable) {
            std::wstring backup;
            if (!CreateBackup(*it, backup, log)) {
                result.ok = false;
                continue;
            }

            if (!detail::RegWriteString(HKEY_LOCAL_MACHINE,
                                        detail::FxSub(it->id), kPreMixProp,
                                        kClsid)) {
                log += L"Could not write the slot for " + it->name + L".\r\n";
                result.ok = false;
                continue;
            }

            if (it->disableSysFx) {
                if (detail::RegWriteDword(HKEY_LOCAL_MACHINE,
                                          detail::FxSub(it->id),
                                          kDisableSysFxProp, 0)) {
                    log += L"Re-enabled system effects on " + it->name +
                           L".\r\n";
                } else {
                    log += L"Warning: could not clear the system-effects "
                           L"flag on " +
                           it->name + L".\r\n";
                }
            }

            log += L"Enabled on " + it->name + L".\r\n";
        } else {
            if (!RestoreBackup(*it, log)) {
                result.ok = false;
            }
        }
    }

    result.summary = result.ok ? L"Applied." : L"Applied with errors.";
    result.details = log;
    return result;
}

} // namespace apo
