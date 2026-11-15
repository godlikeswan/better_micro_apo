#pragma once

#include <windows.h>

#include <string>

// Reusable Win32 control factories and helpers. Views describe what a control
// is, not how it is created, so changing a number field into a slider only
// touches this module and the notification helper below.

namespace ui {

struct SliderSpec {
    double min = 0.0;
    double max = 1.0;
    // Quantum for linear sliders. Logarithmic sliders snap to three
    // significant digits instead, so low values stay adjustable.
    double step = 1.0;
    bool logarithmic = false;
    int decimals = 0;
};

int Scale(int value);
void ApplyFont(HWND control, HFONT font);
void CreateFonts();
void DestroyFonts();

HWND Label(HWND parent, int id, const wchar_t* text, DWORD style = SS_LEFT);
HWND Button(HWND parent, int id, const wchar_t* text);
HWND CheckBox(HWND parent, int id, const wchar_t* text);
HWND Slider(HWND parent, int id, const SliderSpec& spec);
HWND GroupFrame(HWND parent, int id, const wchar_t* text);
HWND ComboBox(HWND parent, int id, DWORD style);
HWND ReportList(HWND parent, int id, DWORD style);
HWND TabControl(HWND parent, int id);

// Swap the list view checkbox glyphs between the native images (light mode)
// and the owner-drawn images (dark mode).
void ApplyListStateImages(HWND list);

std::wstring FormatNumber(double value);
void SetSliderValue(HWND slider, double value);
bool GetSliderValue(HWND slider, double& value);

// True when a WM_COMMAND notification means the slider's value changed.
bool IsSliderChange(WORD notification);

} // namespace ui
