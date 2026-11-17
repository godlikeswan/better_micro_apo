# Noise reduction

Spectral subtraction of a room-noise profile captured from the same device. It
reduces steady background noise (fan, air conditioning, room hiss) while
leaving speech mostly alone. It needs a captured profile to do anything.

Typical placement: first in the chain, before the gate and compressor, so it
sees the raw signal. At most 4 instances are supported.

## Parameters

| Parameter | Default | Clamp | Unit / meaning |
| --- | --- | --- | --- |
| Reduction | 12 dB | 0–70 dB | Maximum attenuation applied to any frequency bin. |

Newly added instances are enabled by `MakeEffect`. Without a valid profile the
stage pool creates no noise stage, so the effect is skipped even when enabled.

## Implementation details

`NoiseStage` (`src/dsp/stages.cpp`) works on short-time spectra:

- Analysis uses a fixed 512-point FFT with a 256-sample hop and a square-root
  Hann window with 50 % overlap. The fixed size keeps the captured profile and
  the running stage on the same frequency grid.
- `LockForProcess` loads and analyzes the `.noise` file with the same window and
  hop, averages the magnitude spectrum across channels, and resamples bins if
  the profile and stream use different sample rates. Missing or invalid files
  leave the pool empty.
- Each hop subtracts the profile magnitude and applies a gain per bin, capped
  by `reductionDb`, then smooths the gains across bins and time. One shared gain
  per frequency bin is applied to all channels, preserving the stereo image.
- Analysis/synthesis buffers, the FFT plan, window, profile bins, gain arrays,
  and overlap-add state are allocated in `Configure`, sized from the stream and
  the FFT geometry. `Process` is allocation-free.
- `NeedsSilence()` is true while enabled so the overlap-add tail is flushed
  through silent buffers.
- The stage delays the stream by about 512 samples while enabled.

## GUI notes

**Capture noise profile** records about three seconds from the device with
every effect temporarily disabled (a passthrough snapshot), renders silence to
the output so the room is not played back, writes the raw float32 capture to
`devices\{id}.noise`, then restores the settings and enables the first Noise
effect (or appends one if there was none and the list has room). Capture forces
an audio restart because `LockForProcess` is the only place the profile is
loaded. The reduction slider is the only adjustable parameter; the stage also
reports `NeedsSilence` for its overlap tail.
