# DLL ↔ GUI settings sharing

The GUI and the APO are separate processes. They share per-device settings
through a file-backed memory mapping so effect edits can be applied to a
running stream without restarting audio. Noise profile capture uses a second
mapping for the request/result handshake, and a third mapping carries APO→GUI
stage telemetry for the live graphs and meters. Source: `src/runtime_settings.*`,
`src/stage_capture.*`, `src/stage_monitor.*`, `src/apo_settings.cpp`,
`src/better_micro_apo.cpp`, `gui/apo_ops.cpp`, and `gui/main.cpp`.

## Files per endpoint

`{endpoint-guid}` is the last `{GUID}` from the MMDevices endpoint id, made
filename-safe with `apo::SafeEndpointId`.

| File | Writer | Reader | Purpose |
| --- | --- | --- | --- |
| `C:\ProgramData\better_micro_apo\devices\{id}.ini` | GUI | APO at `LockForProcess` | Durable settings; authoritative on restart. |
| `C:\ProgramData\better_micro_apo\devices\{id}.noise` | GUI | APO at `LockForProcess` and after a live capture | Durable noise profile, with precomputed magnitude bins. |
| `C:\ProgramData\better_micro_apo\runtime\{id}.dat` | GUI | APO while streaming | Live settings snapshot. |
| `C:\ProgramData\better_micro_apo\runtime\{id}.capture` | Both | Both while streaming | One-shot stage capture control block. |
| `C:\ProgramData\better_micro_apo\runtime\{id}.monitor` | APO | GUI while streaming | Continuous per-stage levels, engagement, and spectra. |
| `C:\ProgramData\better_micro_apo\gui.ini` | GUI | GUI | `show_all_devices` and `last_device` preferences; not used by the APO. |

## Runtime snapshot layout

`RuntimeSettingsFile` (`src/runtime_settings.h`):

| Field | Type | Meaning |
| --- | --- | --- |
| `sequence` | `volatile uint32_t` | Seqlock: odd while writing, even when complete. |
| `magic` | `uint32_t` | `0x50414D42` (`"BMAP"`). |
| `version` | `uint32_t` | `10`; bump whenever `ApoSettings` changes layout. |
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
version=8
preset_count=4
preset_index=0
isolate_effects=1
preset_0_effect_count=...
preset_0_next_effect_id=...
preset_0_effect_0_id=...
preset_0_effect_0_type=noise
preset_0_effect_0_enabled=1
preset_0_effect_0_noise_reduction_db=...
preset_0_effect_0_noise_fft_size=512
...
```

Each preset stores `effect_count`, `next_effect_id`, and one block per entry
with `id`, `type`, `enabled`, and the type's parameter keys. Effect types are
`input`, `gain`, `noise`, `dynamics`, `equalizer`, `mono`, `saturation`, and
`reverb`; parameter keys include `gain_db` (also used by Input),
`dynamics_threshold_db`, `dynamics_ratio`, `dynamics_hold_ms`,
`equalizer_frequency_hz`, `equalizer_level_db`, `saturation_function`,
`reverb_algorithm`, `reverb_level`, `reverb_max_length_ms`, and
`reverb_predelay_ms`. Every preset has an Input row at index 0; it is forced on
and clamped to −24…+24 dB.
The `id` is the stable instance id the stage pool uses to tell whether a slot
now holds a different effect. Four presets are always written; only
`preset_index` is processed.

`LoadSettings` starts with defaults. If `[dsp] version` is missing or is not
**8**, it deletes the INI file and returns the defaults; there is no migration
path for old settings. For a current file, `ClampSettings` repairs invalid
values: non-finite numbers fall back to defaults (except `dynamics_ratio`,
where positive infinity is valid and is written as `inf`), every numeric field
is clamped to its safe range (Noise reduction to 0–50 dB, unsupported
`noise_fft_size` values fall back to 512), Input is inserted/repaired at row 0,
and effect ids are normalized to unique positive values. `isolate_effects` is
forced to `1` on every load/save/publish and is not a user option.

## Stage monitor channel

`StageMonitorFile` (`src/stage_monitor.h`, magic `0x544F4D42` / `"BMOT"`,
version 4) lives in `runtime\{id}.monitor`. It has a fixed header and one
`StageMonitorSlot` per possible chain position (`kMaxEffects + 2`: the normal
effect slots, the temporary capture tap, and one padding slot). Each slot has
its own seqlock and carries
the bound effect id/type, observed flag, input/output peak and RMS, two
cross-buffer output peak holds, a stage-specific engagement value, and 128
spectrum bins from a shared 256-point FFT. Spectra are published only for Input
and Equalizer. All values are `float` except the 32-bit control fields, so the
mapping has a stable cross-process layout.

Both peak holds are accumulated in the APO on every buffer rather than sampled
only when the GUI polls. Their decay is measured in audio time (0.06 s and
0.6 s half-lives), so a transient between two 100 ms GUI polls reaches both the
meter fill and the peak line. The shorter hold drives the fill and falls back
sooner; the longer one remains as the line. The GUI colours the line from the
bar zone it sits in, so the line and the bar always agree.

The APO opens the file read/write in `LockForProcess`, closes it in
`UnlockForProcess`/`FinalRelease`, and is the only writer. `BeginBuffer`,
`ObserveStage` and `EndBuffer` run from `APOProcess` without allocation, locks,
logging, or file I/O. Slots not observed in a buffer are cleared so silence does
not leave stale meters; `Close` publishes one final all-empty generation. The
temporary capture tap uses an explicit sentinel binding so it can never be
mistaken for an effect by the GUI.

The GUI keeps one read-only `StageMonitorView` open for the selected endpoint
and polls it from timer id 5 (100 ms). It looks slots up by the stable effect id
and type, then passes the snapshot to `ui::SetGraphMonitor`; missing or stale
telemetry calls `ui::ClearGraphMonitor` instead. Polling never writes settings,
touches bindings, or stops Listen. This channel is independent of the bottom
Peak/RMS meter, which comes from the WASAPI Listen stream.

## Noise profile format

`devices\{id}.noise` starts with a packed `BMNP` header. New captures write
version 2, which stores the analysis result instead of raw samples:

| Field | Type | Meaning |
| --- | --- | --- |
| `magic` | 4 bytes | `"BMNP"` |
| `version` | `uint32_t` | `2` |
| `sampleRate` | `uint32_t` | Stream rate the bins were measured at. |
| `binCount` | `uint32_t` | Number of `double` magnitudes that follow: 129, 257, 513, 1025, or 2049 for FFT sizes 256 through 4096. |

Version 1 files (20-byte header with `channels` and `frames`, followed by raw
interleaved float32 samples) are still read for compatibility, but are not
written any more. Legacy files are analyzed with the default 512-point FFT, so
they load as 257 bins. Files over 64 MiB are rejected, as are v1 files with
fewer than 512 frames.

`LockForProcess` loads either version and stores the leading `binCount` bins.
Each noise stage maps those bins onto its active FFT size using the profile and
stream sample rates, so profiles remain usable when the FFT setting changes. A
missing or invalid profile leaves the stage as a passthrough. The APO never
reads the `.noise` file from the real-time path; a captured profile is applied
from the shared capture block instead.

## Stage capture channel

`StageCaptureFile` (`src/stage_capture.h`, magic `0x50414353` / `"SCAP"`,
version 2) lives in `runtime\{id}.capture`. It holds the selected preset index
and effect id, requested duration, sample rate, frame counters, `binCount`, an
error code, an acknowledge field, and `double bins[2049]`. All control fields
are 32-bit and are read or written with `Interlocked` operations; state changes
are the publication barriers for the bins. A mismatched version or struct size
is reinitialized instead of being reused.

1. The GUI waits for `Idle`, writes the target and duration, then sets
   `Requested`.
2. The APO verifies that the target is an enabled Noise effect in the active
   preset, inserts `StageCaptureTap` immediately before it, and sets
   `Capturing`.
3. The tap analyzes the double-precision samples it receives with the selected
   effect's FFT size, a 50% hop, and a square-root Hann window. It never changes
   the samples. When enough frames have passed, the APO writes the averaged
   magnitude bins, sets `binCount`, and sets `Done`.
4. The GUI saves the leading `binCount` bins as a v2 `.noise` file and writes
   `resultAck`. On success the APO copies those bins into the live
   `NoiseProfile`, sets its `binCount`, calls `StagePool::UpdateProfileBins`,
   then removes the tap and returns the block to `Idle`.

No effect is disabled, no settings file or runtime snapshot is rewritten, and
`StopAudio`/`StartAudio` are not called. The selected Noise effect must already
be enabled because the tap is placed at that point in the running chain.
