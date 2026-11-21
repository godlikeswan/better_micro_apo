# Widgets and controls

`gui/ui/widgets.*` provides the reusable control factories and the custom
drawing used by the single screen. `gui/ui/graph.cpp` owns the effect graph;
`gui/ui/structure.cpp` owns the info icons and tooltips.

## Factories in widgets.cpp

| Factory | Native class / style | Notes |
| --- | --- | --- |
| `ui::Label` | Static (`WS_CLIPSIBLINGS`) | Plain text; also used for status and meter text. |
| `ui::Button` | `BS_PUSHBUTTON` | Subclassed for themed painting. |
| `ui::IconButton` | `BS_OWNERDRAW` | Borderless icon button; used for the `×` and ↺ glyphs. |
| `ui::CheckBox` | `BS_AUTOCHECKBOX` | Themed glyph in dark mode. |
| `ui::ComboBox` | Drop-down list | Native control with dark theme applied. |
| `ui::ReportList` | `SysListView32`, report | Checkboxes, full-row select, double buffer; subclassed. |
| `ui::Slider` | Custom window class | Owner-drawn, themed, with logarithmic support. |
| `ui::GroupFrame` | `BS_GROUPBOX` | Paints its own face and frame. |
| `ui::TabControl` | `SysTabControl32` | Available in the toolkit; the current screen has no tabs. |

`Scale(value)` converts design units using the DPI read at startup. `CreateFonts`
/ `DestroyFonts` manage the shared fonts; `ApplyFont` assigns one to a control.

## Custom drawing and subclasses

Subclass procedures in `widgets.cpp`:

- `ControlProc` — buttons and checkboxes; draws themed faces and hot state and
  invalidates on mouse/state changes.
- `GroupFrameProc` — paints the group-box face and frame itself. The system
  draws the frame but leaves the interior to the parent, and `WS_CLIPCHILDREN`
  on the page panel means hiding a control would otherwise leave its pixels
  behind.
- `SliderProc` — handles mouse tracking, keyboard, snapping, and repaint. A
  `SliderSpec` holds min, max, step, logarithmic flag, decimal places, and an
  `infiniteAtMax` flag. On an infinite slider the last finite position is a
  stop, the right edge snaps to ∞, and the label renders ∞; `SetSliderValue` /
  `GetSliderValue` are the public accessors, and `IsSliderChange` recognizes the
  custom notification.
- `ListViewProc` — dark-mode background plus the state-image workaround below.
- `TabProc` — themed tab painting for the unused tab control.

`DrawPushButton`, `DrawCheckBox`, `DrawCheckGlyph`, and `PaintSlider` contain
the actual palette use. Changing a widget's appearance is local to these
functions; changing behavior is local to its subclass.

## Effect graph and meters

`gui/ui/graph.cpp` registers its own class and creates the graph with
`ui::Graph`. `SetGraphEffect` stores the current effect and invalidates the
control. `GraphSupported` reports seven graph types: Input, Gain,
Saturation/Overdrive/Distortion, Gate/Expander, Compressor/Limiter, Equalizer,
and Tuner. `PaintGraph` draws a grid and one of:

- Input live spectrum,
- Gain input/output curve with a live operating-point dot,
- saturation transfer curve with a mirrored input-peak operating point,
- expander/gate transfer curve for Gate/Expander,
- compressor/limiter transfer curve for Compressor/Limiter,
- equalizer Level bell with a dot at the selected frequency, a yellow live
  dynamic curve, and a dashed directional Range limit, with the live spectrum
  behind them.
- tuner pitch display with note ticks sliding past a centered current pitch and
  a C0–C8 map below.

The saturation and dynamics curves call the same inline functions in
`src/dsp/transfer.h` as the DSP stages, so the displayed curve cannot drift from
what is applied. The equalizer graph normalizes the analog band-pass prototype
that `EqualizerStage` approximates to 0…1 and multiplies it by the signed Level,
so positive values lift the band, negative values cut it, and the skirts return
to 0 dB. A dot marks the static Level at the selected Frequency. When Dynamic is
on, a yellow curve shows the APO's current signed dynamic gain shaped by that
band, and a dashed curve shows the Range limit: below zero for Compressor, above
zero for Expander. The response curves use a centered **−96…+96 dB** scale with
+96/0/−96 labels just inside the plot's right edge. The spectrum behind them
keeps the left-hand **−96…+24 dBFS** scale and its labels; the plot rectangle is
unchanged. Input, Gain, Gate/Expander, and Compressor/Limiter also use
**−96…+24 dBFS**, so boosts above
+24 dB can extend off-screen. Saturation uses normalized −1…1 transfer axes.

The tuner graph has no output meter. Its upper strip is 2.5 semitones wide:
the detected pitch stays centered while bold, enlarged note labels slide past;
only labels that fit fully are drawn, so names do not stack at the edges. Grid
lines are blended toward the text colour so they stay visible over the blue
plot. The readout uses fixed columns for status (**LIVE**, **LAST**, or
**NONE**), note, cents, frequency, and clarity, so values do not shift each
other when their length changes. The lower strip spans C0–C8 with octave labels
and a transparent rectangle with a thicker outline marking the local window.
When the APO has no clear pitch, the last reading remains on screen with the
**LAST** status.

`SetGraphMonitor` receives a snapshot from the APO's `.monitor` file; only the
slowly changing engagement value is smoothed here. The Gain dot, Saturation
marker, and meter fill use APO-side cross-buffer short holds, so a transient
between GUI polls still reaches them. `ClearGraphMonitor` resets the state when
telemetry is missing or the effect changes. Both spectrum graphs draw FFT
bin 0 as well, clamped to the 20 Hz left edge, so the plot reaches the axis
instead of leaving a gap up to the first non-DC bin. The Gain graph highlights
the 0 dB cross and captions it "Transfer curve (peak, dB)". Each meter has
green/yellow/red zones, dB labels to the right, a fill driven by a short (0.06 s)
cross-buffer
peak hold, and a slightly thicker line driven by a longer (0.6 s) hold. Both
catch transients between GUI polls; the fill falls back sooner. The line's
colour is the same as the filled bar zone it sits in: green below −12 dBFS,
yellow from −12 to −3 dBFS, and red above −3 dBFS. The Gain dot uses the same
input/output peak holds as the meter, so a meter reading above 0 dB has a
matching point above 0 on the graph. The engagement bar on both dynamics
effects is offset to stay clear of the left-hand dB labels. Saturation
draws the fixed shaping curve and marks the operating point by feeding the
input peak through the Drive pre-gain, so a higher Drive widens the marker gap
toward the flat part instead of bending the curve. These are stage-local
displays, independent of the bottom Peak/RMS label fed by the post-DSP APO audio
ring used by Listen.

## Icons, tooltips, and reset buttons

`InfoIcon` in `structure.cpp` creates a static control with `SS_NOTIFY` and
installs `InfoIconProc` via `SetWindowSubclass`; it paints the stopwatch or
warning-triangle glyph directly. `AddTooltip` associates a tooltip string with
a control. `RefreshInfoIcons` / `DestroyInfoIcons` invalidate and tear down the
subclasses; `PlaceInfoIcon` computes the icon offset next to a label (including
the `Scale(20)` checkbox marker offset).

`CreateParamResetButtons` creates one borderless ↺ `IconButton` per editable
parameter. `UpdateParamResetButtons` in `main.cpp` disables a reset icon when
the value already equals the default from `apo::MakeEffect`, and
`ResetParameter` changes only that parameter.

## List checkbox glyphs (dark mode)

The list view's checkboxes come from its state image list. In dark mode the
app replaces the two native glyphs in place with `ImageList_Replace` rather
than assigning its own list: applying a window theme makes comctl32 recreate
and destroy the state image list, so a cached handle goes stale and can be
aliased by the replacement. The glyph is drawn at the list's real icon size
(13×13 at 96 DPI), not the 15 px standalone box, or its border falls outside
the bitmap. Check-state index 1 (unchecked) maps to image 0 and index 2
(checked) to image 1, so both images contain a visible box.

See [Theming and repainting](theming.md) for the broader set of Win32
workarounds behind these controls.
