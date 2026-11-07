# Architecture

Better Micro APO is a capture APO built on `CBaseAudioProcessingObject`. COM
identity and aggregation are handled by ATL (`CComObjectRootEx`, `CComCoClass`,
`CComAggObject`), the same structure Microsoft's SwapAPO sample uses.

## Interfaces

- `IAudioProcessingObject`
- `IAudioProcessingObjectConfiguration`
- `IAudioProcessingObjectRT`
- `IAudioSystemEffects`

## Processing

- 1 input / 1 output, in-place. Latency is zero unless the gate lookahead is
  enabled.
- Supported sample formats: float32, PCM16, PCM24, PCM32 (container size must
  equal the valid-bit size).
- The DSP is a chain of `apo::dsp::Stage` objects (`src/dsp`). `ConfigureDsp()`
  assembles it from the per-device settings:

  ```
  optional GateStage -> optional CompressorStage -> optional GainStage (makeup)
  -> optional MonoStage
  ```

  Adding or reordering a stage means writing one `Stage` subclass and changing
  `ConfigureDsp()`; the real-time path does not change.
- `Chain::Process` decodes the interleaved buffer into a preallocated
  `double` scratch buffer, runs each stage in place, then encodes and clips the
  result. The scratch buffer is sized in `LockForProcess` from the connection's
  maximum frame count, so `APOProcess` stays allocation-free.
- `GateStage` can delay the audio by up to 300 ms while its detector watches the
  incoming signal, so the gate opens before a transient reaches the output. The
  delay line is allocated in `Configure`, cleared in `Reset`, and its state is
  advanced for silent buffers too (`Stage::NeedsSilence` /
  `Chain::ProcessSilence`) so buffered audio is not replayed later.
- The gate and compressor both use a linked peak detector across channels, so a
  stereo source keeps its image.
- Optional mono mode copies the processed left channel to every other channel,
  for a mono source connected to input 1 of a stereo interface.

## Per-device settings

One DLL serves every capture device. Each APO instance identifies its endpoint
from `APOInitSystemEffects2` in `Initialize`: the last item in
`pDeviceCollection` is the endpoint `IMMDevice`, and the last `{GUID}` in
`IMMDevice::GetId()` is the key used under
`HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\MMDevices\Audio\Capture`.

`LockForProcess` reads
`C:\ProgramData\better_micro_apo\devices\{endpoint-guid}.ini`, clamps the
values, and precomputes the DSP coefficients. `APOProcess` only uses the
cached instance state, so the real-time path does no file I/O and no
allocation. Missing files fall back to the defaults.

The INI keys are `gate_enabled`, `gate_threshold_db`, `gate_range_db`,
`gate_attack_ms`, `gate_hold_ms`, `gate_release_ms`, `gate_lookahead_ms`,
`compressor_enabled`, `compressor_threshold_db`, `compressor_ratio`,
`compressor_attack_ms`, `compressor_release_ms`, `makeup_enabled`, `makeup_db`,
and `mono`. Legacy keys (`threshold_db`,
`ratio`, `attack_ms`, `release_ms`) are still read for the compressor. A legacy
`input_gain_db` key is folded into the compressor threshold and makeup gain
(`threshold -= input_gain`, `makeup += input_gain`) so settings written before
the input gain stage was removed keep the same sound.

The GUI writes the same files during `Apply`, inside the existing audio-stop
section, then restarts the audio service once.

## Listen monitor

`gui/apo_monitor.cpp` opens the selected capture endpoint in shared mode and
renders it to the default output, so the APO runs on the monitored stream. The
capture thread accumulates the peak, the sum of squared samples, and the sample
count per packet. Silent packets count as zero-energy samples so the RMS window
decays instead of holding the previous level. The GUI timer samples these
counters every 100 ms and keeps a 500 ms history: the displayed peak is the
maximum of that history and the RMS is
`sqrt(sum(sumSquares) / sum(samples))`. Clip and drop counters are latched and
reset when listening starts.

## Device binding

The GUI writes one of two registry values under the endpoint's `FxProperties`
key, then restarts audio once:

- `PKEY_FX_PreMixEffectClsid`
  (`{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},1`, `REG_SZ`) for devices without an
  effect pack.
- `PKEY_CompositeFX_StreamEffectClsid`
  (`{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},13`, `REG_MULTI_SZ`) for devices
  whose driver publishes composite FX entries. Windows ignores the single FX
  slots on those devices, so the APO CLSID is appended to the existing driver
  chain instead. The driver entries are preserved and restored on disable.

Before changing either slot the GUI exports the whole `FxProperties` key with
`reg.exe` to `C:\ProgramData\better_micro_apo\backup`. Restore parses the
backup, puts back the original PreMix value and composite chain, and removes
only this APO's CLSID from anything it did not write.

## Registration and logging

- CLSID: `{6A8BE200-CD89-4E77-97F5-D51E8F16EF90}`
- Logs setup calls to `C:\ProgramData\better_micro_apo\better_micro_apo.log`
  and to `OutputDebugStringW`. `APOProcess` never logs or allocates.

## Packaging

The build embeds the APO DLL in `better_micro_apo_gui.exe` as an `RCDATA`
resource (`IDR_APO_DLL`), so the GUI is the only file that needs to be
distributed. CMake stages the built DLL at
`build/apo_embed/better_micro_apo.dll`, generates the resource script from
`gui/apo_dll.rc.in`, and touches it after every DLL build so the resource
compiler picks up the new bytes. `Install` extracts the resource to
`C:\Program Files\better_micro_apo\better_micro_apo.dll` and registers it.

## Further reading

For the general install/uninstall flow, registry layout, and pitfalls, see the
[Custom APO Installer Guide](https://gist.github.com/godlikeswan/5152c5594d79e50980ad2cf831d3f06d).
