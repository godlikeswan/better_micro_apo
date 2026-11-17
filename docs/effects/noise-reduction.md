# Noise reduction

Quiets the kind of background noise that stays roughly the same: fan hum, air
conditioning, room hiss. It learns what your room sounds like from a short
recording, then reduces that sound while leaving your voice mostly alone.

## Try this

- Put Noise reduction first in the effect list, before [Gate](gate.md) and
  [Compressor](compressor.md), so it sees the raw microphone signal.
- Sit in your usual spot, be quiet, and click **Capture noise profile**. It
  records about three seconds with every effect temporarily disabled. You will
  not hear the room played back — the capture renders silence to the output —
  and your old settings are restored afterward.
- Capturing briefly restarts audio and then enables the first Noise effect (or
  adds one if there was none and the list has room). This restart is needed
  because the profile is loaded when the audio chain is rebuilt.
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
- `LockForProcess` loads and analyzes the `.noise` file with the same window and
  hop, averages the magnitude spectrum across channels, and resamples bins if
  the profile and stream use different sample rates. Missing or invalid files
  leave the pool empty, so the effect is skipped even when enabled.
- Each hop subtracts the profile magnitude and applies a gain per bin, capped
  by `reductionDb`, then smooths the gains across bins and time. One shared gain
  per frequency bin is applied to all channels, preserving the stereo image.
- Analysis/synthesis buffers, the FFT plan, window, profile bins, gain arrays,
  and overlap-add state are allocated in `Configure`, sized from the stream and
  the FFT geometry. `Process` is allocation-free.
- `NeedsSilence()` is true while enabled so the overlap-add tail is flushed
  through silent buffers. The stage delays the stream by about 512 samples while
  enabled.
- Capture writes the raw float32 capture to `devices\{id}.noise`, then restores
  the saved settings.
