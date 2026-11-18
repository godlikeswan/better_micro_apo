# Noise reduction

Quiets the kind of background noise that stays roughly the same: fan hum, air
conditioning, room hiss. It learns what your room sounds like from a short
recording, then reduces that sound while leaving your voice mostly alone.

## Try this

- Put Noise reduction first in the effect list, before [Gate](gate.md) and
  [Compressor](compressor.md), so it sees the microphone signal before the other
  effects shape it.
- Select the **enabled** Noise effect you want to calibrate, sit in your usual
  spot, be quiet, and click **Capture noise profile**. It measures about three
  seconds at that effect's position in the chain — after any effects above it
  and before anything below it.
- Nothing is switched off or restarted during capture. Your settings stay
  exactly as they are, audio keeps playing, and **Listen** keeps running.
- The new profile goes straight into the running effect. If this was the first
  profile, the selected Noise effect starts reducing noise as soon as the
  capture finishes; you do not need to re-enable it or restart audio.
- Talk normally with **Listen to this device** on. Move **Reduction** to the
  right until the steady hiss is clearly quieter. If your voice starts sounding
  chewed up or watery, move it back to the left. You do not have to remove every
  last bit of noise; a little left sounds more natural.
- This effect is for steady noise. For sounds that come and go between words,
  use [Gate](gate.md) after it. If turning the mic up makes room noise obvious,
  fix the noise here rather than lowering the voice level.
- Check the bottom meters while you talk: aim for peaks around **−5 dBFS**, RMS
  around **−20 dBFS**, and no **CLIP**. At most 4 instances are supported.

## Controls

| Control | Default | Range | What it does |
| --- | --- | --- | --- |
| Reduction | 12 dB | 0 to 70 dB | Maximum attenuation applied to a frequency bin. |

## Under the hood

`NoiseStage` (`src/dsp/stages.cpp`):

- Analysis uses a fixed 512-point FFT with a 256-sample hop and a square-root
  Hann window with 50% overlap. The fixed size keeps the captured profile and
  the running stage on the same frequency grid.
- `LockForProcess` loads the `.noise` profile, even when the effect is off, so it
  can be enabled live. Missing or invalid files leave the stage as a pure
  passthrough.
- Each hop subtracts the profile magnitude and applies a gain per bin, capped
  by `reductionDb`, then smooths the gains across bins and time. One shared gain
  per frequency bin is applied to all channels, preserving the stereo image.
- Analysis/synthesis buffers, the FFT plan, window, profile bins, gain arrays,
  and overlap-add state are allocated in `Configure`, sized from the stream and
  the FFT geometry. `Process` is allocation-free.
- `NeedsSilence()` is true while enabled with a valid profile so the overlap-add
  tail is flushed through silent buffers. The stage delays the stream by about
  512 samples while active.

Capture is handled by `StageCaptureTap` (`src/stage_capture.cpp`). The APO adds
it just before the selected Noise stage for one request, analyzes the signal
that reaches that stage with the same FFT geometry, and writes precomputed
magnitude bins to `devices\{id}.noise`. The GUI saves the file and the APO
retunes the live stages from those bins. No effect is disabled, no setting is
rewritten, and the audio service is not restarted. See
[DLL ↔ GUI settings sharing](../runtime-sharing.md) for the file and control
block layouts.
