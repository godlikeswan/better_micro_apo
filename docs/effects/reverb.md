# Reverb

Adds a sense of space — a small room or a hall — with a tail that keeps going
after you stop talking. It is most often used for music, game voice, or special
effects; use it sparingly on speech or the words get muddy.

## Try this

- Put Reverb last in the chain, after cleanup and level effects, so you are not
  also reverberating noise. It works well when **Treat as mono** is checked on
  the [Input](input.md) row, because a mono source copied to both channels still
  gets a wide tail.
- Start with **Level** around **10 to 20%** and talk normally. Level changes only
  the wet return; the dry voice is never attenuated. Raise it if you want an
  obvious room.
- **Algorithm** selects the tail generator:
  - **Comb/allpass** is the classic parallel-comb and series-allpass network.
    It is efficient and has a focused, slightly resonant character.
  - **Feedback delay network (FDN)** couples eight delay lines through a mixing
    matrix. It is smoother and more diffuse, and is usually the better choice
    for a natural-sounding hall.
- **Max length** sets the decay ceiling in seconds. This is the main tail-length
  control. Raise it for a long hall, lower it for a tight room. Changing it does
  not change the dry path or the wet level.
- **Size** trades low-end body. Low values cut the low band for a tight, small
  sound; high values boost it for a big, warm room. It no longer sets tail
  length; Max length does that.
- **Damping** absorbs high frequencies in the tail's feedback loop. It has a
  wide range in FDN mode. Turn it up if the reverb sounds harsh or clangy; turn
  it down if the voice sounds dull behind it.
- **Width** controls how different the left and right tails are. Low values keep
  the tail centered; high values make it wrap around the listener.
- **Density** controls diffusion. Low values leave more discrete echoes; high
  values smooth the tail into a wash.
- **Predelay** puts a short gap before the tail starts. Small values can make a
  room feel bigger without muddying the voice, but they add the same amount of
  delay to the stream. The ⏱ marker on the slider is there as a reminder — keep
  it low or at 0 ms when latency matters.
- Reverb tail can add to the overall level. Check the bottom meters with
  **Listen to this device**: aim for peaks around **−5 dBFS**, RMS around
  **−20 dBFS**, and no **CLIP**. If the voice gets lost, lower Level first.
- A newly added Reverb uses Comb/allpass, 20% Level, 2.0 s Max length, 50%
  Size, 50% Damping, 50% Width, 50% Density, and 20 ms Predelay. The small ↺
  buttons reset individual controls. At most 4 instances are supported.

## Controls

| Control | Default | Range | What it does |
| --- | --- | --- | --- |
| Algorithm | Comb/allpass | Comb/allpass, FDN | Selects the tail generator. |
| Level | 20% | 0 to 100% | Wet return level. Dry stays at unity. |
| Max length | 2.0 s | 0.2 to 8.0 s | Decay ceiling for the tail. |
| Size | 50% | 0 to 100% | Room weight and low-end body. |
| Damping | 50% | 0 to 100% | High-frequency absorption in the tail. |
| Width | 50% | 0 to 100% | Stereo decorrelation of the tail. |
| Density | 50% | 0 to 100% | Diffusion of the tail. |
| Predelay | 20 ms | 0 to 200 ms | Gap before the tail starts; adds the same latency while enabled. |

## Under the hood

`ReverbStage` (`src/dsp/stages.cpp`):

- Both engines share predelay, wet level, damping, stereo shaping, and a wet low
  shelf for Size. The dry path is always added at unity; Level only scales the
  wet return.
- **Comb/allpass** gives each channel a tank of 8 damped combs (classic tunings
  scaled to the stream's sample rate) followed by 4 allpass filters. Odd
  channels get the usual stereo spread offset. Density changes the allpass
  coefficient; Width changes how much of the mono sum feeds each tank.
- **FDN** uses 8 incommensurate delay lines and an orthogonal mixing matrix
  built from pairwise rotations. Density raises the rotation angle, so the tail
  becomes more diffuse without losing level at halfway settings. Mono excitation
  feeds the common mode so it projects strongly onto the stereo output; Width
  shapes opposite left/right output taps.
- In FDN mode, Size applies a signed gain to the wet low band: about −14 dB at
  0%, neutral at 50%, and a clear boost at 100%. Damping independently controls
  high-frequency absorption in the feedback loop with a wide range.
- Max length is converted to a feedback gain from the average loop time, giving
  an approximately −60 dB decay over the chosen time. Size only changes the wet
  tone, so length and body are independent controls.
- Comb/allpass, FDN, and predelay buffers are allocated in `Configure`. A live
  `Update` only recomputes coefficients and taps, so changing any control never
  allocates. Switching Algorithm resets both tails to avoid replaying old audio
  through a different network.
- `NeedsSilence()` is true while enabled with a nonzero Level so the tail is
  flushed through silent buffers instead of being cut off or replayed later.
- `Process` itself is allocation-free. There is no graph for reverb; the
  transfer-curve control is used by Saturation/Overdrive/Distortion,
  Gate/Expander, and Compressor/Limiter.
