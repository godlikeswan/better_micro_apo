# APO overview

The APO is a capture (microphone) processing object in `better_micro_apo.dll`.
One DLL serves every device: each instance discovers which endpoint it belongs
to and loads that device's settings.

## Identity and interfaces

The class is built on ATL and `CBaseAudioProcessingObject`, following the
structure of Microsoft's SwapAPO sample:

- `CComObjectRootEx<CComMultiThreadModel>` — COM reference counting and threading.
- `CComCoClass<BetterMicroApo, &CLSID_BetterMicroApo>` — class factory and
  aggregation. The engine creates APOs with an outer controlling `IUnknown`,
  so ATL uses `CComAggObject`; COM identity is owned by the outer object.
- `CBaseAudioProcessingObject` — implements:
  - `IAudioProcessingObject`
  - `IAudioProcessingObjectConfiguration`
  - `IAudioProcessingObjectRT`
- `IAudioSystemEffects` — marks this as a system effect.

CLSID: `{6A8BE200-CD89-4E77-97F5-D51E8F16EF90}` (`src/apo_ids.h`).

Registration properties (`m_Properties`) are the friendly name "Better Micro
APO" and the APO flags used by the base class.

## Processing at a glance

- 1 input / 1 output, in place. No latency unless gate lookahead or noise
  reduction is enabled.
- Supported formats: float32, PCM16, PCM24, PCM32. The container size must
  equal the valid-bit size. 24-valid-bits-in-a-32-bit-container is not
  supported.
- The DSP is an ordered list of `apo::dsp::Stage` objects. `StagePool`
  preallocates stages, `Update` binds each enabled effect by its stable id, and
  `Chain` runs them in list order. The same effect type may appear more than
  once.
- Each device has four presets; only the selected one is processed.
- `APOProcess` does no file I/O, no allocation, no locks, and no logging.
  Settings published by the GUI are picked up by polling a mapped file.

See [APO operation](operation.md) for the call sequence and allocations, and
the [list of effects](../effects/index.md) for each stage.

## Files

| Path | Purpose |
| --- | --- |
| `C:\Program Files\better_micro_apo\better_micro_apo.dll` | Installed APO DLL. |
| `C:\ProgramData\better_micro_apo\devices\{endpoint-guid}.ini` | Per-device settings and presets. |
| `C:\ProgramData\better_micro_apo\devices\{endpoint-guid}.noise` | Captured room-noise profile. |
| `C:\ProgramData\better_micro_apo\runtime\{endpoint-guid}.dat` | Live settings snapshot shared with the GUI. |
| `C:\ProgramData\better_micro_apo\backup\FxProperties-{endpoint}.reg` | Canonical registry backup for a device. |
| `C:\ProgramData\better_micro_apo\better_micro_apo.log` | Setup-path log (never written from `APOProcess`). |
| `C:\ProgramData\better_micro_apo\gui.ini` | GUI preferences `show_all_devices` and `last_device`. |

The INI is authoritative on restart; the runtime snapshot only overrides it
for the life of a stream. The sharing mechanics are described in
[DLL ↔ GUI settings sharing](../runtime-sharing.md).

## More detail

- [Installation and registration](installation.md)
- [Device binding](device-binding.md)
- [APO operation](operation.md)
- [List of effects](../effects/index.md)
