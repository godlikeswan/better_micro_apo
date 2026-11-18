# Gate/Compressor/Limiter/Expander

One dynamics effect with two modes. **Compressor/Limiter** acts on signal above
the threshold so loud words do not get too loud. **Expander/Gate** acts on signal
below the threshold so room noise between phrases is quieted. Both modes share
the threshold, ratio, timing, input/output trim, and lookahead controls.

## Try this

- Put this effect after [Gain](gain.md) and any cleanup effects. Use
  [Noise reduction](noise-reduction.md) first if the background noise is a
  steady hiss or hum.
- Start in **Compressor/Limiter** mode with the default **−15 dBFS** threshold
  and **5:1** ratio. Talk normally; the transfer-curve graph shows where the
  gain starts changing. Raise the threshold if every word is squashed, or lower
  it if only the loudest peaks are affected.
- Set **Ratio** higher for a firmer ceiling or gate. The slider runs from
  **0.01** to **∞**: the last finite position is 1000, and the right edge snaps
  to infinity (shown as `∞`, stored as `inf`). In Compressor/Limiter mode,
  infinity is a hard limiter; in Expander/Gate mode it is a hard gate. Values
  below 1 move the affected side away from the threshold instead of holding it
  back.
- Switch **Mode** to **Expander/Gate** to attenuate signal below the threshold.
  Sit quietly and raise the threshold until the background is reliably blocked,
  then talk to make sure soft words still come through.
- **Input** is applied before the detector and the signal; **Output** is applied
  after the dynamics. Use Output for makeup level instead of adding a second
  Gain when you only want to compensate this effect.
- **Attack** and **Release** set the one-pole smoothing. Shorter attack catches
  a transient; longer release avoids a jumpy or pumped sound between words.
- **Hold** keeps the computed gain change active after the signal stops demanding
  it, so a short peak still gets the full attack ramp before release. Raise it if
  brief loud sounds still get through; lower it for a more natural, less
  “grabby” sound. The hold includes the lookahead, so it is measured at the
  output.
- **Lookahead** lets the detector see a transient before it reaches the output,
  which helps leading consonants stay audible. It delays the stream by the same
  amount, so keep it low or at 0 ms when latency matters. The ⏱ marker is a
  reminder.
- A newly added instance starts in Compressor/Limiter mode at −15 dBFS, 5:1, 0 dB
  input and output, 5 ms attack, 100 ms release, 50 ms hold, and 0 ms lookahead.
  The small ↺ buttons reset individual controls. At most 4 instances are
  supported.

## Controls

| Control | Default | Range | What it does |
| --- | --- | --- | --- |
| Mode | Compressor/Limiter | Compressor/Limiter, Expander/Gate | Which side of the threshold is affected. |
| Input | 0 dB | −24 to +30 dB | Trim before the detector and the signal path. |
| Threshold | −15 dBFS | −80 to 0 dBFS | Level where the dynamics start acting. |
| Ratio | 5:1 | 0.01:1 to ∞ | Slope on the affected side; 1 is unity and ∞ is hard. |
| Attack | 5 ms | 0.1 to 1000 ms | Smoothing when the gain change deepens. |
| Release | 100 ms | 1 to 5000 ms | Smoothing when the gain change returns to unity. |
| Hold | 50 ms | 0 to 2000 ms | Keeps the computed gain change active before releasing. |
| Output | 0 dB | −24 to +24 dB | Trim after the dynamics. |
| Lookahead | 0 ms | 0 to 300 ms | Delay before processing; adds the same latency while enabled. |

## Under the hood

`DynamicsStage` (`src/dsp/stages.cpp`):

- A linked peak detector (the loudest channel in each frame) feeds the shared
  `DynamicsGainDb` function in `src/dsp/transfer.h`. Compressor/Limiter mode
  computes a gain from signal above the threshold; Expander/Gate mode computes
  one from signal below it. The settled gain is limited to −96 dB of cut and
  +24 dB of boost.
- Input gain is applied before detection; output gain is applied after the
  dynamics. Attack and release are chosen by the magnitude of the gain change,
  so both modes smooth correctly whether they are cutting or boosting.
- When a deeper gain change is detected, its target is latched and kept active
  for `holdMs` plus the lookahead. Attack ramps toward that latched target even
  if the peak has already gone, then release returns toward the live target.
  This lets short peaks receive the full attack ramp at the output.
- With lookahead, the incoming frame enters a per-stage delay line while the
  detector computes the gain for the frame one lookahead behind it. The line is
  allocated in `Configure` for the 300 ms maximum, so changing lookahead live
  never allocates. `Reset` runs when enable state, mode, or lookahead changes.
- `NeedsSilence()` is true while enabled with a nonzero delay, so buffered audio
  is flushed through silent buffers rather than replayed later.
- `Process` and `Update` are allocation-free. The graph calls
  `DynamicsTransferDb` from the same transfer header and includes the input and
  output trim, so it matches the audio path in either mode.
