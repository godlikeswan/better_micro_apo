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
- The DSP is a list of `apo::dsp::Stage` objects (`src/dsp`). `StagePool`
  preallocates a fixed number of stages per effect type in `LockForProcess`.
  `StagePool::Update` walks the active preset's effect list, binds each enabled
  effect to a pooled stage by its stable instance id, retunes it in place, and
  fills a non-owning `Chain` in list order. The same type may appear more than
  once, and the list order is the processing order. `Update` clears and refills
  the chain without allocating, so it can run from the runtime polling path.
  Heavy types (noise reduction, gate, reverb) are capped at four instances each;
  the whole list is capped at `kMaxEffects` (16). Each device has four presets
  and only `presetIndex` is processed at a time.

  Adding a new effect type means writing one `Stage` subclass, extending
  `EffectType`, and adding it to `StagePool::Configure`; the real-time path does
  not change.
- `Chain::Process` decodes the interleaved buffer into a preallocated
  `double` scratch buffer, runs each stage in place, then encodes and clips the
  result. The scratch buffer is sized in `LockForProcess` from the connection's
  maximum frame count, so `APOProcess` stays allocation-free.
- `NoiseStage` performs a short-time Fourier transform with a fixed 512-point
  FFT and a 256-sample hop, subtracts the captured magnitude profile, and
  applies one gain per frequency bin to all channels so the stereo image is
  preserved. The gain is limited by the configured reduction and smoothed
  across bins and time. Analysis and synthesis use a square-root Hann window
  with 50% overlap, so the stage adds about 512 samples of latency. All buffers
  are allocated in `Configure`; the stage reports `NeedsSilence() == true` so
  its overlap tail is flushed through silent buffers. The pool only creates
  noise stages when a valid profile was loaded, so a Noise effect is skipped
  otherwise.
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
  cleared in `Configure`/`Reset`. In the initial list it runs before the
  broadband compressor so it sees the input dynamics.
- Optional mono mode copies the processed left channel to every other channel,
  for a mono source connected to input 1 of a stereo interface.
- `DistortionStage` applies a selectable soft saturation curve: tanh, arctan,
  cubic polynomial, logarithmic, exponential, or error function. `driveDb` sets
  the level into the curve, `curvature` bends it from nearly linear to fully
  saturated, and `outputDb` sets the level after it. Every curve is odd and
  maps -1..1 to -1..1, so no DC blocker is needed. The curve coefficients are
  derived in `Update`, leaving only the curve evaluation in the real-time path.
  In the initial list it runs after mono and before reverb so the tail stays
  clean.
- The distortion, gate, and compressor transfer functions live in
  `src/dsp/transfer.h`. The stages and the GUI's curve control call the same
  inline functions, so those displayed curves cannot drift from the audio path.
  The dynamic graph draws the analog band-pass prototype that `DynamicStage`
  approximates with its state-variable filter.
- `ReverbStage` is a Freeverb-style room/hall reverb: eight damped comb filters
  feed four allpass filters per channel, with the classic stereo spread between
  even and odd channels. In the initial list it runs last, after mono, so a mono
  source still produces a wide tail. `mix` crossfades dry and wet, `size` sets
  the comb feedback,
  `damping` sets the high-frequency absorption, and `predelayMs` delays the
  excitation by up to 200 ms. Delay lines and the predelay buffer are sized in
  `Configure`; `Update` only retunes feedback, damping, mix, and the predelay
  read offset. The stage reports `NeedsSilence()` while enabled so the tail is
  flushed through silent buffers.

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

The INI file has a `[dsp]` section with `version=4`, `preset_count`,
`preset_index`, `isolate_effects`, and one block per preset entry:
`preset_<p>_effect_<n>_type`, `preset_<p>_effect_<n>_id`,
`preset_<p>_effect_<n>_enabled`, plus the same parameter keys used before with
the `preset_<p>_effect_<n>_` prefix (for example
`preset_0_effect_0_gate_threshold_db`). `preset_<p>_effect_<n>_id` is the
stable instance id the pool uses to rebind a stage after a live update. Each
device has four independent lists, and only `preset_index` is processed at a
time. Files written before version 4 are not migrated: they load as four empty
presets, and only `isolate_effects` is carried over.

The captured noise profile is stored next to the settings as
`C:\ProgramData\better_micro_apo\devices\{endpoint-guid}.noise`. The file has a
small `BMNP` header (version, sample rate, channel count, frame count) followed
by the raw interleaved float32 capture. `LockForProcess` analyzes it with the
same FFT size and window as `NoiseStage`, averages the magnitude spectrum across
channels, and resamples the bins if the profile and the stream use different
sample rates. A missing or invalid profile leaves the noise pool empty, so a
Noise effect in the list is skipped even when it is enabled.

`isolate_effects` is a GUI binding option rather than a DSP value: it tells the
GUI to leave only this APO on the device by dropping the driver's endpoint chain
and the driver's other composite chains. The APO still reads it from the same
INI file but does not use it while processing.

The GUI writes the INI files when effect settings are auto-applied (about half
a second after the last edit) or when **Apply** runs, and publishes the same
snapshot to `C:\ProgramData\better_micro_apo\runtime\{endpoint-guid}.dat`. The
APO maps that file read-only in `LockForProcess` and polls a generation counter
from `APOProcess`; a new generation retunes the existing stages in place through
`StagePool::Update`. A seqlock (`sequence`) makes a torn read impossible, and the
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
the owner-drawn effect sliders; `gui/ui/structure.*` builds the control tree and
lays it out; `gui/ui/menu.*` owns the themed menu bar; and `gui/ui/theme.*` owns
the palette, system-theme detection, native control theming, and the
`WM_CTLCOLOR` handlers. `gui/main.cpp` keeps the app logic, background work,
monitor, and message routing. Changing how a slider looks or behaves, or adding
a control to a view, is therefore local to the widget factory, the view
construction, and its layout function.

The theme follows the Windows app mode (`AppsUseLightTheme`). The app reads it
before creating any window and reacts to `WM_SETTINGCHANGE`/`WM_THEMECHANGED`
for live switching. Light mode keeps the native themed appearance. Dark mode
applies the DWM dark title bar, themes the native combo, list, and header
controls, and owner-draws the tab strip, push buttons, checkboxes, and sliders
through subclasses in `gui/ui/widgets.cpp`. The tab item parameter stores
whether a tab is reachable, so the custom painter can gray out unavailable tabs.

The native menu bar is owner-drawn by `gui/ui/menu.cpp` and uses the theme
background brush through `SetMenuInfo(MIM_BACKGROUND)`, so it follows the same
light/dark palette and is refreshed on theme changes. `gui/preferences.*`
persists the **View** mode in `C:\ProgramData\better_micro_apo\gui.ini`; the
default is **Simple**.

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
Its Apply writes bindings and isolation and finishes any pending migration from
the old stream slots; effect edits are left to the Effects view. **Reset**
restores the persisted checkboxes and reloads only the saved isolation value, so
effect edits made in the other view survive.

The **Effects** view lists bound devices in a dropdown, with a **Preset**
selector (1–4) next to it, and shows the selected preset's effect list in
processing order. Each device has four independent lists; switching presets
commits the editor, rebuilds the list from that preset, and publishes the
selection through the live runtime channel. Effects added to a preset stay with
that preset. The checkboxes map directly to each instance's enable flag in the
active `EffectPreset`; selecting a row swaps the parameter panel on the right to
that instance's type. Column 1 of each row is a `×` delete button (handled
through `NM_CLICK`). **Add effect** opens a modal type picker
(`gui/ui/effect_dialog.cpp`) and appends the new instance to the end of the
list; types already at their per-type capacity are left out of the picker. The
same type may appear more than once, and deleting an instance discards its
values. Adding or deleting rebuilds the list view and marks the device dirty,
but still goes through the live runtime channel rather than an audio restart.
Edits are saved and published automatically after a short debounce, so the
running APO picks them up without interrupting audio; a restart is only needed
the first time an endpoint has no runtime file or when the installed DLL
predates the runtime channel. **Apply** remains as the fallback for those cases,
and **Reset** reloads the persisted effect settings while preserving any
pending isolation edit.

Each view tracks its own pending state. `Endpoint::settingsDirty` covers effect
edits, while `isolateSaved`/`isolationDirty` cover the isolation checkbox. An
effect edit marks the device dirty, locks the device dropdown, and highlights
Apply until the debounced live apply saves it (or Apply is used because the live
channel is not available). The preset dropdown stays enabled: switching presets
does not discard edits. A Devices Apply keeps unsaved effect edits in memory
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
processing so the profile describes the raw room noise. The GUI saves a
settings snapshot with every effect in the list disabled, and applies it with
the normal `ApplyChanges` path when the device is bound. The
APO then runs an empty chain, which is a plain passthrough. The GUI reuses the
listen monitor in a capture mode (`apo::CaptureNoise`): it records about three
seconds of interleaved float32 samples, converts PCM formats on the fly, and
renders silence to the default output so the room is not played back through
speakers. The raw capture is written to the device's `.noise` file, then the
real settings are restored: the first Noise effect in the list is enabled, or a
new Noise effect is appended when the list has none and still has room. The
restored settings are applied again. If the capture or the save fails, the
original settings are restored instead. Devices that are not bound are captured
directly and only their settings file is written. The capture path forces an
audio restart for both the passthrough snapshot and the restored settings,
because `LockForProcess` is the only place the noise profile is loaded.

The noise stage never reads files: `LockForProcess` loads and analyzes the
profile before the stream starts, and `APOProcess` only uses the cached bins.

## Device binding

The GUI writes the endpoint-effect value under the endpoint's `FxProperties`
key, then restarts audio once:

- `PKEY_FX_EndpointEffectClsid`
  (`{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},7`, `REG_SZ`) for devices without an
  effect pack.
- `PKEY_CompositeFX_EndpointEffectClsid`
  (`{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},15`, `REG_MULTI_SZ`) for devices
  whose driver publishes composite FX entries. The APO CLSID is appended to the
  driver's endpoint chain; the driver entries are preserved and restored on
  disable.

Endpoint effects are used instead of stream effects because Windows applies
endpoint effects to raw streams while stream effects are skipped. Chromium and
Electron apps (Discord, WhatsApp) open the microphone raw whenever the endpoint
reports raw-processing support, so a stream-slot binding silently stops
processing there. Older builds bound the APO in the PreMix slot (`...,1`) and
later in the composite stream chain (`...,13`). The bind path now also removes
this APO from `...,1`, `...,5`, and `...,13` so a migrated device is not
processed twice. A device that is still bound only through an old slot reports
as pending; **Apply** migrates it, and **Install**/**Update** runs the same
migration while audio is stopped.

The per-device `isolate_effects` option leaves only this APO on the device. It
keeps this APO's entry in the endpoint chain (`...,15`), drops the driver's
entries from that chain, and clears the other composite chains: mode (`...,14`),
keyword detector (`...,16`-`...,18`), and offload (`...,19`-`...,20`).
Isolation is a desired state, not just a transition, so it is re-applied on
every Apply while it is set; a driver that republishes its chains is cleaned up
again. Turning it off rebuilds the chains from the canonical backup.

Before changing any slot the GUI exports the whole `FxProperties` key with
`reg.exe` to a single canonical file per endpoint,
`C:\ProgramData\better_micro_apo\backup\FxProperties-{endpoint}.reg`. While it
exists, binds reuse it instead of writing another snapshot. A successful
restore moves it to `backup\old_backups` with a timestamp, so the next bind
records the freshly restored driver state. Older builds left one timestamped
file per bind; the first bind adopts the best of those (one that still holds a
driver chain, then one with no chain, never one containing only this APO) and
moves the rest to `backup\old_backups`. Restore parses the backup, puts back the
original endpoint, PreMix, and stream values and every composite chain, and
removes only this APO's CLSID from anything it did not write.

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
