# Compressor

Keeps loud words from getting too loud. Use it when your voice jumps between
shouting and mumbling, or when a strong syllable is pushing the meters into the
red.

## Try this

- Put Compressor after [Gain](gain.md) and any cleanup effects. It is often
  followed by another Gain if the level comes down too much.
- **Threshold** decides when it starts working. Talk normally and set it so the
  meter dips only on the louder words. If everything sounds squashed, raise the
  threshold.
- Set **Ratio** around **2:1 to 4:1** to start. Higher values hold the level
  more firmly and can sound pumped or unnatural; lower values are gentler. The
  default 3:1 is a good middle ground.
- Use **Attack** to control how quickly it grabs a loud word. Shorter values
  catch the first part of a word; longer values let a little transient through.
  If it sounds like it is stepping on the start of every word, lengthen it.
- Use **Release** to control how quickly it lets go. Too short can sound jumpy;
  too long can keep the volume down between words. Aim for a smooth return
  between phrases.
- If the overall level drops after compressing, add or raise a [Gain](gain.md)
  effect after it. Watch the bottom meters: for speech, peaks around **−5 dBFS**
  and RMS around **−20 dBFS** are a good target, with no **CLIP**.
- A newly added Compressor starts at −58 dBFS, 3:1, 5 ms attack, and 100 ms
  release. The small ↺ buttons reset individual controls. Up to 16 instances are
  supported.

## Controls

| Control | Default | Range | What it does |
| --- | --- | --- | --- |
| Threshold | −58 dBFS | −60 to 0 dBFS | Level above which reduction starts. |
| Ratio | 3:1 | 1:1 to 20:1 | How firmly the level is held back. |
| Attack | 5 ms | 0.1 to 1000 ms | How quickly the gain comes down. |
| Release | 100 ms | 1 to 5000 ms | How quickly the gain comes back. |

## Under the hood

`CompressorStage` (`src/dsp/stages.cpp`):

- The detector is the peak of all channels in a frame (linked), so the stereo
  image is preserved.
- For a frame above the threshold, the target gain in dB is
  `(threshold - level) * (1 - 1/ratio)`, clamped to be non-positive.
- An envelope in dB moves toward the target with the one-pole attack/release
  coefficients from `TimeCoefficient`; attack is used when more reduction is
  needed, release when less is needed.
- The envelope is converted to linear gain and applied to every channel.
- Coefficients are recomputed in `Configure`/`Update`; the only state is the
  envelope, cleared in `Reset`. No buffers are allocated and `Process` is
  allocation-free.
- The parameter panel shows a live compressor transfer curve from the same
  inline function in `src/dsp/transfer.h` used by the stage.
