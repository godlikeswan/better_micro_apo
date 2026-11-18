# DLL ↔ GUI settings sharing

The GUI and the APO are separate processes. They share per-device settings
through a file-backed memory mapping so effect edits can be applied to a
running stream without restarting audio. Noise profile capture uses a second
mapping for the request/result handshake. Source: `src/runtime_settings.*`,
`src/stage_capture.*`, `src/apo_settings.cpp`, `src/better_micro_apo.cpp`,
`gui/apo_ops.cpp`, and `gui/main.cpp`.

## Files per endpoint

`{endpoint-guid}` is the last `{GUID}` from the MMDevices endpoint id, made
filename-safe with `apo::SafeEndpointId`.

| File | Writer | Reader | Purpose |
| --- | --- | --- | --- |
| `C:\ProgramData\better_micro_apo\devices\{id}.ini` | GUI | APO at `LockForProcess` | Durable settings; authoritative on restart. |
| `C:\ProgramData\better_micro_apo\devices\{id}.noise` | GUI | APO at `LockForProcess` and after a live capture | Durable noise profile, with precomputed magnitude bins. |
| `C:\ProgramData\better_micro_apo\runtime\{id}.dat` | GUI | APO while streaming | Live settings snapshot. |
| `C:\ProgramData\better_micro_apo\runtime\{id}.capture` | Both | Both while streaming | One-shot stage capture control block. |
| `C:\ProgramData\better_micro_apo\gui.ini` | GUI | GUI | `show_all_devices` and `last_device` preferences; not used by the APO. |

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
- **Noise-profile capture** uses the stage capture channel below. The selected
  Noise effect stays enabled, all other effects keep running, settings are not
  rewritten, and audio and Listen are not interrupted. The APO retunes the live
  noise stages from the returned bins before the request is acknowledged.
- **Binding/unbinding and isolation changes** require `LockForProcess` to
  reload state, so they restart the audio service. The first apply on a device
  without a ready runtime file also restarts once. `apo::ChangesNeedRestart`
  computes this before the GUI decides whether to stop audio; listening is
  resumed afterwards only when a restart happened.
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

`devices\{id}.noise` starts with a packed `BMNP` header. New captures write
version 2, which stores the analysis result instead of raw samples:

| Field | Type | Meaning |
| --- | --- | --- |
| `magic` | 4 bytes | `"BMNP"` |
| `version` | `uint32_t` | `2` |
| `sampleRate` | `uint32_t` | Stream rate the bins were measured at. |
| `binCount` | `uint32_t` | Number of `double` magnitudes that follow (257 for the current FFT). |

Version 1 files (20-byte header with `channels` and `frames`, followed by raw
interleaved float32 samples) are still read for compatibility, but are not
written any more. Files over 64 MiB are rejected, as are v1 files with fewer
than 512 frames.

`LockForProcess` loads either version and resamples the bins to the stream rate
if needed. A missing or invalid profile leaves the stage as a passthrough. The
APO never reads the `.noise` file from the real-time path; a captured profile is
applied from the shared capture block instead.

## Stage capture channel

`StageCaptureFile` (`src/stage_capture.h`, magic `0x50414353` / `"SCAP"`,
version 1) lives in `runtime\{id}.capture`. It holds the selected preset index
and effect id, requested duration, sample rate, frame counters, an error code,
an acknowledge field, and `double bins[257]`. All control fields are 32-bit and
are read or written with `Interlocked` operations; state changes are the
publication barriers for the bins.

1. The GUI waits for `Idle`, writes the target and duration, then sets
   `Requested`.
2. The APO verifies that the target is an enabled Noise effect in the active
   preset, inserts `StageCaptureTap` immediately before it, and sets
   `Capturing`.
3. The tap analyzes the double-precision samples it receives with the same
   512-point FFT, 256-sample hop, and square-root Hann window as `NoiseStage`.
   It never changes the samples. When enough frames have passed, the APO writes
   the averaged magnitude bins and sets `Done`.
4. The GUI saves the bins as a v2 `.noise` file and writes `resultAck`. On
   success the APO copies the bins into the live `NoiseProfile`, calls
   `StagePool::UpdateProfileBins`, then removes the tap and returns the block
   to `Idle`.

No effect is disabled, no settings file or runtime snapshot is rewritten, and
`StopAudio`/`StartAudio` are not called. The selected Noise effect must already
be enabled because the tap is placed at that point in the running chain.
