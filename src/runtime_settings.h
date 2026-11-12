#pragma once

#include <cstdint>
#include <string>

#include "apo_settings.h"

namespace apo {

// Fixed-size settings snapshot shared between the GUI and the APO.
//
// The GUI writes one file per endpoint under
// C:\ProgramData\better_micro_apo\runtime\{endpoint-guid}.dat and maps it as
// shared memory. The APO maps the same file read-only in LockForProcess and
// polls `generation` from the real-time path. A seqlock (`sequence`) makes a
// torn read impossible: the writer stores an odd value while updating and an
// even value once the snapshot is complete.
struct RuntimeSettingsFile
{
    volatile std::uint32_t sequence = 0;
    std::uint32_t magic = 0;
    std::uint32_t version = 0;
    std::uint32_t structSize = 0;
    volatile std::uint64_t generation = 0;
    ApoSettings settings;
};

inline constexpr std::uint32_t kRuntimeSettingsMagic = 0x50414D42u; // "BMAP"
inline constexpr std::uint32_t kRuntimeSettingsVersion = 1u;

// C:\ProgramData\better_micro_apo\runtime
std::wstring RuntimeSettingsDirectory();

// C:\ProgramData\better_micro_apo\runtime\{endpoint-guid}.dat
std::wstring RuntimeSettingsPath(const std::wstring& endpointId);

// GUI side: writes a complete snapshot and bumps the generation. The file is
// grown in place instead of recreated, so an existing APO mapping stays valid.
bool WriteRuntimeSettings(const std::wstring& endpointId,
                          const ApoSettings& settings);

// True when the endpoint's runtime file already exists at full size. An APO
// that locked before the file was created has no mapping, so the first Apply
// for that endpoint still needs an audio restart.
bool RuntimeSettingsFileReady(const std::wstring& endpointId);

} // namespace apo
