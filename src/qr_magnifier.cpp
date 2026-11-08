// Game Optimizer - the QR magnifier. Implementation of src\qr_magnifier.h, which carries the rules.
//
// Plain GDI on purpose: the QR is whole-pixel rectangles on a cream fill, which GDI draws exactly,
// and the popup must work on the GDI strip's no-GDI+ path too. The fade is the window's own alpha
// (WS_EX_LAYERED + SetLayeredWindowAttributes), so nothing is re-rendered while it fades.
#define NOMINMAX
#include "qr_magnifier.h"

#include <commctrl.h>

#include <cmath>

#include "goatcam_qr.h"
#include "sponsor.h"   // AnimationsEnabled, sponsor_motion::EaseIn / EaseOut

namespace cd {
namespace {

const wchar_t* const kClass = L"GameOptimizerQrMagnifier";
const wchar_t* const kName  = L"QR code that opens GOAT Cam on the App Store";

constexpr UINT_PTR kFadeTimer   = 1;
constexpr UINT     kFadeTickMs  = 16;
constexpr double   kFadeInMs    = 200.0;   // SPEC 7.4: enter ~200 ms
constexpr double   kFadeOutMs   = 120.0;   // leave ~120 ms - leaving is faster than arriving
constexpr UINT_PTR kSubclassId  = 0x47435152;   // 'GCQR'
constexpr int      kGapDip      = 8;       // between the tile and the popup

// The tile and its edge: goatcam.dagoat.io's QR tile cream, the code's espresso, and the
// site's --cream-soft as a soft one-pixel border. CS_DROPSHADOW adds the shadow.
constexpr COLORREF kTile   = RGB(0xfb, 0xf5, 0xea);
constexpr COLORREF kDark   = RGB(0x1c, 0x13, 0x0c);
constexpr COLORREF kBorder = RGB(0xd8, 0xc2, 0xa5);

struct State {
    HWND   hwnd  = nullptr;     // the popup
    HWND   root  = nullptr;     // the top-level window it is owned by and watches
    bool   watching = false;    // the subclass on `root` is installed
    bool   shown = false;       // shown or fading in; false while fading out
    RECT   tile{};              // the tile it is shown for
    int    dpi = 96;
    // the fade: alpha goes from `from` to `to` over `dur` ms starting at t0
    double from = 0.0, to = 0.0, t0 = 0.0, dur = 0.0;
    bool   timer = false;
};
State g;

double NowMs() {
    static const double perMs = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return static_cast<double>(f.QuadPart) / 1000.0;
    }();
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return static_cast<double>(c.QuadPart) / perMs;
}

int Dip(int v, int dpi) { return MulDiv(v, dpi <= 0 ? 96 : dpi, 96); }

double AlphaAt(double now) {
    if (g.dur <= 0.0) return g.to;
    const double t = (now - g.t0) / g.dur;
    if (t <= 0.0) return g.from;
    if (t >= 1.0) return g.to;
    const double e = g.to > g.from ? sponsor_motion::EaseIn(t) : sponsor_motion::EaseOut(t);
    return g.from + (g.to - g.from) * e;
}

void ApplyAlpha(double a) {
    if (g.hwnd == nullptr) return;
    const int v = static_cast<int>(std::lround((a < 0.0 ? 0.0 : (a > 1.0 ? 1.0 : a)) * 255.0));
    SetLayeredWindowAttributes(g.hwnd, 0, static_cast<BYTE>(v), LWA_ALPHA);
}

void StopTimer() {
    if (g.timer && g.hwnd != nullptr) KillTimer(g.hwnd, kFadeTimer);
    g.timer = false;
}

// Starts a fade to `target`; with animations off it lands at once. The timer runs only while
// the fade does.
void FadeTo(double target, double fullMs) {
    const double now = NowMs();
    const double cur = AlphaAt(now);
    g.from = cur;
    g.to = target;
    g.t0 = now;
    g.dur = AnimationsEnabled() ? fullMs * std::fabs(target - cur) : 0.0;
    if (g.dur < 1.0) {
        g.dur = 0.0;
        StopTimer();
        ApplyAlpha(target);
        return;
    }
    ApplyAlpha(cur);
    if (!g.timer && g.hwnd != nullptr) g.timer = SetTimer(g.hwnd, kFadeTimer, kFadeTickMs, nullptr) != 0;
    if (!g.timer) {
        // No clock to run the fade: land on the target now rather than freeze half-faded, and a
        // fade-out that cannot run ends hidden, exactly as one that ran would.
        g.from = target;
        g.dur = 0.0;
        ApplyAlpha(target);
        if (target <= 0.0 && g.hwnd != nullptr) ShowWindow(g.hwnd, SW_HIDE);
    }
}

void Paint(HDC dc) {
    if (g.hwnd == nullptr || dc == nullptr) return;
    RECT rc;
    GetClientRect(g.hwnd, &rc);
    const int m = QrMagnifierModulePx(g.dpi);
    const int b = (rc.right - rc.left - goatcam_qr::kN * m) / 2;   // the border, both sides
    HBRUSH edge = CreateSolidBrush(kBorder);
    HBRUSH tile = CreateSolidBrush(kTile);
    HBRUSH dark = CreateSolidBrush(kDark);
    if (edge != nullptr) FillRect(dc, &rc, edge);
    RECT in = rc;
    InflateRect(&in, -b, -b);
    if (tile != nullptr) FillRect(dc, &in, tile);
    if (dark != nullptr) {
        for (int y = 0; y < goatcam_qr::kN; ++y) {
            for (int x = 0; x < goatcam_qr::kN;) {
                const int e = goatcam_qr::DarkRunEnd(y, x);
                if (e == x) { ++x; continue; }
                RECT r = { in.left + x * m, in.top + y * m, in.left + e * m, in.top + (y + 1) * m };
                FillRect(dc, &r, dark);
                x = e;
            }
        }
    }
    if (edge != nullptr) DeleteObject(edge);
    if (tile != nullptr) DeleteObject(tile);
    if (dark != nullptr) DeleteObject(dark);
}

// The owner's top-level window: anything that moves, hides, deactivates or destroys it takes
// the popup down at once, because the tile it points at is no longer where it was.
LRESULT CALLBACK RootSubclass(HWND h, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR) {
    switch (msg) {
    case WM_WINDOWPOSCHANGED: {
        const WINDOWPOS* p = reinterpret_cast<const WINDOWPOS*>(lp);
        if (p != nullptr && ((p->flags & SWP_NOMOVE) == 0 || (p->flags & SWP_NOSIZE) == 0 ||
                             (p->flags & SWP_HIDEWINDOW) != 0))
            QrMagnifierHideNow();
        break;
    }
    case WM_ACTIVATE:
        if (LOWORD(wp) == WA_INACTIVE) QrMagnifierHideNow();
        break;
    case WM_ACTIVATEAPP:
        if (wp == FALSE) QrMagnifierHideNow();
        break;
    case WM_SHOWWINDOW:
        if (wp == FALSE) QrMagnifierHideNow();
        break;
    case WM_ENTERSIZEMOVE:
    case WM_SYSCOMMAND:
    case WM_CANCELMODE:
        QrMagnifierHideNow();
        break;
    case WM_NCDESTROY:
        QrMagnifierHideNow();
        RemoveWindowSubclass(h, RootSubclass, id);
        if (g.root == h) { g.root = nullptr; g.watching = false; }
        break;
    default:
        break;
    }
    return DefSubclassProc(h, msg, wp, lp);
}

// Installs the watch on `root`. FALSE means nothing would take the popup down when its owner
// moves, hides, deactivates or dies - so the caller must not show it (fail closed).
bool Watch(HWND root) {
    if (g.watching && g.root == root) return true;
    if (g.watching && g.root != nullptr && IsWindow(g.root))
        RemoveWindowSubclass(g.root, RootSubclass, kSubclassId);
    g.watching = false;
    g.root = root;
    g.watching = root != nullptr && SetWindowSubclass(root, RootSubclass, kSubclassId, 0) != FALSE;
    return g.watching;
}

void Unwatch() {
    if (g.watching && g.root != nullptr && IsWindow(g.root))
        RemoveWindowSubclass(g.root, RootSubclass, kSubclassId);
    g.watching = false;
}

void ResetShownState() {
    g.shown = false;
    Unwatch();
    StopTimer();
    g.from = g.to = 0.0;
    g.dur = 0.0;
}

LRESULT CALLBACK MagProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    case WM_NCHITTEST:
        return HTTRANSPARENT;           // the pointer passes through; it never hovers the popup
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        Paint(dc);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_PRINTCLIENT:
        Paint(reinterpret_cast<HDC>(wp));
        return 0;
    case WM_TIMER:
        if (wp == kFadeTimer) {
            const double now = NowMs();
            ApplyAlpha(AlphaAt(now));
            if (now >= g.t0 + g.dur) {
                StopTimer();
                if (g.to <= 0.0) ShowWindow(hwnd, SW_HIDE);   // a fade-out ends hidden
            }
            return 0;
        }
        break;
    case WM_NCDESTROY:
        // Owned windows die with their owner. Forget everything; the next Show creates afresh.
        if (g.hwnd == hwnd) {
            g.timer = false;
            g.shown = false;
            g.hwnd = nullptr;
            Unwatch();
        }
        break;
    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

bool EnsureWindow(HWND root) {
    if (g.hwnd != nullptr && GetWindow(g.hwnd, GW_OWNER) != root) {
        ResetShownState();       // fail closed before tearing down the old owner's popup
        DestroyWindow(g.hwnd);   // an owner cannot be swapped cleanly; make a new one
        g.hwnd = nullptr;
    }
    if (g.hwnd != nullptr) return true;
    HINSTANCE inst = GetModuleHandleW(nullptr);
    WNDCLASSEXW existing;
    ZeroMemory(&existing, sizeof(existing));
    existing.cbSize = sizeof(existing);
    if (!GetClassInfoExW(inst, kClass, &existing)) {
        WNDCLASSEXW wc;
        ZeroMemory(&wc, sizeof(wc));
        wc.cbSize        = sizeof(wc);
        wc.style         = CS_DROPSHADOW;
        wc.lpfnWndProc   = MagProc;
        wc.hInstance     = inst;
        wc.hCursor       = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = nullptr;
        wc.lpszClassName = kClass;
        if (RegisterClassExW(&wc) == 0) return false;
    }
    // Created HIDDEN. WS_EX_NOACTIVATE + WS_EX_TOOLWINDOW: never activated, never on the taskbar
    // or in Alt+Tab. WS_EX_TOPMOST: above the Settings window and everything else while shown.
    g.hwnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_LAYERED,
                             kClass, kName, WS_POPUP, 0, 0, 0, 0, root, nullptr, inst, nullptr);
    if (g.hwnd == nullptr) return false;
    ApplyAlpha(0.0);
    return true;
}

}  // namespace

int QrMagnifierModulePx(int dpi) {
    const int m = MulDiv(4, dpi <= 0 ? 96 : dpi, 96);
    return m < 4 ? 4 : m;
}

SIZE QrMagnifierSize(int dpi) {
    const int side = goatcam_qr::kN * QrMagnifierModulePx(dpi) + 2 * Dip(1, dpi);
    SIZE s;
    s.cx = side;
    s.cy = side;
    return s;
}

RECT QrMagnifierPlace(const RECT& tile, const RECT& work, SIZE size, int gap) {
    RECT r;
    long x = (tile.left + tile.right) / 2 - size.cx / 2;
    long y = tile.top - gap - size.cy;                            // above, by preference
    if (y < work.top) y = tile.bottom + gap;                      // else below
    if (y + size.cy > work.bottom) y = work.bottom - size.cy;     // else pinned inside
    if (y < work.top) y = work.top;
    if (x + size.cx > work.right) x = work.right - size.cx;
    if (x < work.left) x = work.left;
    r.left = x;
    r.top = y;
    r.right = x + size.cx;
    r.bottom = y + size.cy;
    return r;
}

void QrMagnifierShow(HWND owner, const RECT& tileScreenRect, int dpi) {
    if (owner == nullptr) return;
    HWND root = GetAncestor(owner, GA_ROOT);
    if (root == nullptr) root = owner;
    // Never over a window the user cannot see: an owner that is hidden or minimised gets no
    // popup, and one already up comes down.
    if (!IsWindowVisible(root) || IsIconic(root)) {
        if (g.shown) QrMagnifierHideNow();
        return;
    }
    // COALESCED: the same tile, already up -> nothing at all. No SetWindowPos, no fade restart.
    if (g.shown && g.hwnd != nullptr && g.root == root && g.dpi == dpi &&
        EqualRect(&g.tile, &tileScreenRect))
        return;
    if (!EnsureWindow(root)) {
        QrMagnifierHideNow();   // fail closed if the popup window cannot be ensured
        return;
    }
    // FAIL CLOSED, and BEFORE anything is shown: the watch on the owner is what takes the popup
    // down when that window moves, hides, deactivates or dies. Without it there is no popup.
    if (!Watch(root)) {
        QrMagnifierHideNow();
        return;
    }

    g.dpi = dpi;
    g.tile = tileScreenRect;
    MONITORINFO mi;
    ZeroMemory(&mi, sizeof(mi));
    mi.cbSize = sizeof(mi);
    RECT work = tileScreenRect;
    if (GetMonitorInfoW(MonitorFromRect(&tileScreenRect, MONITOR_DEFAULTTONEAREST), &mi)) work = mi.rcWork;
    const RECT at = QrMagnifierPlace(tileScreenRect, work, QrMagnifierSize(dpi), Dip(kGapDip, dpi));

    // Position and size it WITHOUT showing it (no SWP_SHOWWINDOW): ShowWindow below is the one
    // place it becomes visible, and that one never activates.
    SetWindowPos(g.hwnd, HWND_TOPMOST, at.left, at.top, at.right - at.left, at.bottom - at.top,
                 SWP_NOACTIVATE | SWP_NOOWNERZORDER);
    InvalidateRect(g.hwnd, nullptr, FALSE);
    const bool wasShown = g.shown;
    g.shown = true;
    FadeTo(1.0, kFadeInMs);
    if (!wasShown) ShowWindow(g.hwnd, SW_SHOWNOACTIVATE);
}

void QrMagnifierHide() {
    if (g.hwnd == nullptr || !g.shown) return;
    g.shown = false;
    Unwatch();
    FadeTo(0.0, kFadeOutMs);
    if (!g.timer) ShowWindow(g.hwnd, SW_HIDE);   // no fade running: hidden now
}

void QrMagnifierHideNow() {
    ResetShownState();
    if (g.hwnd == nullptr) return;
    ApplyAlpha(0.0);
    ShowWindow(g.hwnd, SW_HIDE);
}

bool QrMagnifierIsShown() { return g.hwnd != nullptr && g.shown; }

HWND QrMagnifierWindow() { return g.hwnd; }

}  // namespace cd
