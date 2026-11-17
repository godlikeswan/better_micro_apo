# Gate

Cuts the signal when it falls below a level, so room noise between phrases does
not keep hissing in the background. Optional lookahead can catch the first part
of a word before it reaches the output.

## Try this

- Put Gate after [Noise reduction](noise-reduction.md) and before
  [Compressor](compressor.md).
- Turn on **Listen to this device**, then sit quietly without speaking. Move the
  **Threshold** slider to the right until the background noise is reliably
  blocked. Talk at your normal volume and make sure soft words still come
  through. If the beginnings of words disappear, move the threshold back to the
  left.
- **Range** sets how much the gate closes. The default 60 dB is a strong mute;
  lower it if the silence sounds unnaturally abrupt.
- If the gate chops the start of words, lower **Attack** so it opens faster, or
  add a little **Lookahead**. Lookahead lets the detector see a transient before
  it reaches the output, which helps leading consonants like *s*, *t*, and *k*
  stay audible. Be careful with it: lookahead adds the same amount of delay to
  the stream, so keep it low or at 0 ms when latency matters. The ⏱ marker on the
  slider is there as a reminder.
- **Hold** keeps the gate open briefly after you stop talking, which stops it
  clicking shut between short words. **Release** controls how smoothly it
  closes afterward.
- A newly added Gate starts with a −50 dBFS threshold and no lookahead. The
  little ↺ buttons reset individual controls. At most 4 instances are
  supported.

## Controls

| Control | Default | Range | What it does |
| --- | --- | --- | --- |
| Threshold | −50 dBFS | −80 to 0 dBFS | Level below which the gate starts closing. |
| Range | 60 dB | 0 to 96 dB | Maximum attenuation while closed. |
| Attack | 5 ms | 0.1 to 1000 ms | Time constant for opening (increasing gain). |
| Hold | 50 ms | 0 to 2000 ms | Time the gate stays open after the level falls. |
| Release | 100 ms | 1 to 5000 ms | Time constant for closing (reducing gain). |
| Lookahead | 0 ms | 0 to 300 ms | Delay before processing; adds the same latency while enabled. |

## Under the hood

`GateStage` (`src/dsp/stages.cpp`):

- Uses a linked peak detector: the loudest channel of each frame decides the
  gate state, so a stereo source keeps its image.
- The detector's hold counter is extended by the lookahead length so the
  configured hold time is preserved at the output.
- Gain moves toward 1.0 (open) or `10^(-range/20)` (closed) with the one-pole
  attack/release coefficients from `TimeCoefficient`.
- With lookahead, the delay line holds the incoming frame while the gain
  computed from it is applied to the frame one lookahead behind. The line is
  allocated in `Configure` for the maximum clamp (300 ms × sample rate ×
  channels), so changing lookahead live never allocates. Changing enable state
  or lookahead length resets the line.
- `NeedsSilence()` is true while enabled with a nonzero delay, so buffered audio
  is flushed through silent buffers rather than replayed later.
- `Process` and `Update` are allocation-free. The parameter panel draws the
  live gate transfer curve from the same functions in `src/dsp/transfer.h` as
  the DSP.
