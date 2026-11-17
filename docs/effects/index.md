# Effect reference

The DSP is a list of effects in processing order. The same type may appear more
than once, and the list order is the order in which the signal passes through
the stages. Each device has four independent presets; only the selected preset
is processed.

Page order in this index follows the `EffectType` enum, which is also the order
of a sensible default chain:

1. [Noise reduction](noise-reduction.md)
2. [Gate](gate.md)
3. [Dynamic (proximity)](dynamic.md)
4. [Compressor](compressor.md)
5. [Gain](gain.md)
6. [Mono](mono.md)
7. [Distortion](distortion.md)
8. [Reverb](reverb.md)

Each effect page follows the same structure: description, parameters with
defaults and clamps, implementation details, and GUI notes.

## The abstract Stage interface

All effects implement `apo::dsp::Stage` (`src/dsp/dsp.h`):

| Method | When it runs | Contract |
| --- | --- | --- |
| `Configure(format)` | `LockForProcess` only | Precompute format-dependent coefficients and allocate any buffers. |
| `Update(effect, format)` | Setup and runtime polling | Retune from one `EffectInstance` without allocating. |
| `Reset()` | Setup and when instance identity/shape changes | Clear internal state, keep configuration. |
| `NeedsSilence()` | Query from the chain | True when the stage buffers audio across calls and needs silent buffers to flush its tail. |
| `Process(samples, frames, channels)` | Every `APOProcess` | Process `frames * channels` interleaved `double` values in `[-1, 1]`, in place. Real-time safe. |

`Chain` holds non-owning `Stage*` pointers in list order. `Chain::Process`
decodes the interleaved input into a preallocated scratch buffer, runs each
stage in place, then encodes and clips the result. `Chain::ProcessSilence`
feeds zeros through the chain when any stage reported `NeedsSilence()`, so a
lookahead delay or reverb tail is flushed instead of being replayed later.

`StagePool` owns one stage per possible instance of each type. `Update` walks
the active preset, skips disabled effects, assigns the n-th enabled instance of
a type to the n-th pool slot, resets a slot whose stable instance id changed,
retunes it with `Update`, and refills the chain without allocating. That is why
effect edits can be applied from the runtime polling path.

## Limits and defaults

- The list holds at most `kMaxEffects` (16) effects in total.
- Noise reduction, gate, and reverb are heavy: at most 4 instances of each
  (`kMaxHeavyEffects`). Other types can fill the whole list.
- `apo::MakeEffect(type, id)` builds an instance from the type's struct defaults
  and **enables it**. The struct's own `enabled = false` is the "not set yet"
  value; a newly added effect in the GUI is always on.
- `ClampSettings` repairs non-finite values using the fallback defaults below,
  clamps every number to the listed range, normalizes ids, and forces
  `isolateEffects = true`.

## Graph and transfer math

The GUI draws a live curve for distortion, compressor, and gate, and the
frequency-band shape for dynamic. The distortion, gate, and compressor stages
and the graph call the same inline functions in `src/dsp/transfer.h`, so the
displayed curve cannot drift from the audio path. The dynamic graph shows the
analog band-pass prototype approximated by the stage's state-variable filter.

## Adding a new effect type

Write a `Stage` subclass, add its `EffectType`, name, settings block and clamps
in `src/apo_settings.*`, create it in `StagePool::Configure`, add GUI controls,
and add a page here. `APOProcess` itself does not change.
