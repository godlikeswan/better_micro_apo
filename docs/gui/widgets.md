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

## Effect graph

`gui/ui/graph.cpp` registers its own class and creates the graph with
`ui::Graph`. `SetGraphEffect` stores the current effect and invalidates the
control. `PaintGraph` draws a grid and one of:

- saturation transfer curve,
- mode-aware transfer curve for Gate/Compressor/Limiter/Expander,
- equalizer band shape.

The saturation and dynamics curves call the same inline functions in
`src/dsp/transfer.h` as the DSP stages, so the displayed curve cannot drift from
what is applied. The equalizer graph draws the analog band-pass prototype that
`EqualizerStage` approximates. `GraphSupported` reports which effect types have
a graph.

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
