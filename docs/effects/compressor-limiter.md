# Compressor/Limiter

Reduces signal above a threshold so loud words do not get too loud. There is no
mode selector: a finite **Ratio** behaves as a compressor, and the right end stop
at **∞** is a hard limiter.

## Try this

- Put Compressor/Limiter after [Gate/Expander](gate-expander.md), so low-level
  noise is removed before what remains is leveled.
- Start at the default **−15 dBFS** threshold and **5:1** ratio. Talk normally;
  the transfer-curve graph shows where the gain starts changing, the **ENG** bar
  on its left shows how hard the compressor is working, and the **OUT** meter on
  the right shows the output level. Raise the threshold if every word is
  squashed, or lower it if only the loudest peaks are affected.
- Set **Ratio** higher for a firmer ceiling. The slider runs from **0.01** to
  **∞**: the last finite position is 1000, and the right edge snaps to infinity
  (shown as `∞`, stored as `inf`). Infinity is a hard limiter. Values below 1
  lift signal above the threshold instead of holding it back.
- **Input** is applied before the detector and the signal; **Output** is applied
  after the dynamics. Use Output for makeup level instead of adding a second
  [Gain](gain.md) when you only want to compensate this effect.
- **Attack** engages the gain change and **Release** recovers from it. **Hold**
  keeps a latched reduction active so short peaks still get the full attack
  ramp. The hold includes the lookahead, so it is measured at the output.
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
| Threshold | −15 dBFS | −96 to +24 dBFS | Level where the dynamics start acting. |
| Ratio | 5:1 | 0.01:1 to ∞ | Slope above the threshold; 1 is unity and ∞ is a hard limiter. |
| Attack | 5 ms | 0.1 to 1000 ms | Engage the gain change. |
| Release | 100 ms | 1 to 5000 ms | Recover from the gain change. |
| Hold | 50 ms | 0 to 2000 ms | Keep a latched reduction active. |
| Output | 0 dB | −24 to +24 dB | Trim after the dynamics. |
| Lookahead | 0 ms | 0 to 300 ms | Delay before processing; adds the same latency while enabled. |

## Under the hood

`DynamicsStage` (`src/dsp/stages.cpp`) also implements Gate/Expander, with the
transfer half fixed by the effect type rather than stored in settings.

- A linked peak detector (the loudest channel in each frame) feeds
  `DynamicsGainDb` in `src/dsp/transfer.h`. Compressor/Limiter computes a gain
  from signal above the threshold. The settled gain is limited to −96 dB of cut
  and +48 dB of boost.
- Input gain is applied before detection; output gain is applied after the
  dynamics. Attack is used when the gain change deepens and Release when it
  recovers.
- Hold latches a deeper reduction for `holdMs` plus the lookahead so short peaks
  receive the full attack ramp.
- With lookahead, the incoming frame enters a per-stage delay line while the
  detector computes the gain for the frame one lookahead behind it. The line is
  allocated in `Configure` for the 300 ms maximum, so changing lookahead live
  never allocates. `Reset` runs when enable state or lookahead changes.
- `NeedsSilence()` is true while enabled with a nonzero delay, so buffered audio
  is flushed through silent buffers rather than replayed later.
- `Process` and `Update` are allocation-free. The graph calls
  `DynamicsTransferDb` from the same transfer header and includes the input and
  output trim, so it matches the audio path.
- `MonitorValue()` reports `min(|gainDb| / 24, 1)`, so gain reduction lights the
  **ENG** bar. The APO publishes that value together with output peak/RMS to
  `.monitor`. The GUI smooths the engagement value on a 100 ms poll; the meter
  uses APO-side short holds.
