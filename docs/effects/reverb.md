# Reverb

A Freeverb-style room/hall reverb built from damped comb filters and allpass
filters. It adds a stereo tail even when the source is mono.

Typical placement: last in the chain, after mono, so a mono microphone copied
to both channels still produces a wide tail. At most 4 instances are supported.

## Parameters

| Parameter | Default | Clamp | Unit / meaning |
| --- | --- | --- | --- |
| Mix | 20 % | 0–100 % | Dry/wet balance. |
| Size | 50 % | 0–100 % | Comb feedback, i.e. decay length. |
| Damping | 50 % | 0–100 % | High-frequency absorption in the tail. |
| Predelay | 20 ms | 0–200 ms | Gap before the tail starts. |

Newly added instances are enabled by `MakeEffect`.

## Implementation details

`ReverbStage` (`src/dsp/stages.cpp`):

- Each channel has a tank of 8 damped combs (classic Freeverb tunings scaled to
  the stream's sample rate) followed by 4 allpass filters. Odd channels get the
  usual stereo spread offset.
- `Mix` crossfades dry and wet; `Size` sets comb feedback; `Damping` absorbs
  highs in the tail; `Predelay` delays the excitation.
- Comb/allpass buffers and a predelay line sized for the 200 ms maximum are
  allocated in `Configure`. `Update` only recomputes feedback, damping, mix,
  and the predelay read offset, so live changes never allocate.
- `NeedsSilence()` is true while enabled with a nonzero mix so the tail is
  flushed through silent buffers instead of being cut off or replayed later.
- `Process` itself is allocation-free.

## GUI notes

The Predelay slider carries a latency marker (⏱) and the tooltip "Adds delay
to the audio stream." There is no graph for reverb; the transfer-curve control
is used by distortion, compressor, gate, and dynamic instead.
