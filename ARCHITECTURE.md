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

- 1 input / 1 output, in-place. Latency is zero unless the gate lookahead or
  noise reduction is enabled.
- Supported sample formats: float32, PCM16, PCM24, PCM32 (container size must
  equal the valid-bit size).
- The DSP is a chain of `apo::dsp::Stage` objects (`src/dsp`). `ConfigureDsp()`
  assembles it from the per-device settings:

  ```
  optional NoiseStage -> optional GateStage -> optional CompressorStage
  -> optional GainStage (makeup) -> optional MonoStage
  ```

  Adding or reordering a stage means writing one `Stage` subclass and changing
  `ConfigureDsp()`; the real-time path does not change.
- `Chain::Process` decodes the interleaved buffer into a preallocated
  `double` scratch buffer, runs each stage in place, then encodes and clips the
  result. The scratch buffer is sized in `LockForProcess` from the connection's
  maximum frame count, so `APOProcess` stays allocation-free.
- `NoiseStage` runs first so it sees the raw input. It performs a short-time
  Fourier transform with a fixed 512-point FFT and a 256-sample hop, subtracts
  the captured magnitude profile, and applies one gain per frequency bin to all
  channels so the stereo image is preserved. The gain is limited by the
  configured reduction and smoothed across bins and time. Analysis and
  synthesis use a square-root Hann window with 50% overlap, so the stage adds
  about 512 samples of latency. All buffers are allocated in `Configure`; the
  stage reports `NeedsSilence() == true` so its overlap tail is flushed through
  silent buffers.
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

The INI keys are `noise_enabled`, `noise_reduction_db`, `gate_enabled`,
`gate_threshold_db`, `gate_range_db`, `gate_attack_ms`, `gate_hold_ms`,
`gate_release_ms`, `gate_lookahead_ms`, `compressor_enabled`,
`compressor_threshold_db`, `compressor_ratio`, `compressor_attack_ms`,
`compressor_release_ms`, `makeup_enabled`, `makeup_db`, `mono`, and
`isolate_effects`. Legacy keys (`threshold_db`, `ratio`, `attack_ms`,
`release_ms`) are still read for the compressor. A legacy `input_gain_db` key is
folded into the compressor threshold and makeup gain (`threshold -= input_gain`,
`makeup += input_gain`) so settings written before the input gain stage was
removed keep the same sound.

The captured noise profile is stored next to the settings as
`C:\ProgramData\better_micro_apo\devices\{endpoint-guid}.noise`. The file has a
small `BMNP` header (version, sample rate, channel count, frame count) followed
by the raw interleaved float32 capture. `LockForProcess` analyzes it with the
same FFT size and window as `NoiseStage`, averages the magnitude spectrum across
channels, and resamples the bins if the profile and the stream use different
sample rates. A missing or invalid profile leaves the stage out of the chain
even when `noise_enabled=1`.

`isolate_effects` is a GUI binding option rather than a DSP value: it tells the
GUI to replace the composite stream chain with only this APO instead of
appending to the driver's chain. The APO still reads it from the same INI file
but does not use it while processing.

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

## Noise profile capture

**Capture noise profile** records the device without any of this APO's
processing so the profile describes the raw room noise. The GUI saves a settings
snapshot with the noise, gate, compressor, gain, and mono stages disabled, and
applies it with the normal `ApplyChanges` path when the device is bound. The
APO then runs an empty chain, which is a plain passthrough. The GUI reuses the
listen monitor in a capture mode (`apo::CaptureNoise`): it records about three
seconds of interleaved float32 samples, converts PCM formats on the fly, and
renders silence to the default output so the room is not played back through
speakers. The raw capture is written to the device's `.noise` file, then the
real settings are restored with `noise_enabled=1` and applied again. If the
capture or the save fails, the original settings are restored instead. Devices
that are not bound are captured directly and only their settings file is
written.

The noise stage never reads files: `LockForProcess` loads and analyzes the
profile before the stream starts, and `APOProcess` only uses the cached bins.

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

The per-device `isolate_effects` option replaces the effect pack's composite
chains with only this APO's CLSID. It sets the stream chain (`...,13`) to this
APO and clears the mode (`...,14`), endpoint (`...,15`), keyword-detector
(`...,16`-`...,18`), and offload (`...,19`-`...,20`) chains. Turning it off
rebuilds every one of those values from the newest backup that still has driver
entries, plus this APO, and
disabling the device restores the driver chains without this APO. If isolation
is already saved but other composite chains are still present, `Apply` finishes
the cleanup without requiring a checkbox toggle.

Before changing either slot the GUI exports the whole `FxProperties` key with
`reg.exe` to `C:\ProgramData\better_micro_apo\backup`. Restore parses the
backup, puts back the original PreMix value and every composite chain, and
removes only this APO's CLSID from anything it did not write.

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
