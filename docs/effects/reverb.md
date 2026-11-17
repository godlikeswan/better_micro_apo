# Reverb

Adds a sense of space — a small room or a hall — with a tail that keeps going
after you stop talking. It is most often used for music, game voice, or special
effects; use it sparingly on speech or the words get muddy.

## Try this

- Put Reverb last in the chain, after cleanup and level effects, so you are not
  also reverberating noise. It works well after [Mono](mono.md), because a mono
  source copied to both channels still gets a wide tail.
- Start with **Mix** around **10 to 20%** and talk normally. You want a little
  space around the voice, not a wash. Raise it if you want an obvious room.
- **Size** sets the decay length. Lower values sound close and tight; higher
  values sound like a larger room or hall.
- **Damping** absorbs high frequencies in the tail. Turn it up if the reverb
  sounds harsh or clangy; turn it down if the voice sounds dull behind it.
- **Predelay** puts a short gap before the tail starts. Small values can make a
  room feel bigger without muddying the voice, but they add the same amount of
  delay to the stream. The ⏱ marker on the slider is there as a reminder — keep
  it low or at 0 ms when latency matters.
- Reverb tail can add to the overall level. Check the bottom meters with
  **Listen to this device**: aim for peaks around **−5 dBFS**, RMS around
  **−20 dBFS**, and no **CLIP**. If the voice gets lost, lower Mix first.
- A newly added Reverb starts at 20% Mix, 50% Size, 50% Damping, and 20 ms
  Predelay. The small ↺ buttons reset individual controls. At most 4 instances
  are supported.

## Controls

| Control | Default | Range | What it does |
| --- | --- | --- | --- |
| Mix | 20% | 0 to 100% | Dry/wet balance. |
| Size | 50% | 0 to 100% | Comb feedback, i.e. decay length. |
| Damping | 50% | 0 to 100% | High-frequency absorption in the tail. |
| Predelay | 20 ms | 0 to 200 ms | Gap before the tail starts; adds the same latency while enabled. |

## Under the hood

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
- `Process` itself is allocation-free. There is no graph for reverb; the
  transfer-curve control is used by distortion, compressor, gate, and dynamic.
