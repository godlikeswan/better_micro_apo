# Tuner

Shows the detected pitch of the signal without changing it. Tuner is a
monitoring-only pass-through: it does not change gain, tone, stereo width, or
latency, and it never buffers audio into the output. It analyzes a mono sum of
the signal at its position in the chain, so placing it last shows the pitch of
what you hear after all other effects.

## Try this

- Add Tuner after the effects you want to tune against. A clean, monophonic
  source works best. With silence, broad noise, or chords, the display holds the
  last clear reading and marks it **LAST** instead of jumping back to A4. Before
  the first clear reading it shows **NONE** with dashes.
- **Algorithm** starts at **Autocorrelation**, which favors the fundamental of
  guitar, voice, and similar sources. Choose **FFT peak** for a faster display
  that can follow stronger harmonics more readily.
- **Sample size** is the analysis window in samples. **8192** (about 171 ms at
  48 kHz) is the default; smaller values react faster but wobble more, while
  larger values are steadier and slower. The detector uses overlapping windows,
  so the readout updates about four times per window length.
- **Smoothing** averages recent readings in cents, so small pitch changes
  behave the same in every octave. **0%** is the raw estimate, **50%** is the
  default and stays quick, and **100%** averages about **two seconds** with the
  default 8192-sample window. One-octave corrections are smoothed instead of
  snapping; only jumps larger than two octaves start a new average. Pitch
  detection starts at about 55 Hz (A1), so sub-audio rumble cannot capture the
  display.
- **Reference A4 (Hz)** calibrates note names and cents. Leave it at **440 Hz**
  for standard tuning, or move it within **430–450 Hz** to match a differently
  tuned ensemble.
- The upper strip is **2.5 semitones** wide. The detected pitch stays fixed at
  the center while bold, enlarged neighboring note labels slide past it; only
  labels that fit fully are drawn. The bright center line marks the current
  pitch. Fixed-width readout columns show status (**LIVE**, **LAST**, or
  **NONE**), the nearest note, cents offset, frequency, and clarity, so values
  do not move each other when their length changes.
- The lower strip is a thin **C0–C8** map with octave labels. A transparent
  rectangle with a thicker border shows where the local strip currently sits.
- A newly added Tuner uses Autocorrelation, 8192 samples, 50% smoothing, and a
  440 Hz reference. The small ↺ buttons reset individual controls. At most 4
  instances are supported.

## Controls

| Control | Default | Range | What it does |
| --- | --- | --- | --- |
| Algorithm | Autocorrelation | Autocorrelation, FFT peak | Selects the pitch estimator. |
| Sample size | 8192 samples | 2048, 4096, 8192, 16384, 32768 | Analysis window length; larger is steadier but slower. |
| Smoothing | 50% | 0 to 100% | Cents-domain smoothing of successive estimates. |
| Reference A4 (Hz) | 440 Hz | 430 to 450 Hz | Tuning reference used for note names and cents. |

## Under the hood

`TunerStage` (`src/dsp/stages.cpp`):

- `Process` never writes to the sample buffer. It averages channels into a
  preallocated mono window, applies a one-pole 45 Hz high-pass to that analysis
  copy only, and runs a detector whenever a full window is available. Windows
  overlap by 75%, which keeps the display responsive without changing the audio
  stream. `NeedsSilence()` is false, so Tuner never requests a silence flush.
- `Configure` allocates the analysis arrays for the largest sample size, builds
  FFT plans for every supported size, and initializes the rumble filter from the
  sample rate. A live `Update` only switches the active size, settings, and
  filter coefficient; changing Algorithm, Sample size, or sample rate resets the
  analysis, but does not allocate.
- **Autocorrelation** decimates the window to about 12 kHz, then uses a
  cumulative-mean normalized difference function (the YIN cost) over the
  plausible lag range, from about 55 Hz (or the short window's resolution limit)
  up to 4000 Hz. It chooses the first clear
  local minimum, then promotes a two- or three-times-longer dip when that dip is
  much deeper, which recovers a fundamental masked by a strong upper harmonic.
  The chosen lag is refined with parabolic interpolation and converted to
  frequency. Confidence comes from how deep that minimum is.
- **FFT peak** applies a Hann window, finds the largest bin between about 55 and
  4000 Hz, and prefers a strong peak at half or a third of that frequency when
  the loudest bin looks like a harmonic. Interpolation uses the log magnitudes
  of neighboring bins. Confidence combines the peak's rise above the local
  average with the strength of any promoted subharmonic.
- Detected frequency is converted to a MIDI-style note number using the
  configured A4 reference. Smoothing averages that note value in cents over a
  time-stamped ring: `2 seconds * amount^3`, so 100% holds about two seconds and
  50% about 0.25 seconds. The ring is sized once in `Configure` for the longest
  window at the fastest detection rate, so live smoothing changes never
  allocate. Only jumps of 24 semitones or more, or an invalid reading, flush the
  average, so an octave correction does not snap. The GUI derives note name and
  cents from the published frequency, so the display and the Reference control
  cannot disagree.
- Telemetry is explicit: `Stage::MonitorTuner()` publishes frequency,
  confidence, and validity, and `StageMonitorSlot` carries
  `tunerFrequencyHz` / `tunerConfidence` plus
  `kStageMonitorFlagTunerValid`. The GUI only reads this channel; polling it
  never writes settings, rebuilds the chain, or stops Listen.
- Settings use the `tuner_algorithm`, `tuner_sample_size`, `tuner_smoothing`, and
  `tuner_reference_hz` keys. Tuner arrived in schema version 11; version 12
  added Echo, Chorus, and Pitch Shifter, version 13 added Pitch Shifter's Dry
  level, version 14 added Tremolo and Vibrato, and version 15 moved mono copying
  to the [Input](input.md) row. A file with any other version is deleted rather
  than migrated.
