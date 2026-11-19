# Gate/Expander

Quiets signal below a threshold. Use it to reduce room noise between phrases
without changing the level of speech above the threshold. There is no mode
selector: a finite **Ratio** behaves as an expander, and the right end stop at
**∞** is a hard gate.

## Try this

- Put Gate/Expander after [Noise reduction](noise-reduction.md) and before
  [Compressor/Limiter](compressor-limiter.md). This removes low-level noise first,
  then levels what remains.
- Start at the default **−15 dBFS** threshold and **5:1** ratio. Sit quietly and
  raise the threshold until the background is reliably attenuated, then talk to
  make sure soft words still come through. The transfer-curve graph shows where
  attenuation starts, the **ENG** bar shows how hard the gate is working, and the
  **OUT** meter shows the output level.
- Set **Ratio** higher for firmer gating. The slider runs from **0.01** to **∞**:
  the last finite position is 1000, and the right edge snaps to infinity (shown
  as `∞`, stored as `inf`). At infinity the gain below the threshold is cut to
  −96 dB, a hard gate. Ratios below 1 lift quiet signal instead of holding it
  back.
- **Input** is applied before the detector and the signal; **Output** is applied
  after the gate. Use Output for makeup level instead of adding a second
  [Gain](gain.md) when you only want to compensate this effect.
- **Attack** opens the gate when speech returns; **Release** closes it after the
  level falls below the threshold. **Hold** keeps the gate open briefly after
  the level drops, delaying closure; a signal above the threshold cancels the
  hold immediately. The hold includes the lookahead, so it is measured at the
  output.
- **Lookahead** lets the detector see a transient before it reaches the output,
  which helps leading consonants stay audible. It delays the stream by the same
  amount, so keep it low or at 0 ms when latency matters. The ⏱ marker is a
  reminder.
- A newly added instance starts at −15 dBFS, 5:1, 0 dB input and output, 5 ms
  attack, 100 ms release, 50 ms hold, and 0 ms lookahead. The small ↺ buttons
  reset individual controls. At most 4 instances are supported.

## Controls

| Control | Default | Range | What it does |
| --- | --- | --- | --- |
| Input | 0 dB | −24 to +30 dB | Trim before the detector and the signal path. |
| Threshold | −15 dBFS | −96 to +24 dBFS | Level where attenuation starts. |
| Ratio | 5:1 | 0.01:1 to ∞ | Slope below the threshold; 1 is unity and ∞ is a hard gate. |
| Attack | 5 ms | 0.1 to 1000 ms | Open when speech returns. |
| Release | 100 ms | 1 to 5000 ms | Close when the level falls below the threshold. |
| Hold | 50 ms | 0 to 2000 ms | Delay closing after the level drops. |
| Output | 0 dB | −24 to +24 dB | Trim after the gate. |
| Lookahead | 0 ms | 0 to 300 ms | Delay before processing; adds the same latency while enabled. |

## Under the hood

`DynamicsStage` (`src/dsp/stages.cpp`) also implements Compressor/Limiter, with
the transfer half fixed by the effect type rather than stored in settings.

- A linked peak detector (the loudest channel in each frame) feeds
  `DynamicsGainDb` in `src/dsp/transfer.h`. Gate/Expander computes a gain from
  signal below the threshold. The settled gain is limited to −96 dB of cut and
  +48 dB of boost.
- Input gain is applied before detection; output gain is applied after the gate.
  Attack is used when the gate opens and Release when it closes.
- Hold keeps the open state for `holdMs` plus the lookahead after the signal
  drops below the threshold, then closes with Release. A signal above the
  threshold cancels the hold immediately.
- With lookahead, the incoming frame enters a per-stage delay line while the
  detector computes the gain for the frame one lookahead behind it. The line is
  allocated in `Configure` for the 300 ms maximum, so changing lookahead live
  never allocates. `Reset` runs when enable state or lookahead changes.
- `NeedsSilence()` is true while enabled with a nonzero delay, so buffered audio
  is flushed through silent buffers rather than replayed later.
- `Process` and `Update` are allocation-free. The graph calls
  `DynamicsTransferDb` from the same transfer header and includes the input and
  output trim, so it matches the audio path.
- `MonitorValue()` reports `min(|gainDb| / 24, 1)`, so gate closure lights the
  **ENG** bar. The APO publishes that value together with output peak/RMS to
  `.monitor`. The GUI smooths the engagement value on a 100 ms poll; the meter
  uses APO-side short holds.
