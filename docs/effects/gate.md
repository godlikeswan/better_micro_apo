# Gate

A downward gate that attenuates the signal below a threshold, to silence room
noise between phrases. Optional lookahead lets the detector open the gate
before a transient reaches the output.

Typical placement: after noise reduction and before the compressor. At most 4
instances are supported.

## Parameters

| Parameter | Default | Clamp | Unit / meaning |
| --- | --- | --- | --- |
| Threshold | −50 dBFS | −80 to 0 dBFS | Level below which the gate starts closing. |
| Range | 60 dB | 0–96 dB | Maximum attenuation while closed. |
| Attack | 5 ms | 0.1–1000 ms | Time constant for opening (increasing gain). |
| Hold | 50 ms | 0–2000 ms | Time the gate stays open after the level falls. |
| Release | 100 ms | 1–5000 ms | Time constant for closing (reducing gain). |
| Lookahead | 0 ms | 0–300 ms | Delay before processing; adds the same latency while enabled. |

Newly added instances are enabled by `MakeEffect`.

## Implementation details

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
- `NeedsSilence()` is true while enabled with a nonzero delay, so buffered
  audio is flushed through silent buffers rather than replayed later.
- `Process` and `Update` are allocation-free.

## GUI notes

The parameter panel shows a live gate transfer curve drawn from the same
functions in `src/dsp/transfer.h` as the DSP. The lookahead slider carries a
latency marker (⏱) and the tooltip "Adds delay to the audio stream." Keep it at
0 ms when zero latency matters.
