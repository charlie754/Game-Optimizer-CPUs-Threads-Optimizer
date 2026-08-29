// Game Optimizer - implementation of the visual theme declared in theme.h.
//
// Plain GDI only: no GDI+, no Direct2D, no third-party library. dwmapi is resolved at
// runtime with LoadLibraryW/GetProcAddress so the executable does not gain an import and
// still starts on a system where the DLL or the attribute is absent.
//
// GDI OWNERSHIP RULES OBSERVED HERE:
//   * Every pen/brush created inside a drawing helper is selected out and deleted in the
//     same function. A leaked object in a paint path is fatal in a process that runs all
//     day - the handle table fills and the whole desktop starts failing to draw.
//   * The two long-lived caches (fonts, solid brushes) hand out handles the caller must
//     NEVER delete; Shutdown() frees them.
//
// RADIUS UNITS: FillRoundRect / DrawRoundRect take a radius in DEVICE pixels. Callers scale
// through Dp() first; the helpers that own a token radius (DrawCard, the nav rail) do that
// scaling themselves.

#include "theme.h"

#include <cmath>
#include <new>
#include <vector>

namespace cd {
namespace theme {

const wchar_t* kNavClass = L"GameOptimizerNav";

namespace {

// ---------------------------------------------------------------------------
// Small utilities. windows.h defines min/max as macros, so hand-rolled helpers
// avoid every argument-dependent surprise.
// ---------------------------------------------------------------------------
inline int IMin(int a, int b) { return a < b ? a : b; }
inline int IMax(int a, int b) { return a > b ? a : b; }

inline double DClamp(double v, double lo, double hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

// Linear interpolation between two colours. Plain GDI has no alpha, so every "translucent"
// effect in this UI - the nav hover surface, a dimmed disabled button - is an explicit
// channel-wise mix against the surface behind it.
COLORREF Blend(COLORREF c1, COLORREF c2, double t) {
    t = DClamp(t, 0.0, 1.0);
    const int r = static_cast<int>(static_cast<double>(GetRValue(c1)) +
                                  (static_cast<double>(GetRValue(c2)) -
                                   static_cast<double>(GetRValue(c1))) * t + 0.5);
    const int g = static_cast<int>(static_cast<double>(GetGValue(c1)) +
                                  (static_cast<double>(GetGValue(c2)) -
                                   static_cast<double>(GetGValue(c1))) * t + 0.5);
    const int b = static_cast<int>(static_cast<double>(GetBValue(c1)) +
                                  (static_cast<double>(GetBValue(c2)) -
                                   static_cast<double>(GetBValue(c1))) * t + 0.5);
    return RGB(static_cast<BYTE>(IMin(255, IMax(0, r))),
               static_cast<BYTE>(IMin(255, IMax(0, g))),
               static_cast<BYTE>(IMin(255, IMax(0, b))));
}

// ---------------------------------------------------------------------------
// One critical section guards both caches. Contention is nil (paint is on the UI
// thread) but GetFont is callable from anywhere and a torn cache would be a
// double-delete at shutdown.
// ---------------------------------------------------------------------------
class Section {
public:
    Section() { InitializeCriticalSection(&cs_); }
    ~Section() { DeleteCriticalSection(&cs_); }
    CRITICAL_SECTION* Raw() { return &cs_; }
private:
    CRITICAL_SECTION cs_;
    Section(const Section&) = delete;
    Section& operator=(const Section&) = delete;
};

CRITICAL_SECTION* Cs() {
    static Section s;   // C++11 magic static: initialised once, thread-safe under MSVC.
    return s.Raw();
}

class Guard {
public:
    Guard() { EnterCriticalSection(Cs()); }
    ~Guard() { LeaveCriticalSection(Cs()); }
private:
    Guard(const Guard&) = delete;
    Guard& operator=(const Guard&) = delete;
};

// ---------------------------------------------------------------------------
// Font cache, keyed on (Font, dpi).
// ---------------------------------------------------------------------------
struct FontEntry {
    Font  f;
    int   dpi;
    HFONT h;
};

std::vector<FontEntry>& FontCache() {
    static std::vector<FontEntry> v;
    return v;
}

struct FontSpec {
    const wchar_t* face[3];   // preference order; a null entry ends the list
    int            pt;
    int            weight;
};

FontSpec SpecFor(Font f) {
    const wchar_t* const kUi1 = L"Segoe UI Variable Text";
    const wchar_t* const kUi2 = L"Segoe UI";
    const wchar_t* const kMo1 = L"Cascadia Mono";
    const wchar_t* const kMo2 = L"Consolas";
    const wchar_t* const kMo3 = L"Courier New";

    FontSpec s;
    s.face[0] = kUi1; s.face[1] = kUi2; s.face[2] = nullptr;
    s.pt = 9; s.weight = FW_NORMAL;

    switch (f) {
    case Font::UiSmall:     s.pt = 8;  s.weight = FW_NORMAL;   break;
    case Font::UiBody:      s.pt = 9;  s.weight = FW_NORMAL;   break;
    case Font::UiStrong:    s.pt = 9;  s.weight = FW_BOLD;     break;
    case Font::UiHeading:   s.pt = 11; s.weight = FW_SEMIBOLD; break;
    case Font::UiTitle:     s.pt = 16; s.weight = FW_SEMIBOLD; break;
    case Font::MonoSmall:
        s.face[0] = kMo1; s.face[1] = kMo2; s.face[2] = kMo3;
        s.pt = 8;  s.weight = FW_NORMAL;
        break;
    case Font::MonoBody:
        s.face[0] = kMo1; s.face[1] = kMo2; s.face[2] = kMo3;
        s.pt = 9;  s.weight = FW_NORMAL;
        break;
    case Font::MonoDisplay:
        s.face[0] = kMo1; s.face[1] = kMo2; s.face[2] = kMo3;
        s.pt = 22; s.weight = FW_NORMAL;
        break;
    default: break;
    }
    return s;
}

// Create the first candidate face that ACTUALLY resolves. GDI silently substitutes a
// different family when a name is unknown, so asking for "Cascadia Mono" on a machine
// without it yields a proportional face and the columnar numbers stop lining up. The only
// reliable test is to select the font and read GetTextFaceW back.
HFONT CreateBestFont(const FontSpec& spec, int dpi) {
    const int height = -MulDiv(spec.pt, dpi, 72);
    HDC screen = GetDC(nullptr);
    HFONT chosen = nullptr;

    for (int i = 0; i < 3 && spec.face[i] != nullptr; ++i) {
        HFONT f = CreateFontW(height, 0, 0, 0, spec.weight, FALSE, FALSE, FALSE,
                              DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
                              CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, spec.face[i]);
        if (f == nullptr) continue;
        if (screen == nullptr) { chosen = f; break; }

        HGDIOBJ old = SelectObject(screen, f);
        wchar_t actual[LF_FACESIZE];
        actual[0] = L'\0';
        GetTextFaceW(screen, LF_FACESIZE, actual);
        SelectObject(screen, old);

        if (lstrcmpiW(actual, spec.face[i]) == 0) { chosen = f; break; }
        DeleteObject(f);
    }

    if (chosen == nullptr) {
        // Nothing in the preference list exists: take the system UI face and only override
        // the size and weight, so the result is still DPI-correct.
        LOGFONTW lf;
        ZeroMemory(&lf, sizeof(lf));
        HFONT stock = static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
        if (stock != nullptr && GetObjectW(stock, static_cast<int>(sizeof(lf)), &lf) != 0) {
            lf.lfHeight  = height;
            lf.lfWidth   = 0;
            lf.lfWeight  = spec.weight;
            lf.lfQuality = CLEARTYPE_QUALITY;
            chosen = CreateFontIndirectW(&lf);
        }
        if (chosen == nullptr) {
            chosen = CreateFontW(height, 0, 0, 0, spec.weight, FALSE, FALSE, FALSE,
                                 DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                 CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, nullptr);
        }
    }

    if (screen != nullptr) ReleaseDC(nullptr, screen);
    return chosen;
}

// ---------------------------------------------------------------------------
// Solid-brush cache. WM_CTLCOLOR* fires thousands of times a minute; creating a
// brush per message is a guaranteed GDI leak.
// ---------------------------------------------------------------------------
struct BrushEntry {
    COLORREF c;
    HBRUSH   h;
};

std::vector<BrushEntry>& BrushCache() {
    static std::vector<BrushEntry> v;
    return v;
}

HBRUSH CachedBrush(COLORREF c) {
    Guard lock;
    std::vector<BrushEntry>& v = BrushCache();
    for (size_t i = 0; i < v.size(); ++i) {
        if (v[i].c == c) return v[i].h;
    }
    HBRUSH h = CreateSolidBrush(c);
    if (h != nullptr) {
        BrushEntry e;
        e.c = c;
        e.h = h;
        v.push_back(e);
    }
    return h;
}

// ---------------------------------------------------------------------------
// dwmapi, resolved late.
// ---------------------------------------------------------------------------
typedef HRESULT (WINAPI* PFN_DwmSetWindowAttribute)(HWND, DWORD, LPCVOID, DWORD);

HMODULE g_dwm = nullptr;
PFN_DwmSetWindowAttribute g_dwmSet = nullptr;
bool g_dwmTried = false;

PFN_DwmSetWindowAttribute DwmSet() {
    Guard lock;
    if (!g_dwmTried) {
        g_dwmTried = true;
        g_dwm = LoadLibraryW(L"dwmapi.dll");
        if (g_dwm != nullptr) {
            g_dwmSet = reinterpret_cast<PFN_DwmSetWindowAttribute>(
                reinterpret_cast<void*>(GetProcAddress(g_dwm, "DwmSetWindowAttribute")));
        }
    }
    return g_dwmSet;
}

// ---------------------------------------------------------------------------
// Per-monitor DPI without a hard dependency on a Windows 10 export.
// ---------------------------------------------------------------------------
int DpiOfWindow(HWND hwnd) {
    typedef UINT (WINAPI* PFN_GetDpiForWindow)(HWND);
    static PFN_GetDpiForWindow fn = []() -> PFN_GetDpiForWindow {
        HMODULE u = GetModuleHandleW(L"user32.dll");
        if (u == nullptr) return nullptr;
        return reinterpret_cast<PFN_GetDpiForWindow>(
            reinterpret_cast<void*>(GetProcAddress(u, "GetDpiForWindow")));
    }();

    if (fn != nullptr && hwnd != nullptr) {
        const UINT d = fn(hwnd);
        if (d != 0) return static_cast<int>(d);
    }
    HDC dc = GetDC(hwnd);
    int d = 96;
    if (dc != nullptr) {
        const int got = GetDeviceCaps(dc, LOGPIXELSX);
        if (got > 0) d = got;
        ReleaseDC(hwnd, dc);
    }
    return d;
}

// Outline-only rounded rectangle (focus rings, hover outlines).
void StrokeRoundRect(HDC dc, const RECT& rc, int radius, COLORREF colour) {
    const int w = rc.right - rc.left;
    const int h = rc.bottom - rc.top;
    if (w <= 0 || h <= 0) return;
    const int r = IMax(0, IMin(radius, IMin(w, h) / 2));

    HPEN pen = CreatePen(PS_SOLID, 1, colour);
    if (pen == nullptr) return;
    HGDIOBJ oldPen = SelectObject(dc, pen);
    HGDIOBJ oldBr  = SelectObject(dc, GetStockObject(NULL_BRUSH));
    RoundRect(dc, rc.left, rc.top, rc.right, rc.bottom, r * 2, r * 2);
    SelectObject(dc, oldBr);
    SelectObject(dc, oldPen);
    DeleteObject(pen);
}

}  // namespace

// ===========================================================================
// Palette
// ===========================================================================
const Palette& P() {
    static const Palette p = []() -> Palette {
        Palette q;
        q.appBg         = RGB(0x0F, 0x11, 0x15);
        q.sidebarBg     = RGB(0x0B, 0x0D, 0x11);
        q.cardBg        = RGB(0x16, 0x1A, 0x20);
        q.cardBgAlt     = RGB(0x1E, 0x23, 0x2B);
        q.inputBg       = RGB(0x12, 0x15, 0x1A);
        q.border        = RGB(0x23, 0x28, 0x30);
        q.borderStrong  = RGB(0x33, 0x3A, 0x45);

        q.textPrimary   = RGB(0xE6, 0xE9, 0xEF);
        q.textSecondary = RGB(0x8A, 0x93, 0xA3);
        q.textDim       = RGB(0x5D, 0x66, 0x75);
        q.textOnAccent  = RGB(0xFF, 0xFF, 0xFF);

        q.accent        = RGB(0x5B, 0x9B, 0xFF);
        q.accentHover   = RGB(0x74, 0xAD, 0xFF);
        q.accentPressed = RGB(0x3F, 0x80, 0xE6);
        q.good          = RGB(0x4A, 0xDE, 0x80);
        q.warn          = RGB(0xF5, 0xA5, 0x24);
        q.danger        = RGB(0xF0, 0x44, 0x38);

        q.cacheDomain   = RGB(0x5B, 0x9B, 0xFF);
        q.freqDomain    = RGB(0x9B, 0x8A, 0xFF);
        q.parkedHatch   = RGB(0x3A, 0x41, 0x4D);
        return q;
    }();
    return p;
}

// ===========================================================================
// Metrics
// ===========================================================================
int Dp(int logical, int dpi) {
    if (dpi <= 0) dpi = 96;
    return MulDiv(logical, dpi, 96);
}

// ===========================================================================
// Fonts
// ===========================================================================
HFONT GetFont(Font f, int dpi) {
    if (dpi <= 0) dpi = 96;

    Guard lock;
    std::vector<FontEntry>& v = FontCache();
    for (size_t i = 0; i < v.size(); ++i) {
        if (v[i].f == f && v[i].dpi == dpi) return v[i].h;
    }

    HFONT h = CreateBestFont(SpecFor(f), dpi);
    if (h == nullptr) return static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));

    FontEntry e;
    e.f   = f;
    e.dpi = dpi;
    e.h   = h;
    v.push_back(e);
    return h;
}

void Shutdown() {
    Guard lock;

    std::vector<FontEntry>& fv = FontCache();
    for (size_t i = 0; i < fv.size(); ++i) {
        if (fv[i].h != nullptr) DeleteObject(fv[i].h);
    }
    fv.clear();

    std::vector<BrushEntry>& bv = BrushCache();
    for (size_t i = 0; i < bv.size(); ++i) {
        if (bv[i].h != nullptr) DeleteObject(bv[i].h);
    }
    bv.clear();

    if (g_dwm != nullptr) {
        FreeLibrary(g_dwm);
        g_dwm = nullptr;
    }
    g_dwmSet  = nullptr;
    g_dwmTried = false;
}

// ===========================================================================
// Window-level setup
// ===========================================================================
bool ApplyDarkFrame(HWND hwnd) {
    if (hwnd == nullptr) return false;
    PFN_DwmSetWindowAttribute set = DwmSet();
    if (set == nullptr) return false;

    // 20 = DWMWA_USE_IMMERSIVE_DARK_MODE on Windows 10 2004 and later.
    // 19 = the same attribute on the 1809/1903 builds, before it was renumbered.
    BOOL on = TRUE;
    if (SUCCEEDED(set(hwnd, 20, &on, static_cast<DWORD>(sizeof(on))))) return true;
    on = TRUE;
    if (SUCCEEDED(set(hwnd, 19, &on, static_cast<DWORD>(sizeof(on))))) return true;
    return false;
}

void FillBackground(HDC dc, const RECT& rc) {
    if (dc == nullptr) return;
    RECT r = rc;
    HBRUSH br = CachedBrush(P().appBg);
    if (br != nullptr) FillRect(dc, &r, br);
}

// ===========================================================================
// Drawing primitives
// ===========================================================================
void FillRoundRect(HDC dc, const RECT& rc, int radius, COLORREF fill) {
    // A NULL_PEN would clip the rounded edge one pixel short, so the pen matches the fill.
    DrawRoundRect(dc, rc, radius, fill, fill);
}

void DrawRoundRect(HDC dc, const RECT& rc, int radius, COLORREF fill, COLORREF border) {
    if (dc == nullptr) return;
    const int w = rc.right - rc.left;
    const int h = rc.bottom - rc.top;
    if (w <= 0 || h <= 0) return;
    const int r = IMax(0, IMin(radius, IMin(w, h) / 2));

    HPEN   pen   = CreatePen(PS_SOLID, 1, border);
    HBRUSH brush = CreateSolidBrush(fill);
    if (pen == nullptr || brush == nullptr) {
        if (pen   != nullptr) DeleteObject(pen);
        if (brush != nullptr) DeleteObject(brush);
        return;
    }

    HGDIOBJ oldPen   = SelectObject(dc, pen);
    HGDIOBJ oldBrush = SelectObject(dc, brush);
    RoundRect(dc, rc.left, rc.top, rc.right, rc.bottom, r * 2, r * 2);
    SelectObject(dc, oldBrush);
    SelectObject(dc, oldPen);
    DeleteObject(brush);
    DeleteObject(pen);
}

RECT DrawCard(HDC dc, const RECT& rc, int dpi) {
    DrawRoundRect(dc, rc, Dp(metric::kCardRadius, dpi), P().cardBg, P().border);
    RECT inner = rc;
    const int pad = Dp(metric::kCardPad, dpi);
    InflateRect(&inner, -pad, -pad);
    if (inner.right  < inner.left) inner.right  = inner.left;
    if (inner.bottom < inner.top)  inner.bottom = inner.top;
    return inner;
}

void DrawPill(HDC dc, const RECT& rc, const std::wstring& text, int dpi,
              COLORREF bg, COLORREF fg) {
    if (dc == nullptr) return;
    const int h = rc.bottom - rc.top;
    if (h <= 0 || rc.right <= rc.left) return;

    FillRoundRect(dc, rc, h / 2, bg);

    RECT t = rc;
    const int padx = Dp(8, dpi);
    t.left  += padx;
    t.right -= padx;
    if (t.right < t.left) t.right = t.left;
    DrawText(dc, t, text, Font::UiSmall, dpi, fg,
             DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
}

void DrawStatusDot(HDC dc, int cx, int cy, int radius, COLORREF colour) {
    if (dc == nullptr) return;
    const int r = IMax(1, radius);

    HPEN   pen   = CreatePen(PS_SOLID, 1, colour);
    HBRUSH brush = CreateSolidBrush(colour);
    if (pen == nullptr || brush == nullptr) {
        if (pen   != nullptr) DeleteObject(pen);
        if (brush != nullptr) DeleteObject(brush);
        return;
    }
    HGDIOBJ oldPen   = SelectObject(dc, pen);
    HGDIOBJ oldBrush = SelectObject(dc, brush);
    Ellipse(dc, cx - r, cy - r, cx + r + 1, cy + r + 1);
    SelectObject(dc, oldBrush);
    SelectObject(dc, oldPen);
    DeleteObject(brush);
    DeleteObject(pen);
}

void DrawRingGauge(HDC dc, const RECT& rc, double pct, int dpi,
                   COLORREF arc, COLORREF track,
                   const std::wstring& centreText, const std::wstring& centreUnit) {
    if (dc == nullptr) return;
    const int w = rc.right - rc.left;
    const int h = rc.bottom - rc.top;
    if (w <= 0 || h <= 0) return;

    pct = DClamp(pct, 0.0, 1.0);

    const int side  = IMin(w, h);
    const int thick = IMax(1, Dp(6, dpi));
    const int cx    = rc.left + w / 2;
    const int cy    = rc.top  + h / 2;
    const int R     = side / 2 - thick / 2 - 1;

    if (R > 1) {
        // A plain CreatePen gives butt caps; the arc then ends in a hard square edge and the
        // ring reads as a broken segment rather than a gauge. PS_GEOMETRIC + PS_ENDCAP_ROUND
        // is the only way to get a rounded cap out of GDI.
        LOGBRUSH lb;
        lb.lbStyle = BS_SOLID;
        lb.lbColor = track;
        lb.lbHatch = 0;

        HPEN trackPen = ExtCreatePen(PS_GEOMETRIC | PS_SOLID | PS_ENDCAP_ROUND | PS_JOIN_ROUND,
                                     static_cast<DWORD>(thick), &lb, 0, nullptr);
        lb.lbColor = arc;
        HPEN arcPen = ExtCreatePen(PS_GEOMETRIC | PS_SOLID | PS_ENDCAP_ROUND | PS_JOIN_ROUND,
                                   static_cast<DWORD>(thick), &lb, 0, nullptr);

        HGDIOBJ oldBrush = SelectObject(dc, GetStockObject(NULL_BRUSH));
        const int oldDir = SetArcDirection(dc, AD_CLOCKWISE);

        if (trackPen != nullptr) {
            HGDIOBJ oldPen = SelectObject(dc, trackPen);
            Ellipse(dc, cx - R, cy - R, cx + R, cy + R);
            SelectObject(dc, oldPen);
        }

        if (arcPen != nullptr && pct > 0.0) {
            HGDIOBJ oldPen = SelectObject(dc, arcPen);
            if (pct >= 0.999) {
                Ellipse(dc, cx - R, cy - R, cx + R, cy + R);
            } else {
                // 12 o'clock, sweeping clockwise.
                const double a  = pct * 6.283185307179586;
                const int    sx = cx;
                const int    sy = cy - R;
                const int    ex = cx + static_cast<int>(std::floor(R * std::sin(a) + 0.5));
                const int    ey = cy - static_cast<int>(std::floor(R * std::cos(a) + 0.5));
                // Arc() draws a FULL ellipse when the two endpoints coincide, so a sliver
                // that rounds to zero length must be skipped rather than drawn.
                if (!(ex == sx && ey == sy)) {
                    Arc(dc, cx - R, cy - R, cx + R, cy + R, sx, sy, ex, ey);
                }
            }
            SelectObject(dc, oldPen);
        }

        SetArcDirection(dc, oldDir);
        SelectObject(dc, oldBrush);
        if (arcPen   != nullptr) DeleteObject(arcPen);
        if (trackPen != nullptr) DeleteObject(trackPen);
    }

    // Centre label: the value in MonoDisplay with the unit riding on its baseline.
    if (!centreText.empty() || !centreUnit.empty()) {
        const SIZE big  = MeasureText(dc, centreText, Font::MonoDisplay, dpi);
        const SIZE unit = centreUnit.empty() ? SIZE{0, 0}
                                             : MeasureText(dc, centreUnit, Font::UiSmall, dpi);
        const int gap   = centreUnit.empty() ? 0 : Dp(2, dpi);
        const int total = static_cast<int>(big.cx) + gap + static_cast<int>(unit.cx);

        int x = cx - total / 2;
        RECT bt;
        bt.left   = x;
        bt.right  = x + static_cast<int>(big.cx);
        bt.top    = cy - static_cast<int>(big.cy) / 2;
        bt.bottom = bt.top + static_cast<int>(big.cy);
        if (!centreText.empty()) {
            DrawText(dc, bt, centreText, Font::MonoDisplay, dpi, P().textPrimary,
                     DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        }
        if (!centreUnit.empty()) {
            RECT ut;
            ut.left   = bt.right + gap;
            ut.right  = ut.left + static_cast<int>(unit.cx);
            ut.bottom = bt.bottom - Dp(3, dpi);
            ut.top    = ut.bottom - static_cast<int>(unit.cy);
            DrawText(dc, ut, centreUnit, Font::UiSmall, dpi, P().textSecondary,
                     DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        }
    }
}

// NOTE: with UNICODE defined, windows.h has already rewritten the token DrawText to
// DrawTextW, in the header and here alike - so this defines cd::theme::DrawTextW and the
// Win32 API must be reached through the global scope operator.
void DrawText(HDC dc, const RECT& rc, const std::wstring& s, Font f, int dpi,
              COLORREF colour, UINT format) {
    if (dc == nullptr) return;
    HFONT font = GetFont(f, dpi);
    HGDIOBJ oldFont = SelectObject(dc, font);
    const int      oldMode  = SetBkMode(dc, TRANSPARENT);
    const COLORREF oldColour = SetTextColor(dc, colour);

    RECT r = rc;
    ::DrawTextW(dc, s.c_str(), static_cast<int>(s.size()), &r, format);

    SetTextColor(dc, oldColour);
    SetBkMode(dc, oldMode);
    SelectObject(dc, oldFont);
}

SIZE MeasureText(HDC dc, const std::wstring& s, Font f, int dpi) {
    SIZE sz;
    sz.cx = 0;
    sz.cy = 0;
    if (dc == nullptr) return sz;

    HFONT font = GetFont(f, dpi);
    HGDIOBJ oldFont = SelectObject(dc, font);
    if (s.empty()) {
        TEXTMETRICW tm;
        if (GetTextMetricsW(dc, &tm)) sz.cy = tm.tmHeight;
    } else {
        GetTextExtentPoint32W(dc, s.c_str(), static_cast<int>(s.size()), &sz);
    }
    SelectObject(dc, oldFont);
    return sz;
}

// ===========================================================================
// Owner-draw handlers
// ===========================================================================
namespace {

std::wstring WindowText(HWND h) {
    if (h == nullptr) return std::wstring();
    const int len = GetWindowTextLengthW(h);
    if (len <= 0) return std::wstring();
    std::vector<wchar_t> buf(static_cast<size_t>(len) + 1, L'\0');
    const int got = GetWindowTextW(h, &buf[0], len + 1);
    if (got <= 0) return std::wstring();
    return std::wstring(&buf[0], static_cast<size_t>(got));
}

}  // namespace

BOOL DrawButton(const DRAWITEMSTRUCT* di, ButtonKind kind, int dpi) {
    if (di == nullptr || di->hDC == nullptr) return FALSE;

    const Palette& p = P();
    const RECT rc = di->rcItem;
    if (rc.right <= rc.left || rc.bottom <= rc.top) return FALSE;

    const bool pressed  = (di->itemState & ODS_SELECTED) != 0;
    const bool disabled = (di->itemState & (ODS_DISABLED | ODS_GRAYED)) != 0;
    const bool hot      = (di->itemState & ODS_HOTLIGHT) != 0;
    const bool focused  = (di->itemState & ODS_FOCUS) != 0;

    COLORREF fill   = p.cardBgAlt;
    COLORREF border = p.border;
    COLORREF text   = p.textPrimary;
    bool     hasFill = true;

    switch (kind) {
    case ButtonKind::Primary:
        fill   = pressed ? p.accentPressed : (hot ? p.accentHover : p.accent);
        border = fill;
        text   = p.textOnAccent;
        break;
    case ButtonKind::Secondary:
        fill   = pressed ? Blend(p.cardBgAlt, p.appBg, 0.35)
                         : (hot ? Blend(p.cardBgAlt, p.borderStrong, 0.30) : p.cardBgAlt);
        border = hot ? p.borderStrong : p.border;
        text   = p.textPrimary;
        break;
    case ButtonKind::Ghost:
        // No surface of its own, in ANY state - the rect is left entirely unpainted.
        //
        // GDI cannot sample the pixels behind a child window, so an owner-draw button has no
        // way to reproduce whatever surface it sits on. Filling with appBg guessed that the
        // surface was the app background; on a card it is not, and the guess stamped a
        // visible appBg patch into the card. Painting nothing keeps what the parent already
        // drew in its own WM_PAINT, which runs before the child controls draw, so a Ghost
        // button is correct on the app background and on a card alike.
        //
        // The cost is that press and hover can only be shown in the caption colour: any
        // surface painted for those states would have to be un-painted on the way out, and
        // this control cannot repaint the parent underneath itself.
        fill    = p.appBg;     // unused while hasFill is false, but kept sane for `disabled`
        border  = p.appBg;
        text    = (pressed || hot) ? p.textPrimary : p.textSecondary;
        hasFill = false;
        break;
    case ButtonKind::Danger:
        fill   = pressed ? Blend(p.danger, RGB(0, 0, 0), 0.25)
                         : (hot ? Blend(p.danger, RGB(255, 255, 255), 0.12) : p.danger);
        border = fill;
        text   = p.textOnAccent;
        break;
    default:
        break;
    }

    if (disabled) {
        fill   = Blend(fill, p.appBg, 0.55);
        border = Blend(border, p.appBg, 0.55);
        text   = p.textDim;
    }

    const int radius = Dp(metric::kButtonRadius, dpi);
    if (hasFill) {
        DrawRoundRect(di->hDC, rc, radius, fill, border);
    }

    RECT t = rc;
    const int padx = Dp(10, dpi);
    t.left  += padx;
    t.right -= padx;
    if (t.right < t.left) t.right = t.left;
    const std::wstring caption = WindowText(di->hwndItem);
    if (!caption.empty()) {
        DrawText(di->hDC, t, caption, Font::UiBody, dpi, text,
                 DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
    }

    if (focused && !disabled) {
        RECT fr = rc;
        const int inset = Dp(metric::kFocusInset, dpi);
        InflateRect(&fr, -inset, -inset);
        StrokeRoundRect(di->hDC, fr, IMax(0, radius - inset), p.borderStrong);
    }
    return TRUE;
}

BOOL DrawComboBox(const DRAWITEMSTRUCT* di, int dpi) {
    if (di == nullptr || di->hDC == nullptr) return FALSE;

    const Palette& p = P();
    RECT rc = di->rcItem;
    if (rc.right <= rc.left || rc.bottom <= rc.top) return FALSE;

    const bool selected = (di->itemState & ODS_SELECTED) != 0;
    const bool disabled = (di->itemState & (ODS_DISABLED | ODS_GRAYED)) != 0;

    HBRUSH bg = CachedBrush(selected ? p.cardBgAlt : p.inputBg);
    if (bg != nullptr) FillRect(di->hDC, &rc, bg);

    // itemID == -1 means the control has no items at all - there is nothing to read and
    // asking for the text of item (UINT)-1 would be a bad index.
    std::wstring text;
    if (di->itemID != static_cast<UINT>(-1) && di->hwndItem != nullptr) {
        const LRESULT len = SendMessageW(di->hwndItem, CB_GETLBTEXTLEN,
                                         static_cast<WPARAM>(di->itemID), 0);
        if (len > 0) {
            std::vector<wchar_t> buf(static_cast<size_t>(len) + 1, L'\0');
            const LRESULT got = SendMessageW(di->hwndItem, CB_GETLBTEXT,
                                             static_cast<WPARAM>(di->itemID),
                                             reinterpret_cast<LPARAM>(&buf[0]));
            if (got > 0) text.assign(&buf[0], static_cast<size_t>(got));
        }
    }

    // NO CHEVRON IS DRAWN HERE, DELIBERATELY.
    //
    // CBS_OWNERDRAWFIXED hands the owner the ITEM, not the control: for a CBS_DROPDOWNLIST
    // combo the system still paints its own dropdown button, outside di->rcItem and beside
    // the area we are given. Drawing a second arrow inside rcItem put two chevrons side by
    // side on every combo in the app. The system button is the one the user can click, so
    // it is the one that stays; we paint the item background and its text and nothing else.
    //
    // Nothing else in this function reads the chevron: the only other thing it ever did was
    // reserve horizontal room, which is now a plain symmetric text inset. ODS_COMBOBOXEDIT
    // is consequently no longer needed to distinguish the closed field from a list row.
    RECT t = rc;
    const int padx = Dp(8, dpi);
    t.left  += padx;
    t.right -= padx;
    if (t.right < t.left) t.right = t.left;
    if (!text.empty()) {
        DrawText(di->hDC, t, text, Font::UiBody, dpi,
                 disabled ? p.textDim : p.textPrimary,
                 DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
    }
    return TRUE;
}

BOOL DrawListBoxItem(const DRAWITEMSTRUCT* di, int dpi) {
    if (di == nullptr || di->hDC == nullptr) return FALSE;

    const Palette& p = P();
    RECT rc = di->rcItem;
    if (rc.right <= rc.left || rc.bottom <= rc.top) return FALSE;

    const bool selected = (di->itemState & ODS_SELECTED) != 0;
    const bool disabled = (di->itemState & (ODS_DISABLED | ODS_GRAYED)) != 0;

    HBRUSH bg = CachedBrush(selected ? p.cardBgAlt : p.inputBg);
    if (bg != nullptr) FillRect(di->hDC, &rc, bg);

    if (di->itemID == static_cast<UINT>(-1)) return TRUE;   // empty list: background only

    const int barW = Dp(3, dpi);
    if (selected) {
        RECT bar = rc;
        bar.right = bar.left + barW;
        HBRUSH accentBrush = CachedBrush(p.accent);
        if (accentBrush != nullptr) FillRect(di->hDC, &bar, accentBrush);
    }

    std::wstring text;
    if (di->hwndItem != nullptr) {
        const LRESULT len = SendMessageW(di->hwndItem, LB_GETTEXTLEN,
                                         static_cast<WPARAM>(di->itemID), 0);
        if (len > 0) {
            std::vector<wchar_t> buf(static_cast<size_t>(len) + 1, L'\0');
            const LRESULT got = SendMessageW(di->hwndItem, LB_GETTEXT,
                                             static_cast<WPARAM>(di->itemID),
                                             reinterpret_cast<LPARAM>(&buf[0]));
            if (got > 0) text.assign(&buf[0], static_cast<size_t>(got));
        }
    }

    RECT t = rc;
    t.left  += barW + Dp(8, dpi);
    t.right -= Dp(8, dpi);
    if (t.right < t.left) t.right = t.left;
    if (!text.empty()) {
        DrawText(di->hDC, t, text, Font::UiBody, dpi,
                 disabled ? p.textDim : p.textPrimary,
                 DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
    }
    return TRUE;
}

BOOL DrawCheckBox(const DRAWITEMSTRUCT* di, bool checked, int dpi) {
    if (di == nullptr || di->hDC == nullptr) return FALSE;

    const Palette& p = P();
    const RECT rc = di->rcItem;
    if (rc.right <= rc.left || rc.bottom <= rc.top) return FALSE;

    const bool disabled = (di->itemState & (ODS_DISABLED | ODS_GRAYED)) != 0;
    const bool focused  = (di->itemState & ODS_FOCUS) != 0;

    // NO BACKGROUND IS PAINTED HERE, for the same reason ButtonKind::Ghost paints none: GDI
    // cannot sample the pixels behind a child window, so any fill would be a guess at which
    // surface the control sits on, and every check box in this app sits on a card rather
    // than on the app background. The caller owns the surface.

    const int side = IMax(1, IMin(Dp(15, dpi), rc.bottom - rc.top));
    RECT box;
    box.left   = rc.left;
    box.right  = box.left + side;
    box.top    = rc.top + ((rc.bottom - rc.top) - side) / 2;
    box.bottom = box.top + side;
    if (box.right > rc.right) box.right = rc.right;

    COLORREF fill   = checked ? p.accent : p.inputBg;
    COLORREF border = checked ? p.accent : p.borderStrong;
    COLORREF tick   = p.textOnAccent;
    if (disabled) {
        fill   = Blend(fill, p.appBg, 0.55);
        border = Blend(border, p.appBg, 0.55);
        tick   = Blend(tick, fill, 0.45);
    }
    DrawRoundRect(di->hDC, box, Dp(3, dpi), fill, border);

    if (checked) {
        const int bw = box.right - box.left;
        const int bh = box.bottom - box.top;
        POINT pts[3];
        pts[0].x = box.left + (bw * 24) / 100;   pts[0].y = box.top + (bh * 52) / 100;
        pts[1].x = box.left + (bw * 44) / 100;   pts[1].y = box.top + (bh * 72) / 100;
        pts[2].x = box.left + (bw * 78) / 100;   pts[2].y = box.top + (bh * 30) / 100;

        // A GEOMETRIC pen, not a cosmetic one: only a geometric pen honours the end-cap and
        // join styles, and a square join on a 2px tick leaves a visibly ragged corner at the
        // bottom of the V.
        LOGBRUSH lb;
        lb.lbStyle = BS_SOLID;
        lb.lbColor = tick;
        lb.lbHatch = 0;
        HPEN pen = ExtCreatePen(PS_GEOMETRIC | PS_SOLID | PS_ENDCAP_ROUND | PS_JOIN_ROUND,
                                static_cast<DWORD>(IMax(1, Dp(2, dpi))), &lb, 0, nullptr);
        if (pen != nullptr) {
            HGDIOBJ oldPen = SelectObject(di->hDC, pen);
            Polyline(di->hDC, pts, 3);
            SelectObject(di->hDC, oldPen);
            DeleteObject(pen);
        }
    }

    RECT t = rc;
    t.left = box.right + Dp(8, dpi);
    if (t.left > t.right) t.left = t.right;
    const std::wstring caption = WindowText(di->hwndItem);
    if (!caption.empty()) {
        DrawText(di->hDC, t, caption, Font::UiBody, dpi,
                 disabled ? p.textDim : p.textPrimary,
                 DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
    }

    if (focused && !disabled) {
        RECT fr = rc;
        const int inset = Dp(metric::kFocusInset, dpi);
        InflateRect(&fr, -inset, -inset);
        StrokeRoundRect(di->hDC, fr, IMax(0, Dp(metric::kButtonRadius, dpi) - inset),
                        p.borderStrong);
    }
    return TRUE;
}

void OverdrawComboFrame(HDC parentDc, const RECT& comboRectInParent, int dpi, bool focused) {
    if (parentDc == nullptr) return;
    const RECT rc = comboRectInParent;
    if (rc.right - rc.left < 2 || rc.bottom - rc.top < 2) return;

    // Outline only - a NULL_BRUSH, so the combo's own item text and the system's drop-down
    // button keep showing through and only the light 1px edge is replaced.
    StrokeRoundRect(parentDc, rc, Dp(4, dpi), focused ? P().accent : P().border);
}

// ===========================================================================
// Control colouring
// ===========================================================================
HBRUSH OnCtlColor(UINT msg, HDC dc, HWND ctl) {
    (void)ctl;
    if (dc == nullptr) return nullptr;
    const Palette& p = P();

    switch (msg) {
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN:
        SetTextColor(dc, p.textPrimary);
        SetBkColor(dc, p.appBg);
        return CachedBrush(p.appBg);

    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX:
        SetTextColor(dc, p.textPrimary);
        SetBkColor(dc, p.inputBg);
        return CachedBrush(p.inputBg);

    default:
        return nullptr;
    }
}

// ===========================================================================
// The sidebar nav control
// ===========================================================================
namespace {

struct NavItem {
    int          id;
    std::wstring label;
    std::wstring glyph;
    std::wstring badge;
};

struct NavState {
    std::vector<NavItem> items;
    int  sel      = -1;   // index, not id
    int  hot      = -1;
    bool tracking = false;
};

NavState* NavGet(HWND h) {
    if (h == nullptr) return nullptr;
    return reinterpret_cast<NavState*>(GetWindowLongPtrW(h, GWLP_USERDATA));
}

int NavIndexOfId(NavState* st, int id) {
    if (st == nullptr) return -1;
    for (size_t i = 0; i < st->items.size(); ++i) {
        if (st->items[i].id == id) return static_cast<int>(i);
    }
    return -1;
}

RECT NavItemRect(HWND hwnd, int dpi, int index) {
    RECT client;
    GetClientRect(hwnd, &client);
    const int itemH = Dp(metric::kNavItemH, dpi);
    const int top   = Dp(metric::kGapTight, dpi);
    RECT r;
    r.left   = client.left;
    r.right  = client.right;
    r.top    = top + index * itemH;
    r.bottom = r.top + itemH;
    return r;
}

int NavHitTest(HWND hwnd, NavState* st, int dpi, int x, int y) {
    if (st == nullptr || st->items.empty()) return -1;
    RECT client;
    GetClientRect(hwnd, &client);
    if (x < client.left || x >= client.right) return -1;

    const int itemH = Dp(metric::kNavItemH, dpi);
    const int top   = Dp(metric::kGapTight, dpi);
    if (itemH <= 0 || y < top) return -1;
    const int idx = (y - top) / itemH;
    if (idx < 0 || idx >= static_cast<int>(st->items.size())) return -1;
    return idx;
}

void NavNotify(HWND hwnd) {
    HWND parent = GetParent(hwnd);
    if (parent == nullptr) return;
    const WORD id = static_cast<WORD>(GetDlgCtrlID(hwnd));
    SendMessageW(parent, WM_COMMAND, MAKEWPARAM(id, NAVN_SELCHANGED),
                 reinterpret_cast<LPARAM>(hwnd));
}

void NavPaintTo(HWND hwnd, HDC dc, const RECT& client, NavState* st, int dpi) {
    const Palette& p = P();

    RECT bg = client;
    HBRUSH back = CachedBrush(p.sidebarBg);
    if (back != nullptr) FillRect(dc, &bg, back);
    if (st == nullptr) return;

    const bool focused = (GetFocus() == hwnd);
    const int  inset   = Dp(6, dpi);
    const int  radius  = Dp(metric::kNavRadius, dpi);

    for (size_t i = 0; i < st->items.size(); ++i) {
        const int idx = static_cast<int>(i);
        RECT row = NavItemRect(hwnd, dpi, idx);
        if (row.top >= client.bottom) break;

        RECT surf = row;
        surf.left  += inset;
        surf.right -= inset;
        surf.top    += Dp(2, dpi);
        surf.bottom -= Dp(2, dpi);

        const bool sel = (idx == st->sel);
        const bool hot = (idx == st->hot) && !sel;

        if (sel) {
            FillRoundRect(dc, surf, radius, p.cardBgAlt);
            if (focused) StrokeRoundRect(dc, surf, radius, p.borderStrong);
        } else if (hot) {
            // No alpha in plain GDI: the hover surface is an explicit mix of the two tokens.
            FillRoundRect(dc, surf, radius, Blend(p.cardBg, p.cardBgAlt, 0.5));
        }

        const COLORREF fg = sel ? p.textPrimary : p.textSecondary;

        RECT g = surf;
        g.left  += Dp(10, dpi);
        g.right  = g.left + Dp(18, dpi);
        if (!st->items[i].glyph.empty()) {
            DrawText(dc, g, st->items[i].glyph, Font::UiBody, dpi,
                     sel ? p.accent : p.textSecondary,
                     DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        }

        // Badge first, so the label can be clipped against it rather than overlapping.
        int labelRight = surf.right - Dp(10, dpi);
        if (!st->items[i].badge.empty()) {
            const SIZE bs = MeasureText(dc, st->items[i].badge, Font::UiSmall, dpi);
            const int  bh = Dp(18, dpi);
            const int  bw = IMax(bh, static_cast<int>(bs.cx) + Dp(16, dpi));
            RECT br;
            br.right  = surf.right - Dp(8, dpi);
            br.left   = br.right - bw;
            br.top    = (surf.top + surf.bottom) / 2 - bh / 2;
            br.bottom = br.top + bh;
            if (br.left > g.right) {
                DrawPill(dc, br, st->items[i].badge, dpi,
                         sel ? p.accent : p.cardBgAlt,
                         sel ? p.textOnAccent : p.textSecondary);
                labelRight = br.left - Dp(6, dpi);
            }
        }

        RECT lr = surf;
        lr.left  = g.right + Dp(8, dpi);
        lr.right = labelRight;
        if (lr.right < lr.left) lr.right = lr.left;
        if (!st->items[i].label.empty()) {
            DrawText(dc, lr, st->items[i].label, Font::UiBody, dpi, fg,
                     DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
        }
    }
}

void NavOnPaint(HWND hwnd) {
    PAINTSTRUCT ps;
    HDC hdc = BeginPaint(hwnd, &ps);
    if (hdc == nullptr) {
        // EndPaint must still run or the update region stays dirty and WM_PAINT repeats
        // forever, pinning a core.
        EndPaint(hwnd, &ps);
        return;
    }

    RECT client;
    GetClientRect(hwnd, &client);
    const int w = client.right - client.left;
    const int h = client.bottom - client.top;
    const int dpi = DpiOfWindow(hwnd);
    NavState* st = NavGet(hwnd);

    // Double-buffered: the rail repaints on every hover change and a direct-to-screen
    // background fill would flicker on each one.
    HDC     mem    = (w > 0 && h > 0) ? CreateCompatibleDC(hdc) : nullptr;
    HBITMAP bmp    = (mem != nullptr) ? CreateCompatibleBitmap(hdc, w, h) : nullptr;
    HGDIOBJ oldBmp = nullptr;

    if (mem != nullptr && bmp != nullptr) {
        oldBmp = SelectObject(mem, bmp);
        NavPaintTo(hwnd, mem, client, st, dpi);
        BitBlt(hdc, client.left, client.top, w, h, mem, 0, 0, SRCCOPY);
        SelectObject(mem, oldBmp);
    } else {
        NavPaintTo(hwnd, hdc, client, st, dpi);
    }

    if (bmp != nullptr) DeleteObject(bmp);
    if (mem != nullptr) DeleteDC(mem);
    EndPaint(hwnd, &ps);
}

void NavMoveSelection(HWND hwnd, NavState* st, int delta) {
    if (st == nullptr || st->items.empty()) return;
    const int count = static_cast<int>(st->items.size());
    int next = st->sel < 0 ? 0 : st->sel + delta;
    if (next < 0) next = 0;
    if (next >= count) next = count - 1;
    if (next == st->sel) return;
    st->sel = next;
    InvalidateRect(hwnd, nullptr, FALSE);
    NavNotify(hwnd);
}

LRESULT CALLBACK NavProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_NCCREATE: {
        NavState* st = new (std::nothrow) NavState();
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(st));
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
    case WM_NCDESTROY: {
        NavState* st = NavGet(hwnd);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        delete st;
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
    case WM_ERASEBKGND:
        return 1;   // WM_PAINT covers every pixel.

    case WM_PAINT:
        NavOnPaint(hwnd);
        return 0;

    case WM_GETDLGCODE:
        return DLGC_WANTARROWS;

    case WM_SETFOCUS:
    case WM_KILLFOCUS:
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;

    case WM_MOUSEMOVE: {
        NavState* st = NavGet(hwnd);
        if (st == nullptr) break;
        if (!st->tracking) {
            TRACKMOUSEEVENT tme;
            ZeroMemory(&tme, sizeof(tme));
            tme.cbSize    = sizeof(tme);
            tme.dwFlags   = TME_LEAVE;
            tme.hwndTrack = hwnd;
            if (TrackMouseEvent(&tme)) st->tracking = true;
        }
        const int x = static_cast<int>(static_cast<short>(LOWORD(lParam)));
        const int y = static_cast<int>(static_cast<short>(HIWORD(lParam)));
        const int hit = NavHitTest(hwnd, st, DpiOfWindow(hwnd), x, y);
        if (hit != st->hot) {
            st->hot = hit;
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    }
    case WM_MOUSELEAVE: {
        NavState* st = NavGet(hwnd);
        if (st != nullptr) {
            st->tracking = false;
            if (st->hot != -1) {
                st->hot = -1;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
        }
        return 0;
    }
    case WM_LBUTTONDOWN: {
        NavState* st = NavGet(hwnd);
        if (st == nullptr) break;
        SetFocus(hwnd);
        const int x = static_cast<int>(static_cast<short>(LOWORD(lParam)));
        const int y = static_cast<int>(static_cast<short>(HIWORD(lParam)));
        const int hit = NavHitTest(hwnd, st, DpiOfWindow(hwnd), x, y);
        if (hit >= 0 && hit != st->sel) {
            st->sel = hit;
            InvalidateRect(hwnd, nullptr, FALSE);
            NavNotify(hwnd);
        }
        return 0;
    }
    case WM_KEYDOWN: {
        NavState* st = NavGet(hwnd);
        if (st == nullptr) break;
        if (wParam == VK_UP)   { NavMoveSelection(hwnd, st, -1); return 0; }
        if (wParam == VK_DOWN) { NavMoveSelection(hwnd, st, +1); return 0; }
        break;
    }
    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

}  // namespace

void NavRegister(HINSTANCE hInst) {
    WNDCLASSEXW existing;
    ZeroMemory(&existing, sizeof(existing));
    existing.cbSize = sizeof(existing);
    if (GetClassInfoExW(hInst, kNavClass, &existing)) return;   // already registered

    WNDCLASSEXW wc;
    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc   = NavProc;
    wc.hInstance     = hInst;
    wc.hCursor       = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;   // WM_PAINT owns every pixel
    wc.lpszClassName = kNavClass;
    RegisterClassExW(&wc);
}

void NavAddItem(HWND nav, int id, const std::wstring& label, const std::wstring& glyph) {
    NavState* st = NavGet(nav);
    if (st == nullptr) return;
    NavItem item;
    item.id    = id;
    item.label = label;
    item.glyph = glyph;
    st->items.push_back(item);
    if (st->sel < 0) st->sel = 0;   // the rail always shows one active section
    InvalidateRect(nav, nullptr, FALSE);
}

void NavSetSelected(HWND nav, int id) {
    NavState* st = NavGet(nav);
    if (st == nullptr) return;
    const int idx = NavIndexOfId(st, id);
    if (idx < 0 || idx == st->sel) return;
    st->sel = idx;
    InvalidateRect(nav, nullptr, FALSE);
}

int NavGetSelected(HWND nav) {
    NavState* st = NavGet(nav);
    if (st == nullptr) return -1;
    if (st->sel < 0 || st->sel >= static_cast<int>(st->items.size())) return -1;
    return st->items[static_cast<size_t>(st->sel)].id;
}

void NavSetBadge(HWND nav, int id, const std::wstring& badge) {
    NavState* st = NavGet(nav);
    if (st == nullptr) return;
    const int idx = NavIndexOfId(st, id);
    if (idx < 0) return;
    st->items[static_cast<size_t>(idx)].badge = badge;
    InvalidateRect(nav, nullptr, FALSE);
}

// ===========================================================================
// The top tab bar
// ===========================================================================
// Deliberately a near-copy of the rail's shape - same GWLP_USERDATA state, same
// double-buffered WM_PAINT, same TrackMouseEvent hover - rather than a shared generic
// "strip control". The two differ in the axis, and every hit-test, layout and key handler
// differs with it; a parameterised version would be a chain of `if (horizontal)` inside
// each of them, which reads worse than two straight implementations.
//
// THE ONE STRUCTURAL DIFFERENCE FROM THE RAIL: a rail item's height is a constant, so its
// rect is arithmetic. A tab's width is its TEXT width, so layout needs a DC with the right
// font selected before any rect exists. Every rect therefore comes out of TabBarLayout(),
// which the paint path feeds its memory DC and the hit-test path feeds a short-lived
// GetDC(hwnd). Nothing caches those widths: a DPI change or a SetBadge would have to
// invalidate the cache, and measuring a handful of short strings is far cheaper than a
// stale-cache bug.
const wchar_t* kTabBarClass = L"GameOptimizerTabBar";

namespace {

struct TabItem {
    int          id;
    std::wstring label;
    std::wstring badge;
};

struct TabState {
    std::vector<TabItem> items;
    int  sel      = -1;   // index, not id
    int  hot      = -1;
    bool tracking = false;
};

TabState* TabGet(HWND h) {
    if (h == nullptr) return nullptr;
    return reinterpret_cast<TabState*>(GetWindowLongPtrW(h, GWLP_USERDATA));
}

int TabIndexOfId(TabState* st, int id) {
    if (st == nullptr) return -1;
    for (size_t i = 0; i < st->items.size(); ++i) {
        if (st->items[i].id == id) return static_cast<int>(i);
    }
    return -1;
}

// Badge geometry, shared by layout and paint so the width reserved is the width drawn.
int TabBadgeH(int dpi) { return Dp(18, dpi); }

int TabBadgeW(HDC dc, const std::wstring& badge, int dpi) {
    if (badge.empty()) return 0;
    const SIZE bs = MeasureText(dc, badge, Font::UiSmall, dpi);
    return IMax(TabBadgeH(dpi), static_cast<int>(bs.cx) + Dp(16, dpi));
}

// Full-height cell rects, left to right, one per item. `dc` must be a DC the font cache can
// select into - MeasureText does the selecting, so any compatible DC works.
void TabBarLayout(HDC dc, HWND hwnd, TabState* st, int dpi, std::vector<RECT>& out) {
    out.clear();
    if (st == nullptr || dc == nullptr) return;

    RECT client;
    GetClientRect(hwnd, &client);

    const int padX = Dp(metric::kTabPadX, dpi);
    const int gap  = Dp(metric::kTabGap, dpi);

    int x = gap;   // a gap before the first tab too, so it does not touch the window edge
    for (size_t i = 0; i < st->items.size(); ++i) {
        const SIZE ts = MeasureText(dc, st->items[i].label, Font::UiBody, dpi);
        int w = padX * 2 + static_cast<int>(ts.cx);
        const int bw = TabBadgeW(dc, st->items[i].badge, dpi);
        if (bw > 0) w += Dp(6, dpi) + bw;   // the badge widens the tab; padding stays as spec'd

        RECT r;
        r.left   = client.left + x;
        r.right  = r.left + w;
        r.top    = client.top;
        r.bottom = client.bottom;
        out.push_back(r);
        x += w + gap;
    }
}

int TabHitTest(HWND hwnd, TabState* st, int dpi, int x, int y) {
    if (st == nullptr || st->items.empty()) return -1;
    RECT client;
    GetClientRect(hwnd, &client);
    if (y < client.top || y >= client.bottom) return -1;

    HDC dc = GetDC(hwnd);
    if (dc == nullptr) return -1;
    std::vector<RECT> rects;
    TabBarLayout(dc, hwnd, st, dpi, rects);
    ReleaseDC(hwnd, dc);

    for (size_t i = 0; i < rects.size(); ++i) {
        if (x >= rects[i].left && x < rects[i].right) return static_cast<int>(i);
    }
    return -1;
}

void TabBarNotify(HWND hwnd) {
    HWND parent = GetParent(hwnd);
    if (parent == nullptr) return;
    const WORD id = static_cast<WORD>(GetDlgCtrlID(hwnd));
    SendMessageW(parent, WM_COMMAND, MAKEWPARAM(id, TABN_SELCHANGED),
                 reinterpret_cast<LPARAM>(hwnd));
}

void TabBarPaintTo(HWND hwnd, HDC dc, const RECT& client, TabState* st, int dpi) {
    const Palette& p = P();

    RECT bg = client;
    HBRUSH back = CachedBrush(p.appBg);
    if (back != nullptr) FillRect(dc, &bg, back);

    // The 1px line along the bottom edge of the whole bar. This, not a card outline, is what
    // separates the menu from the page beneath it.
    RECT line = client;
    line.top = line.bottom - 1;
    if (line.top < client.top) line.top = client.top;
    HBRUSH lineBrush = CachedBrush(p.border);
    if (lineBrush != nullptr && line.bottom > line.top) FillRect(dc, &line, lineBrush);

    if (st == nullptr) return;

    std::vector<RECT> rects;
    TabBarLayout(dc, hwnd, st, dpi, rects);

    const bool focused = (GetFocus() == hwnd);
    const int  radius  = Dp(metric::kTabRadius, dpi);
    const int  underH  = IMax(1, Dp(metric::kUnderlineH, dpi));
    const int  padX    = Dp(metric::kTabPadX, dpi);
    const int  vInset  = Dp(5, dpi);

    for (size_t i = 0; i < rects.size(); ++i) {
        const RECT cell = rects[i];
        // OVERFLOW POLICY: clip, do not scroll. Tabs past the right edge are simply not
        // drawn and cannot be clicked. See notDone.
        if (cell.left >= client.right) break;

        const int  idx = static_cast<int>(i);
        const bool sel = (idx == st->sel);
        const bool hot = (idx == st->hot) && !sel;

        RECT surf = cell;
        surf.top    = cell.top + vInset;
        surf.bottom = client.bottom - underH - Dp(2, dpi);
        if (surf.bottom < surf.top) surf.bottom = surf.top;

        if (hot) {
            FillRoundRect(dc, surf, radius, p.cardBgAlt);
        }
        if (sel && focused) {
            // Keyboard focus ring, matching the rail. The underline alone does not say which
            // control owns the caret.
            StrokeRoundRect(dc, surf, radius, p.borderStrong);
        }

        const COLORREF fg = (sel || hot) ? p.textPrimary : p.textSecondary;

        int x = cell.left + padX;
        if (!st->items[i].label.empty()) {
            const SIZE ts = MeasureText(dc, st->items[i].label, Font::UiBody, dpi);
            RECT lr;
            lr.left   = x;
            lr.right  = IMin(cell.right - padX, x + static_cast<int>(ts.cx));
            lr.top    = surf.top;
            lr.bottom = surf.bottom;
            if (lr.right < lr.left) lr.right = lr.left;
            DrawText(dc, lr, st->items[i].label, Font::UiBody, dpi, fg,
                     DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
            x = lr.right;
        }

        if (!st->items[i].badge.empty()) {
            const int bh = TabBadgeH(dpi);
            const int bw = TabBadgeW(dc, st->items[i].badge, dpi);
            RECT br;
            br.left   = x + Dp(6, dpi);
            br.right  = br.left + bw;
            br.top    = (surf.top + surf.bottom) / 2 - bh / 2;
            br.bottom = br.top + bh;
            if (br.right <= cell.right) {
                DrawPill(dc, br, st->items[i].badge, dpi,
                         sel ? p.accent : p.cardBgAlt,
                         sel ? p.textOnAccent : p.textSecondary);
            }
        }

        if (sel) {
            // The underline sits ON the bottom border line, covering it for the tab's width,
            // which is what ties the active tab to the page below it.
            RECT ul;
            ul.left   = cell.left;
            ul.right  = IMin(cell.right, client.right);
            ul.bottom = client.bottom;
            ul.top    = ul.bottom - underH;
            if (ul.top < client.top) ul.top = client.top;
            HBRUSH ab = CachedBrush(p.accent);
            if (ab != nullptr && ul.right > ul.left) FillRect(dc, &ul, ab);
        }
    }
}

void TabBarOnPaint(HWND hwnd) {
    PAINTSTRUCT ps;
    HDC hdc = BeginPaint(hwnd, &ps);
    if (hdc == nullptr) {
        EndPaint(hwnd, &ps);   // see NavOnPaint: skipping this spins WM_PAINT forever
        return;
    }

    RECT client;
    GetClientRect(hwnd, &client);
    const int w   = client.right - client.left;
    const int h   = client.bottom - client.top;
    const int dpi = DpiOfWindow(hwnd);
    TabState* st  = TabGet(hwnd);

    HDC     mem    = (w > 0 && h > 0) ? CreateCompatibleDC(hdc) : nullptr;
    HBITMAP bmp    = (mem != nullptr) ? CreateCompatibleBitmap(hdc, w, h) : nullptr;
    HGDIOBJ oldBmp = nullptr;

    if (mem != nullptr && bmp != nullptr) {
        oldBmp = SelectObject(mem, bmp);
        TabBarPaintTo(hwnd, mem, client, st, dpi);
        BitBlt(hdc, client.left, client.top, w, h, mem, 0, 0, SRCCOPY);
        SelectObject(mem, oldBmp);
    } else {
        TabBarPaintTo(hwnd, hdc, client, st, dpi);
    }

    if (bmp != nullptr) DeleteObject(bmp);
    if (mem != nullptr) DeleteDC(mem);
    EndPaint(hwnd, &ps);
}

void TabBarMoveSelection(HWND hwnd, TabState* st, int delta) {
    if (st == nullptr || st->items.empty()) return;
    const int count = static_cast<int>(st->items.size());
    int next = st->sel < 0 ? 0 : st->sel + delta;
    if (next < 0) next = 0;
    if (next >= count) next = count - 1;
    if (next == st->sel) return;
    st->sel = next;
    InvalidateRect(hwnd, nullptr, FALSE);
    TabBarNotify(hwnd);
}

LRESULT CALLBACK TabBarProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_NCCREATE: {
        TabState* st = new (std::nothrow) TabState();
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(st));
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
    case WM_NCDESTROY: {
        TabState* st = TabGet(hwnd);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        delete st;
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
    case WM_ERASEBKGND:
        return 1;   // WM_PAINT covers every pixel.

    case WM_PAINT:
        TabBarOnPaint(hwnd);
        return 0;

    case WM_GETDLGCODE:
        return DLGC_WANTARROWS;

    case WM_SETFOCUS:
    case WM_KILLFOCUS:
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;

    case WM_MOUSEMOVE: {
        TabState* st = TabGet(hwnd);
        if (st == nullptr) break;
        if (!st->tracking) {
            TRACKMOUSEEVENT tme;
            ZeroMemory(&tme, sizeof(tme));
            tme.cbSize    = sizeof(tme);
            tme.dwFlags   = TME_LEAVE;
            tme.hwndTrack = hwnd;
            if (TrackMouseEvent(&tme)) st->tracking = true;
        }
        const int x = static_cast<int>(static_cast<short>(LOWORD(lParam)));
        const int y = static_cast<int>(static_cast<short>(HIWORD(lParam)));
        const int hit = TabHitTest(hwnd, st, DpiOfWindow(hwnd), x, y);
        if (hit != st->hot) {
            st->hot = hit;
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    }
    case WM_MOUSELEAVE: {
        TabState* st = TabGet(hwnd);
        if (st != nullptr) {
            st->tracking = false;
            if (st->hot != -1) {
                st->hot = -1;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
        }
        return 0;
    }
    case WM_LBUTTONDOWN: {
        TabState* st = TabGet(hwnd);
        if (st == nullptr) break;
        SetFocus(hwnd);
        const int x = static_cast<int>(static_cast<short>(LOWORD(lParam)));
        const int y = static_cast<int>(static_cast<short>(HIWORD(lParam)));
        const int hit = TabHitTest(hwnd, st, DpiOfWindow(hwnd), x, y);
        if (hit >= 0 && hit != st->sel) {
            st->sel = hit;
            InvalidateRect(hwnd, nullptr, FALSE);
            TabBarNotify(hwnd);
        }
        return 0;
    }
    case WM_KEYDOWN: {
        TabState* st = TabGet(hwnd);
        if (st == nullptr) break;
        // LEFT/RIGHT, not UP/DOWN: the control is horizontal, and a user who presses DOWN
        // here expects to move into the page, which is the dialog manager's job.
        if (wParam == VK_LEFT)  { TabBarMoveSelection(hwnd, st, -1); return 0; }
        if (wParam == VK_RIGHT) { TabBarMoveSelection(hwnd, st, +1); return 0; }
        break;
    }
    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

}  // namespace

void TabBarRegister(HINSTANCE hInst) {
    WNDCLASSEXW existing;
    ZeroMemory(&existing, sizeof(existing));
    existing.cbSize = sizeof(existing);
    if (GetClassInfoExW(hInst, kTabBarClass, &existing)) return;   // already registered

    WNDCLASSEXW wc;
    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc   = TabBarProc;
    wc.hInstance     = hInst;
    wc.hCursor       = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;   // WM_PAINT owns every pixel
    wc.lpszClassName = kTabBarClass;
    RegisterClassExW(&wc);
}

void TabBarAddItem(HWND bar, int id, const std::wstring& label) {
    TabState* st = TabGet(bar);
    if (st == nullptr) return;
    TabItem item;
    item.id    = id;
    item.label = label;
    st->items.push_back(item);
    if (st->sel < 0) st->sel = 0;   // the bar always shows one active tab
    InvalidateRect(bar, nullptr, FALSE);
}

void TabBarSetSelected(HWND bar, int id) {
    TabState* st = TabGet(bar);
    if (st == nullptr) return;
    const int idx = TabIndexOfId(st, id);
    if (idx < 0 || idx == st->sel) return;
    st->sel = idx;
    InvalidateRect(bar, nullptr, FALSE);
}

int TabBarGetSelected(HWND bar) {
    TabState* st = TabGet(bar);
    if (st == nullptr) return -1;
    if (st->sel < 0 || st->sel >= static_cast<int>(st->items.size())) return -1;
    return st->items[static_cast<size_t>(st->sel)].id;
}

void TabBarSetBadge(HWND bar, int id, const std::wstring& badge) {
    TabState* st = TabGet(bar);
    if (st == nullptr) return;
    const int idx = TabIndexOfId(st, id);
    if (idx < 0) return;
    st->items[static_cast<size_t>(idx)].badge = badge;
    // A badge changes the tab's WIDTH, so every tab to its right moves: invalidate the whole
    // bar, never just the one cell.
    InvalidateRect(bar, nullptr, FALSE);
}

// ===========================================================================
// Search box chrome
// ===========================================================================
void DrawSearchChrome(HDC parentDc, const RECT& editRectInParent, int dpi,
                      bool focused, bool empty, const std::wstring& placeholder) {
    if (parentDc == nullptr) return;
    const RECT rc = editRectInParent;
    const int w = rc.right - rc.left;
    const int h = rc.bottom - rc.top;
    if (w < 4 || h < 4) return;

    const Palette& p = P();

    // Fill as well as outline. The parent is WS_CLIPCHILDREN, so the EDIT's own pixels are
    // clipped out of this DC and only the gutter around it takes the fill - which is exactly
    // the strip the magnifier and the placeholder live in.
    DrawRoundRect(parentDc, rc, Dp(6, dpi), p.inputBg, focused ? p.accent : p.border);

    // ---- Magnifier, drawn with GDI primitives on purpose -------------------
    // A font glyph (U+1F50D, or a Segoe MDL2 codepoint) renders as a missing-glyph box on a
    // machine without that face installed. A circle and a line cannot.
    const int cy = (rc.top + rc.bottom) / 2;
    const int r  = IMax(2, Dp(4, dpi));
    const int cx = rc.left + Dp(11, dpi);

    HPEN pen = CreatePen(PS_SOLID, IMax(1, Dp(1, dpi)), p.textDim);
    if (pen != nullptr) {
        HGDIOBJ oldPen   = SelectObject(parentDc, pen);
        HGDIOBJ oldBrush = SelectObject(parentDc, GetStockObject(NULL_BRUSH));
        Ellipse(parentDc, cx - r, cy - r, cx + r + 1, cy + r + 1);
        // The handle: a short diagonal off the lower-right of the lens.
        const int tail = IMax(2, Dp(3, dpi));
        const int sx = cx + (r * 7) / 10;
        const int sy = cy + (r * 7) / 10;
        MoveToEx(parentDc, sx, sy, nullptr);
        LineTo(parentDc, sx + tail, sy + tail);
        SelectObject(parentDc, oldBrush);
        SelectObject(parentDc, oldPen);
        DeleteObject(pen);
    }

    // ---- Placeholder -------------------------------------------------------
    // Hidden the moment the box takes focus, so it never sits behind a caret the user is
    // about to type at.
    if (empty && !focused && !placeholder.empty()) {
        RECT t = rc;
        t.left  = cx + r + Dp(7, dpi);
        t.right = rc.right - Dp(6, dpi);
        if (t.right > t.left) {
            DrawText(parentDc, t, placeholder, Font::UiBody, dpi, p.textDim,
                     DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
        }
    }
}

// ===========================================================================
// Inline CPU meter
// ===========================================================================
void DrawCpuMeter(HDC dc, const RECT& rc, double pct, int dpi, double threshold) {
    if (dc == nullptr) return;
    const int availW = rc.right - rc.left;
    const int availH = rc.bottom - rc.top;
    if (availW <= 1 || availH <= 0) return;

    const Palette& p = P();
    pct       = DClamp(pct, 0.0, 100.0);
    threshold = DClamp(threshold, 1.0, 100.0);

    // The track is a fixed slim height centred in whatever row rect the caller hands over,
    // so the meter lines up with the text beside it instead of stretching to the row.
    // Dp(6) rather than the Dp(4) this used to be: at 96 dpi a 4px groove carrying a 1px
    // outline leaves a 2px channel for the fill, which is not a bar, it is a line.
    int trackH = IMax(3, Dp(6, dpi));
    if (trackH > availH) trackH = availH;

    RECT track;
    track.left   = rc.left;
    track.right  = rc.right;
    track.top    = rc.top + (availH - trackH) / 2;
    track.bottom = track.top + trackH;

    const int trackW = track.right - track.left;
    const int radius = trackH / 2;

    // THE GROOVE HAS TO BE VISIBLE AT 0%, AND A BARE inputBg FILL IS NOT.
    // inputBg is ALSO the unselected row colour of the owner-drawn heavy-apps list box
    // (settings.cpp DrawRowBackground fills the row with pal.inputBg), so an inputBg track on
    // an inputBg row is RGB delta (0,0,0) - the track was being drawn correctly and was
    // literally invisible, which is the defect this replaces. The 1px P().border outline is
    // what makes the empty groove readable; it costs nothing on a surface that already
    // differs, and it is the difference between "no meter" and "meter reading zero".
    DrawRoundRect(dc, track, radius, p.inputBg, p.border);

    // Ramp: comfortable below 60% of the threshold, warning as it approaches, danger once the
    // auto-pin rule would fire. The user sees the trip coming rather than only its result.
    // The fill sits INSIDE the outline so the groove keeps its edge at every value.
    RECT inner = track;
    InflateRect(&inner, -1, -1);
    const int innerW = inner.right - inner.left;
    const int innerH = inner.bottom - inner.top;
    if (innerW > 0 && innerH > 0) {
        COLORREF fill = p.good;
        if (pct >= threshold)            fill = p.danger;
        else if (pct >= threshold * 0.6) fill = p.warn;

        int fillW = static_cast<int>((pct / 100.0) * static_cast<double>(innerW) + 0.5);
        if (fillW > innerW) fillW = innerW;
        if (pct > 0.0 && fillW < innerH) fillW = IMin(innerW, innerH);  // a pill needs its caps
        if (fillW > 0) {
            RECT f = inner;
            f.right = f.left + fillW;
            FillRoundRect(dc, f, innerH / 2, fill);
        }
    }

    // The threshold tick: drawn last so the fill cannot cover it, and spanning exactly the
    // track's own height. It used to overhang the track by Dp(2) top and bottom, which read
    // as a stray floating hairline once the track itself went invisible; ON the track it
    // reads as a graduation mark, which is what it is.
    const int tickW = IMax(1, Dp(1, dpi));
    int tx = track.left +
             static_cast<int>((threshold / 100.0) * static_cast<double>(trackW) + 0.5);
    if (tx > track.right - tickW) tx = track.right - tickW;
    if (tx < track.left)          tx = track.left;
    RECT tick;
    tick.left   = tx;
    tick.right  = tx + tickW;
    tick.top    = track.top;
    tick.bottom = track.bottom;
    if (tick.right > tick.left && tick.bottom > tick.top) {
        HBRUSH tb = CachedBrush(p.borderStrong);
        if (tb != nullptr) FillRect(dc, &tick, tb);
    }
}

}  // namespace theme
}  // namespace cd
