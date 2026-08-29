// Game Optimizer - visual theme: design tokens and the drawing helpers built on them.
//
// The look is modelled on FreeToken Desktop (github.com/FlashML-org/FreeToken, Apache-2.0):
// a dark, card-based dashboard with a left sidebar, rounded surfaces, a restrained accent,
// and MONOSPACE for anything numeric or technical. Only the visual language is adopted -
// no code, no assets and no logo from that project appear here.
//
// WHY A TOKEN LAYER AND NOT COLOURS SPRINKLED THROUGH THE UI FILES:
// every colour, radius, font and spacing value lives here exactly once. A theme change is
// then an edit to this file, not an archaeology exercise across four thousand lines of
// settings.cpp. It also makes the light variant a data change rather than a rewrite.
//
// STANDARD WIN32 CONTROLS DO NOT GO DARK ON THEIR OWN. Three mechanisms are used, in
// descending order of how well documented they are:
//   1. WM_CTLCOLOR* - documented, reliable. Covers STATIC, EDIT, LISTBOX backgrounds/text.
//   2. Owner-draw (BS_OWNERDRAW, CBS_OWNERDRAWFIXED, LBS_OWNERDRAWFIXED) - documented.
//      Buttons and combos are drawn by us, because their themed light chrome cannot be
//      recoloured any other way.
//   3. DwmSetWindowAttribute(DWMWA_USE_IMMERSIVE_DARK_MODE) - documented since Windows 11,
//      for the title bar and the window frame only.
// A fourth route exists (the undocumented uxtheme ordinals SetPreferredAppMode /
// AllowDarkModeForWindow) which would darken scrollbars and combo dropdowns "for free".
// It is DELIBERATELY NOT USED: undocumented ordinals shift between Windows builds and this
// app must not break on a Windows update. The cost is that a few system-drawn parts - the
// scrollbar and the combo's dropdown list - stay light. That is a stated, accepted trade.
#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <string>

namespace cd {
namespace theme {

// ---- Palette ---------------------------------------------------------------
// Dark is the default and the only variant v1 ships. Keep every value here.
struct Palette {
    COLORREF appBg;        // window background behind everything
    COLORREF sidebarBg;    // the nav rail, slightly darker than appBg
    COLORREF cardBg;       // panel/card surface
    COLORREF cardBgAlt;    // a raised surface: active nav item, hovered row
    COLORREF inputBg;      // EDIT / LISTBOX interior
    COLORREF border;       // 1px card and input outline
    COLORREF borderStrong; // focus ring, selected cell outline

    COLORREF textPrimary;
    COLORREF textSecondary;
    COLORREF textDim;      // captions, units, disabled
    COLORREF textOnAccent;

    COLORREF accent;       // primary action, selection, gauge arc
    COLORREF accentHover;
    COLORREF accentPressed;
    COLORREF good;         // healthy state
    COLORREF warn;         // the parked-mask warning, degraded states
    COLORREF danger;       // blocked, failed

    COLORREF cacheDomain;  // the cache CCD in the core map
    COLORREF freqDomain;   // the frequency CCD
    COLORREF parkedHatch;  // hatch drawn over parked processors
};

const Palette& P();

// ---- Metrics, in logical units. Scale everything through Dp(). -------------
namespace metric {
constexpr int kSidebarW    = 208;
constexpr int kNavItemH    = 38;
constexpr int kNavRadius   = 8;
constexpr int kCardRadius  = 10;
constexpr int kCardPad     = 16;
constexpr int kGap         = 12;
constexpr int kGapTight    = 6;
constexpr int kRowH        = 26;   // one control row
constexpr int kButtonH     = 30;
constexpr int kButtonW     = 104;
constexpr int kButtonRadius= 7;
constexpr int kPillRadius  = 999;  // clamped to height/2 by the drawing code
constexpr int kFocusInset  = 2;
}  // namespace metric

// Scale a logical value to the given DPI. Use for EVERY coordinate; a hardcoded pixel
// layout is unreadable at 150%, which is a common desktop setting.
int Dp(int logical, int dpi);

// ---- Fonts -----------------------------------------------------------------
// UI  : Segoe UI Variable Text, falling back to Segoe UI, then the system UI font.
// Mono: Cascadia Mono, falling back to Consolas, then Courier New.
//
// Monospace is not decoration here. Logical-processor indices, CPU Set ids, cache sizes
// and mask names are columnar data, and proportional digits make them impossible to scan.
enum class Font {
    UiSmall,      // captions, units
    UiBody,       // default control text
    UiStrong,     // emphasised body
    UiHeading,    // section title
    UiTitle,      // page title
    MonoSmall,    // dense numeric tables, core map cell labels
    MonoBody,     // technical strings: ids, signatures, exe names
    MonoDisplay,  // the big hero number on a stat card
};

// Cached per (Font, dpi). Never delete the returned handle.
HFONT GetFont(Font f, int dpi);

// Frees every cached font and brush. Call once at shutdown.
void Shutdown();

// ---- Window-level setup ----------------------------------------------------
// Dark title bar + frame via DwmSetWindowAttribute. Safe on Windows 10 (the call simply
// fails and the frame stays light); returns whether the attribute was accepted.
bool ApplyDarkFrame(HWND hwnd);

// Fills rc with appBg. Use from WM_ERASEBKGND so no light flash appears.
void FillBackground(HDC dc, const RECT& rc);

// ---- Drawing primitives ----------------------------------------------------
// All of these are plain GDI - no GDI+, no Direct2D, so nothing new is linked and the
// binary stays dependency-free. Rounded corners come from RoundRect with a matched pen.

void FillRoundRect(HDC dc, const RECT& rc, int radius, COLORREF fill);
void DrawRoundRect(HDC dc, const RECT& rc, int radius, COLORREF fill, COLORREF border);

// A card: rounded, filled with cardBg, 1px border. Returns the inner content rect with
// kCardPad already applied, so callers never repeat the padding maths.
RECT DrawCard(HDC dc, const RECT& rc, int dpi);

// A small rounded label, e.g. "High confidence", "16 parked". Height comes from the text.
void DrawPill(HDC dc, const RECT& rc, const std::wstring& text, int dpi,
              COLORREF bg, COLORREF fg);

// A filled circle used as a status indicator before a label.
void DrawStatusDot(HDC dc, int cx, int cy, int radius, COLORREF colour);

// A thin ring gauge with `pct` of its circumference drawn in `arc`, the remainder in
// `track`, and the value printed in the middle in MonoDisplay. This is the FreeToken
// signature element; used for "logical processors in this mask" and similar ratios.
void DrawRingGauge(HDC dc, const RECT& rc, double pct, int dpi,
                   COLORREF arc, COLORREF track,
                   const std::wstring& centreText, const std::wstring& centreUnit);

// Text helpers that set colour + font + transparent background and restore afterwards.
void DrawText(HDC dc, const RECT& rc, const std::wstring& s, Font f, int dpi,
              COLORREF colour, UINT format);
SIZE MeasureText(HDC dc, const std::wstring& s, Font f, int dpi);

// ---- Owner-draw handlers ---------------------------------------------------
// Wired from the parent's WM_DRAWITEM. Each returns TRUE when it handled the item.
enum class ButtonKind { Primary, Secondary, Ghost, Danger };

// Store the kind on the control with SetWindowLongPtr(GWLP_USERDATA) or pass it in.
BOOL DrawButton(const DRAWITEMSTRUCT* di, ButtonKind kind, int dpi);
BOOL DrawComboBox(const DRAWITEMSTRUCT* di, int dpi);
BOOL DrawListBoxItem(const DRAWITEMSTRUCT* di, int dpi);

// A check box drawn to match the rest of the theme. The system's check box glyph is drawn by
// the theme engine in the user's OS light/dark preference and cannot be recoloured through
// WM_CTLCOLOR, so on a dark card it appears as a bright white square. Owner-drawing it is the
// only route that keeps one visual language.
//
// `checked` comes from BM_GETCHECK; the caller owns that query because only it knows whether
// the control is a check box or a radio button. Draws the box, the tick, and the caption.
BOOL DrawCheckBox(const DRAWITEMSTRUCT* di, bool checked, int dpi);

// Paints a 1px themed frame over the light rectangle the system draws around a
// CBS_DROPDOWNLIST combo. Call from the PARENT's WM_PAINT, after the child has drawn, passing
// the combo's rect in parent client coordinates. This is a cover-up rather than a cure - the
// honest fix is owner-drawing the whole control, which is not worth it for a border.
void OverdrawComboFrame(HDC parentDc, const RECT& comboRectInParent, int dpi, bool focused);

// ---- Control colouring -----------------------------------------------------
// Call from the parent's WM_CTLCOLORSTATIC / EDIT / LISTBOX / BTN. Sets the DC's text and
// background colours and returns the brush to use, or nullptr when unhandled.
HBRUSH OnCtlColor(UINT msg, HDC dc, HWND ctl);

// ---- Sidebar navigation ----------------------------------------------------
// A registered child window class implementing the FreeToken-style rail: a vertical list
// of icon+label items, the active one on a raised rounded surface.
//
// The sidebar is not only cosmetic. It replaces a single tall scrolling page with one
// section at a time, which is what removes the scroll-repaint failure mode entirely
// rather than patching it - see docs\spec\04-measurements.md 2.7.
extern const wchar_t* kNavClass;
constexpr WORD NAVN_SELCHANGED = 0x8101;   // sent to the parent via WM_COMMAND

void NavRegister(HINSTANCE hInst);
void NavAddItem(HWND nav, int id, const std::wstring& label, const std::wstring& glyph);
void NavSetSelected(HWND nav, int id);
int  NavGetSelected(HWND nav);
// A small count or status badge on the right of an item; empty string removes it.
void NavSetBadge(HWND nav, int id, const std::wstring& badge);

// ---- Top tab bar -----------------------------------------------------------
// A horizontal strip of tabs across the top of the window, replacing the left rail.
// Operator decision, 2026-08-28: menu on top, no side panel. The rail stays in the codebase
// because it is what the first-run wizard uses, and because a future window narrow enough
// would want it back.
//
// Same contract as the rail so a caller can swap one for the other: the parent receives
// WM_COMMAND with HIWORD(wParam) == TABN_SELCHANGED.
extern const wchar_t* kTabBarClass;
constexpr WORD TABN_SELCHANGED = 0x8102;

namespace metric {
constexpr int kTabBarH     = 42;
constexpr int kTabPadX     = 14;   // horizontal padding inside one tab
constexpr int kTabGap      = 4;
constexpr int kTabRadius   = 8;
constexpr int kUnderlineH  = 2;    // accent underline beneath the active tab
}  // namespace metric

void TabBarRegister(HINSTANCE hInst);
void TabBarAddItem(HWND bar, int id, const std::wstring& label);
void TabBarSetSelected(HWND bar, int id);
int  TabBarGetSelected(HWND bar);
void TabBarSetBadge(HWND bar, int id, const std::wstring& badge);

// ---- Search box ------------------------------------------------------------
// Paints the themed frame and the magnifier affordance around a plain EDIT that the caller
// owns, plus the placeholder text when the edit is empty and unfocused. Call from the
// parent's WM_PAINT with the edit's rect in parent client coordinates - the same
// after-EndPaint GetDCEx route OverdrawComboFrame needs, because the parent is
// WS_CLIPCHILDREN and a BeginPaint DC has every child clipped out of it.
void DrawSearchChrome(HDC parentDc, const RECT& editRectInParent, int dpi,
                      bool focused, bool empty, const std::wstring& placeholder);

// ---- Inline CPU meter ------------------------------------------------------
// A slim horizontal bar for showing one process's live CPU use next to its name, as asked for
// in the Heavy apps list. `pct` is machine-wide percent, matching Task Manager's convention.
// Colours ramp good -> warn -> danger so a process that is about to trip the auto-pin
// threshold is visible before it does.
void DrawCpuMeter(HDC dc, const RECT& rc, double pct, int dpi, double threshold);

}  // namespace theme
}  // namespace cd
