#include "menu.h"

#include "theme.h"
#include "ui_state.h"
#include "widgets.h"

#include <cwchar>

namespace ui {
namespace {

struct MenuEntry {
    const wchar_t* text;
    bool topLevel;
};

const MenuEntry kFile{L"&File", true};
const MenuEntry kView{L"&View", true};
const MenuEntry kExit{L"E&xit", false};
const MenuEntry kSimple{L"&Simple", false};
const MenuEntry kAdvanced{L"&Advanced", false};

HMENU g_menuBar = nullptr;
HMENU g_viewMenu = nullptr;

void InsertOwnerDrawItem(HMENU menu, UINT id, const MenuEntry* entry,
                         HMENU submenu)
{
    MENUITEMINFOW info{};
    info.cbSize = sizeof(info);
    info.fMask = MIIM_FTYPE | MIIM_ID | MIIM_DATA | MIIM_STRING;
    info.fType = MFT_OWNERDRAW;
    info.wID = id;
    info.dwItemData = reinterpret_cast<ULONG_PTR>(entry);
    // Keep the text on the item as well, so Alt mnemonics and accessibility
    // still see a normal caption even though the item is owner-drawn.
    info.dwTypeData = const_cast<wchar_t*>(entry->text);
    info.cch = static_cast<UINT>(wcslen(entry->text));
    if (submenu != nullptr) {
        info.fMask |= MIIM_SUBMENU;
        info.hSubMenu = submenu;
    }
    InsertMenuItemW(menu, static_cast<UINT>(GetMenuItemCount(menu)), TRUE,
                    &info);
}

void ApplyMenuBackground(HWND window)
{
    if (g_menuBar == nullptr) {
        return;
    }

    MENUINFO info{};
    info.cbSize = sizeof(info);
    info.fMask = MIM_BACKGROUND | MIM_APPLYTOSUBMENUS;
    info.hbrBack = GetTheme().backgroundBrush;
    SetMenuInfo(g_menuBar, &info);

    if (window != nullptr) {
        DrawMenuBar(window);
    }
}

void CheckMode(gui::InterfaceMode mode)
{
    if (g_viewMenu == nullptr) {
        return;
    }

    // CheckMenuItem changes only the state, so the owner-draw type survives.
    // CheckMenuRadioItem would replace the item type and hide the custom text.
    CheckMenuItem(g_viewMenu, IDM_VIEW_SIMPLE,
                  MF_BYCOMMAND | MF_UNCHECKED);
    CheckMenuItem(g_viewMenu, IDM_VIEW_ADVANCED,
                  MF_BYCOMMAND | MF_UNCHECKED);
    CheckMenuItem(g_viewMenu,
                  mode == gui::InterfaceMode::Advanced ? IDM_VIEW_ADVANCED
                                                       : IDM_VIEW_SIMPLE,
                  MF_BYCOMMAND | MF_CHECKED);
}

HFONT MenuFont()
{
    return g_font != nullptr ? g_font
                             : static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
}

} // namespace

void CreateMenuBar(HWND window, gui::InterfaceMode mode)
{
    DestroyMenuBar(window);

    g_menuBar = CreateMenu();
    if (g_menuBar == nullptr) {
        return;
    }

    HMENU fileMenu = CreatePopupMenu();
    g_viewMenu = CreatePopupMenu();
    if (fileMenu == nullptr || g_viewMenu == nullptr) {
        if (fileMenu != nullptr) {
            DestroyMenu(fileMenu);
        }
        if (g_viewMenu != nullptr) {
            DestroyMenu(g_viewMenu);
        }
        DestroyMenu(g_menuBar);
        g_menuBar = nullptr;
        g_viewMenu = nullptr;
        return;
    }

    InsertOwnerDrawItem(fileMenu, IDM_FILE_EXIT, &kExit, nullptr);
    InsertOwnerDrawItem(g_viewMenu, IDM_VIEW_SIMPLE, &kSimple, nullptr);
    InsertOwnerDrawItem(g_viewMenu, IDM_VIEW_ADVANCED, &kAdvanced,
                        nullptr);
    InsertOwnerDrawItem(g_menuBar, 0, &kFile, fileMenu);
    InsertOwnerDrawItem(g_menuBar, 0, &kView, g_viewMenu);

    SetMenu(window, g_menuBar);
    CheckMode(mode);
    ApplyMenuBackground(window);
}

void DestroyMenuBar(HWND window)
{
    if (g_menuBar == nullptr) {
        return;
    }

    if (window != nullptr) {
        SetMenu(window, nullptr);
    }
    // Destroying the menu bar also destroys its popup menus.
    DestroyMenu(g_menuBar);
    g_menuBar = nullptr;
    g_viewMenu = nullptr;
}

void RefreshMenuBar(HWND window)
{
    ApplyMenuBackground(window);
}

void UpdateMenuMode(HWND window, gui::InterfaceMode mode)
{
    CheckMode(mode);
    if (window != nullptr) {
        DrawMenuBar(window);
    }
}

bool MeasureMenuItem(HWND window, MEASUREITEMSTRUCT* item)
{
    if (item == nullptr || item->CtlType != ODT_MENU) {
        return false;
    }

    const auto* entry = reinterpret_cast<const MenuEntry*>(item->itemData);
    if (entry == nullptr || entry->text == nullptr) {
        return false;
    }

    HDC dc = GetDC(window);
    HGDIOBJ oldFont = SelectObject(dc, MenuFont());

    SIZE textSize{};
    GetTextExtentPoint32W(dc, entry->text,
                          static_cast<int>(wcslen(entry->text)), &textSize);

    TEXTMETRICW metrics{};
    GetTextMetricsW(dc, &metrics);

    SelectObject(dc, oldFont);
    ReleaseDC(window, dc);

    const int height = metrics.tmHeight + Scale(8);
    if (entry->topLevel) {
        item->itemWidth = textSize.cx + Scale(20);
        item->itemHeight = height;
    } else {
        item->itemWidth = textSize.cx + Scale(40);
        item->itemHeight = height + Scale(2);
    }
    return true;
}

bool DrawMenuItem(HWND window, DRAWITEMSTRUCT* item)
{
    (void)window;

    if (item == nullptr || item->CtlType != ODT_MENU) {
        return false;
    }

    const auto* entry = reinterpret_cast<const MenuEntry*>(item->itemData);
    if (entry == nullptr || entry->text == nullptr) {
        return false;
    }

    const Theme& theme = GetTheme();
    const bool selected = (item->itemState & ODS_SELECTED) != 0;
    const bool disabled = (item->itemState & ODS_DISABLED) != 0;
    const COLORREF background =
        selected ? theme.color.accent : theme.color.background;
    const COLORREF textColor =
        selected ? theme.color.accentText
                 : (disabled ? theme.color.disabledText : theme.color.text);

    HBRUSH backgroundBrush = CreateSolidBrush(background);
    FillRect(item->hDC, &item->rcItem, backgroundBrush);
    DeleteObject(backgroundBrush);

    RECT textRect = item->rcItem;
    if (!entry->topLevel) {
        textRect.left += Scale(24);

        if ((item->itemState & ODS_CHECKED) != 0) {
            const int centerX = item->rcItem.left + Scale(11);
            const int centerY = (item->rcItem.top + item->rcItem.bottom) / 2;
            const int radius = Scale(3);

            HBRUSH dot = CreateSolidBrush(textColor);
            HGDIOBJ oldBrush = SelectObject(item->hDC, dot);
            HGDIOBJ oldPen =
                SelectObject(item->hDC, GetStockObject(NULL_PEN));
            Ellipse(item->hDC, centerX - radius, centerY - radius,
                    centerX + radius + 1, centerY + radius + 1);
            SelectObject(item->hDC, oldPen);
            SelectObject(item->hDC, oldBrush);
            DeleteObject(dot);
        }
    }

    HGDIOBJ oldFont = SelectObject(item->hDC, MenuFont());
    SetBkMode(item->hDC, TRANSPARENT);
    SetTextColor(item->hDC, textColor);
    DrawTextW(item->hDC, entry->text, -1, &textRect,
              DT_SINGLELINE | DT_VCENTER |
                  (entry->topLevel ? DT_CENTER : DT_LEFT));
    SelectObject(item->hDC, oldFont);
    return true;
}

} // namespace ui
