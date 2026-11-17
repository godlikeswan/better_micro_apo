# Theming and repainting

The GUI follows the Windows app mode live. The implementation is Win32-only and
relies on a few undocumented or brittle behaviors, collected here so changes do
not reintroduce old repaint bugs.

## Theme detection and palette

`gui/ui/theme.cpp` reads
`HKCU\Software\Microsoft\Windows\CurrentVersion\Themes\Personalize\AppsUseLightTheme`.
`BuildTheme` fills a palette of brushes and colors for light or dark;
`ShutdownTheme` releases the brushes. `InitializeTheme` calls the undocumented
`uxtheme.dll` ordinal 135 (`SetPreferredAppMode`, value 1 = allow dark) before
creating windows.

Live changes arrive as `WM_SETTINGCHANGE` with `ImmersiveColorSet` or as
`WM_THEMECHANGED`. `WindowProc` responds by rebuilding the palette, refreshing
the menu, applying the title bar and native child themes, applying list state
images, refreshing info icons, invalidating the frame, and repainting.

## Dark title bar and native controls

`ApplyTitleBarTheme` loads `dwmapi.dll` and calls `DwmSetWindowAttribute` with
attribute 20 (`DwmUseImmersiveDarkMode`), falling back to 19 on older Windows
versions.

`ApplyNativeTheme` deliberately targets only classes with usable dark variants:

- `Edit` and `ComboBox` → `DarkMode_CFD`
- `SysListView32`, `SysHeader32`, `SysTabControl32`, tooltips →
  `DarkMode_Explorer` / `DarkMode_ItemsView`

It calls the undocumented `uxtheme.dll` ordinal 133
(`AllowDarkModeForWindow`) first. Push buttons and checkboxes are **not** given
an empty theme name: that would disable their visual style and look worse in
light mode, so they are owner-drawn instead.

## Menu theming

`gui/ui/menu.cpp` owner-draws menu items but keeps each caption in the
`MENUITEMINFOW` structure so Alt mnemonics and accessibility still see a normal
string. `SetMenuInfo(MIM_BACKGROUND | MIM_APPLYTOSUBMENUS)` sets the themed menu
brush, and `DrawMenuBar` is called after a theme change. Windows repaints the
menu-bar border with a bright inactive color, so `WM_NCPAINT` paints our divider
and `WM_NCACTIVATE` forces a second non-client paint to make the divider win.

## Win32 workarounds worth keeping

- **Group frames paint themselves.** The system draws a group-box border but
  leaves its interior to the parent. The page panel uses `WS_CLIPCHILDREN`, so
  its erase pass never reaches the area under a frame and hidden effect
  controls leave stale pixels. `GroupFrameProc` fills the face and draws the
  frame on every `WM_PAINT`.
- **Frames are sent to the back.** Child windows are inserted at the top of the
  sibling z-order, so new group frames covered the controls they framed and
  `WS_CLIPSIBLINGS` clipped those controls away. `CreateControls` calls
  `SetWindowPos(group, HWND_BOTTOM, ...)` after creating them.
- **List glyphs are replaced in place.** See
  [Widgets and controls](widgets.md#list-checkbox-glyphs-dark-mode). Never cache
  the list's state image list handle across a theme change.
- **Info icons are direct-painted statics.** The subclass handles `WM_PAINT`
  itself so icons follow the palette without a bitmap resource.

## Update and redraw pitfalls

- `RepaintAll()` invalidates the root window and the page panel with
  `RDW_INVALIDATE | RDW_ALLCHILDREN`. It deliberately does **not** use
  `RDW_UPDATENOW`: forcing an immediate repaint shows intermediate frames and
  can flash a white panel while layouts change. Each control paints its own
  background, so a separate erase pass only adds a visible flash.
- Sibling controls do not always repaint when a drop-down closes over them, so
  `Relayout()` always follows `Layout()` with `RepaintAll()`.
- Long status messages share the bottom row with a 320 px meter; the layout
  gives status the remaining width rather than overlapping the meter.
- The mouse cursor is never set to a busy shape while work runs. Controls are
  disabled and the cursor stays normal.
- Effect-list state changes are rejected in `LVN_ITEMCHANGING` while busy, so
  the list stays visible but read-only instead of being disabled and repainted.
- The effect graph invalidates with `FALSE` for value changes and `TRUE` when
  its geometry or theme changes, avoiding needless full redraws while sliders
  move.
