# DLL ↔ GUI settings sharing

The GUI and the APO are separate processes. They share per-device settings
through a file-backed memory mapping so effect edits can be applied to a
running stream without restarting audio. Source: `src/runtime_settings.*`,
`src/apo_settings.cpp`, `src/better_micro_apo.cpp`, `gui/apo_ops.cpp`, and
`gui/main.cpp`.

## Files per endpoint

`{endpoint-guid}` is the last `{GUID}` from the MMDevices endpoint id, made
filename-safe with `apo::SafeEndpointId`.

| File | Writer | Reader | Purpose |
| --- | --- | --- | --- |
| `C:\ProgramData\better_micro_apo\devices\{id}.ini` | GUI | APO at `LockForProcess` | Durable settings; authoritative on restart. |
| `C:\ProgramData\better_micro_apo\devices\{id}.noise` | GUI | APO at `LockForProcess` | Raw room-noise capture. |
| `C:\ProgramData\better_micro_apo\runtime\{id}.dat` | GUI | APO while streaming | Live settings snapshot. |
| `C:\ProgramData\better_micro_apo\gui.ini` | GUI | GUI | `show_all_devices` preference; not used by the APO. |

## Runtime snapshot layout

`RuntimeSettingsFile` (`src/runtime_settings.h`):

| Field | Type | Meaning |
| --- | --- | --- |
| `sequence` | `volatile uint32_t` | Seqlock: odd while writing, even when complete. |
| `magic` | `uint32_t` | `0x50414D42` (`"BMAP"`). |
| `version` | `uint32_t` | `6`; bump whenever `ApoSettings` changes layout. |
| `structSize` | `uint32_t` | Size of the full structure. |
| `generation` | `volatile uint64_t` | Incremented on each published snapshot; this is what the APO polls. |
| `settings` | `ApoSettings` | The complete settings blob, copied by value. |

`ApoSettings` keeps each effect's whole parameter block in the struct, not just
the block for its type. That makes the snapshot trivially copyable and safe to
publish through shared memory.

### Writer side

`apo::WriteRuntimeSettings` (GUI):

1. Clamps the settings.
2. Opens the file with read/write and all share flags, creating it if needed.
3. **Grows the file in place** to `sizeof(RuntimeSettingsFile)` if it is too
   small. Recreating the file would invalidate a mapping the APO already holds.
4. Maps it read/write and recovers from an interrupted write by rounding an odd
   sequence up to the next even value.
5. Sets `sequence` odd with a release fence, writes magic/version/size,
   increments `generation`, copies the settings, then sets `sequence` even with
   release fences.
6. Unmaps and closes.

`RuntimeSettingsFileReady` checks that the file exists at full size. An APO
that locked its stream before the file was created has no mapping to poll, so
the first apply for that endpoint still needs an audio restart.

### Reader side

`BetterMicroApo::OpenRuntimeSettings` maps the file read-only in
`LockForProcess` and validates magic, version, and size. It seeds
`m_runtimeGeneration` with the current generation so the snapshot just loaded
from the INI is not applied twice.

`PollRuntimeSettings` runs from `APOProcess`:

1. Load `sequence` with an acquire fence; if odd, skip (the GUI is writing).
2. Validate magic, version, and struct size.
3. Load `generation`; if unchanged, return. The common case is this single
   load — no file I/O and no allocation.
4. Copy `settings`, acquire fence, and re-check `sequence`. If it changed, a
   write raced with the copy, so discard the snapshot.
5. Clamp the copy, store the new generation, and call
   `StagePool::Update(settings, format, chain)`.

This seqlock makes a torn read impossible, and the generation check ensures a
new snapshot is applied exactly once.

## Apply behavior

- **Settings-only edits** are saved to the INI and published to the `.dat`
  file about 200 ms after the last edit (`kLiveApplyTimer`). The monitor keeps
  running and audio is not interrupted.
- **Binding/unbinding, isolation changes, and noise-profile capture** require
  `LockForProcess` to reload state, so they restart the audio service. The
  first apply on a device without a ready runtime file also restarts once.
  `apo::ChangesNeedRestart` computes this before the GUI decides whether to
  stop audio; listening is resumed afterwards only when a restart happened.
- **On restart** the INI is authoritative. The runtime file is a live overlay
  for the duration of a stream; it never replaces the durable settings.

## Durable INI format

`devices\{id}.ini` has a single `[dsp]` section:

```
version=4
preset_count=4
preset_index=0
isolate_effects=1
preset_0_effect_count=...
preset_0_next_effect_id=...
preset_0_effect_0_id=...
preset_0_effect_0_type=noise
preset_0_effect_0_enabled=1
preset_0_effect_0_noise_reduction_db=...
...
```

Each preset stores `effect_count`, `next_effect_id`, and one block per entry
with `id`, `type`, `enabled`, and the type's parameter keys (for example
`gate_threshold_db`, `dynamic_frequency_hz`, `distortion_function`,
`reverb_predelay_ms`). The `id` is the stable instance id the stage pool uses to
tell whether a slot now holds a different effect. Four presets are always
written; only `preset_index` is processed.

`LoadSettings` repairs invalid values with `ClampSettings`: non-finite numbers
fall back to defaults, every numeric field is clamped to its safe range, and
effect ids are normalized to unique positive values. Files written by older
versions (a single effect list) are **not migrated**: they load as four empty
presets. `isolate_effects` is forced to `1` on every load/save/publish and is
not a user option.

## Noise profile format

`devices\{id}.noise` starts with a packed 20-byte `BMNP` header:

| Field | Type |
| --- | --- |
| `magic` | 4 bytes, `"BMNP"` |
| `version` | `uint32_t`, currently 1 |
| `sampleRate` | `uint32_t` |
| `channels` | `uint32_t` |
| `frames` | `uint32_t` |

followed by raw interleaved float32 samples. Files over 64 MiB or with fewer
than 512 frames are rejected.

`LockForProcess` analyzes the profile with the same 512-point FFT, 256-sample
hop, and square-root Hann window as `NoiseStage`, averages the magnitude
spectrum across channels, and resamples the bins if the profile and stream use
different sample rates. A missing or invalid profile leaves the noise pool
empty, so a Noise effect in the list is skipped even when enabled. The APO
never reads the profile from the real-time path.
