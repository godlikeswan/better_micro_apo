# Input

Every preset starts with an **Input** effect at row 0. It is always first,
always enabled, and cannot be added, deleted, or unchecked. Use it to bring the
microphone into a useful range before the addable effects run.

Unlike [Gain](gain.md), Input starts at **0 dB** and is limited to **−24…+24 dB**.
Its row has no `×` delete cell, and the checkbox stays checked. Its **Treat as
mono** checkbox copies the first channel to every other channel at this point in
the chain, so a mono mic on input 1 of a stereo interface feeds all downstream
effects and both output sides. It replaces the old standalone Mono effect.

## Getting a good level

- Put on headphones, check **Listen to this device**, and talk at the volume you
  normally use.
- Select the Input row. The panel shows a live spectrum of the signal entering
  the rest of the chain, so you can see where the voice and the room noise sit.
- Raise **Input gain (dB)** until the spectrum and the thin output meter look
  comfortable. For speech, aim for peaks around **−5 dBFS** and RMS around
  **−20 dBFS** on the bottom meter from the APO audio tap. The meter next to the graph follows
  the Input stage output, after the gain trim.
- If turning the mic up also makes room noise obvious, fix that with
  [Noise reduction](noise-reduction.md), [Gate/Expander](gate-expander.md), or
  [Compressor/Limiter](compressor-limiter.md) instead of lowering the voice level
  again.
- Check **Treat as mono** when only the first input channel carries the mic. It
  applies before every addable effect, so stereo widening, reverb, and echo see
  the same signal on both sides. It does nothing on an already-mono stream.
- The ↺ button resets Input gain to 0 dB. The checkbox has no reset arrow; clear
  it to turn mono copying off.

## Controls

| Control | Default | Range | What it does |
| --- | --- | --- | --- |
| Input gain (dB) | 0 dB | −24 to +24 dB | First trim applied to every frame. |
| Treat as mono | Off | Off / On | Copy the first channel to all channels at the Input stage. |

## Under the hood

Input reuses `GainStage` (`src/dsp/stages.cpp`) with the Input-specific gain
default and clamp. The same stage copies channel 0 to the remaining channels
when `gain.treatAsMono` is set and the effect type is Input; standalone Gain
forces that flag off in `ClampEffect`. The flag is stored as the
`input_treat_as_mono` key on the Input row and is clamped only by the settings
version check, so no numeric range applies. `EffectEnabled()` returns `true` for
Input regardless of the stored flag, and the GUI prevents row 0 from being
deleted, reordered, or disabled. `ClampSettings` inserts it at row 0 when
reading a current settings file.

The APO publishes Input's output peak/RMS and a smoothed 256-point spectrum to
the `.monitor` file. The GUI polls that channel and draws the spectrum plus a
green/yellow/red output meter. Polling is read-only: it does not change settings,
rebuild the chain, or stop Listen.
