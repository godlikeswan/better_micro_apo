# Better Micro APO

![Better Micro APO GUI](docs/screenshot.png)

Better Micro APO is a Windows program that adjusts the sound of input devices
like microphones or guitars. It can turn the volume up, quiet down loud sounds,
reduce background noise, and add effects like saturation or echo.

## Getting started

Download the latest version from [Releases](../../releases) and run
`better_micro_apo_gui.exe`. Approve the Windows permission prompt when asked —
setting up the APO needs administrator rights.

1. Select your microphone or other input device.
2. Check **Enable**. The audio will stop for about a second while Windows sets
   up the effect.
3. Check **Listen to this device** and use headphones to hear the input with
   the effects applied.
4. Select the fixed **Input** row. It is always first and enabled; set
   **Input gain** (0 dB by default) and watch its live spectrum and output
   meter.
5. Click **Add effect**. **Gain** is first in the Add effect menu and starts at
   **+12 dB**; move its slider until the meters look comfortable.
6. Add more effects and experiment. The [effect documentation](docs/effects/index.md)
   explains what each one does.

To uninstall, uncheck **Enable** on every device, or use **File → Uninstall**.

## Useful notes

- Each device has four presets. Change **Preset** (1–4) to keep up to four
  different effect sets for the same device. Each preset starts with an
  always-on **Input** row that cannot be added, deleted, or disabled.
- While listening, **Peak** and **RMS** at the bottom show the input level.
  Peak is the loudest moment; RMS is the average level
  ([what RMS means](https://en.wikipedia.org/wiki/Root_mean_square)). **CLIP**
  lights up when the signal is too loud, and **drops** counts gaps in the
  listening stream. This bottom meter comes from the Listen stream.
- The selected effect's panel has its own live graph from the APO: Input
  spectrum, Gain input/output peak dot, Gate/Expander or Compressor/Limiter
  engagement and output, Equalizer spectrum with static/live/limit band curves,
  Saturation operating point and output, or Tuner note, cents, and a C0–C8 pitch
  map. The level graphs include a thin output meter with green/yellow/red zones.
  Gain, Saturation, and the meter use short holds so brief sounds are not skipped
  between polls. Polling these graphs never changes settings or stops Listen.
- Effects apply as you change them — there is no Apply button. Each slider has
  a small ↺ button that resets only that setting.
- **Gate/Expander** attenuates signal below the threshold: a finite **Ratio**
  expands and **∞** is a hard gate. **Compressor/Limiter** reduces signal above
  the threshold: a finite Ratio compresses and **∞** is a hard limiter. Neither
  has a mode selector; both ratios go up to **∞**, and lookahead is available
  when you need to catch transients.
- **Equalizer** starts with a **−12 dB** Level and **Dynamic** set to **No**.
  Switch Dynamic to **Compressor** to tame loud lows or **Expander** to lift
  quiet lows; its detector controls are greyed out when Dynamic is No. Level
  spans −96…+96 dB, Ratio reaches ∞, and Range goes up to 96 dB.
- **Tuner** is monitoring-only: it shows the nearest note, cents, and a C0–C8
  pitch map without changing or delaying the audio.
- If your device is missing from the list, turn on **View → Show all devices**
  to see inactive inputs too.
- Windows briefly stops audio when effects are installed or attached to a
  device. That is Windows reloading the audio chain. Settings changes and noise
  profile captures apply to the running chain without stopping audio.
- Noise reduction, gate/compressor/equalizer lookahead, and reverb predelay/tail buffer
  a little audio, so they add a small delay. Those controls are marked with ⏱.
- A settings file from a different schema version is deleted when it is read;
  the device starts from defaults rather than having old values migrated.
- Supported input formats are float32, PCM16, PCM24, and PCM32. 24-bit samples
  in a 32-bit container are not supported.

## Documentation

- [What the APO does](docs/apo/overview.md)
- [How the GUI works](docs/gui/overview.md)
- [List of effects](docs/effects/index.md)
- [DLL ↔ GUI settings sharing](docs/runtime-sharing.md)
- [Full documentation index](docs/index.md)

## Building from source

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

The GUI executable is self-contained: the APO DLL is embedded in it during the
build, so it is the only file you need to distribute.
