# Equalizer

Shapes one frequency band of the microphone signal. The default **−12 dB**
**Level** makes a gentle cut around 120 Hz, which tames the boomy low end you get
close to a mic. A positive Level makes an upward lobe instead. Only the chosen
band is changed; the rest of the voice is left alone.

## Try this

- Put Equalizer before [Gate/Expander](gate-expander.md) or
  [Compressor/Limiter](compressor-limiter.md), while the level still has its
  natural movement.
- Leave **Dynamic** at **No** to start. Talk normally and move **Level** down
  until the chesty low end is under control, or up if the voice needs body. The
  graph draws a signed bell around 0 dB at the vertical center: positive values
  go up, negative values go down, and the skirts return to 0 dB. A dot marks
  Level at the selected Frequency.
- Set **Dynamic** to **Compressor** to cut only loud lows. Talk at your normal
  volume and move **Threshold** to the right until the low-end thumps start
  getting held back. Lower **Range** if the sound gets thin. On the graph, the
  yellow curve follows the live cut and a dashed curve below zero marks the
  Range limit for the current band.
- Set **Dynamic** to **Expander** to lift quiet lows. Raise **Range** a little
  at a time until the distant voice has body without sounding muddy. Silence has
  nothing to lift, so the expander only reacts to a non-zero band. The yellow
  curve rises above the center line and a dashed curve above it shows the limit.
  When the band gets louder, Hold latches the resulting lower lift for a while,
  so a short peak is not lifted into clipping on its way out.
- **Frequency** sets which band is watched; 120 Hz is a useful starting point
  for a close voice. If the boom is around 200–300 Hz, move the slider there.
  **Q** controls width: a low Q around 0.7 acts like a broad shelf, while a
  higher Q targets a narrower problem.
- **Hold** is mode-aware. In Compressor mode it keeps a latched cut active so a
  short peak still gets the full attack ramp. In Expander mode it holds the
  *lowest* lift target: any time the band gets louder and the target drops, that
  lower value is latched for Hold plus Lookahead. A further drop re-latches an
  even lower value; the lift can only rise again after the timer expires. No
  threshold crossing is needed, so raising Hold keeps the boost lower for longer
  on ordinary loudness changes.
  **Lookahead** delays the audio so the detector can act before a transient
  reaches the output; it adds the same latency to the stream. Both start at 0,
  with Hold defaulting to 50 ms.
- **Ratio**, **Attack**, and **Release** work like the controls in the
  standalone Gate/Expander and Compressor/Limiter effects, but only on this
  band. Keep Ratio moderate and use a slower
  Release if the low end pumps between words.
- When Dynamic is **No**, the detector controls are greyed out but keep their
  values. Changing back to Compressor or Expander restores them.
- A newly added Equalizer starts at 120 Hz, Q 0.7, −12 dB Level, Dynamic **No**,
  a −45 dBFS threshold, 4:1 ratio, 18 dB range, 10 ms attack, 150 ms release,
  50 ms hold, and 0 ms lookahead. The little ↺ buttons reset individual
  controls. Up to 16 instances are supported.

## Controls

| Control | Default | Range | What it does |
| --- | --- | --- | --- |
| Frequency | 120 Hz | 20 to 20000 Hz | Center of the isolated band. |
| Q | 0.7 | 0.1 to 30 | Bandwidth; low Q is broad. |
| Level | −12 dB | −96 to +96 dB | Signed static shift: negative cuts, positive boosts. |
| Dynamic | No | No, Compressor, Expander | Chooses no dynamics, a cut of loud lows, or a lift of quiet lows. |
| Threshold | −45 dBFS | −96 to +24 dBFS | Level where the dynamics start acting. |
| Ratio | 4:1 | 1:1 to ∞ | Slope applied above/below the threshold; ∞ is a hard limiter/gate. |
| Range | 18 dB | 0 to 96 dB | Maximum dynamic cut or boost. |
| Attack | 10 ms | 0.1 to 1000 ms | Gain smoothing when the effect deepens. |
| Release | 150 ms | 1 to 5000 ms | Gain smoothing when it returns to zero. |
| Hold | 50 ms | 0 to 2000 ms | Holds a latched cut (Compressor) or the lowest lift target (Expander), including lookahead. |
| Lookahead | 0 ms | 0 to 300 ms | Delays audio so the detector can act early; adds latency. |

The controls are laid out in this order: Frequency/Q, Level/Dynamic,
Threshold/Ratio, Range/Attack, Release/Hold, then Lookahead.

## Under the hood

`EqualizerStage` (`src/dsp/stages.cpp`):

- A topology-preserving state-variable bandpass (Simper) isolates the band.
  Damping `k = 1/Q`, and the bandpass output is scaled by `k` to normalize its
  peak to unity at the center frequency. The frequency is limited to 49% of the
  sample rate.
- The band peak is linked across channels, then smoothed with a fast detector
  envelope (100 ms release) so the gain does not pulse at zero crossings. Its
  attack scales with the selected frequency to about half a band period,
  clamped to 0.05–1 ms: low bands keep the original 1 ms, while high bands react
  within a cycle instead of averaging short bursts away.
- **Compressor** computes a negative target from the level above the threshold;
  **Expander** computes a positive target from the level below it. Both are
  limited by `rangeDb` and by the ratio slope, then smoothed with the configured
  attack/release. With **No**, the dynamic gain is always 0 dB.
- Hold is mode-aware. Compressor latches a deeper cut for `holdMs` plus the
  lookahead time so short peaks receive the full attack ramp. Expander latches
  the lowest lift target: whenever the target drops because the band got louder,
  that lower value is held for `holdMs` plus the lookahead time, and a further
  drop re-latches an even lower value and restarts the timer. The lift can only
  rise after the timer expires. The threshold still sets the target's size but
  no longer starts the hold, so Hold acts on ordinary loudness changes too.
  Compressor uses Attack to deepen the cut and Release to recover; Expander
  uses Attack to remove the boost and Release to engage it.
- Lookahead delays both the incoming sample and its isolated band; the detector
  runs on the new band while its gain is applied to the matching delayed frame.
  The buffers are allocated in `Configure` for the 300 ms maximum, so changing
  lookahead live never allocates in `Process`.
- The resulting gain is added back to the band only. `levelDb` is always applied
  with it: `out = x + band * (DecibelsToLinear(gainDb + levelDb) - 1)`.
- Per-channel filter, delay, and band state are allocated and cleared in
  `Configure`/`Reset`. Changing enable state, mode, frequency, Q, or lookahead
  resets that state. `Process` and `Update` do not allocate.
- The stage exposes its signed `gainDb` through `MonitorGainDb()`; `.monitor`
  publishes it as an unsmoothed dB value. The Gain/Saturation overlays use the
  short cross-buffer holds instead.
- The graph normalizes the analog band-pass prototype to 0…1 and multiplies it
  by the signed Level, so the static curve peaks at Level in the center and
  returns to 0 dB on the skirts. A dot marks that peak at the selected Frequency.
  Response curves use a centered −96…+96 dB scale with labels at the plot's right
  edge; the spectrum behind them keeps the left-hand −96…+24 dBFS scale, and the
  plot rectangle does not move. With dynamics on, a yellow curve shows
  `dynamicGainDb × band shape` from the APO's current gain, and one dashed curve
  shows `−Range × band shape` for Compressor or `+Range × band shape` for
  Expander as the limit for that band. The live input spectrum and output peak
  meter come from the same `.monitor` channel.
