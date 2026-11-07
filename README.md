# Better Micro APO

Better Micro APO (internally `better_micro_apo`) is a simple microphone effect
for Windows. Its main goal is to raise the input level without distortion: a
peak compressor tames the peaks and makeup gain lifts the result. An optional
gate silences the input between phrases.

![Better Micro APO GUI](docs/screenshot.png)

## Installation and usage

Run the GUI (it requests elevation):

```
build\bin\Release\better_micro_apo_gui.exe
```

The GUI is self-contained: the APO DLL is embedded in it, so it is the only
file you need to distribute.

1. Click **Install** to extract the embedded APO DLL to
   `C:\Program Files\better_micro_apo`, register its COM class, and restart
   audio. Audio stops for about a second.
2. Check the capture devices to enable the effect on. Inactive devices are
   shown grayed out.
3. Select a device and edit its settings in the panel on the right. Each
   capture device keeps its own settings. If a mono source is plugged into the
   left input of a stereo device, check **Mono (left to all channels)** so both
   sides carry it. This affects every app that uses the device.
4. Click **Apply** to save the settings and bind the selected devices. Audio
   stops for about a second. Devices that use an effect pack are bound in the
   composite stream chain; other devices use the PreMix slot.
5. On a device that uses an effect pack, **Ignore other effects** replaces the
   driver's composite stream chain with only this APO. This is per device and is
   applied with **Apply**; unchecking it restores the driver chain from the
   backup. It only changes the stream chain, not the effect pack's mode or
   endpoint effects.
6. Check **Listen to this device** to hear the selected device through the
   default output. This is the quickest way to audition the applied settings.
   Use headphones to avoid feedback from speakers. Listening stops on its own
   when you change the selection, apply, install, or uninstall. While
   listening, **Peak** is the maximum level over the last 500 ms and **RMS** is
   the average level over the same window, both measured after the APO. **CLIP**
   latches when the peak reaches full scale, and **drops** counts gaps in the
   monitor stream.
7. Click **Uninstall** to unbind the devices, unregister the APO, and remove the
   installed DLL directory. Backups, settings, and logs stay in
   `C:\ProgramData\better_micro_apo`.

The GUI binds either the PreMix slot or, on devices that use an effect pack,
the composite stream chain. **Ignore other effects** changes only that stream
chain, so the effect pack's mode and endpoint chains keep running. **Open log**
opens `C:\ProgramData\better_micro_apo\better_micro_apo.log`.

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

The chain order is fixed; stages can be disabled, and makeup and mono are
optional:

```
optional gate -> peak compressor -> optional makeup gain -> optional mono copy
```

- **Gate** attenuates the signal when it falls below the threshold. `Range` is
  the maximum attenuation while closed, and `Hold` keeps it open after the level
  drops. `Lookahead` delays the audio so the gate opens before a transient
  arrives; it adds that much latency while the gate is enabled. The gate is off
  by default.
- **Compressor** uses a linked peak detector, so a stereo source keeps its
  image.
- **Gain** enables the makeup gain stage. The configured value is kept while
  the checkbox is off, so it can be bypassed without losing it.
- **Makeup** gain is applied after the compressor.
- **Mono (left to all channels)** duplicates the processed left channel to all
  other channels. It is per device and is saved with the other settings.

Default settings:

| Gate | Threshold | Range | Attack | Hold | Release | Lookahead |
| --- | --- | --- | --- | --- | --- | --- |
| off | -50 dBFS | 60 dB | 5 ms | 50 ms | 100 ms | 0 ms |

| Compressor | Threshold | Ratio | Attack | Release | Makeup |
| --- | --- | --- | --- | --- | --- |
| enabled | -58 dBFS | 3:1 | 5 ms | 100 ms | +28 dB |

Gain is enabled by default.

Each capture device stores its own settings in
`C:\ProgramData\better_micro_apo\devices\{endpoint-guid}.ini`. The APO reads
that file when a stream starts, so one DLL serves every device. **Listen to
this device** opens a new stream and therefore plays the saved settings; click
**Apply** first to hear unsaved edits.

Older settings files that contain the removed `input_gain_db` key are migrated
automatically: the value is folded into the compressor threshold and makeup
gain, so the sound stays the same.

The GUI clamps values to a safe range:

| Setting | Range |
| --- | --- |
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
| Makeup gain | -24 to +48 dB |

## Known limitations

- PCM 24-bit valid samples stored in a 32-bit container (24-in-32) are not
  supported yet.
- Settings take effect after **Apply**, which briefly stops audio. Changing
  them while audio keeps running is not supported yet.
- Gate lookahead delays the stream by the configured lookahead time while the
  gate is enabled. Keep it at 0 ms if you need zero latency.

## Architecture

See [ARCHITECTURE.md](ARCHITECTURE.md) for implementation details and a general
guide to installing custom APOs.
