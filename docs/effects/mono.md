# Mono (left to all channels)

Copies the processed left channel to every other channel. Intended for a mono
microphone connected to input 1 of a stereo interface, where only the left
channel carries signal.

Typical placement: after gain and before distortion/reverb, so a mono source
still produces a wide reverb tail. Up to 16 instances are supported.

## Parameters

This effect has no adjustable parameters. It is a single enable flag.

## Implementation details

`MonoStage` (`src/dsp/stages.cpp`):

- `Update` stores the enable flag.
- `Process` reads `values[0]` (the left channel) for each frame and writes it to
  channels 1 through `channels - 1`. It does nothing when disabled or when the
  stream is already mono.
- No buffers, coefficients, or state are allocated, and there is no latency.

## GUI notes

The parameter panel shows a short explanatory label ("Copies the processed
left channel to every other channel.") because there are no sliders. The setting
is stored per device with the other effect settings.
