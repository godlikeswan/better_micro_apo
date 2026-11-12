# Architecture

Better Micro APO is a capture APO built on `CBaseAudioProcessingObject`. COM
identity and aggregation are handled by ATL (`CComObjectRootEx`, `CComCoClass`,
`CComAggObject`), the same structure Microsoft's SwapAPO sample uses.

## Interfaces

- `IAudioProcessingObject`
- `IAudioProcessingObjectConfiguration`
- `IAudioProcessingObjectRT`
- `IAudioSystemEffects`

## Processing

- 1 input / 1 output, in-place. Latency is zero unless the gate lookahead or
  noise reduction is enabled.
- Supported sample formats: float32, PCM16, PCM24, PCM32 (container size must
  equal the valid-bit size).
- The DSP is a chain of `apo::dsp::Stage` objects (`src/dsp`). `ConfigureDsp()`
  assembles it from the per-device settings:

  ```
  NoiseStage -> GateStage -> DynamicStage -> CompressorStage
  -> GainStage (makeup) -> MonoStage
  ```

  Every stage is always in the chain and checks its own `enabled` flag, so a
  live settings update can turn a stage on or off without rebuilding the chain.
  Adding or reordering a stage means writing one `Stage` subclass and changing
  `ConfigureDsp()`; the real-time path does not change.
- `Chain::Process` decodes the interleaved buffer into a preallocated
  `double` scratch buffer, runs each stage in place, then encodes and clips the
  result. The scratch buffer is sized in `LockForProcess` from the connection's
  maximum frame count, so `APOProcess` stays allocation-free.
- `NoiseStage` runs first so it sees the raw input. It performs a short-time
  Fourier transform with a fixed 512-point FFT and a 256-sample hop, subtracts
  the captured magnitude profile, and applies one gain per frequency bin to all
  channels so the stereo image is preserved. The gain is limited by the
  configured reduction and smoothed across bins and time. Analysis and
  synthesis use a square-root Hann window with 50% overlap, so the stage adds
  about 512 samples of latency. All buffers are allocated in `Configure`; the
  stage reports `NeedsSilence() == true` so its overlap tail is flushed through
  silent buffers.
- `GateStage` can delay the audio by up to 300 ms while its detector watches the
  incoming signal, so the gate opens before a transient reaches the output. The
  delay line is allocated in `Configure`, cleared in `Reset`, and its state is
  advanced for silent buffers too (`Stage::NeedsSilence` /
  `Chain::ProcessSilence`) so buffered audio is not replayed later.
- The gate and compressor both use a linked peak detector across channels, so a
  stereo source keeps its image.
- `DynamicStage` is a frequency-selective compressor/expander for the proximity
  effect. A topology-preserving state-variable bandpass (Simper) isolates a band
  around `frequencyHz`; the band is normalized to unity at its center so a broad
  Q acts like a dynamic low shelf. The linked band peak feeds a fast-attack,
  slow-release envelope, and the resulting gain is added back to the band only
  (`out = x + band * (gain - 1)`). Reduce mode cuts the band above the threshold;
  extend mode boosts it below the threshold. The gain itself is smoothed with
  the configured attack/release and limited by `rangeDb`. The stage is
  allocation-free in `Process`; filter and envelope state are allocated and
  cleared in `Configure`/`Reset`. It runs before the broadband compressor so it
  sees the input dynamics.
- Optional mono mode copies the processed left channel to every other channel,
  for a mono source connected to input 1 of a stereo interface.

## Per-device settings

One DLL serves every capture device. Each APO instance identifies its endpoint
from `APOInitSystemEffects2` in `Initialize`: the last item in
`pDeviceCollection` is the endpoint `IMMDevice`, and the last `{GUID}` in
`IMMDevice::GetId()` is the key used under
`HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\MMDevices\Audio\Capture`.

`LockForProcess` reads
`C:\ProgramData\better_micro_apo\devices\{endpoint-guid}.ini`, clamps the
values, and precomputes the DSP coefficients. `APOProcess` only uses the
cached instance state, so the real-time path does no file I/O and no
allocation. Missing files fall back to the defaults.

The INI keys are `noise_enabled`, `noise_reduction_db`, `gate_enabled`,
`gate_threshold_db`, `gate_range_db`, `gate_attack_ms`, `gate_hold_ms`,
`gate_release_ms`, `gate_lookahead_ms`, `compressor_enabled`,
`compressor_threshold_db`, `compressor_ratio`, `compressor_attack_ms`,
`compressor_release_ms`, `dynamic_enabled`, `dynamic_expand`,
`dynamic_frequency_hz`, `dynamic_q`, `dynamic_threshold_db`, `dynamic_ratio`,
`dynamic_range_db`, `dynamic_attack_ms`, `dynamic_release_ms`,
`makeup_enabled`, `makeup_db`, `mono`, and `isolate_effects`. Legacy keys
(`threshold_db`, `ratio`, `attack_ms`, `release_ms`) are still read for the
compressor. A legacy `input_gain_db` key is
folded into the compressor threshold and makeup gain (`threshold -= input_gain`,
`makeup += input_gain`) so settings written before the input gain stage was
removed keep the same sound.

The captured noise profile is stored next to the settings as
`C:\ProgramData\better_micro_apo\devices\{endpoint-guid}.noise`. The file has a
small `BMNP` header (version, sample rate, channel count, frame count) followed
by the raw interleaved float32 capture. `LockForProcess` analyzes it with the
same FFT size and window as `NoiseStage`, averages the magnitude spectrum across
channels, and resamples the bins if the profile and the stream use different
sample rates. A missing or invalid profile leaves the stage out of the chain
even when `noise_enabled=1`.

`isolate_effects` is a GUI binding option rather than a DSP value: it tells the
GUI to replace the composite stream chain with only this APO instead of
appending to the driver's chain. The APO still reads it from the same INI file
but does not use it while processing.

The GUI writes the INI files when effect settings are auto-applied (about half
a second after the last edit) or when **Apply** runs, and publishes the same
snapshot to `C:\ProgramData\better_micro_apo\runtime\{endpoint-guid}.dat`. The
APO maps that file read-only in `LockForProcess` and polls a generation counter
from `APOProcess`; a new generation retunes the existing stages in place through
`Chain::Update`. A seqlock (`sequence`) makes a torn read impossible, and the
writer grows the file in place so an existing mapping stays valid. The poll is a
single generation load unless the settings changed, so the real-time path still
does no file I/O and no allocation. On restart the INI remains authoritative:
`LockForProcess` baselines the generation to the snapshot it just loaded.

Binding and isolation changes still restart the audio service so the engine
reloads the endpoint's effect chain. A settings-only change skips the restart
unless the endpoint has no usable runtime file yet (the running APO would have
no mapping to poll).

## GUI structure

`gui/main.cpp` is split into three tabbed views. The tab order also expresses
the prerequisite chain, so a later tab stays disabled until the earlier step is
done: **Install** is always available, **Devices** requires the APO files or at
least one bound device, and **Effects** requires a full install and at least one
bound device. The app opens on the most advanced available tab and falls back
when an operation removes a prerequisite.

The UI is split into small modules so a view describes *what* controls it has
without also owning their construction or geometry. `gui/ui/ui_state.*` holds
the control identifiers, handles, fonts, DPI, and the selected view/effect;
`gui/ui/widgets.*` provides the reusable control factories, font helpers, and
number-field parsing; `gui/ui/structure.*` builds the control tree and lays it
out; and `gui/ui/theme.*` owns the palette, system-theme detection, native
control theming, and the `WM_CTLCOLOR` handlers. `gui/main.cpp` keeps the app
logic, background work, monitor, and message routing. Changing a number field
into a slider, or adding a control to a view, is therefore local to the widget
factory, the view construction, and its layout function.

The theme follows the Windows app mode (`AppsUseLightTheme`). The app reads it
before creating any window and reacts to `WM_SETTINGCHANGE`/`WM_THEMECHANGED`
for live switching. Light mode keeps the native themed appearance. Dark mode
applies the DWM dark title bar, themes the native edit, combo, list, and header
controls, and owner-draws the tab strip, push buttons, and checkboxes through
subclasses in `gui/ui/widgets.cpp`. The tab item parameter stores whether a tab
is reachable, so the custom painter can gray out unavailable tabs.

List checkboxes are drawn from the control's state image list. In dark mode the
app replaces the two native glyphs inside that list (`ImageList_Replace`) instead
of installing a list of its own, because applying a window theme makes comctl32
recreate and destroy the state image list; a cached handle would go stale and
could alias the replacement. The glyph is drawn at the list's real icon size
(13×13 at 96 DPI), not the 15px standalone box, or the border falls outside the
bitmap. The list maps check-state index 1 (unchecked) to image 0 and index 2
(checked) to image 1, so both images carry the same glyph as the regular
checkboxes. Switching back to light mode keeps the native glyphs that comctl32
rebuilds for the light theme.

While a background operation runs, both lists stay enabled and reject state
changes in `LVN_ITEMCHANGING`. Disabling a themed list makes the native control
paint its disabled background light, which caused a white panel for the whole
busy period. Full repaints invalidate the window and its children without
`RDW_UPDATENOW`, so layout changes coalesce into one paint instead of flashing
intermediate frames.

The **Devices** view owns binding and the per-device `isolate_effects` option.
Its Apply writes only bindings and isolation; effect edits are left for the
Effects view. **Reset** restores the persisted checkboxes and reloads only the
saved isolation value, so effect edits made in the other view survive.

The **Effects** view lists bound devices in a dropdown and shows every optional
stage in a checked list. The checkboxes map directly to the enable flags in
`ApoSettings`; selecting a row swaps the parameter panel on the right. Edits are
saved and published automatically after a short debounce, so the running APO
picks them up without interrupting audio; a restart is only needed the first
time an endpoint has no runtime file or when the installed DLL predates the
runtime channel. **Apply** remains as the fallback for those cases, and
**Reset** reloads the persisted effect settings while preserving any pending
isolation edit.

Each view tracks its own pending state. `Endpoint::settingsDirty` covers effect
edits, while `isolateSaved`/`isolationDirty` cover the isolation checkbox. An
effect edit marks the device dirty, locks the dropdown, and highlights Apply
until the debounced live apply saves it (or Apply is used because the live
channel is not available). A Devices Apply keeps unsaved effect edits in memory
across the endpoint refresh instead of silently discarding them.

## Listen monitor

`gui/apo_monitor.cpp` opens the selected capture endpoint in shared mode and
renders it to the default output, so the APO runs on the monitored stream. The
capture thread accumulates the peak, the sum of squared samples, and the sample
count per packet. Silent packets count as zero-energy samples so the RMS window
decays instead of holding the previous level. The GUI timer samples these
counters every 100 ms and keeps a 500 ms history: the displayed peak is the
maximum of that history and the RMS is
`sqrt(sum(sumSquares) / sum(samples))`. Clip and drop counters are latched and
reset when listening starts. **Apply** stops the monitor only when the change
requires an audio restart, and then starts it again for the same endpoint when
the work finishes. Settings-only applies leave the monitor running.

## Noise profile capture

**Capture noise profile** records the device without any of this APO's
processing so the profile describes the raw room noise. The GUI saves a settings
snapshot with the noise, gate, compressor, gain, and mono stages disabled, and
applies it with the normal `ApplyChanges` path when the device is bound. The
APO then runs an empty chain, which is a plain passthrough. The GUI reuses the
listen monitor in a capture mode (`apo::CaptureNoise`): it records about three
seconds of interleaved float32 samples, converts PCM formats on the fly, and
renders silence to the default output so the room is not played back through
speakers. The raw capture is written to the device's `.noise` file, then the
real settings are restored with `noise_enabled=1` and applied again. If the
capture or the save fails, the original settings are restored instead. Devices
that are not bound are captured directly and only their settings file is
written. The capture path forces an audio restart for both the passthrough
snapshot and the restored settings, because `LockForProcess` is the only place
the noise profile is loaded.

The noise stage never reads files: `LockForProcess` loads and analyzes the
profile before the stream starts, and `APOProcess` only uses the cached bins.

## Device binding

The GUI writes one of two registry values under the endpoint's `FxProperties`
key, then restarts audio once:

- `PKEY_FX_PreMixEffectClsid`
  (`{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},1`, `REG_SZ`) for devices without an
  effect pack.
- `PKEY_CompositeFX_StreamEffectClsid`
  (`{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},13`, `REG_MULTI_SZ`) for devices
  whose driver publishes composite FX entries. Windows ignores the single FX
  slots on those devices, so the APO CLSID is appended to the existing driver
  chain instead. The driver entries are preserved and restored on disable.

The per-device `isolate_effects` option replaces the effect pack's composite
chains with only this APO's CLSID. It sets the stream chain (`...,13`) to this
APO and clears the mode (`...,14`), endpoint (`...,15`), keyword-detector
(`...,16`-`...,18`), and offload (`...,19`-`...,20`) chains. Turning it off
rebuilds every one of those values from the canonical backup, plus this APO,
and disabling the device restores the driver chains without this APO. If
isolation is already saved but other composite chains are still present,
`Apply` finishes the cleanup without requiring a checkbox toggle.

Before changing either slot the GUI exports the whole `FxProperties` key with
`reg.exe` to a single canonical file per endpoint,
`C:\ProgramData\better_micro_apo\backup\FxProperties-{endpoint}.reg`. While it
exists, binds reuse it instead of writing another snapshot. A successful
restore moves it to `backup\old_backups` with a timestamp, so the next bind
records the freshly restored driver state. Older builds left one timestamped
file per bind; the first bind adopts the best of those (one that still holds a
driver chain, then one with no chain, never one containing only this APO) and
moves the rest to `backup\old_backups`. Restore parses the backup, puts back the
original PreMix value and every composite chain, and removes only this APO's
CLSID from anything it did not write.

## Registration and logging

- CLSID: `{6A8BE200-CD89-4E77-97F5-D51E8F16EF90}`
- Logs setup calls to `C:\ProgramData\better_micro_apo\better_micro_apo.log`
  and to `OutputDebugStringW`. `APOProcess` never logs or allocates.

## Packaging

The build embeds the APO DLL in `better_micro_apo_gui.exe` as an `RCDATA`
resource (`IDR_APO_DLL`), so the GUI is the only file that needs to be
distributed. CMake stages the built DLL at
`build/apo_embed/better_micro_apo.dll`, generates the resource script from
`gui/apo_dll.rc.in`, and touches it after every DLL build so the resource
compiler picks up the new bytes. `Install` extracts the resource to
`C:\Program Files\better_micro_apo\better_micro_apo.dll` and registers it. On
every refresh the GUI compares the size and an FNV-1a hash of the embedded
payload with the installed DLL; when they differ it reports the DLL as
outdated and enables **Update** so the installed copy can be replaced.

## Further reading

For the general install/uninstall flow, registry layout, and pitfalls, see the
[Custom APO Installer Guide](https://gist.github.com/godlikeswan/5152c5594d79e50980ad2cf831d3f06d).
