# Better Micro APO

![Better Micro APO GUI](docs/screenshot.png)

Better Micro APO is a Windows program that adjusts the sound of input devices
like microphones or guitars. It can turn the volume up, quiet down loud sounds,
reduce background noise, and add effects like saturation, echo, chorus, tremolo,
vibrato, pitch shifting, and looping.

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
   **Input gain** (0 dB by default), check **Treat as mono** if a mono mic is
   plugged into input 1 of a stereo interface, and watch its live spectrum and
   output meter.
5. Click **Add effect**. **Gain** is first in the Add effect menu and starts at
   **+12 dB**; move its slider until the meters look comfortable.
6. Add more effects and experiment. The [effect documentation](docs/effects/index.md)
   explains what each one does.

To uninstall, uncheck **Enable** on every device, or use **File → Uninstall**.

## Useful notes

- Each device has four presets. Change **Preset** (1–4) to keep up to four
  different effect sets for the same device. Each preset starts with an
  always-on **Input** row that cannot be added, deleted, or disabled. Its
  **Treat as mono** checkbox copies the first channel to all channels before
  the other effects run, so a mono mic on a stereo input feeds both sides. The
  old standalone Mono effect has been removed.
- While listening, **Peak** and **RMS** at the bottom show the input level.
  Peak is the loudest moment; RMS is the average level
  ([what RMS means](https://en.wikipedia.org/wiki/Root_mean_square)). **CLIP**
  lights up when the signal is too loud, and **drops** counts gaps in the
  listening stream. This bottom meter comes from the APO's post-DSP audio tap.
- The selected effect's panel has its own live graph from the APO: Input
  spectrum, Gain input/output peak dot, Gate/Expander or Compressor/Limiter
  engagement and output, Equalizer spectrum with static/live/limit band curves,
  Saturation operating point and output, or Tuner note, cents, and a C0–C8 pitch
  map. The level graphs include a thin output meter with green/yellow/red zones.
  Gain, Saturation, and the meter use short holds so brief sounds are not skipped
  between polls. Polling these graphs never changes settings or stops Listen.
  Echo, Chorus, Tremolo, Vibrato, Pitch Shifter, and Looper have controls but no
  live graph; the graph is hidden for those effects.
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
- **Echo** repeats the signal through a damped delay. **Stereo** keeps one loop
  per channel; **Ping-pong** alternates repeats between channels. The dry voice
  stays at unity and **Level** sets only the wet repeats.
- **Chorus** thickens the sound with several detuned, slowly swept delays. More
  **Voices** sound denser, **Rate** sets the sweep speed, **Depth** sets how far
  each delay moves, and **Width** spreads the voices across the stereo field.
- **Tremolo** cycles the level with a **Rate**, **Depth**, and **Waveform** of
  Sine, Triangle, or Square. **Stereo spread** moves the pulse between speakers;
  Depth 0 leaves the audio untouched.
- **Vibrato** cycles the pitch with the same waveforms. Its **Depth** is in
  cents, so small values sound like natural vocal or guitar vibrato. It buffers
  audio while active, and Depth 0 passes the signal through.
- **Pitch Shifter** covers octave shifts and other intervals with one effect.
  **Shift** ranges from −24 to +24 semitones and defaults to −12. Choose
  **Granular**, **WSOLA**, or **Phase vocoder**; **Window** is a sample count,
  and Phase vocoder rounds it up to the next power of two. **Dry level** scales
  the unshifted signal (100% by default) and **Level** sets the shifted return.
- **Tuner** is monitoring-only: it shows the nearest note, cents, and a C0–C8
  pitch map without changing or delaying the audio. **Smoothing** averages
  recent readings in cents; 100% holds about two seconds with the default
  window, while 50% stays much quicker.
- **Looper** records five tracks, each up to **120 s**. With Looper selected,
  keys **1**–**4** and **Space** tap tracks 1–5; the buttons also accept tap,
  double tap, and long press. Keyboard recording starts and finishes on the
  first press, with no double-tap delay; a later double tap stops, while holding
  a newly started take discards it. A tap starts recording, finishes a take,
  overdubs, or plays a stopped track; recording wraps and layers until you tap
  again. Double tap stops; long press undoes the last take or clears a stopped
  track.
  **Free length** keeps the exact take length. Otherwise the first synced take
  sets the reference and later takes snap to `1/8`, `1/4`, `1/2`, `1`, `2`, `4`,
  or `8` times it. **Stop at loop end** is off by default. Undo is one level and
  only for the track that recorded last; takes are saved per device and come
  back after audio restarts and GUI reopen.
- If your device is missing from the list, turn on **View → Show all devices**
  to see inactive inputs too.
- Windows briefly stops audio when effects are installed or attached to a
  device. That is Windows reloading the audio chain. Settings changes and noise
  profile captures apply to the running chain without stopping audio.
- Noise reduction, gate/compressor/equalizer lookahead, and reverb predelay/tail buffer
  a little audio. Echo, Chorus, Vibrato, and Pitch Shifter buffer more: Echo adds
  roughly its **Time** before the first repeat, Chorus adds a small variable
  delay, Vibrato keeps a center delay while active, and Pitch Shifter uses its
  **Window** for analysis. Tremolo changes level without buffering. Looper
  keeps its takes and one undo bank in memory and saves them to a per-device
  `.looper` bank at shutdown; active buffers use about 352 MiB at 48 kHz
  stereo. The lookahead and predelay controls are marked with ⏱.
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
