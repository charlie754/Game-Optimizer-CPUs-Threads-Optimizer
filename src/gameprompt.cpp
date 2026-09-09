// Game Optimizer - the "we noticed you started a game" toast.
//
// ONE window lives here now. This file used to hold a second one, PickGame, a modal picker
// over cd::DiscoverGames(), and it was DELETED on 2026-09-09 rather than left to rot. The
// reason is recorded because "why is there no game picker" is the obvious question:
//
//   The picker listed DISCOVERED games only and had no free-text escape hatch, so Cancel
//   was its only exit for a game it had not found. Its one caller, the Profiles page's
//   "Add game..." button, therefore COULD NOT create a profile for an undiscovered game -
//   and the operator worked around that by picking an arbitrary listed game intending to
//   correct the exe afterwards. One correction was missed, which is how two enabled
//   profiles came to name the same executable. The UI manufactured bad data.
//
//   "Add profile..." plus the Game field's "Browse..." is a strict superset: Browse reaches
//   any file on disk, discovered or not. So the picker was not replaced, it was redundant.
//
// cd::DiscoverGames() ITSELF IS STILL LIVE and games.h is untouched - engine.cpp builds the
// "All Games" profile's candidate list from it. Only this dialog went.
//
// ---------------------------------------------------------------------------
// ShowGamePrompt - a bottom-right toast that MUST NOT STEAL FOCUS
// ---------------------------------------------------------------------------
// ui.h states the rule and it is the single hardest constraint in this file: the user is
// very likely inside a fullscreen game when this appears, and taking the foreground away
// from a fullscreen game minimises it, drops its render loop, and in some engines costs the
// user the match. So:
//
//   * WS_POPUP | WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE, shown with
//     SW_SHOWNOACTIVATE, and WM_MOUSEACTIVATE answered with MA_NOACTIVATE.
//   * IT HAS NO CHILD CONTROLS AT ALL. That is deliberate and it is the part that is easy
//     to get wrong: a real BUTTON calls SetFocus on itself from its WM_LBUTTONDOWN handler,
//     and SetFocus is documented to activate the focused window's top-level parent. One
//     click on a themed BS_OWNERDRAW button would therefore be enough to pull the
//     foreground off the game, which is exactly the defect the header forbids. The three
//     buttons are drawn by this window's own WM_PAINT and hit-tested in its own mouse
//     handlers, so no SetFocus is ever called and no activation can be requested.
//   * The price of having no button HWND is that theme::DrawButton, which reads its caption
//     from di->hwndItem, cannot draw the caption for us - it is given a null hwndItem and
//     paints the surface only, and the caption is drawn here immediately afterwards.
//     CaptionColour() below mirrors theme.cpp's per-kind text colour; it is the one piece of
//     duplication this design costs and it is named so it can be found if the theme changes.
//   * A 20 s timer dismisses it as "Not now". A prompt that waits forever on top of a
//     fullscreen game is a nuisance, and the user who ignored it has already answered.
//
// Every exit path - button, timer, close, destroy - goes through PostResult, which posts
// WM_APP_GAMEPROMPT exactly once with a heap std::wstring the receiver deletes.
//
// NOTHING HERE TOUCHES THE NETWORK.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <windowsx.h>

#include <string>

#include "ui.h"
#include "theme.h"
#include "util.h"

namespace cd {

namespace {

// ---------------------------------------------------------------------------
// Shared helpers (deliberately local: ui.h is frozen, so nothing new is exported)
// ---------------------------------------------------------------------------

int DpiOf(HWND h) {
    UINT d = h ? GetDpiForWindow(h) : 0;
    if (d == 0) d = GetDpiForSystem();
    if (d == 0) d = 96;
    return static_cast<int>(d);
}

int Dp(int v, int dpi) { return theme::Dp(v, dpi); }

// The caption colour theme::DrawButton would have used. Only needed for the toast, whose
// buttons have no HWND for DrawButton to read a caption from - see the file header.
COLORREF CaptionColour(theme::ButtonKind kind, bool hot, bool pressed) {
    const theme::Palette& p = theme::P();
    switch (kind) {
        case theme::ButtonKind::Primary: return p.textOnAccent;
        case theme::ButtonKind::Danger:  return p.textOnAccent;
        case theme::ButtonKind::Ghost:   return (hot || pressed) ? p.textPrimary
                                                                 : p.textSecondary;
        case theme::ButtonKind::Secondary:
        default:                         return p.textPrimary;
    }
}

bool PtIn(const RECT& rc, POINT pt) {
    return pt.x >= rc.left && pt.x < rc.right && pt.y >= rc.top && pt.y < rc.bottom;
}

// ===========================================================================
// ShowGamePrompt
// ===========================================================================

const wchar_t kToastClass[] = L"GameOptimizerGamePrompt";

enum : int { BTN_APPLY = 0, BTN_NOTNOW = 1, BTN_NEVER = 2, BTN_COUNT = 3 };

const UINT_PTR kToastTimer   = 1;
const UINT     kToastTimeout = 20000;   // 20 s, then "Not now"

// Passed as lpCreateParams and read once, in WM_NCCREATE. The window allocates and owns the
// real ToastState from it, which is what removes the classic double-free: if CreateWindowEx
// fails AFTER WM_NCCREATE ran, WM_NCDESTROY has already freed everything the window owned,
// and the caller has nothing left to clean up.
struct ToastInit {
    HWND notify = nullptr;
    const std::wstring* exe = nullptr;
    const std::wstring* displayName = nullptr;
    const std::wstring* maskName = nullptr;
    int dpi = 96;
};

struct ToastState {
    HWND         notify = nullptr;
    std::wstring exe;
    std::wstring displayName;
    std::wstring maskName;
    int          dpi = 96;

    RECT rcTitle = { 0, 0, 0, 0 };
    RECT rcBody  = { 0, 0, 0, 0 };
    RECT rcExe   = { 0, 0, 0, 0 };
    RECT rcBtn[BTN_COUNT];

    int  hot = -1;        // index under the cursor, -1 for none
    int  pressed = -1;    // index the left button went down on
    bool tracking = false;
    bool posted = false;  // WM_APP_GAMEPROMPT has already been posted for this toast
};

// At most one toast at a time. A second detection while one is up replaces it rather than
// stacking two overlapping windows in the same corner.
HWND g_toast = nullptr;

const wchar_t* ButtonCaption(int i) {
    switch (i) {
        case BTN_APPLY:  return L"Apply";
        case BTN_NOTNOW: return L"Not now";
        case BTN_NEVER:  return L"Never for this game";
        default:         return L"";
    }
}

theme::ButtonKind ButtonKindFor(int i) {
    switch (i) {
        case BTN_APPLY:  return theme::ButtonKind::Primary;
        case BTN_NOTNOW: return theme::ButtonKind::Secondary;
        case BTN_NEVER:  return theme::ButtonKind::Ghost;
        default:         return theme::ButtonKind::Secondary;
    }
}

// The one place WM_APP_GAMEPROMPT is posted. The receiver takes ownership of the string, so
// a post that FAILS has to free it here - nobody else can.
void PostGamePromptResult(HWND notify, const std::wstring& exe, GamePromptResult r) {
    if (notify == nullptr || !IsWindow(notify)) return;
    std::wstring* payload = new std::wstring(exe);
    if (!PostMessageW(notify, WM_APP_GAMEPROMPT, static_cast<WPARAM>(r),
                      reinterpret_cast<LPARAM>(payload))) {
        delete payload;
    }
}

// Posts the result exactly once. Safe to call from every exit path, including WM_DESTROY,
// which is what guarantees the caller always hears back even if the window dies unexpectedly.
void PostResult(ToastState* st, GamePromptResult r) {
    if (st == nullptr || st->posted) return;
    st->posted = true;
    PostGamePromptResult(st->notify, st->exe, r);
}

void ToastFinish(HWND hwnd, ToastState* st, GamePromptResult r) {
    PostResult(st, r);
    DestroyWindow(hwnd);
}

// Laid out from the top for the title and from the BOTTOM for the buttons and the exe line,
// so the body paragraph absorbs whatever height is left over and no fixed pixel budget can
// overflow at a scaling factor nobody tested.
void ToastLayout(HWND hwnd, ToastState* st) {
    RECT client;
    GetClientRect(hwnd, &client);

    RECT card = client;
    InflateRect(&card, -Dp(1, st->dpi), -Dp(1, st->dpi));

    HDC dc = GetDC(hwnd);
    RECT inner = card;
    const int pad = Dp(theme::metric::kCardPad, st->dpi);
    InflateRect(&inner, -pad, -pad);

    const int titleH = dc ? theme::MeasureText(dc, L"Ag", theme::Font::UiHeading,
                                               st->dpi).cy
                          : Dp(18, st->dpi);
    const int exeH   = dc ? theme::MeasureText(dc, L"Ag", theme::Font::MonoBody,
                                               st->dpi).cy
                          : Dp(14, st->dpi);
    const int btnH   = Dp(theme::metric::kButtonH, st->dpi);
    const int gap    = Dp(theme::metric::kGapTight, st->dpi);

    // Buttons, right to left along the bottom row.
    const int btnY = inner.bottom - btnH;
    int widths[BTN_COUNT];
    for (int i = 0; i < BTN_COUNT; ++i) {
        const std::wstring cap = ButtonCaption(i);
        const int textW = dc ? theme::MeasureText(dc, cap, theme::Font::UiBody, st->dpi).cx
                             : Dp(60, st->dpi);
        widths[i] = textW + Dp(24, st->dpi);
    }

    RECT r;
    r.top = btnY; r.bottom = btnY + btnH;
    r.right = inner.right;
    r.left  = r.right - widths[BTN_APPLY];
    st->rcBtn[BTN_APPLY] = r;

    r.right = st->rcBtn[BTN_APPLY].left - gap;
    r.left  = r.right - widths[BTN_NOTNOW];
    st->rcBtn[BTN_NOTNOW] = r;

    r.left  = inner.left;
    r.right = r.left + widths[BTN_NEVER];
    // Never overlap the two answers on its right; the caption ellipsises instead.
    if (r.right > st->rcBtn[BTN_NOTNOW].left - gap) {
        r.right = st->rcBtn[BTN_NOTNOW].left - gap;
    }
    if (r.right < r.left) r.right = r.left;
    st->rcBtn[BTN_NEVER] = r;

    st->rcExe = inner;
    st->rcExe.bottom = btnY - gap;
    st->rcExe.top    = st->rcExe.bottom - exeH;

    st->rcTitle = inner;
    st->rcTitle.bottom = inner.top + titleH;

    st->rcBody = inner;
    st->rcBody.top    = st->rcTitle.bottom + gap;
    st->rcBody.bottom = st->rcExe.top - gap;
    if (st->rcBody.bottom < st->rcBody.top) st->rcBody.bottom = st->rcBody.top;

    if (dc) ReleaseDC(hwnd, dc);
}

void ToastPaint(HWND hwnd, ToastState* st, HDC dc) {
    RECT client;
    GetClientRect(hwnd, &client);
    theme::FillBackground(dc, client);

    RECT card = client;
    InflateRect(&card, -Dp(1, st->dpi), -Dp(1, st->dpi));
    theme::DrawCard(dc, card, st->dpi);

    const theme::Palette& p = theme::P();

    theme::DrawText(dc, st->rcTitle, L"Optimize CPU for this game?",
                    theme::Font::UiHeading, st->dpi, p.textPrimary,
                    DT_LEFT | DT_TOP | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);

    std::wstring body = L"Game Optimizer can put ";
    body += st->displayName;
    body += L" on ";
    body += st->maskName;
    body += L" and keep background apps off those cores.";
    theme::DrawText(dc, st->rcBody, body, theme::Font::UiBody, st->dpi, p.textSecondary,
                    DT_LEFT | DT_TOP | DT_WORDBREAK | DT_NOPREFIX | DT_END_ELLIPSIS);

    theme::DrawText(dc, st->rcExe, st->exe, theme::Font::MonoBody, st->dpi, p.textDim,
                    DT_LEFT | DT_TOP | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);

    for (int i = 0; i < BTN_COUNT; ++i) {
        const RECT& rc = st->rcBtn[i];
        if (rc.right <= rc.left) continue;
        const bool pressed = (st->pressed == i && st->hot == i);
        const bool hot     = (st->hot == i);

        DRAWITEMSTRUCT di;
        ZeroMemory(&di, sizeof(di));
        di.CtlType   = ODT_BUTTON;
        di.itemAction= ODA_DRAWENTIRE;
        di.hDC       = dc;
        di.rcItem    = rc;
        di.hwndItem  = nullptr;          // no child control exists - see the file header
        di.itemState = (pressed ? ODS_SELECTED : 0) | (hot ? ODS_HOTLIGHT : 0);

        const theme::ButtonKind kind = ButtonKindFor(i);
        theme::DrawButton(&di, kind, st->dpi);

        RECT t = rc;
        const int padx = Dp(10, st->dpi);
        t.left  += padx;
        t.right -= padx;
        if (t.right < t.left) t.right = t.left;
        theme::DrawText(dc, t, ButtonCaption(i), theme::Font::UiBody, st->dpi,
                        CaptionColour(kind, hot, pressed),
                        DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX |
                            DT_END_ELLIPSIS);
    }
}

int ToastHitTest(ToastState* st, POINT pt) {
    for (int i = 0; i < BTN_COUNT; ++i) {
        if (st->rcBtn[i].right > st->rcBtn[i].left && PtIn(st->rcBtn[i], pt)) return i;
    }
    return -1;
}

void ToastInvalidateButtons(HWND hwnd, ToastState* st) {
    for (int i = 0; i < BTN_COUNT; ++i) {
        if (st->rcBtn[i].right > st->rcBtn[i].left) {
            InvalidateRect(hwnd, &st->rcBtn[i], FALSE);
        }
    }
}

GamePromptResult ResultForButton(int i) {
    switch (i) {
        case BTN_APPLY: return GamePromptResult::Apply;
        case BTN_NEVER: return GamePromptResult::Never;
        case BTN_NOTNOW:
        default:        return GamePromptResult::Dismissed;
    }
}

LRESULT CALLBACK ToastProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    ToastState* st = reinterpret_cast<ToastState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    switch (msg) {
        case WM_NCCREATE: {
            CREATESTRUCTW* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
            const ToastInit* init =
                cs ? reinterpret_cast<const ToastInit*>(cs->lpCreateParams) : nullptr;
            if (!init) return FALSE;
            ToastState* ns = new ToastState();
            ns->notify      = init->notify;
            if (init->exe)         ns->exe         = *init->exe;
            if (init->displayName) ns->displayName = *init->displayName;
            if (init->maskName)    ns->maskName    = *init->maskName;
            ns->dpi = init->dpi;
            for (int i = 0; i < BTN_COUNT; ++i) SetRectEmpty(&ns->rcBtn[i]);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(ns));
            break;
        }
        case WM_CREATE: {
            st = reinterpret_cast<ToastState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
            if (!st) return -1;
            st->dpi = DpiOf(hwnd);
            ToastLayout(hwnd, st);
            SetTimer(hwnd, kToastTimer, kToastTimeout, nullptr);
            return 0;
        }
        // The whole point of the window. Answering MA_NOACTIVATE means a click reaches this
        // window without the foreground - the game - ever changing.
        case WM_MOUSEACTIVATE:
            return MA_NOACTIVATE;

        case WM_SIZE:
            if (st) { ToastLayout(hwnd, st); InvalidateRect(hwnd, nullptr, TRUE); }
            return 0;

        case WM_ERASEBKGND: {
            RECT rc;
            GetClientRect(hwnd, &rc);
            theme::FillBackground(reinterpret_cast<HDC>(wp), rc);
            return 1;
        }

        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(hwnd, &ps);
            if (dc && st) ToastPaint(hwnd, st, dc);
            EndPaint(hwnd, &ps);
            return 0;
        }

        case WM_MOUSEMOVE: {
            if (!st) break;
            POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
            const int hit = ToastHitTest(st, pt);
            if (hit != st->hot) {
                st->hot = hit;
                ToastInvalidateButtons(hwnd, st);
            }
            if (!st->tracking) {
                TRACKMOUSEEVENT tme;
                ZeroMemory(&tme, sizeof(tme));
                tme.cbSize    = sizeof(tme);
                tme.dwFlags   = TME_LEAVE;
                tme.hwndTrack = hwnd;
                if (TrackMouseEvent(&tme)) st->tracking = true;
            }
            return 0;
        }

        case WM_MOUSELEAVE: {
            if (!st) break;
            st->tracking = false;
            if (st->hot != -1) { st->hot = -1; ToastInvalidateButtons(hwnd, st); }
            return 0;
        }

        case WM_LBUTTONDOWN: {
            if (!st) break;
            POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
            st->pressed = ToastHitTest(st, pt);
            st->hot     = st->pressed;
            if (st->pressed >= 0) {
                // Capture is per-thread and needs no activation, unlike SetFocus.
                SetCapture(hwnd);
                ToastInvalidateButtons(hwnd, st);
            }
            return 0;
        }

        case WM_LBUTTONUP: {
            if (!st) break;
            const int was = st->pressed;
            st->pressed = -1;
            if (GetCapture() == hwnd) ReleaseCapture();
            POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
            const int hit = ToastHitTest(st, pt);
            ToastInvalidateButtons(hwnd, st);
            if (was >= 0 && hit == was) {
                ToastFinish(hwnd, st, ResultForButton(was));
            }
            return 0;
        }

        case WM_CAPTURECHANGED:
            if (st && st->pressed >= 0) {
                st->pressed = -1;
                ToastInvalidateButtons(hwnd, st);
            }
            return 0;

        case WM_TIMER:
            if (st && wp == kToastTimer) {
                KillTimer(hwnd, kToastTimer);
                ToastFinish(hwnd, st, GamePromptResult::Dismissed);
            }
            return 0;

        case WM_CLOSE:
            if (st) ToastFinish(hwnd, st, GamePromptResult::Dismissed);
            return 0;

        case WM_DESTROY:
            KillTimer(hwnd, kToastTimer);
            // Backstop: a destroy that did not come through ToastFinish still answers.
            if (st) PostResult(st, GamePromptResult::Dismissed);
            if (g_toast == hwnd) g_toast = nullptr;
            return 0;

        case WM_NCDESTROY:
            if (st) {
                SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
                delete st;
            }
            break;

        default:
            break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void RegisterToastClass() {
    static bool done = false;
    if (done) return;
    WNDCLASSEXW wc;
    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = ToastProc;
    wc.hInstance     = GetModuleHandleW(nullptr);
    wc.hCursor       = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;      // WM_ERASEBKGND paints appBg; no light flash
    wc.lpszClassName = kToastClass;
    RegisterClassExW(&wc);
    done = true;
}

}  // namespace

// ---------------------------------------------------------------------------
// Public entry points
// ---------------------------------------------------------------------------

void ShowGamePrompt(HWND notify, const std::wstring& exeBaseName,
                    const std::wstring& displayName, const std::wstring& maskName) {
    if (exeBaseName.empty()) return;
    RegisterToastClass();

    // Replace rather than stack: two toasts in the same corner would overlap.
    if (g_toast != nullptr && IsWindow(g_toast)) {
        DestroyWindow(g_toast);      // posts Dismissed for the toast being replaced
        g_toast = nullptr;
    }

    const int dpi = DpiOf(notify ? notify : GetDesktopWindow());

    const std::wstring shownName = displayName.empty() ? exeBaseName : displayName;
    const std::wstring shownMask =
        maskName.empty() ? std::wstring(L"the game mask") : maskName;

    ToastInit init;
    init.notify      = notify;
    init.exe         = &exeBaseName;
    init.displayName = &shownName;
    init.maskName    = &shownMask;
    init.dpi         = dpi;

    const int w = Dp(340, dpi);
    const int h = Dp(150, dpi);

    // SPI_GETWORKAREA, so the toast sits above the taskbar rather than under it.
    RECT work = { 0, 0, 0, 0 };
    if (!SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0)) {
        work.left = 0; work.top = 0;
        work.right  = GetSystemMetrics(SM_CXSCREEN);
        work.bottom = GetSystemMetrics(SM_CYSCREEN);
    }
    const int margin = Dp(16, dpi);
    const int x = work.right - w - margin;
    const int y = work.bottom - h - margin;

    // `notify` is the OWNER, not a parent: for a WS_POPUP that hWndParent slot is ownership,
    // and an owned window is destroyed with its owner. That is what guarantees an unanswered
    // toast cannot outlive the app's own window and paint with the fonts theme::Shutdown has
    // already freed. WS_EX_TOOLWINDOW keeps it off the taskbar and out of Alt+Tab.
    HWND hwnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE,
                                kToastClass, L"", WS_POPUP,
                                x, y, w, h, notify, nullptr,
                                GetModuleHandleW(nullptr), &init);
    if (!hwnd) {
        // Nobody will ever see this prompt, so answer for it rather than leaving the caller
        // waiting for a message that is not coming. Whether WM_NCCREATE ran or not, the
        // window owned and freed its own state - there is nothing to delete here. A window
        // that died AFTER WM_NCCREATE has already posted its own Dismissed, so this can post
        // a second one; Dismissed is the do-nothing result on every receiver path, and one
        // redundant no-op is the right price for never dropping the answer entirely.
        PostGamePromptResult(notify, exeBaseName, GamePromptResult::Dismissed);
        return;
    }
    g_toast = hwnd;
    // SW_SHOWNOACTIVATE, never ShowWindow(SW_SHOW): the game keeps the foreground.
    ShowWindow(hwnd, SW_SHOWNOACTIVATE);
    UpdateWindow(hwnd);
}

}  // namespace cd
