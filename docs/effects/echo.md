# Echo

Repeats the signal after a delay, from a short slapback to a long fading tail.
The dry voice stays at unity; **Level** sets only the repeated return.

## Try this

- Put Echo after cleanup and level effects, usually before
  [Reverb](reverb.md), so you are not echoing room noise. Check **Treat as
  mono** on the [Input](input.md) row if you want the repeats spread across
  both channels.
- Start with **Time** around **300 ms** and talk normally. This is the gap
  before the first repeat, so it also adds that much latency while Echo is
  enabled. Shorter values sound like a slap; longer values read as distinct
  repeats.
- **Feedback** controls how many repeats you hear. **35%** is a gentle single
  repeat; higher values make a longer series. The clamp stops at **98%** so a
  loop cannot grow without bound.
- **Damping** darkens each repeat. **40%** keeps speech natural; turn it up for
  softer, more distant repeats or down if the echoes sound too bright.
- **Mode** chooses the stereo shape. **Stereo** runs one loop per channel;
  **Ping-pong** sends each repeat to the opposite channel. On a mono stream
  there is no opposite channel, so Ping-pong behaves like a single loop.
- Watch the bottom meters with **Listen to this device**: wet returns add to the
  dry level. If peaks approach **CLIP**, lower Level or Feedback first.
- A newly added Echo uses Stereo, 300 ms Time, 35% Feedback, 40% Damping, and
  25% Level. The small ↺ buttons reset individual controls. At most 4 instances
  are supported.
- Echo has controls but no live graph; the graph is hidden while it is selected.

## Controls

| Control | Default | Range | What it does |
| --- | --- | --- | --- |
| Mode | Stereo | Stereo, Ping-pong | Keeps repeats in each channel or alternates them between channels. |
| Time | 300 ms | 5 to 1200 ms | Delay before each repeat; adds the same latency to the stream. |
| Feedback | 35% | 0 to 98% | Amount of each repeat fed back into the delay line. |
| Damping | 40% | 0 to 100% | Low-pass filtering in the feedback loop; higher is darker. |
| Level | 25% | 0 to 100% | Wet return level. Dry stays at unity. |

## Under the hood

`EchoStage` (`src/dsp/stages.cpp`):

- `Configure` allocates a circular delay of `ceil(1200 ms * sampleRate) + 2`
  frames per channel and one feedback low-pass state per channel. A live Time
  change only moves the read offset, so no control edit allocates.
- Each frame reads a damped tap, writes the current input plus feedback at the
  write pointer, then adds the undamped delayed tap to the output. The damped
  taps are snapshotted before writing so a sample cannot feed back into itself
  in the same frame.
- Damping is a one-pole low-pass in the feedback path. Its cutoff runs from
  about 20 kHz at 0% to 200 Hz at 100%, so 0% is effectively transparent.
- **Stereo** keeps independent loops. **Ping-pong** takes the damped tap from the
  opposite channel when writing each channel's delay input, which alternates the
  repeats. Enabling or changing Mode resets the delay to avoid replaying the old
  loop through the new routing.
- `NeedsSilence()` is true while enabled with a nonzero Level, so the tail is
  flushed through silent buffers. There is no graph or stage telemetry for Echo;
  the dry path is never attenuated.
- Settings use the `echo_mode`, `echo_time_ms`, `echo_feedback`, `echo_damping`,
  and `echo_level` keys. Settings schema version 12 added Echo; a file with any
  other version is deleted rather than migrated.
