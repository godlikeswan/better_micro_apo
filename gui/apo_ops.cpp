#include "apo.h"

#include "apo_internal.h"
#include "apo_resource.h"
#include "runtime_settings.h"

#include <algorithm>
#include <cstdio>
#include <cwchar>
#include <filesystem>
#include <iterator>
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

bool ExtractEmbeddedDll(const std::wstring& destination, std::wstring& log)
{
    const HRSRC resource =
        FindResourceW(nullptr, MAKEINTRESOURCEW(IDR_APO_DLL), RT_RCDATA);
    if (resource == nullptr) {
        log += L"The embedded APO DLL is missing from this build.\r\n";
        return false;
    }

    const DWORD size = SizeofResource(nullptr, resource);
    const HGLOBAL handle = LoadResource(nullptr, resource);
    const void* data = handle ? LockResource(handle) : nullptr;
    if (data == nullptr || size == 0) {
        log += L"Could not read the embedded APO DLL.\r\n";
        return false;
    }

    // Write the payload to a temporary file first, then copy it into place
    // with retries: audiodg can keep the installed DLL open for a moment
    // after the audio service stops.
    wchar_t tempDirectory[MAX_PATH]{};
    wchar_t tempFile[MAX_PATH]{};
    if (GetTempPathW(static_cast<DWORD>(std::size(tempDirectory)),
                     tempDirectory) == 0 ||
        GetTempFileNameW(tempDirectory, L"bma", 0, tempFile) == 0) {
        log += L"Could not create a temporary file: " +
               FormatWin32Error(GetLastError()) + L"\r\n";
        return false;
    }

    bool written = false;
    const HANDLE file =
        CreateFileW(tempFile, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                    FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        log += L"Could not write the temporary DLL: " +
               FormatWin32Error(GetLastError()) + L"\r\n";
    } else {
        DWORD count = 0;
        written = WriteFile(file, data, size, &count, nullptr) != FALSE &&
                  count == size;
        if (!written) {
            log += L"Could not write the temporary DLL: " +
                   FormatWin32Error(GetLastError()) + L"\r\n";
        }
        CloseHandle(file);
    }

    bool ok = false;
    if (written) {
        ok = CopyWithRetry(tempFile, destination, log);
    }
    DeleteFileW(tempFile);
    return ok;
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

// Filename prefix of the timestamped backups older builds wrote. It is
// compared against directory entry names, so it must not include the folder.
std::wstring BackupPrefix(const std::wstring& id)
{
    return L"FxProperties-" + detail::SafeId(id) + L"-";
}

bool ContainsClsid(const std::vector<std::wstring>& values,
                   const wchar_t* clsid)
{
    return std::any_of(values.begin(), values.end(),
                       [&](const std::wstring& value) {
                           return _wcsicmp(value.c_str(), clsid) == 0;
                       });
}

void RemoveClsid(std::vector<std::wstring>& values, const wchar_t* clsid)
{
    values.erase(std::remove_if(values.begin(), values.end(),
                                [&](const std::wstring& value) {
                                    return _wcsicmp(value.c_str(), clsid) == 0;
                                }),
                 values.end());
}

std::wstring JoinValues(const std::vector<std::wstring>& values)
{
    std::wstring text;
    for (const std::wstring& value : values) {
        if (!text.empty()) {
            text += L"|";
        }
        text += value;
    }
    return text;
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
        if (ta != tb) {
            return ta > tb;
        }
        return a.filename().wstring() > b.filename().wstring();
    });
    return files;
}

std::wstring CompositeProp(int index)
{
    return std::wstring(kFxPropBase) + L"," + std::to_wstring(index);
}

// Exactly one backup per endpoint lives at backup\FxProperties-{id}.reg.
// Superseded and already-restored files are moved to backup\old_backups, so
// the next bind records the restored driver state instead of accumulating
// snapshots of our own changes.
std::wstring BackupBase(const std::wstring& id)
{
    return std::wstring(kBackupDir) + L"\\FxProperties-" + detail::SafeId(id);
}

std::filesystem::path CanonicalBackup(const std::wstring& id)
{
    return std::filesystem::path(BackupBase(id) + L".reg");
}

std::wstring SidecarPath(const std::filesystem::path& reg)
{
    std::wstring sidecar = reg.wstring();
    sidecar.resize(sidecar.size() - 4);
    sidecar += L".txt";
    return sidecar;
}

enum class BackupKind {
    Driver,   // holds the driver's own chain
    Empty,    // no chain at all
    OursOnly, // this APO is the whole chain; cannot restore anything
};

BackupKind ClassifyBackup(const std::wstring& text)
{
    std::vector<std::wstring> stream;
    if (!detail::ExtractRegMultiString(text, kCompositeStreamProp, stream)) {
        return BackupKind::Empty;
    }
    if (!ContainsClsid(stream, kClsid)) {
        return BackupKind::Driver;
    }
    RemoveClsid(stream, kClsid);
    return stream.empty() ? BackupKind::OursOnly : BackupKind::Driver;
}

std::filesystem::path FindRestoreBackup(const std::wstring& id)
{
    std::error_code ec;
    const auto canonical = CanonicalBackup(id);
    if (std::filesystem::exists(canonical, ec)) {
        return canonical;
    }

    // Older builds wrote a timestamped backup per bind. Prefer one that holds
    // the driver's chain, then one with no chain; never an ours-only snapshot.
    const auto backups = FindBackups(id);
    for (const BackupKind want : {BackupKind::Driver, BackupKind::Empty}) {
        for (const auto& path : backups) {
            if (ClassifyBackup(detail::ReadTextFile(path.wstring())) == want) {
                return path;
            }
        }
    }
    return {};
}

bool HasLegacyDriverBackup(const std::wstring& id)
{
    for (const auto& path : FindBackups(id)) {
        if (ClassifyBackup(detail::ReadTextFile(path.wstring())) ==
            BackupKind::Driver) {
            return true;
        }
    }
    return false;
}

// One-time migration: pick the best timestamped backup from an older build,
// copy it to the canonical name, and move the rest to old_backups.
bool AdoptLegacyBackup(const std::wstring& id, std::wstring& log)
{
    const auto backups = FindBackups(id);
    if (backups.empty()) {
        return false;
    }

    std::filesystem::path chosen;
    for (const BackupKind want : {BackupKind::Driver, BackupKind::Empty}) {
        for (const auto& path : backups) {
            if (ClassifyBackup(detail::ReadTextFile(path.wstring())) == want) {
                chosen = path;
                break;
            }
        }
        if (!chosen.empty()) {
            break;
        }
    }
    if (chosen.empty()) {
        return false;
    }

    std::error_code ec;
    const auto canonical = CanonicalBackup(id);
    std::filesystem::copy_file(
        chosen, canonical, std::filesystem::copy_options::overwrite_existing,
        ec);
    if (ec) {
        log += L"Could not adopt the previous backup for " + id + L": " +
               FormatWin32Error(static_cast<DWORD>(ec.value())) + L"\r\n";
        return false;
    }
    const std::wstring sidecar = SidecarPath(chosen);
    if (std::filesystem::exists(sidecar, ec)) {
        std::filesystem::copy_file(
            sidecar, SidecarPath(canonical),
            std::filesystem::copy_options::overwrite_existing, ec);
    }

    const std::filesystem::path oldDir =
        std::filesystem::path(kBackupDir) / L"old_backups";
    std::filesystem::create_directories(oldDir, ec);
    for (const auto& path : backups) {
        const std::filesystem::path target = oldDir / path.filename();
        std::filesystem::rename(path, target, ec);
        if (ec) {
            ec.clear();
            std::filesystem::copy_file(
                path, target,
                std::filesystem::copy_options::overwrite_existing, ec);
            if (!ec) {
                std::filesystem::remove(path, ec);
            }
        }
        const std::wstring side = SidecarPath(path);
        if (std::filesystem::exists(side, ec)) {
            std::filesystem::rename(
                side, oldDir / std::filesystem::path(side).filename(), ec);
        }
    }

    log += L"Adopted the previous backup for " + id + L".\r\n";
    return true;
}

// Keeps a restored backup in ProgramData without letting it be reused.
void ArchiveBackup(const std::filesystem::path& path, std::wstring& log)
{
    std::error_code ec;
    const std::filesystem::path oldDir =
        std::filesystem::path(kBackupDir) / L"old_backups";
    std::filesystem::create_directories(oldDir, ec);

    std::filesystem::path target =
        oldDir / (path.stem().wstring() + L"-" + detail::Timestamp() +
                  L".reg");
    for (int suffix = 1; std::filesystem::exists(target, ec) && suffix < 1000;
         ++suffix) {
        target = oldDir / (path.stem().wstring() + L"-" +
                           detail::Timestamp() + L"-" +
                           std::to_wstring(suffix) + L".reg");
    }
    std::filesystem::rename(path, target, ec);
    if (ec) {
        log += L"Could not move the backup to old_backups: " +
               path.wstring() + L"\r\n";
        return;
    }
    const std::wstring sidecar = SidecarPath(path);
    if (std::filesystem::exists(sidecar, ec)) {
        std::filesystem::rename(
            sidecar, oldDir / (target.stem().wstring() + L".txt"), ec);
    }
}

// Reads the best backup for an endpoint. Returns false when there is no backup
// file.
bool ReadNewestBackupText(const Endpoint& endpoint, std::wstring& text)
{
    const auto path = FindRestoreBackup(endpoint.id);
    if (path.empty()) {
        return false;
    }
    text = detail::ReadTextFile(path.wstring());
    return !text.empty();
}

bool HasCompositeValues(const std::wstring& fx, int first, int last)
{
    for (int index = first; index <= last; ++index) {
        std::vector<std::wstring> values;
        const std::wstring name = CompositeProp(index);
        if (detail::RegReadMultiString(HKEY_LOCAL_MACHINE, fx, name.c_str(),
                                       values) &&
            !values.empty()) {
            return true;
        }
    }
    return false;
}

// Removes the composite effect chains in the inclusive index range.
bool ClearCompositeValues(const std::wstring& fx, int first, int last,
                          const std::wstring& device, std::wstring& log)
{
    bool ok = true;
    for (int index = first; index <= last; ++index) {
        const std::wstring name = CompositeProp(index);
        if (!detail::RegDeleteValue(HKEY_LOCAL_MACHINE, fx, name.c_str())) {
            log += L"Could not clear " + name + L" on " + device + L".\r\n";
            ok = false;
        }
    }
    return ok;
}

// Puts the composite effect chains in the inclusive index range back to the
// values in the newest backup. Values that were absent in the backup are
// removed.
bool RestoreCompositeValues(const std::wstring& fx,
                            const std::wstring& backupText, int first,
                            int last, const std::wstring& device,
                            std::wstring& log)
{
    bool ok = true;
    for (int index = first; index <= last; ++index) {
        const std::wstring name = CompositeProp(index);
        std::vector<std::wstring> values;
        if (detail::ExtractRegMultiString(backupText, name, values)) {
            // Never restore a stale entry that points at this APO.
            RemoveClsid(values, kClsid);
            if (values.empty()) {
                if (!detail::RegDeleteValue(HKEY_LOCAL_MACHINE, fx,
                                            name.c_str())) {
                    log += L"Could not clear " + name + L" on " + device +
                           L".\r\n";
                    ok = false;
                }
            } else if (!detail::RegWriteMultiString(HKEY_LOCAL_MACHINE, fx,
                                                    name.c_str(), values)) {
                log += L"Could not restore " + name + L" on " + device +
                       L".\r\n";
                ok = false;
            }
        } else if (!detail::RegDeleteValue(HKEY_LOCAL_MACHINE, fx,
                                           name.c_str())) {
            log += L"Could not clear " + name + L" on " + device + L".\r\n";
            ok = false;
        }
    }
    return ok;
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

    // One backup per endpoint: keep the existing one until it has been
    // restored and moved to old_backups.
    const std::filesystem::path canonical = CanonicalBackup(endpoint.id);
    if (std::filesystem::exists(canonical, ec)) {
        const BackupKind kind =
            ClassifyBackup(detail::ReadTextFile(canonical.wstring()));
        // An empty or ours-only snapshot taken while the driver chain was
        // already gone must not block a legacy backup that still has it.
        if (kind == BackupKind::Driver ||
            !HasLegacyDriverBackup(endpoint.id)) {
            file = canonical.wstring();
            log += L"Using the existing backup for " + endpoint.name +
                   L".\r\n";
            return true;
        }
        log += L"Replacing the empty backup for " + endpoint.name +
               L".\r\n";
    }

    // Migrate the timestamped backups left by older builds once, choosing the
    // best one instead of snapshotting the current (possibly damaged) state.
    if (AdoptLegacyBackup(endpoint.id, log)) {
        file = canonical.wstring();
        return true;
    }

    std::wstring oldValue;
    const bool existed = detail::RegReadString(
        HKEY_LOCAL_MACHINE, detail::FxSub(endpoint.id), kPreMixProp, oldValue);

    std::vector<std::wstring> composite;
    const bool compositeExisted = detail::RegReadMultiString(
        HKEY_LOCAL_MACHINE, detail::FxSub(endpoint.id), kCompositeStreamProp,
        composite);

    file = canonical.wstring();

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
        L"composite  = " + std::wstring(kCompositeStreamProp) + L"\r\n" +
        L"compositeExisted = " + (compositeExisted ? L"True" : L"False") +
        L"\r\n" +
        L"compositeValue = " + JoinValues(composite) + L"\r\n" +
        L"newValue   = " + std::wstring(kClsid) + L"\r\n";
    detail::WriteTextFile(sidecar, text);

    log += L"Backed up " + endpoint.name + L" to " + file + L"\r\n";
    return true;
}

bool RestoreBackup(const Endpoint& endpoint, std::wstring& log)
{
    const std::wstring fx = detail::FxSub(endpoint.id);

    // Restore from the single canonical backup. After a successful restore it
    // moves to old_backups, so the next bind snapshots the driver state again.
    std::wstring chosen;
    std::wstring text;
    const auto restorePath = FindRestoreBackup(endpoint.id);
    if (!restorePath.empty()) {
        chosen = restorePath.wstring();
        text = detail::ReadTextFile(chosen);
    }

    std::wstring premixValue;
    const bool premixExisted =
        !chosen.empty() &&
        detail::ExtractRegString(text, kPreMixProp, premixValue);

    std::vector<std::wstring> compositeValue;
    const bool compositeExisted =
        !chosen.empty() &&
        detail::ExtractRegMultiString(text, kCompositeStreamProp,
                                      compositeValue);

    if (chosen.empty()) {
        log += L"No backup for " + endpoint.name +
               L"; removing the slots.\r\n";
    }

    bool ok = true;

    // A backed-up value that points at this APO is a leftover from an older
    // install, not the original driver value. Treat it as absent.
    if (premixExisted && _wcsicmp(premixValue.c_str(), kClsid) != 0) {
        if (!detail::RegWriteString(HKEY_LOCAL_MACHINE, fx, kPreMixProp,
                                    premixValue)) {
            log += L"Could not restore the PreMix slot on " + endpoint.name +
                   L".\r\n";
            ok = false;
        }
    } else {
        // Remove only our own value; never delete another APO's entry.
        std::wstring current;
        if (detail::RegReadString(HKEY_LOCAL_MACHINE, fx, kPreMixProp,
                                  current) &&
            _wcsicmp(current.c_str(), kClsid) == 0 &&
            !detail::RegDeleteValue(HKEY_LOCAL_MACHINE, fx, kPreMixProp)) {
            log += L"Could not clear the PreMix slot on " + endpoint.name +
                   L".\r\n";
            ok = false;
        }
    }

    std::vector<std::wstring> target;
    bool writeComposite = false;
    if (compositeExisted) {
        target = compositeValue;
        RemoveClsid(target, kClsid);
        writeComposite = true;
    } else {
        // Older backups predate composite support: keep the driver's chain
        // and remove only our CLSID from it.
        std::vector<std::wstring> current;
        detail::RegReadMultiString(HKEY_LOCAL_MACHINE, fx,
                                   kCompositeStreamProp, current);
        target = current;
        const size_t before = target.size();
        RemoveClsid(target, kClsid);
        writeComposite = target.size() != before;
    }

    if (writeComposite) {
        if (target.empty()) {
            if (!detail::RegDeleteValue(HKEY_LOCAL_MACHINE, fx,
                                        kCompositeStreamProp)) {
                log += L"Could not clear the composite chain on " +
                       endpoint.name + L".\r\n";
                ok = false;
            }
        } else if (!detail::RegWriteMultiString(HKEY_LOCAL_MACHINE, fx,
                                                kCompositeStreamProp, target)) {
            log += L"Could not restore the composite chain on " +
                   endpoint.name + L".\r\n";
            ok = false;
        }
    }

    // The isolate option also clears the mode, endpoint, keyword-detector, and
    // offload chains. Put them back from the same backup.
    if (!chosen.empty() &&
        !RestoreCompositeValues(fx, text, kCompositeFirstProp + 1,
                                kCompositeLastProp, endpoint.name, log)) {
        ok = false;
    }

    std::wstring currentPremix;
    if (detail::RegReadString(HKEY_LOCAL_MACHINE, fx, kPreMixProp,
                              currentPremix) &&
        _wcsicmp(currentPremix.c_str(), kClsid) == 0) {
        log += L"Could not remove the APO from " + endpoint.name + L".\r\n";
        ok = false;
    }

    std::vector<std::wstring> currentComposite;
    if (detail::RegReadMultiString(HKEY_LOCAL_MACHINE, fx,
                                   kCompositeStreamProp, currentComposite) &&
        ContainsClsid(currentComposite, kClsid)) {
        log += L"Could not remove the APO from the composite chain on " +
               endpoint.name + L".\r\n";
        ok = false;
    }

    if (ok) {
        log += L"Disabled on " + endpoint.name + L".\r\n";
        // Only the canonical file is retired. A legacy file stays put so the
        // next bind can adopt it (and move the others to old_backups).
        if (!chosen.empty() &&
            chosen == CanonicalBackup(endpoint.id).wstring()) {
            ArchiveBackup(chosen, log);
        }
    }
    return ok;
}

struct ApplyPlan {
    const Endpoint* endpoint = nullptr;
    std::wstring fx;
    bool wasEnabled = false;
    bool bindingTransition = false;
    bool isolateChanged = false;
    bool isolateRebind = false;
    bool needsRestart = false;
    ApoSettings targetSettings;
};

// Pre-compute what every change will touch. Shared by ApplyChanges and the
// GUI's restart check so the Listen monitor is only interrupted when the
// audio engine actually has to reload the APO.
std::vector<ApplyPlan> BuildPlans(const std::vector<Change>& changes,
                                  const std::vector<Endpoint>& endpoints,
                                  bool liveChannelAvailable)
{
    std::vector<ApplyPlan> plans(changes.size());

    for (size_t i = 0; i < changes.size(); ++i) {
        const Change& change = changes[i];
        ApplyPlan& plan = plans[i];

        const auto it = std::find_if(
            endpoints.begin(), endpoints.end(), [&](const Endpoint& endpoint) {
                return _wcsicmp(endpoint.id.c_str(), change.id.c_str()) == 0;
            });
        if (it == endpoints.end()) {
            continue;
        }

        plan.endpoint = &*it;
        plan.fx = detail::FxSub(it->id);
        plan.wasEnabled = it->Enabled();
        plan.bindingTransition = change.enable != plan.wasEnabled;
        plan.isolateChanged =
            change.settingsChanged &&
            it->settings.isolateEffects != change.settings.isolateEffects;
        plan.targetSettings =
            change.settingsChanged ? change.settings : it->settings;

        // Isolation is already saved for this device but some other composite
        // chain is still present. Apply should finish the cleanup without
        // making the user toggle the checkbox off and on again.
        const bool isolateCleanup =
            change.enable && plan.wasEnabled && it->compositePresent &&
            plan.targetSettings.isolateEffects &&
            HasCompositeValues(plan.fx, kCompositeFirstProp + 1,
                               kCompositeLastProp);
        plan.isolateRebind =
            change.enable && plan.wasEnabled && it->compositePresent &&
            (plan.isolateChanged || isolateCleanup);

        plan.needsRestart =
            plan.bindingTransition || plan.isolateRebind ||
            change.forceRestart;

        // A running APO that locked before the runtime file existed has no
        // mapping to poll, so the first settings Apply still needs a restart.
        if (!plan.needsRestart && change.settingsChanged && change.enable &&
            (!liveChannelAvailable || !RuntimeSettingsFileReady(it->id))) {
            plan.needsRestart = true;
        }
    }

    return plans;
}

} // namespace

Result InstallFiles()
{
    Result result;
    std::wstring log;
    const bool updating = GetFilesStatus().FullyInstalled();

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

    if (!ExtractEmbeddedDll(kInstalledDll, log)) {
        result.ok = false;
        result.summary = L"Could not install the APO DLL.";
        result.details = log;
        return result;
    }
    log += L"Extracted the APO DLL to " + std::wstring(kInstalledDll) + L"\r\n";

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

        // The runtime directory holds the live settings files the APO maps.
        std::error_code runtimeEc;
        std::filesystem::create_directories(RuntimeSettingsDirectory(),
                                            runtimeEc);
        if (runtimeEc) {
            log += L"Warning: could not create " +
                   RuntimeSettingsDirectory() + L"\r\n";
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
    result.summary = updating ? L"Updated." : L"Installed.";
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
        if (endpoint.premixOurs || endpoint.compositeOurs ||
            endpoint.streamOurs) {
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

    // Decide up front whether anything needs the engine to reload the APO.
    // Settings-only changes go through the shared runtime file and retune the
    // running chain in place, so they must not interrupt the audio.
    const bool liveChannelAvailable = GetFilesStatus().dllUpToDate;
    const std::vector<ApplyPlan> plans =
        BuildPlans(changes, endpoints, liveChannelAvailable);

    bool needsRestart = false;
    for (const ApplyPlan& plan : plans) {
        if (plan.needsRestart) {
            needsRestart = true;
            break;
        }
    }

    AudioSession session;
    if (needsRestart && !StopAudio(session, log)) {
        result.ok = false;
        result.summary = L"Could not stop the audio service.";
        result.details = log;
        return result;
    }
    auto resume = MakeScopeExit([&] { StartAudio(session, log); });

    for (size_t i = 0; i < changes.size(); ++i) {
        const Change& change = changes[i];
        const ApplyPlan& plan = plans[i];
        if (plan.endpoint == nullptr) {
            log += L"Device not found: " + change.id + L"\r\n";
            result.ok = false;
            continue;
        }

        const Endpoint& endpoint = *plan.endpoint;
        const std::wstring& fx = plan.fx;
        const bool bindingTransition = plan.bindingTransition;
        const bool isolateChanged = plan.isolateChanged;
        const bool isolateRebind = plan.isolateRebind;
        const ApoSettings& targetSettings = plan.targetSettings;

        if (change.settingsChanged) {
            if (SaveSettings(endpoint.id, change.settings)) {
                log += L"Saved settings for " + endpoint.name + L".\r\n";
            } else {
                log += L"Could not save settings for " + endpoint.name +
                       L".\r\n";
                result.ok = false;
            }
        }

        // Publish the snapshot the APO polls. Writing it on every enabled
        // change also creates the file after a bind, so later effect edits
        // stay live without another restart.
        if (change.enable &&
            !WriteRuntimeSettings(endpoint.id, targetSettings)) {
            log += L"Could not publish live settings for " + endpoint.name +
                   L".\r\n";
            if (!needsRestart) {
                result.ok = false;
            }
        }

        if (!bindingTransition && !isolateRebind) {
            continue;
        }

        if (change.enable) {
            // Capture the driver's chains before the first bind and before
            // isolating them, so turning isolation off or disabling the device
            // can always put the driver's effects back. Never back up an
            // already-isolated device: its stream chain may be ours alone.
            if (bindingTransition ||
                (isolateChanged && targetSettings.isolateEffects)) {
                std::wstring backup;
                if (!CreateBackup(endpoint, backup, log)) {
                    result.ok = false;
                    continue;
                }
            }

            if (endpoint.compositePresent) {
                std::vector<std::wstring> chain;
                std::wstring backupText;
                const bool haveBackup =
                    ReadNewestBackupText(endpoint, backupText);

                if (targetSettings.isolateEffects) {
                    // Remove every other effect-pack chain as well as the
                    // driver's stream effects, leaving this APO alone.
                    chain.push_back(kClsid);
                    if (!ClearCompositeValues(fx, kCompositeFirstProp + 1,
                                              kCompositeLastProp, endpoint.name,
                                              log)) {
                        result.ok = false;
                    }
                } else {
                    bool chainLoaded = false;
                    if (!bindingTransition && haveBackup) {
                        // Turning isolation off: the backup holds the driver's
                        // stream chain. A missing value means it was empty.
                        if (!detail::ExtractRegMultiString(
                                backupText, kCompositeStreamProp, chain)) {
                            chain.clear();
                        }
                        chainLoaded = true;
                    }
                    if (!chainLoaded) {
                        detail::RegReadMultiString(HKEY_LOCAL_MACHINE, fx,
                                                   kCompositeStreamProp, chain);
                    }
                    RemoveClsid(chain, kClsid);
                    chain.push_back(kClsid);

                    // Put back the effect-pack chains that isolation cleared.
                    if (!bindingTransition && haveBackup &&
                        !RestoreCompositeValues(fx, backupText,
                                                kCompositeFirstProp + 1,
                                                kCompositeLastProp,
                                                endpoint.name, log)) {
                        result.ok = false;
                    }
                }
                if (!detail::RegWriteMultiString(HKEY_LOCAL_MACHINE, fx,
                                                 kCompositeStreamProp, chain)) {
                    log += L"Could not write the composite chain for " +
                           endpoint.name + L".\r\n";
                    result.ok = false;
                    continue;
                }

                // The single PreMix slot is ignored on these devices, but a
                // value left by an older version would become active again
                // if the effect pack ever disappears.
                std::wstring premix;
                if (detail::RegReadString(HKEY_LOCAL_MACHINE, fx, kPreMixProp,
                                          premix) &&
                    _wcsicmp(premix.c_str(), kClsid) == 0) {
                    detail::RegDeleteValue(HKEY_LOCAL_MACHINE, fx,
                                           kPreMixProp);
                }
            } else if (!detail::RegWriteString(HKEY_LOCAL_MACHINE, fx,
                                               kPreMixProp, kClsid)) {
                log += L"Could not write the slot for " + endpoint.name +
                       L".\r\n";
                result.ok = false;
                continue;
            }

            if (endpoint.disableSysFx) {
                if (detail::RegWriteDword(HKEY_LOCAL_MACHINE,
                                          detail::FxSub(endpoint.id),
                                          kDisableSysFxProp, 0)) {
                    log += L"Re-enabled system effects on " + endpoint.name +
                           L".\r\n";
                } else {
                    log += L"Warning: could not clear the system-effects "
                           L"flag on " +
                           endpoint.name + L".\r\n";
                }
            }

            log += (bindingTransition ? L"Enabled on "
                                      : L"Updated the effect binding on ") +
                   endpoint.name + L".\r\n";
        } else {
            if (!RestoreBackup(endpoint, log)) {
                result.ok = false;
            }
        }
    }

    result.summary = result.ok ? L"Applied." : L"Applied with errors.";
    result.details = log;
    return result;
}

bool ChangesNeedRestart(const std::vector<Change>& changes)
{
    const auto endpoints = EnumEndpoints();
    const std::vector<ApplyPlan> plans =
        BuildPlans(changes, endpoints, GetFilesStatus().dllUpToDate);
    return std::any_of(plans.begin(), plans.end(),
                       [](const ApplyPlan& plan) { return plan.needsRestart; });
}

} // namespace apo
