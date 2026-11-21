# Looper

Records the live input into five independent tracks and plays the takes back
under the dry signal. Only one Looper can be added per preset. It has no live
graph; the per-track **State** labels are the display.

## Try this

- Add Looper after [Tuner](tuner.md) in the Add effect menu. Press the **1** key
  while the state says **Empty** to start recording; the take starts on keydown.
  Press it again while **Recording** to finish and hear **Playback**; that change
  also happens on keydown. Tapping a track that is already playing layers another
  take over it instead of replacing it.
- The key buttons accept mouse gestures. Keys **1**–**4** and **Space** drive
  tracks 1–5 when Looper is selected, no combo box is open, and no
  Ctrl/Alt/Shift modifier is held. Keyboard record start and finish are sent on
  keydown; other keyboard taps are classified after release. Holding a key
  triggers the long press; auto-repeat does not send taps.
- Keyboard recording starts and finishes on the first press: an empty or playing
  track enters **Recording** immediately, and a recording track enters
  **Playback** immediately. If a provisional start later turns out to be a
  double tap or a long press, it is discarded before the gesture's real action
  runs. A second press after an immediate finish still sends Stop.
- A tap is a press shorter than about 400 ms. A second press within about 250 ms
  becomes a double tap. A long press is about 600 ms.
- Tap **1**: Empty → **Recording** → Playback. Tap again in Playback to overdub.
  Double tap in Recording or Playback to stop, then tap in **Stopped** to play
  again. Long press in Playback to undo, in Stopped to clear, or in Recording to
  finish the take.
- Recording never stops just because a loop boundary passed. Keep recording and
  the write position wraps around the current target, layering over what is
  already there. Tap again when you want the take to become **Playback**.
- **Free length** is off by default. With it off, the first synced take sets the
  reference length. Later synced takes automatically choose the nearest of
  `1/8, 1/4, 1/2, 1, 2, 4, 8` times that reference. Check **Free length** to
  keep the exact recorded length and give that track its own playhead.
- **Stop at loop end** is off by default. Check it to defer a double-tap stop
  until the track reaches its loop boundary. A free recording has no known end,
  so it still stops at the double tap.
- **Volume** changes only that track's loop return; the live input is unchanged.
  Zero mutes the return without erasing the take.
- Undo is one level and belongs to the track that recorded last. Clear erases
  only that track. If the reference track is cleared or undone back to
  **Empty**, the next synced take establishes a new reference length. Takes are
  saved per device, so they survive audio restarts, closing and reopening the
  GUI, and PC restarts. Volume and timing settings remain in the INI file; the
  sample data lives in a separate `.looper` bank.

## Gestures

| Gesture | Empty | Recording | Playback | Stopped |
| --- | --- | --- | --- | --- |
| Tap | Start recording | Finish, then play | Start overdub | Play |
| Double tap | No action | Stop | Stop | Play |
| Long press | No action | Finish, then play | Undo last take | Clear track |

For keyboard, **Start recording** and **Finish, then play** are applied on
keydown so the loop boundary matches the press. Mouse gestures are applied after
the tap, double tap, or long press is recognized. A second keyboard press within
the double-tap window still sends Stop, and holding a newly started take
discards it.

## Controls

| Control | Default | Range | What it does |
| --- | --- | --- | --- |
| Key | 1, 2, 3, 4, Space | Tap / double tap / long press | Sends that track's transport gesture; record start and finish apply on keydown. |
| State | Empty | Empty, Recording, Playback, Stopped | Read-only transport state confirmed by the APO. |
| Volume | 100% | 0 to 100% | Level of this track's loop return. |
| Free length | Off | Checkbox | Keep the exact recorded length instead of auto-snapping to the reference. |
| Stop at loop end | Off | Checkbox | Defer a double-tap stop until this loop reaches its end. |

## Under the hood

`LooperStage` (`src/dsp/stages.cpp`):

- `Configure` allocates five track buffers and one shared undo bank. Each is
  `ceil(120 s * sampleRate) * channels` floats (five tracks, the undo bank, and
  two provisional-gesture snapshots), about 352 MiB total at 48 kHz stereo. The
  buffers are allocated only when Looper is in the preset, and no audio-path
  call allocates.
- Every frame snapshots the input before any loop return is mixed in, so a
  recording track captures only the source at its position in the chain, never
  another track. The dry input always stays in the output. While recording, the
  layer that was already at the write position is returned; the current input
  is not returned a second time.
- The first synced take to finish becomes the reference: its exact length, up to
  120 s, is the master length. A later synced take compares its recorded length
  to that reference and snaps to the nearest allowed ratio. The target is
  recomputed as the take grows so wrapping stays in phase with the shared frame
  clock. Free tracks use their exact recorded length and their own playhead.
  Ratio snapping and Free length apply only when starting a new take; overdubs
  preserve the existing length and phase.
- Recording writes are always additive. A fresh take starts from an empty
  buffer, so its first pass is clean; later passes wrap and layer. Overdubs add
  to the existing take on every pass. Nothing automatically finishes a take at
  a boundary.
- Starting a new take copies the old track contents into the shared undo bank,
  along with the current reference state. Undo restores that one level for the
  last recording track and then invalidates the bank; Clear also invalidates it
  for that track. Clearing or undoing the reference track to Empty releases the
  reference.
- Transport requests travel through the runtime snapshot as `LooperCommand`,
  `commandTrack`, and a monotonic `commandSerial`. The APO consumes a command
  when the serial changes at a buffer boundary. The first mapping establishes a
  baseline, stale commands are ignored, and a later settings-only apply carries
  the same serial with the command cleared so it cannot replay. No command is
  written to the durable INI file.
- `SaveLooperBank` writes the five take buffers, the one-level undo bank, the
  shared reference length, playhead positions, and transport states to
  `devices\{id}.looper` just before `UnlockForProcess` destroys the stage pool.
  `LoadLooperBank` restores them after the first chain build has bound the
  Looper's stable effect id. An unfinished provisional gesture is rolled back
  before saving. The file has magic `BMLP` and version 1; a version or stream
  format mismatch deletes the bank instead of migrating or resampling it.
- `MonitorLooper()` publishes the five states through the stage-monitor channel.
  The GUI updates its labels immediately for responsiveness and then confirms
  them from the next 100 ms telemetry poll.
- Settings use `looper_track_0_volume` through `looper_track_4_volume`, plus
  `looper_track_N_free_length` and `looper_track_N_stop_at_loop_end`. Settings
  schema version 17 added the auto-snap behavior; a file with any other version
  is deleted rather than migrated.
