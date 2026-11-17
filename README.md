# Better Micro APO

Better Micro APO (internally `better_micro_apo`) is a simple microphone effect
for Windows. Its main goal is to raise the input level without distortion: a
peak compressor tames the peaks and makeup gain lifts the result. An optional
gate silences the input between phrases, optional noise reduction subtracts a
captured room-noise profile, and a frequency-selective dynamic processor can
tame or extend the proximity effect.

![Better Micro APO GUI](docs/screenshot.png)

## Installation and usage

Run the GUI (it requests elevation):

```
build\bin\Release\better_micro_apo_gui.exe
```

The GUI is self-contained: the APO DLL is embedded in it, so it is the only
file you need to distribute.

The menu bar has:

- **File → Install / Update** extracts the embedded APO DLL to
  `C:\Program Files\better_micro_apo`, registers its COM class, and restarts
  audio. Audio stops for about a second. If the installed DLL does not match
  this build, the command updates it. **Enable** installs or updates the APO
  automatically, so this command is mainly for manual maintenance.
- **File → Uninstall** unbinds every device, unregisters the APO, and removes the
  installed DLL directory. Backups, settings, and logs stay in
  `C:\ProgramData\better_micro_apo`. Unchecking **Enable** on the last enabled
  device performs the same uninstall automatically.
- **File → Exit** closes the GUI.
- **View → Show all devices** adds inactive registry endpoints to the device
  selector, marked `[unplugged]`, `[not present]`, or `[disabled]`. The toggle is
  off by default and is saved in `C:\ProgramData\better_micro_apo\gui.ini`.
- **About → Info** opens a modal summary with the embedded and installed DLL
  hashes, install path and DLL file name, COM CLSID, registry key status, overall
  install state, and the number of devices the APO is applied to.
- **About → Open log** opens
  `C:\ProgramData\better_micro_apo\better_micro_apo.log`.

### Main screen

There is one screen with no tabs and no Simple/Advanced mode. It has a device
selector, the **Enable** checkbox, a **Preset** selector (1–4) next to the device
selector, the effects editor, **Listen to this device**, and a status line. The
device selector normally lists active capture devices only; use **View → Show
all devices** to edit settings on an inactive endpoint.

Pick a capture device and check **Enable**:

- If the APO is missing, or the installed DLL does not match this build, the GUI
  installs or updates it first.
- The APO is then bound to the selected device as an endpoint effect, which also
  works for apps that capture raw audio, such as Discord and WhatsApp.
- While the APO is enabled on a device, other effects on that device are ignored
  unconditionally. The GUI leaves only its own endpoint effect and clears the
  driver's other composite chains; it re-clears them if a driver republishes
  them. There is no per-device option to turn this off.
- Effects on that device apply live. While it is disabled, edits are still saved
  so the next **Enable** starts from them.

Unchecking **Enable** unbinds the selected device. When it was the last enabled
device, the GUI also uninstalls the APO files and registration; device settings,
backups, and logs are kept. **Listen to this device** needs an active, enabled
device.

Each device keeps its own four effect presets. The **Preset** selector switches
which list is edited; there is no migration from the older single-list format.

The list on the left shows the selected preset's effects in processing order.
Use the checkboxes to enable or disable an effect, click a row to edit its
parameters on the right, and click the `×` in a row to delete it. **Add effect**
opens a picker and appends the chosen type to the end of the list. The same type
may appear more than once, and the list order is the DSP order. Adding or
deleting an effect applies live without stopping audio. A deleted effect loses
its values: adding that type again starts from its defaults. A device can hold
up to 16 effects; noise reduction, gate, and reverb are limited to 4 each because
they allocate large buffers. For distortion, compressor, and gate effects, the
top of the parameter panel draws a live transfer curve from the same math as the
DSP, so it follows the sliders and shows what the stage actually applies. The
dynamic effect shows the shape of its selected frequency band instead.

To reduce steady room noise, select **Noise reduction**, keep the room quiet, and
click **Capture noise profile**. The GUI records about three seconds, analyzes
the noise spectrum, and saves it for that device. Capturing stops audio briefly,
temporarily disables every stage so the profile is raw, and turns **Noise
reduction** on when it finishes. **Reduction** caps how much any frequency is
attenuated.

Check **Listen to this device** to hear the selected device through the default
output. This is the quickest way to audition the applied settings. Use
headphones to avoid feedback from speakers. Listening stops on its own when the
selection changes, during install/uninstall, and when an operation restarts
audio; it starts again afterwards for the same endpoint. Settings-only applies
leave the monitor running. While listening, **Peak** is the maximum level over
the last 500 ms and **RMS** is the average level over the same window, both
measured after the APO. **CLIP** latches when the peak reaches full scale, and
**drops** counts gaps in the monitor stream.

Effect settings apply automatically while you drag and shortly after you stop
editing. The running APO picks them up without stopping audio. The first change
on a device that has not published live settings yet, or after the APO DLL was
updated, may restart audio once. Each editable parameter has a small borderless
↺ icon that restores just that parameter to its default and disables itself when
the value is already default. While an effect edit is pending, the device
selector is locked until the automatic apply finishes.

## Build

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

Output:

```
build\bin\Release\better_micro_apo.dll
build\bin\Release\better_micro_apo_gui.exe
```

`better_micro_apo_gui.exe` is the distributable: CMake embeds the DLL into it
as an `RCDATA` resource during the build, so it can be shipped on its own. The
standalone DLL is still produced for debugging.

The DLL uses the static MSVC runtime and has no third-party dependencies.

## DSP settings

Each device has four independent presets. A new device starts with four empty
presets and preset 1 selected. The **Preset** selector next to the device
selector switches the list; effects added to a preset stay with that preset.
The selected preset is processed from top to bottom. The same type may appear
more than once, so two gates or two reverbs are allowed.

A typical list can be built in any order, for example:

```
optional noise reduction -> optional gate -> optional dynamic (proximity) ->
peak compressor -> optional makeup gain -> optional mono copy ->
optional distortion -> optional reverb
```

Delete and re-add an effect to move it to the end of the list; drag-and-drop is
not implemented yet. Every newly added effect starts enabled.

- **Noise reduction** subtracts the captured room-noise spectrum from the input.
  Place it first, before the gate and compressor, so it sees the raw signal. The
  profile is captured per device with **Capture noise profile**: the GUI records
  about three seconds while every effect is temporarily off, then stores the
  analyzed spectrum next to the device settings. **Reduction** limits how much
  any frequency can be attenuated. A profile is required; without one the effect
  is skipped. It is off by default.
- **Gate** attenuates the signal when it falls below the threshold. `Range` is
  the maximum attenuation while closed, and `Hold` keeps it open after the level
  drops. `Lookahead` delays the audio so the gate opens before a transient
  arrives; it adds that much latency while the gate is enabled. The gate is off
  by default.
- **Compressor** uses a linked peak detector, so a stereo source keeps its
  image.
- **Dynamic** is a frequency-selective compressor/expander aimed at the
  proximity effect. `Frequency` and `Q` choose the band; a broad Q behaves like
  a dynamic low shelf. In the default reduce mode it cuts the band when it is
  loud, taming a strong proximity effect up close. Check **Extend quiet lows**
  to do the opposite: lift the band when it is quiet, making distant or thin
  lows fuller. `Threshold`, `Ratio`, `Range`, `Attack`, and `Release` control
  how much and how fast. Place it before the broadband compressor so it sees the
  input dynamics. It is off by default.
- **Gain** applies flat gain. It is the makeup gain after the compressor. A
  newly added Gain starts at 0 dB.
- **Mono (left to all channels)** duplicates the processed left channel to all
  other channels. It is per device and is saved with the other settings.
- **Distortion** adds soft saturation. Choose a curve from the **Function**
  list: `Tanh`, `Arctan`, `Cubic polynomial`, `Logarithmic`, `Exponential`, or
  `Error function`. **Drive** sets the level into the curve, **Curvature**
  bends it from nearly linear to fully saturated, and **Output** sets the level
  after it. Every curve is symmetric and maps full scale to full scale. Place it
  before reverb so the tail stays clean. It is off by default.
- **Reverb** adds a room or hall tail. `Mix` sets the dry/wet balance, `Size`
  moves from a small room to a long hall, `Damping` controls how quickly the
  highs fade, and `Predelay` leaves a gap before the tail starts. Place it last,
  after mono, so a mono microphone still gets a wide stereo tail. It is off by
  default.

Default settings (a newly added effect):

| Noise reduction | Reduction |
| --- | --- |
| off | 12 dB |

| Gate | Threshold | Range | Attack | Hold | Release | Lookahead |
| --- | --- | --- | --- | --- | --- | --- |
| off | -50 dBFS | 60 dB | 5 ms | 50 ms | 100 ms | 0 ms |

| Compressor | Threshold | Ratio | Attack | Release |
| --- | --- | --- | --- | --- |
| enabled | -58 dBFS | 3:1 | 5 ms | 100 ms |

| Gain | Gain |
| --- | --- |
| enabled | +28 dB |

| Dynamic | Mode | Frequency | Q | Threshold | Ratio | Range | Attack | Release |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| off | reduce | 120 Hz | 0.7 | -45 dBFS | 4:1 | 18 dB | 10 ms | 150 ms |

| Distortion | Function | Drive | Curvature | Output |
| --- | --- | --- | --- | --- |
| off | Tanh | 0 dB | 50% | 0 dB |

| Reverb | Mix | Size | Damping | Predelay |
| --- | --- | --- | --- | --- |
| off | 20% | 50% | 50% | 20 ms |

Each capture device stores its own settings in
`C:\ProgramData\better_micro_apo\devices\{endpoint-guid}.ini`. The file has a
`[dsp]` section with `version=4`, `preset_count`, `preset_index`,
`isolate_effects=1`, and one `preset_<p>_effect_<n>_*` block per preset entry, so
duplicates, their order, and the selected preset survive a restart. Files
written by earlier versions are not migrated: every device loads with four
empty presets. `isolate_effects` is always forced on and is written as `1`; it is
not a user option. The APO reads the file when a stream starts, so one DLL serves
every device. **Listen to this device** opens a new stream and plays the saved
settings; edits are saved live, so the audition follows them.

The GUI clamps values to a safe range:

| Setting | Range |
| --- | --- |
| Noise reduction | 0 to 70 dB |
| Gate threshold | -80 to 0 dBFS |
| Gate range | 0 to 96 dB |
| Gate attack | 0.1 to 1000 ms |
| Gate hold | 0 to 2000 ms |
| Gate release | 1 to 5000 ms |
| Gate lookahead | 0 to 300 ms |
| Compressor threshold | -60 to 0 dBFS |
| Compressor ratio | 1:1 to 20:1 |
| Compressor attack | 0.1 to 1000 ms |
| Compressor release | 1 to 5000 ms |
| Dynamic frequency | 20 to 2000 Hz |
| Dynamic Q | 0.1 to 10 |
| Dynamic threshold | -80 to 0 dBFS |
| Dynamic ratio | 1:1 to 20:1 |
| Dynamic range | 0 to 36 dB |
| Dynamic attack | 0.1 to 1000 ms |
| Dynamic release | 1 to 5000 ms |
| Gain | -24 to +48 dB |
| Distortion function | Tanh, Arctan, Cubic polynomial, Logarithmic, Exponential, or Error function |
| Distortion drive | 0 to 36 dB |
| Distortion curvature | 0 to 100% |
| Distortion output | -24 to +12 dB |
| Reverb mix | 0 to 100% |
| Reverb size | 0 to 100% |
| Reverb damping | 0 to 100% |
| Reverb predelay | 0 to 200 ms |

## Known limitations

- PCM 24-bit valid samples stored in a 32-bit container (24-in-32) are not
  supported yet.
- Effect settings apply live a moment after an edit, without stopping audio.
  Binding, install, uninstall, and noise-profile capture still restart the audio
  service briefly. Clearing other effects as part of a binding uses the same
  restart. The first settings change on a device that
  has no runtime file, or after the APO DLL is updated, also restarts audio
  once.
- Gate lookahead delays the stream by the configured lookahead time while the
  gate is enabled. Keep it at 0 ms if you need zero latency.
- Noise reduction delays the stream by about 512 samples while it is enabled.
  The captured profile describes the noise in the room at that moment; capture
  it again if the background noise changes noticeably.

## Architecture

See [ARCHITECTURE.md](ARCHITECTURE.md) for implementation details and a general
guide to installing custom APOs.
