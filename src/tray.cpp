// Game Optimizer - the notification-area icon, its tooltip and its context menu.
//
// No .ico, no .rc, no binary asset anywhere in the repository: both icons are composed at
// runtime with GDI into a CreateDIBSection surface, paired with a zeroed monochrome mask
// and handed to CreateIconIndirect. That keeps the tray glyph crisp at whatever the shell's
// small-icon size happens to be on this machine's DPI, and it keeps the source tree text.
//
// The icon is a rounded square split down the middle:
//   idle    both halves theme textDim                     - nothing is being governed
//   active  left half theme accent, right half textDim    - a profile's game owns a domain
// Two flat colour blocks is about the only shape that stays legible at 16x16.
//
// The colours come from theme::P() rather than from literals here, so the tray glyph and the
// windows agree: the "governed" blue in the notification area is the same blue as a selected
// nav item and a gauge arc. Edges are the palette's own border/pressed tones - at 16x16 a
// one-pixel outline is the whole of the icon's contrast against a light or dark taskbar.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#include <string>
#include <vector>

#include "ui.h"
#include "engine.h"
#include "util.h"
#include "theme.h"

namespace cd {

namespace {

// ---- Module state ----------------------------------------------------------
// Single tray icon, owned by the UI thread only. Every function below must be called on
// the thread that called TrayInit.
HWND  g_hwnd            = nullptr;
UINT  g_callbackMsg     = 0;
UINT  g_taskbarCreated  = 0;      // RegisterWindowMessage("TaskbarCreated")
HICON g_iconIdle        = nullptr;
HICON g_iconActive      = nullptr;
bool  g_added           = false;
bool  g_lastActive      = false;
std::wstring g_lastTip;

constexpr UINT kTrayIconId = 1;

// Magenta is the "nothing was painted here" sentinel. GDI has no alpha-preserving fill, so
// the surface is primed with a colour the palette below never produces, drawn over with
// ordinary GDI calls, and then swept once: sentinel pixels become fully transparent and
// everything else becomes fully opaque. That is what gives the icon real rounded corners
// instead of a grey box.
constexpr DWORD kSentinel = 0x00FF00FFu;

// ---- Small helpers ---------------------------------------------------------
void CopyTruncated(wchar_t* dst, size_t capIncludingNul, const std::wstring& src) {
    if (!dst || capIncludingNul == 0) return;
    size_t n = src.size();
    if (n > capIncludingNul - 1) n = capIncludingNul - 1;
    for (size_t i = 0; i < n; ++i) dst[i] = src[i];
    dst[n] = L'\0';
}

// A menu item string treats '&' as an accelerator prefix. Profile and mask names come from
// the user's config, so they can absolutely contain one.
std::wstring EscapeAmpersands(const std::wstring& s) {
    std::wstring out;
    out.reserve(s.size() + 4);
    for (wchar_t c : s) {
        out.push_back(c);
        if (c == L'&') out.push_back(L'&');
    }
    return out;
}

std::wstring StatusText(const EngineStatus& st) {
    std::wstring t = st.tooltip.empty() ? BuildTooltip(st) : st.tooltip;
    if (t.empty()) t = L"Game Optimizer";
    if (t.size() > 127) t.resize(127);
    return t;
}

// ---- Icon composition ------------------------------------------------------
HICON MakeTrayIcon(int cx, int cy, bool active) {
    if (cx <= 0 || cy <= 0) return nullptr;

    BITMAPINFO bmi;
    ZeroMemory(&bmi, sizeof(bmi));
    bmi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth       = cx;
    bmi.bmiHeader.biHeight      = -cy;              // top-down
    bmi.bmiHeader.biPlanes      = 1;
    bmi.bmiHeader.biBitCount    = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    HBITMAP hbmColor = CreateDIBSection(nullptr, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!hbmColor || !bits) {
        if (hbmColor) DeleteObject(hbmColor);
        return nullptr;
    }

    HDC hdc = CreateCompatibleDC(nullptr);
    if (!hdc) { DeleteObject(hbmColor); return nullptr; }
    HGDIOBJ oldBmp = SelectObject(hdc, hbmColor);

    DWORD* px = static_cast<DWORD*>(bits);
    const int count = cx * cy;
    for (int i = 0; i < count; ++i) px[i] = kSentinel;

    const theme::Palette& pal = theme::P();
    const COLORREF cGrey     = pal.textDim;        // the ungoverned half
    const COLORREF cGreyEdge = pal.borderStrong;   // outline + the centre divider
    const COLORREF cAccent   = pal.accent;         // the governed half
    const COLORREF cAccentEd = pal.accentPressed;  // its darker outline

    const int inset = (cx >= 24) ? 2 : 1;
    RECT r;
    r.left   = inset;
    r.top    = inset;
    r.right  = cx - inset;
    r.bottom = cy - inset;
    if (r.right <= r.left + 2 || r.bottom <= r.top + 2) {
        r.left = 0; r.top = 0; r.right = cx; r.bottom = cy;
    }
    int rad = cx / 3;
    if (rad < 3) rad = 3;

    HBRUSH brGrey  = CreateSolidBrush(cGrey);
    HPEN   penGrey = CreatePen(PS_SOLID, 1, cGreyEdge);
    HGDIOBJ oldBr  = SelectObject(hdc, brGrey);
    HGDIOBJ oldPen = SelectObject(hdc, penGrey);

    RoundRect(hdc, r.left, r.top, r.right, r.bottom, rad, rad);

    const int mid = (r.left + r.right) / 2;

    if (active) {
        // Redraw the whole rounded shape in the accent colour but clipped to the left half,
        // so the left rounded corners stay rounded instead of turning into a square patch.
        HBRUSH brAcc  = CreateSolidBrush(cAccent);
        HPEN   penAcc = CreatePen(PS_SOLID, 1, cAccentEd);
        SelectObject(hdc, brAcc);
        SelectObject(hdc, penAcc);
        IntersectClipRect(hdc, r.left, r.top, mid, r.bottom);
        RoundRect(hdc, r.left, r.top, r.right, r.bottom, rad, rad);
        SelectClipRgn(hdc, nullptr);
        SelectObject(hdc, brGrey);
        SelectObject(hdc, penGrey);
        DeleteObject(brAcc);
        DeleteObject(penAcc);
    }

    // The divider is what makes "two halves" readable when both halves are grey.
    {
        HPEN penDiv = CreatePen(PS_SOLID, 1, cGreyEdge);
        HGDIOBJ prev = SelectObject(hdc, penDiv);
        MoveToEx(hdc, mid, r.top + 1, nullptr);
        LineTo(hdc, mid, r.bottom - 1);
        SelectObject(hdc, prev);
        DeleteObject(penDiv);
    }

    SelectObject(hdc, oldPen);
    SelectObject(hdc, oldBr);
    DeleteObject(penGrey);
    DeleteObject(brGrey);

    GdiFlush();                       // GDI is batched; the bits are not valid until this
    SelectObject(hdc, oldBmp);
    DeleteDC(hdc);

    for (int i = 0; i < count; ++i) {
        px[i] = ((px[i] & 0x00FFFFFFu) == kSentinel) ? 0x00000000u
                                                     : (px[i] | 0xFF000000u);
    }

    // CreateIconIndirect wants a mask even for a 32bpp alpha icon. All-zero means "take the
    // colour bitmap's alpha channel", which is exactly what the sweep above produced.
    const size_t maskStride = ((static_cast<size_t>(cx) + 15) / 16) * 2;
    std::vector<BYTE> maskBits(maskStride * static_cast<size_t>(cy), 0);
    HBITMAP hbmMask = CreateBitmap(cx, cy, 1, 1, maskBits.data());
    if (!hbmMask) { DeleteObject(hbmColor); return nullptr; }

    ICONINFO ii;
    ZeroMemory(&ii, sizeof(ii));
    ii.fIcon    = TRUE;
    ii.hbmMask  = hbmMask;
    ii.hbmColor = hbmColor;
    HICON hIcon = CreateIconIndirect(&ii);

    DeleteObject(hbmMask);
    DeleteObject(hbmColor);
    return hIcon;
}

void BuildIcons() {
    // The shell draws the notification area at the system small-icon size, which already
    // carries the DPI scale for this process (the manifest declares PerMonitorV2).
    int cx = GetSystemMetrics(SM_CXSMICON);
    int cy = GetSystemMetrics(SM_CYSMICON);
    if (cx <= 0) cx = 16;
    if (cy <= 0) cy = 16;

    if (g_iconIdle)   { DestroyIcon(g_iconIdle);   g_iconIdle = nullptr; }
    if (g_iconActive) { DestroyIcon(g_iconActive); g_iconActive = nullptr; }

    g_iconIdle   = MakeTrayIcon(cx, cy, false);
    g_iconActive = MakeTrayIcon(cx, cy, true);
}

void FillCommon(NOTIFYICONDATAW& nid) {
    ZeroMemory(&nid, sizeof(nid));
    nid.cbSize = sizeof(nid);
    nid.hWnd   = g_hwnd;
    nid.uID    = kTrayIconId;
}

bool AddIcon() {
    NOTIFYICONDATAW nid;
    FillCommon(nid);
    nid.uFlags           = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    nid.uCallbackMessage = g_callbackMsg;
    nid.hIcon            = g_lastActive ? g_iconActive : g_iconIdle;
    CopyTruncated(nid.szTip, ARRAYSIZE(nid.szTip),
                  g_lastTip.empty() ? std::wstring(L"Game Optimizer") : g_lastTip);
    g_added = (Shell_NotifyIconW(NIM_ADD, &nid) != FALSE);
    return g_added;
}

}  // namespace

// ---- Public API ------------------------------------------------------------

bool TrayInit(HWND hwnd, UINT callbackMsg) {
    if (!hwnd) return false;

    g_hwnd        = hwnd;
    g_callbackMsg = callbackMsg;
    g_lastActive  = false;
    g_lastTip     = L"Game Optimizer";

    if (g_taskbarCreated == 0) {
        g_taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
    }

    BuildIcons();

    if (!AddIcon()) {
        // Explorer may not be up yet on a very early autostart. The TaskbarCreated
        // broadcast, handled below, is the recovery path for exactly that.
        LogLine(L"[tray] Shell_NotifyIcon(NIM_ADD) failed, err=%lu", GetLastError());
        return false;
    }
    return true;
}

void TrayShutdown() {
    if (g_added) {
        NOTIFYICONDATAW nid;
        FillCommon(nid);
        Shell_NotifyIconW(NIM_DELETE, &nid);
        g_added = false;
    }
    if (g_iconIdle)   { DestroyIcon(g_iconIdle);   g_iconIdle = nullptr; }
    if (g_iconActive) { DestroyIcon(g_iconActive); g_iconActive = nullptr; }
    g_hwnd        = nullptr;
    g_callbackMsg = 0;
    g_lastTip.clear();
    g_lastActive  = false;
}

void TrayUpdate(const EngineStatus& st) {
    // Paused reads as idle deliberately: nothing is governed while paused, so a bright
    // icon would be a lie.
    g_lastActive = st.active && !st.paused;
    g_lastTip    = StatusText(st);

    if (!g_hwnd) return;
    if (!g_added) { AddIcon(); return; }

    NOTIFYICONDATAW nid;
    FillCommon(nid);
    nid.uFlags = NIF_ICON | NIF_TIP;
    nid.hIcon  = g_lastActive ? g_iconActive : g_iconIdle;
    CopyTruncated(nid.szTip, ARRAYSIZE(nid.szTip), g_lastTip);   // szTip is 128 wchar_t
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

void TrayShowMenu(HWND hwnd, const EngineStatus& st, bool startWithWindows) {
    if (!hwnd) return;

    HMENU menu = CreatePopupMenu();
    if (!menu) return;

    const std::wstring status = EscapeAmpersands(StatusText(st));
    AppendMenuW(menu, MF_STRING | MF_GRAYED | MF_DISABLED, IDM_STATUS, status.c_str());
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, IDM_SETTINGS, L"&Settings...");
    AppendMenuW(menu, MF_STRING | (st.paused ? MF_CHECKED : MF_UNCHECKED), IDM_PAUSE,
                st.paused ? L"&Resume" : L"&Pause");
    AppendMenuW(menu, MF_STRING | (startWithWindows ? MF_CHECKED : MF_UNCHECKED),
                IDM_STARTUP, L"Start with &Windows");
    AppendMenuW(menu, MF_STRING, IDM_OPENFOLDER, L"&Open config folder");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, IDM_EXIT, L"E&xit");

    POINT pt;
    if (!GetCursorPos(&pt)) { pt.x = 0; pt.y = 0; }

    // Required Win32 workaround, not a style choice: without the foreground activation the
    // popup never receives the click that should dismiss it, and without the trailing
    // WM_NULL the menu can stay up after the user clicks away. Documented on
    // TrackPopupMenu; the tray has produced this bug for thirty years.
    SetForegroundWindow(hwnd);
    TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_RIGHTALIGN | TPM_BOTTOMALIGN,
                   pt.x, pt.y, 0, hwnd, nullptr);
    PostMessageW(hwnd, WM_NULL, 0, 0);

    DestroyMenu(menu);
}

void TrayNotify(const std::wstring& title, const std::wstring& text) {
    // The caller decides whether notifications are on; this function just sends one.
    if (!g_hwnd || !g_added) return;

    NOTIFYICONDATAW nid;
    FillCommon(nid);
    nid.uFlags      = NIF_INFO;
    nid.dwInfoFlags = NIIF_INFO | NIIF_NOSOUND;
    CopyTruncated(nid.szInfoTitle, ARRAYSIZE(nid.szInfoTitle), title);   // 64 wchar_t
    CopyTruncated(nid.szInfo,      ARRAYSIZE(nid.szInfo),      text);    // 256 wchar_t
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

// Explorer restarting destroys every notification icon without telling the owner anything
// except this broadcast. Re-adding is the only way the icon comes back.
bool TrayHandleTaskbarCreated(UINT msg) {
    if (g_taskbarCreated == 0 || msg != g_taskbarCreated) return false;
    g_added = false;
    BuildIcons();
    AddIcon();
    return true;
}

}  // namespace cd
