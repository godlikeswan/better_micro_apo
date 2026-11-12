#include "runtime_settings.h"

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <string>

namespace apo {
namespace {

constexpr wchar_t kRuntimeDirectory[] =
    L"C:\\ProgramData\\better_micro_apo\\runtime";

} // namespace

std::wstring RuntimeSettingsDirectory()
{
    return kRuntimeDirectory;
}

std::wstring RuntimeSettingsPath(const std::wstring& endpointId)
{
    return RuntimeSettingsDirectory() + L"\\" + SafeEndpointId(endpointId) +
           L".dat";
}

bool RuntimeSettingsFileReady(const std::wstring& endpointId)
{
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (!GetFileAttributesExW(RuntimeSettingsPath(endpointId).c_str(),
                              GetFileExInfoStandard, &data)) {
        return false;
    }
    const ULONGLONG size =
        (static_cast<ULONGLONG>(data.nFileSizeHigh) << 32) |
        data.nFileSizeLow;
    return size >= sizeof(RuntimeSettingsFile);
}

bool WriteRuntimeSettings(const std::wstring& endpointId,
                          const ApoSettings& input)
{
    ApoSettings settings = input;
    ClampSettings(settings);

    CreateDirectoryW(L"C:\\ProgramData\\better_micro_apo", nullptr);
    CreateDirectoryW(kRuntimeDirectory, nullptr);

    const std::wstring path = RuntimeSettingsPath(endpointId);
    HANDLE file = CreateFileW(
        path.c_str(), GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }

    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file, &size)) {
        CloseHandle(file);
        return false;
    }

    // Grow the file in place. Recreating it would invalidate the mapping the
    // APO already holds.
    if (size.QuadPart < static_cast<LONGLONG>(sizeof(RuntimeSettingsFile))) {
        LARGE_INTEGER wanted{};
        wanted.QuadPart =
            static_cast<LONGLONG>(sizeof(RuntimeSettingsFile));
        if (!SetFilePointerEx(file, wanted, nullptr, FILE_BEGIN) ||
            !SetEndOfFile(file)) {
            CloseHandle(file);
            return false;
        }
    }

    HANDLE mapping = CreateFileMappingW(
        file, nullptr, PAGE_READWRITE, 0,
        static_cast<DWORD>(sizeof(RuntimeSettingsFile)), nullptr);
    if (mapping == nullptr) {
        CloseHandle(file);
        return false;
    }

    auto* view = static_cast<RuntimeSettingsFile*>(
        MapViewOfFile(mapping, FILE_MAP_WRITE, 0, 0,
                      sizeof(RuntimeSettingsFile)));
    if (view == nullptr) {
        CloseHandle(mapping);
        CloseHandle(file);
        return false;
    }

    std::uint32_t sequence = view->sequence;
    // Recover from a write that was interrupted mid-update. The sequence is
    // always even between complete snapshots.
    if ((sequence & 1u) != 0) {
        sequence += 1;
    }
    const std::uint64_t generation = view->generation;

    // Odd sequence: a reader that sees this discards the snapshot.
    view->sequence = sequence + 1;
    std::atomic_thread_fence(std::memory_order_release);

    view->magic = kRuntimeSettingsMagic;
    view->version = kRuntimeSettingsVersion;
    view->structSize = static_cast<std::uint32_t>(sizeof(RuntimeSettingsFile));
    view->generation = generation + 1;
    view->settings = settings;

    // Publish the completed snapshot. The generation is what the APO polls, so
    // it must only become visible together with the settings.
    std::atomic_thread_fence(std::memory_order_release);
    view->sequence = sequence + 2;
    std::atomic_thread_fence(std::memory_order_release);

    UnmapViewOfFile(view);
    CloseHandle(mapping);
    CloseHandle(file);
    return true;
}

} // namespace apo
