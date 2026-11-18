# Better Micro APO

![Better Micro APO GUI](docs/screenshot.png)

Better Micro APO is a Windows program that adjusts the sound of input devices
like microphones or guitars. It can turn the volume up, quiet down loud sounds,
reduce background noise, and add effects like overdrive or echo.

## Getting started

Download the latest version from [Releases](../../releases) and run
`better_micro_apo_gui.exe`. Approve the Windows permission prompt when asked —
setting up the APO needs administrator rights.

1. Select your microphone or other input device.
2. Check **Enable**. The audio will stop for about a second while Windows sets
   up the effect.
3. Check **Listen to this device** and use headphones to hear the input with
   the effects applied.
4. Click **Add effect**, choose **Gain**, and drag the slider to hear the
   volume change.
5. Add more effects and experiment. The [effect documentation](docs/effects/index.md)
   explains what each one does.

To uninstall, uncheck **Enable** on every device, or use **File → Uninstall**.

## Useful notes

- Each device has four presets. Change **Preset** (1–4) to keep up to four
  different effect sets for the same device.
- While listening, **Peak** and **RMS** at the bottom show the input level.
  Peak is the loudest moment; RMS is the average level
  ([what RMS means](https://en.wikipedia.org/wiki/Root_mean_square)). **CLIP**
  lights up when the signal is too loud, and **drops** counts gaps in the
  listening stream.
- Effects apply as you change them — there is no Apply button. Each slider has
  a small ↺ button that resets only that setting.
- If your device is missing from the list, turn on **View → Show all devices**
  to see inactive inputs too.
- Windows briefly stops audio when effects are installed or attached to a
  device. That is Windows reloading the audio chain. Settings changes and noise
  profile captures apply to the running chain without stopping audio.
- Gate lookahead and noise reduction need to buffer a little audio to do their
  job, so they add a small delay. Those controls are marked with ⏱.
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
