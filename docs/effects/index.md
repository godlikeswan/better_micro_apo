# Effects

Every preset starts with a fixed [Input](input.md) row. The table lists the
addable types in Add effect menu order; Input is generated automatically and is
not offered in that menu. The other rows can be added in any order and are
processed top to bottom in the list.

| Effect | Type | What it does |
| --- | --- | --- |
| [Input](input.md) | Fixed | First in every chain, always enabled; trims the incoming level, can treat the stream as mono, and shows a live input spectrum. |
| [Gain](gain.md) | Level | Turns the whole signal up or down, starting at +12 dB. |
| [Noise reduction](noise-reduction.md) | Cleanup | Hushes steady room noise such as hiss, fans, and air conditioning. |
| [Gate/Expander](gate-expander.md) | Dynamics | Quiets signal below a threshold; finite Ratio expands, ∞ is a hard gate. |
| [Compressor/Limiter](compressor-limiter.md) | Dynamics | Reduces signal above a threshold; finite Ratio compresses, ∞ is a hard limiter. |
| [Equalizer](equalizer.md) | Dynamics | Tames boomy close-mic lows or fills out thin, distant ones in a chosen band. |
| [Saturation/Overdrive/Distortion](saturation.md) | Tone | Adds soft saturation or a harder overdriven edge. |
| [Reverb](reverb.md) | Space | Adds a room or hall tail so the voice sounds less dry. |
| [Echo](echo.md) | Space | Repeats the signal with a damped Stereo or Ping-pong delay. |
| [Chorus](chorus.md) | Modulation | Thickens the sound with several detuned, LFO-modulated delays. |
| [Tremolo](tremolo.md) | Modulation | Cycles the level with a Sine, Triangle, or Square LFO. |
| [Vibrato](vibrato.md) | Modulation | Cycles the pitch with the same LFO shapes and a stereo spread. |
| [Pitch Shifter](pitch-shifter.md) | Pitch | Shifts pitch with Granular, WSOLA, or Phase vocoder engines. |
| [Tuner](tuner.md) | Analysis | Shows detected pitch, cents, and note range without changing audio. |

Add the remaining effects in any order, use the checkboxes to switch individual
effects on or off, and click a row to adjust its settings. The list is processed
from top to bottom, and the same addable effect can be added more than once.
Input stays at row 0 with no checkbox or delete cell. Each device has four
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
| `MonitorValue()` | Every monitored buffer | Optional 0..1 stage-specific engagement value; Gate/Expander and Compressor/Limiter report gain change, others return zero. |
| `MonitorGainDb()` | Every monitored buffer | Optional signed gain change in dB; Equalizer reports its current dynamic cut/boost, others return zero. |
| `MonitorTuner(data)` | Every monitored buffer | Optional pitch measurement (frequency, confidence, validity); Tuner reports it, others leave it empty. |
| `Process(samples, frames, channels)` | Every `APOProcess` | Process `frames * channels` interleaved `double` values in `[-1, 1]`, in place. Real-time safe. |

`Chain` holds non-owning `Stage*` pointers in list order. `Chain::Process`
decodes the interleaved input into a preallocated scratch buffer, runs each
stage in place, then encodes and clips the result. `Chain::ProcessSilence`
feeds zeros through the chain when any stage reported `NeedsSilence()`, so a
lookahead delay, reverb tail, echo repeat, chorus history, or pitch-shift grain
is flushed instead of being replayed later.

When a monitor sink is present, the chain measures peak and RMS immediately
before and after each stage, calls `ObserveStage` with that stage's binding, and
clears slots that were not observed in the buffer. Tuner publishes its pitch
through `MonitorTuner()`. The sink only observes; it never changes samples.

`StagePool` owns one stage per possible instance of each type. `Update` walks
the active preset, skips disabled effects (Input is always enabled), assigns the
n-th enabled instance of a type to the n-th pool slot, resets a slot whose stable
instance id changed, retunes it with `Update`, and refills the chain without
allocating. Each added stage gets a monitor binding with its stable effect id and
type; the temporary capture tap gets an explicit sentinel so the GUI cannot
mistake it for an effect. That is why effect edits can be applied from the
runtime polling path.

### Limits and defaults

- The list holds at most `kMaxEffects` (16) effects in total, including the
  fixed Input row.
- Input allows one instance; Noise reduction, Gate/Expander,
  Compressor/Limiter, Reverb, Echo, Chorus, Pitch Shifter, and Tuner are heavy
  and allow at most 4 instances each (`kMaxHeavyEffects`). Tremolo and Vibrato
  are lightweight and allow up to 16 instances each (`kMaxEffects`), still
  within the 16-row list limit that includes Input. Other addable types can
  fill the remaining slots.
- `apo::MakeEffect(type, id)` builds an instance from the type's struct defaults
  and **enables it**. The struct's own `enabled = false` is the "not set yet"
  value; a newly added effect in the GUI is always on. Input is forced on even
  if a stale file says otherwise.
- `ClampSettings` repairs non-finite values using the fallback defaults in each
  effect's page, clamps every number to its listed range (the Gate/Expander,
  Compressor/Limiter, and Equalizer ratios also accept positive infinity),
  inserts or repairs Input at row 0, normalizes ids, and forces
  `isolateEffects = true`.
- Settings schema version 15 is the only accepted version. A file with a missing
  or mismatched `[dsp] version` is deleted on load instead of migrated.

### Graph and transfer math

The GUI draws an Input spectrum, a Gain input/output dot on a transfer curve,
Gate/Expander and Compressor/Limiter transfer curves with an engagement bar, an
Equalizer spectrum behind a static Level curve with a center-frequency dot, a
yellow live dynamic curve, and a dashed mode-dependent Range limit, and a
Saturation transfer curve with a mirrored operating point driven by the input
peak after Drive. The Tuner graph replaces the transfer curve with a
2.5-semitone local strip where the detected pitch stays centered and note ticks
slide past it, plus a thin C0–C8 map with a rectangle showing the local window.
The level graphs also
get a thin output meter with green, yellow, and red zones, dB labels, a
short-held fill driven by a 0.06 s peak hold, and a slightly thicker 0.6 s
peak-hold line whose colour matches the bar zone it sits in; both dynamics
effects get a separate engagement bar on the left. The Gain dot and Saturation
marker use the same short APO-side holds as the meter fill, so short sounds are
not skipped between 100 ms GUI polls. Input, Gain, Gate/Expander, and
Compressor/Limiter keep a vertical range of **−96 to +24 dBFS**. The Equalizer's
spectrum keeps that left-hand dBFS scale and
its labels, while its response curves use a centered **−96…+96 dB** scale with
+96/0/−96 marks just inside the right edge; the plot rectangle does not move.
Saturation uses normalized −1…1 transfer axes. Echo, Chorus, Tremolo, Vibrato,
and Pitch Shifter have no live graph; the graph control is hidden when one of
them is selected.

The saturation stage, both dynamics effects, and their graphs call the same inline
functions in `src/dsp/transfer.h`, so the displayed curve cannot drift from the
audio path. The equalizer graph normalizes the analog band-pass prototype
approximated by `EqualizerStage` and multiplies it by the signed **Level** for
the static curve, with a dot at the selected center frequency. The yellow live
curve multiplies the published signed dynamic gain by the same shape, and the
single dashed curve shows **−Range** for Compressor or **+Range** for Expander.
Live overlays come from the APO's `.monitor` channel, not from the Listen stream;
the bottom Peak/RMS display is separate. See
[DLL ↔ GUI settings sharing](../runtime-sharing.md) for the telemetry format.

### Adding a new effect type

Write a `Stage` subclass, add its `EffectType`, name, settings block and clamps
in `src/apo_settings.*`, create it in `StagePool::Configure`, add GUI controls,
and add a page here. If the effect needs live graphics, give it a monitor
binding and extend `ui::GraphSupported`/`PaintGraph`. Non-audio telemetry can
extend `Stage::MonitorTuner()` and `StageMonitorSlot` instead. `APOProcess`
itself does not change.
