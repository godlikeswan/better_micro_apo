# Better Micro APO

Better Micro APO (internally `better_micro_apo`) is a Windows microphone APO
that raises the input level without distortion. A peak compressor controls
peaks and makeup gain lifts the result; optional effects add gating, noise
reduction, frequency-selective dynamics, saturation, and reverb.

![Better Micro APO GUI](docs/screenshot.png)

## Quick start

Build, then run the GUI (it requests elevation):

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
build\bin\Release\better_micro_apo_gui.exe
```

`better_micro_apo_gui.exe` is the only file to distribute: the APO DLL is
embedded in it as an `RCDATA` resource.

1. Pick a capture device.
2. Check **Enable**. The GUI installs or updates the APO if needed and binds
   the device as an endpoint effect, so it also works with apps that capture
   raw audio (Discord, WhatsApp). While the APO is enabled, other effects on
   that device are ignored.
3. Edit the effect list. Changes apply live without stopping audio. The
   **Preset** selector (1–4) keeps independent lists for each device.
4. Check **Listen to this device** to hear the result through the default
   output. Use headphones to avoid feedback.

Unchecking **Enable** on the last enabled device also uninstalls the APO files
and registration. Settings, backups, and logs stay under
`C:\ProgramData\better_micro_apo`.

## Main screen

There is one screen: no tabs and no Simple/Advanced mode. It has a device
selector, **Enable**, the **Preset** selector, the effects editor, **Listen to
this device**, and status with Peak/RMS.

- Effects run top to bottom; the same type can appear more than once. **Add
  effect** inserts after the selected effect, or appends when nothing is
  selected. Click `×` in a row to delete an effect. New effects start enabled.
- Each editable parameter has a small ↺ icon that resets only that parameter.
- **View → Show all devices** adds inactive endpoints, marked `[unplugged]`,
  `[not present]`, or `[disabled]`. The choice is saved in `gui.ini`.
- Parameters that add delay are marked ⏱ and have a tooltip.

## Menu

- **File → Install / Update** extracts, registers, and restarts audio.
  **Enable** does this automatically; the menu is for manual maintenance.
- **File → Uninstall** unbinds every device, unregisters the APO, and removes
  the installed DLL.
- **File → Exit**
- **View → Show all devices**
- **About → Info** shows DLL hashes, install path, CLSID, registry status, and
  how many devices the APO is applied to.
- **About → Open log** opens `C:\ProgramData\better_micro_apo\better_micro_apo.log`.

## Documentation

Start with the [documentation index](docs/index.md).

- [APO overview](docs/apo/overview.md)
- [GUI overview](docs/gui/overview.md)
- [Effect reference](docs/effects/index.md)
- [DLL ↔ GUI settings sharing](docs/runtime-sharing.md)

## Known limitations

- PCM 24-bit valid samples in a 32-bit container (24-in-32) are not supported.
- Binding, install/uninstall, and noise-profile capture restart audio briefly;
  settings-only edits do not. The first live apply on a device with no runtime
  snapshot, or after a DLL update, also restarts audio once.
- Gate lookahead and noise reduction add latency (both marked with ⏱).
