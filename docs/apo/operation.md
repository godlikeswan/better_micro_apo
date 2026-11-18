# APO operation

This document follows a stream from `Initialize` to `APOProcess`, with emphasis
on what happens when a stream binds and what is left for the real-time path.
Source: `src/better_micro_apo.cpp`, `src/dsp/dsp.cpp`, `src/dsp/stages.cpp`.

## Initialize and endpoint discovery

`CBaseAudioProcessingObject::Initialize` validates the init structure and sets
the initialized flag. The APO then calls `EndpointIdFromInitialize`:

- Both `APOInitSystemEffects` and `APOInitSystemEffects2` share the same
  `pDeviceCollection` prefix, so either size is accepted.
- The **last** item of the collection is the endpoint `IMMDevice`.
- The last `{GUID}` in `IMMDevice::GetId()` is the endpoint id used for the
  settings and runtime files under ProgramData.

The init kind is logged for diagnostics (`system-effects2`, `system-effects`,
`base`, or `unknown`). `FinalRelease` closes the runtime mapping; `Reset`
clears the chain and delegates to the base class.

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
6. Buffers are allocated (below) and `ConfigureDsp()` builds the chain.
7. A log line records format, endpoint, profile validity, and effect list.
   Any allocation failure unlocks the APO, closes the mapping, and returns
   `E_OUTOFMEMORY`.

### Allocations made at bind time

All of these happen once per stream bind; none happen in `APOProcess`.

| Owner | Allocation | Size / notes |
| --- | --- | --- |
| `BetterMicroApo` | `m_scratch` | `maxFrames * channels` `double`s. The interleaved input is decoded here, stages run in place, and the result is encoded to the output. |
| `Chain` | `m_stages` | Reserves `kMaxEffects + 1` (17) `Stage*` slots at construction so the temporary capture tap never allocates; `Clear`/refill never allocates. |
| `StagePool` | stage objects | One stage per possible slot per type, `Configure`d once. Heavy types (noise, gate, reverb) allow 4 instances; all others 16. Noise stages are always created and pass audio through until a profile is available. |
| `GateStage` | `m_delay` | `ceil(300 ms * sampleRate) * channels` `double`s, sized for the maximum clamp so a live lookahead change never allocates. |
| `DynamicStage` | `m_ic1`, `m_ic2`, `m_band` | Per-channel filter and band state. |
| `NoiseStage` | FFT plans + analysis buffers | Radix-2 tables and windows for every supported size (256–4096), profile bins, gain arrays, input/output/history/overlap-add and real/imag arrays sized for the largest size. `Update` switches the active size without allocating. |
| `ReverbStage` | tank buffers + predelay | Per channel: 8 damped combs and 4 allpasses (tunings scaled to the sample rate, odd channels get the classic stereo spread) plus `ceil(200 ms * sampleRate) + 1` predelay frames. |
| `BetterMicroApo` | runtime file mapping | A read-only `MapViewOfFile` of `runtime\{endpoint-guid}.dat`, closed in `UnlockForProcess`. |
| `BetterMicroApo` | capture mapping and tap | A read/write mapping of `runtime\{endpoint-guid}.capture` plus one configured `StageCaptureTap`, both closed in `UnlockForProcess`. |

Gain, mono, and distortion stages allocate no buffers at bind time; their
coefficients are derived in `Update`.

`UnlockForProcess` closes the mapping, clears the chain and pool, drops the
profile and format, and releases the scratch buffer.

## APOProcess: the real-time path

`APOProcess` never allocates, locks, logs, or touches files. It:

1. Validates connections, lock state, format, buffer flags, and frame count
   against `maxFrames` (invalid buffers produce `BUFFER_INVALID` with zero
   frames).
2. Calls `PollRuntimeSettings()` and `PollStageCapture()`: one generation/state
   load in the common case. A new settings generation is validated and applied
   through `RebuildChain`. A capture request inserts the tap; a finished
   capture publishes bins and retunes the live noise stages.
3. For `BUFFER_SILENT`, runs `Chain::ProcessSilence` only when a stage needs
   silence (to flush a lookahead or reverb tail); otherwise the output is
   marked silent and zeroed if the engine did not supply an in-place buffer.
4. Otherwise `ProcessBuffer` calls `Chain::Process`: decode interleaved bytes
   into the `double` scratch buffer using the stage format, run every stage in
   place, then encode and clip back into the output buffer.
5. Copies the input's valid frame count to the output.

Format conversion uses `DecodeSample`/`EncodeSample` in `src/dsp/dsp.cpp` and
the normalized `[-1, 1]` range. 24-valid-bits in a 32-bit container is not
supported.

## StagePool and chain updates

`StagePool::Configure` creates one stage per possible slot of each type.
`StagePool::Update` walks the active preset in list order, skips disabled
effects, and assigns the n-th enabled instance of a type to the n-th pool slot.
If a slot now holds a different stable instance id, the stage is reset before
being retuned from the new instance. Each enabled effect then gets
`Stage::Update(effect, format)` and is added to the non-owning `Chain`. The
chain is cleared and refilled without allocation, which is what makes live
settings changes safe from the real-time path. `RebuildChain` is the single
place that fills the chain, so a capture tap survives a settings change made
while a profile is being recorded.

`StagePool::UpdateProfileBins` retunes every allocated noise stage from the
leading `NoiseProfile::binCount` bins without allocating or rebuilding the pool.
The stage maps that profile onto its active FFT size, so the profile stays
usable after an FFT-size change. A stage with no profile is a passthrough and
does not report `NeedsSilence()`.

Disabled effects are simply absent from the chain. Deleting and re-adding an
effect produces a new id, so its stage starts from a clean state; values are not
preserved.

## Adding an effect type

1. Write a `Stage` subclass in `src/dsp/`.
2. Add its type to `EffectType` (`src/apo_settings.h`), its user-facing name,
   and its settings block/clamp in `src/apo_settings.cpp`.
3. Create it in `StagePool::Configure` and add the GUI controls in
   `gui/ui/structure.cpp` / `gui/widgets.cpp`.
4. Write its page in the [list of effects](../effects/index.md).

No change to `APOProcess` is needed. Transfer curves shown in the GUI call the
same inline functions from `src/dsp/transfer.h` as the DSP, so the graph cannot
drift from what is applied.
