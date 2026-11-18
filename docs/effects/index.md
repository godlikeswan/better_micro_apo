# Effects

There is a set of common audio effects present. Here is the list, in the same
order used by the Add effect menu and a newly built chain:

| Effect | Type | What it does |
| --- | --- | --- |
| [Gain](gain.md) | Level | Turns the whole signal up or down, starting at +12 dB. |
| [Noise reduction](noise-reduction.md) | Cleanup | Hushes steady room noise such as hiss, fans, and air conditioning. |
| [Gate/Compressor/Limiter/Expander](dynamics.md) | Dynamics | Controls signal above or below a threshold in one effect, depending on mode. |
| [Equalizer](equalizer.md) | Dynamics | Tames boomy close-mic lows or fills out thin, distant ones in a chosen band. |
| [Mono](mono.md) | Channels | Copies the left channel to all channels for a mono mic plugged into a stereo input. |
| [Saturation/Overdrive/Distortion](saturation.md) | Tone | Adds soft saturation or a harder overdriven edge. |
| [Reverb](reverb.md) | Space | Adds a room or hall tail so the voice sounds less dry. |

Add them in any order, use the checkboxes to switch individual effects on or
off, and click a row to adjust its settings. The list is processed from top to
bottom, and the same effect can be added more than once. Each device has four
presets, so you can keep different effect sets for different situations. Every
effect page starts with what it does and what its controls mean; the
implementation notes come at the end.

---

## Implementation details

This section is background for anyone changing the code.

### The abstract Stage interface

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

### Limits and defaults

- The list holds at most `kMaxEffects` (16) effects in total.
- Noise reduction, the unified dynamics effect, and reverb are heavy: at most
  4 instances of each (`kMaxHeavyEffects`). Other types can fill the whole list.
- `apo::MakeEffect(type, id)` builds an instance from the type's struct defaults
  and **enables it**. The struct's own `enabled = false` is the "not set yet"
  value; a newly added effect in the GUI is always on.
- `ClampSettings` repairs non-finite values using the fallback defaults in each
  effect's page, clamps every number to its listed range (the dynamics ratio
  also accepts positive infinity), normalizes ids, and forces
  `isolateEffects = true`.
- Settings schema version 7 is the only accepted version. A file with a missing
  or mismatched `[dsp] version` is deleted on load instead of migrated.

### Graph and transfer math

The GUI draws a saturation transfer curve, a mode-aware transfer curve for the
unified dynamics effect, and the frequency-band shape for the equalizer. The
saturation and dynamics stages and their graphs call the same inline functions
in `src/dsp/transfer.h`, so the displayed curve cannot drift from the audio
path. The equalizer graph shows the analog band-pass prototype approximated by
`EqualizerStage`.

### Adding a new effect type

Write a `Stage` subclass, add its `EffectType`, name, settings block and clamps
in `src/apo_settings.*`, create it in `StagePool::Configure`, add GUI controls,
and add a page here. `APOProcess` itself does not change.
