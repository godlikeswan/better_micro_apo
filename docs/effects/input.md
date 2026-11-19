# Input

Every preset starts with an **Input** effect at row 0. It is always first,
always enabled, and cannot be added, deleted, or unchecked. Use it to bring the
microphone into a useful range before the addable effects run.

Unlike [Gain](gain.md), Input starts at **0 dB** and is limited to **−24…+24 dB**.
Its row has no `×` delete cell, and the checkbox stays checked.

## Getting a good level

- Put on headphones, check **Listen to this device**, and talk at the volume you
  normally use.
- Select the Input row. The panel shows a live spectrum of the signal entering
  the rest of the chain, so you can see where the voice and the room noise sit.
- Raise **Input gain (dB)** until the spectrum and the thin output meter look
  comfortable. For speech, aim for peaks around **−5 dBFS** and RMS around
  **−20 dBFS** on the bottom Listen meter. The meter next to the graph follows
  the Input stage output, after the gain trim.
- If turning the mic up also makes room noise obvious, fix that with
  [Noise reduction](noise-reduction.md) or the unified
  [Gate/Compressor/Limiter/Expander](dynamics.md) effect instead of lowering the
  voice level again.
- The ↺ button resets Input gain to 0 dB.

## Controls

| Control | Default | Range | What it does |
| --- | --- | --- | --- |
| Input gain (dB) | 0 dB | −24 to +24 dB | First trim applied to every frame. |

## Under the hood

Input reuses `GainStage` (`src/dsp/stages.cpp`) with the Input-specific gain
default and clamp. `EffectEnabled()` returns `true` for it regardless of the
stored flag, and the GUI prevents row 0 from being deleted, reordered, or
disabled. `ClampSettings` inserts it at row 0 when reading a current settings
file.

The APO publishes Input's output peak/RMS and a smoothed 256-point spectrum to
the `.monitor` file. The GUI polls that channel and draws the spectrum plus a
green/yellow/red output meter. Polling is read-only: it does not change settings,
rebuild the chain, or stop Listen.
