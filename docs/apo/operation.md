# APO operation

This document follows a stream from `Initialize` to `APOProcess`, with emphasis
on what happens when a stream binds and what is left for the real-time path.
Source: `src/better_micro_apo.cpp`, `src/dsp/dsp.cpp`, `src/dsp/stages.cpp`,
`src/stage_monitor.cpp`.

## Initialize and endpoint discovery

`CBaseAudioProcessingObject::Initialize` validates the init structure and sets
the initialized flag. The APO then calls `EndpointIdFromInitialize`:

- Both `APOInitSystemEffects` and `APOInitSystemEffects2` share the same
  `pDeviceCollection` prefix, so either size is accepted.
- The **last** item of the collection is the endpoint `IMMDevice`.
- The last `{GUID}` in `IMMDevice::GetId()` is the endpoint id used for the
  settings and runtime files under ProgramData.

The init kind is logged for diagnostics (`system-effects2`, `system-effects`,
`base`, or `unknown`). `FinalRelease` closes the runtime and stage-monitor
mappings; `Reset` clears the chain and delegates to the base class.

## LockForProcess: bind-time setup

The audio engine calls `LockForProcess` when it locks the APO to a stream.
Sequence:

1. The base class validates connection counts, formats, and registration flags,
   caches format information, and sets `m_bIsLocked`.
2. The input format is read from `APO_CONNECTION_DESCRIPTOR` and checked:
   float32, PCM16, PCM24, and PCM32 are supported, with container size equal to
   valid-bit size. Unsupported formats fail with
   `APOERR_INVALID_CONNECTION_FORMAT`.
3. An `AudioFormat` is filled with channels, sample rate, container bytes,
   maximum frame count, and sample format.
4. The endpoint's INI (`devices\{endpoint-guid}.ini`) is loaded, clamped, and
   its `.noise` profile is analyzed — even when noise reduction is off, so it
   can be enabled live without a restart. The capture control block is also
   mapped for one-shot profile requests.
5. The runtime `.dat` snapshot is opened read-only if present and of the
   current version; its generation becomes the baseline (see
   [DLL ↔ GUI settings sharing](../runtime-sharing.md)).
6. The `.monitor` stage-telemetry file is opened read/write and its slots start
   empty. The APO is the only writer; the GUI maps it read-only.
7. Buffers are allocated (below) and `ConfigureDsp()` builds the chain. The
   fixed Input stage is bound first, followed by the enabled effects in list
   order; the temporary capture tap gets a sentinel binding when inserted.
8. A log line records format, endpoint, profile validity, and effect list.
   Any allocation failure unlocks the APO, closes the mappings, and returns
   `E_OUTOFMEMORY`.

### Allocations made at bind time

All of these happen once per stream bind; none happen in `APOProcess`.

| Owner | Allocation | Size / notes |
| --- | --- | --- |
| `BetterMicroApo` | `m_scratch` | `maxFrames * channels` `double`s. The interleaved input is decoded here, stages run in place, and the result is encoded to the output. |
| `Chain` | `m_stages` | Reserves `kMaxEffects + 2` (18) `Stage*` slots and monitor bindings at construction so the temporary capture tap never allocates; `Clear`/refill never allocates. |
| `StagePool` | stage objects | One stage per possible slot per type, `Configure`d once. Input allows 1, heavy types (noise, dynamics, reverb) allow 4, and other addable types allow 16. Noise stages are always created and pass audio through until a profile is available. |
| `DynamicsStage` | `m_delay` | `ceil(300 ms * sampleRate) * channels` `double`s, sized for the maximum clamp so a live lookahead change never allocates. |
| `EqualizerStage` | `m_ic1`, `m_ic2`, `m_band` | Per-channel filter and band state. |
| `NoiseStage` | FFT plans + analysis buffers | Radix-2 tables and windows for every supported size (256–4096), profile bins, gain arrays, input/output/history/overlap-add and real/imag arrays sized for the largest size. `Update` switches the active size without allocating. |
| `ReverbStage` | tank buffers + predelay | Per channel: 8 damped combs and 4 allpasses (tunings scaled to the sample rate, odd channels get the classic stereo spread) plus `ceil(200 ms * sampleRate) + 1` predelay frames. |
| `BetterMicroApo` | runtime file mapping | A read-only `MapViewOfFile` of `runtime\{endpoint-guid}.dat`, closed in `UnlockForProcess`. |
| `BetterMicroApo` | capture mapping and tap | A read/write mapping of `runtime\{endpoint-guid}.capture` plus one configured `StageCaptureTap`, both closed in `UnlockForProcess`. |
| `BetterMicroApo` | stage-monitor mapping | A read/write mapping of `runtime\{endpoint-guid}.monitor`, one shared 256-point FFT plan, and per-slot spectrum history. Opened in `LockForProcess`; `Close` publishes empty slots before unmapping in `UnlockForProcess`/`FinalRelease`. |

Input reuses `GainStage`; it needs no buffers beyond the general scratch buffer.
Gain, mono, and saturation stages also allocate no buffers at bind time; their
coefficients are derived in `Update`.

`UnlockForProcess` closes the runtime, capture, and monitor mappings, clears the
chain and pool, drops the profile and format, and releases the scratch buffer.

## APOProcess: the real-time path

`APOProcess` never allocates, locks, logs, or touches files. It:

1. Validates connections, lock state, format, buffer flags, and frame count
   against `maxFrames` (invalid buffers produce `BUFFER_INVALID` with zero
   frames).
2. Calls `PollRuntimeSettings()` and `PollStageCapture()`: one generation/state
   load in the common case. A new settings generation is validated and applied
   through `RebuildChain`. A capture request inserts the tap; a finished
   capture publishes bins and retunes the live noise stages.
3. Calls `StageMonitor::BeginBuffer()` so slots not observed in this buffer can
   be cleared at the end.
4. For `BUFFER_SILENT`, runs `Chain::ProcessSilence` with the monitor sink only
   when a stage needs silence (to flush a lookahead or reverb tail); otherwise
   the output is marked silent and zeroed if the engine did not supply an
   in-place buffer.
5. Otherwise `ProcessBuffer` calls `Chain::Process` with the monitor sink:
   decode interleaved bytes into the `double` scratch buffer, measure peak/RMS
   before and after each stage, call `Stage::Process`, publish the stage's
   `ObserveStage` metrics and optional spectrum, then encode and clip back into
   the output buffer. `EndBuffer()` clears unobserved slots.
6. Copies the input's valid frame count to the output.

Format conversion uses `DecodeSample`/`EncodeSample` in `src/dsp/dsp.cpp` and
the normalized `[-1, 1]` range. 24-valid-bits in a 32-bit container is not
supported.

## StagePool and chain updates

`StagePool::Configure` creates one stage per possible slot of each type, with
Input reusing the Gain stage implementation. `StagePool::Update` walks the
active preset in list order, skips disabled effects (Input is always enabled),
and assigns the n-th enabled instance of a type to the n-th pool slot. If a slot
now holds a different stable instance id, the stage is reset before being
retuned from the new instance. Each enabled effect then gets
`Stage::Update(effect, format)`, is added to the non-owning `Chain` with a
monitor binding (stable id, type, and spectrum flag), and is included in the
returned active count. Input and Equalizer request spectra; Dynamics supplies
engagement through `Stage::MonitorValue()`, and Equalizer supplies its signed
dynamic gain through `Stage::MonitorGainDb()`. The chain is cleared and refilled
without allocation, which is what makes live settings changes safe from the
real-time path. `RebuildChain` is the single place that fills the chain, so a
capture tap survives a settings change made while a profile is being recorded;
the tap is bound with an explicit sentinel that cannot match a real effect.

`StagePool::UpdateProfileBins` retunes every allocated noise stage from the
leading `NoiseProfile::binCount` bins without allocating or rebuilding the pool.
The stage maps that profile onto its active FFT size, so the profile stays
usable after an FFT-size change. A stage with no profile is a passthrough and
does not report `NeedsSilence()`.

Disabled addable effects are simply absent; the fixed Input stage is always
present at slot 0. Deleting and re-adding an effect produces a new id, so its
stage starts from a clean state; values are not preserved.

## Adding an effect type

1. Write a `Stage` subclass in `src/dsp/`.
2. Add its type to `EffectType` (`src/apo_settings.h`), its user-facing name,
   and its settings block/clamp in `src/apo_settings.cpp`.
3. Create it in `StagePool::Configure` and add the GUI controls in
   `gui/ui/structure.cpp` / `gui/ui/widgets.cpp`.
4. If it needs live graphics, give its chain slot a monitor binding and extend
   `ui::GraphSupported`/`PaintGraph`.
5. Write its page in the [list of effects](../effects/index.md).

No change to `APOProcess` is needed. Transfer curves shown in the GUI call the
same inline functions from `src/dsp/transfer.h` as the DSP, so the graph cannot
drift from what is applied.
