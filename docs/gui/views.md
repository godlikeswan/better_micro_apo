# Views and layout

All main-screen controls are created in `ui::CreateControls` in
`gui/ui/structure.cpp`. IDs and shared `HWND`s live in `gui/ui/ui_state.*`;
`ShowEffectControls` and `EnableEffectControls` in `gui/main.cpp` decide which
parameter controls are visible and editable. The device combo remembers the last
user-selected endpoint as `last_device` in `gui.ini` and selects it again on
launch when it is still in the visible list.

## Control tree

```
Top-level window ("BetterMicroApoInstaller", WindowProc in main.cpp)
├── Tooltip popup (g_infoTooltip, child of the root)
├── Page panel (kPageClass, PageProc)  ← all main-screen controls
│   ├── Device label + combo
│   ├── Enable checkbox + info icon (interrupt warning)
│   ├── Preset label + combo (1–4)
│   ├── Effects group frame + list view + Add effect button
│   ├── Effect parameters group frame
│   │   ├── Heading + effect graph
│   │   ├── Input gain slider + Treat as mono checkbox
│   │   ├── Per-effect labels, sliders, combo, buttons, notes
│   │   └── Per-parameter reset icons (↺)
│   └── Listen to this device checkbox
├── Status label (bottom row, child of the root)
└── Peak/RMS meter label (bottom row, child of the root)
```

The page panel uses `WS_CLIPCHILDREN` so its erase never paints under child
controls. Status and meter are deliberately siblings of the panel: they sit in
the bottom strip of the root window, not inside the page. Group frames are
pushed to the bottom of the sibling z-order after creation, because newly
created frames otherwise land in front of the controls they frame and
`WS_CLIPSIBLINGS` clips those controls away.

The list view is an `LVS_REPORT` control with `LVS_EX_CHECKBOXES`,
`LVS_EX_FULLROWSELECT`, and `LVS_EX_DOUBLEBUFFER`. Column 0 is the effect name;
column 1 is a narrow, centered `×` delete cell handled through `NM_CLICK`.
Input owns row 0 in every preset: its delete cell is empty, its checkbox is
forced checked, and `LVN_ITEMCHANGING`/`HandleNotify` reject attempts to change
either. `LVN_ITEMCHANGING` also rejects changes while a background operation is
busy.

## Geometry flow

`ui::Layout(width, height)` in `structure.cpp` is the single geometry function:

- Outer margin and inner padding are both `Scale(8)`; the bottom strip is
  `Scale(24)` high.
- The page panel occupies the remaining client area above the bottom strip.
- `LayoutEffects(padding, padding, viewWidth, viewHeight)` places the device
  row, effects list, parameter editor, and listen checkbox. The first row is
  followed directly by the panels (`Scale(34)`), with no hint/title row.
- The bottom row is centered vertically in the strip. Status and meter share a
  total inset of `margin + padding` from each edge; the meter is a fixed
  `Scale(320)` wide and the status takes the remaining space.

`Relayout()` re-runs `Layout` with the current client rect and then calls
`RepaintAll`. `WM_SIZE` does the same. Minimum window size is `Scale(940)` by
`Scale(720)`. All measurements go through `ui::Scale` for DPI.

## Dialogs and popups

- **Add effect** (`gui/ui/effect_dialog.cpp`) is a modal effect-type picker.
  Input is omitted because it already exists at row 0; the fixed Input's
  **Treat as mono** checkbox replaces the old standalone Mono type. Types that
  are already at their per-type capacity are omitted too. The chosen effect is
  inserted after the selected row (never above Input), or appended if nothing
  is selected. The
  graph panel supports Input, Gain, Gate/Expander, Compressor/Limiter,
  Equalizer, Saturation, and Tuner. Echo, Chorus, Tremolo, Vibrato, and Pitch
  Shifter have no live graph; selecting one hides the graph control.
- **About → Info** (`gui/ui/info_dialog.cpp`) is a manual modal loop. It shows
  hashes, install path, CLSID, registry status, and `Applied to N devices`, and
  follows theme changes while open. Its OK button is a regular button.
- The info tooltip (`g_infoTooltip`) is a `TOOLTIPS_CLASSW` popup with a 240 px
  maximum width and 250 ms initial delay. Info icons are static controls with
  `InfoIconProc` as a subclass; the one next to Enable warns that toggling it
  briefly interrupts audio, and latency icons say "Adds delay to the audio
  stream."

## Menu

The menu is built by `ui::CreateMenuBar` in `gui/ui/menu.cpp`:

- **File → Install / Update**, **File → Uninstall**, **File → Exit**
- **View → Show all devices** (checkable, persisted in `gui.ini`)
- **About → Info**, **About → Open log**

Menu items are owner-drawn but keep their captions in `MENUITEMINFOW` so
mnemonics and accessibility still work. `UpdateMenuCommands` disables commands
while busy. The menu background follows the theme via `SetMenuInfo`, and a
divider is painted in `WM_NCPAINT` (with `WM_NCACTIVATE` forcing another
non-client paint so the inactive-color repaint cannot win).

## Adding or moving a control

1. Add an ID and `HWND` in `ui_state.h` / `ui_state.cpp`.
2. Create the control in `structure.cpp::CreateControls`.
3. Add its geometry in `LayoutEffects` or `Layout`.
4. Show/hide it in `main.cpp::ShowEffectControls` if it belongs to one effect.
5. Wire its command or notification in `WindowProc` / `HandleNotify`.

For an effect parameter, also add the default/clamp in `apo_settings.cpp`, the
reset icon target in `main.cpp`, and its row in the
[list of effects](../effects/index.md).
