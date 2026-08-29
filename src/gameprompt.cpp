// Game Optimizer - the "we noticed you started a game" toast, and the game picker.
//
// Two windows live here because they are the two places the app talks about GAMES rather
// than about processes, and they share the same discovery data source (games.h).
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
// ---------------------------------------------------------------------------
// PickGame - a modal picker over cd::DiscoverGames()
// ---------------------------------------------------------------------------
// DiscoverGames() walks Steam libraries, Epic manifests, the GOG registry keys and the
// packaged-app store; games.h measures that at tens to a few hundred milliseconds. Running
// it on the UI thread would block the message loop mid-paint, which is the freeze users
// notice and blame on the app. So the scan runs on a std::thread started in WM_CREATE, the
// window shows BundledGames() immediately, and the scan result is merged when it arrives.
// The thread is JOINED in WM_DESTROY: joining is what makes the state pointer the worker
// writes through provably alive, and DiscoverGames is bounded so the join is bounded too.
//
// NOTHING HERE TOUCHES THE NETWORK.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <windowsx.h>

#include <algorithm>
#include <string>
#include <thread>
#include <vector>

#include "ui.h"
#include "games.h"
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

// ===========================================================================
// PickGame
// ===========================================================================

const wchar_t kPickClass[] = L"GameOptimizerGamePicker";

enum : int {
    IDC_GP_SEARCH = 1500,
    IDC_GP_LIST,
    IDC_GP_OK,
    IDC_GP_CANCEL
};

// Posted by the discovery worker when its scan is finished. The payload lives in the state
// the worker was handed, which is kept alive by joining the thread in WM_DESTROY.
const UINT kMsgScanDone = WM_APP + 20;

struct PickState {
    std::vector<GameEntry> all;      // what the list is built from, bundled + discovered
    std::vector<size_t>    shown;    // indices into `all` after filtering
    std::wstring           query;

    std::vector<GameEntry> scanned;  // written by the worker thread ONLY
    std::thread            scan;
    bool                   scanMerged = false;

    HWND hSearch = nullptr;
    HWND hList   = nullptr;
    HWND hOk     = nullptr;
    HWND hCancel = nullptr;

    RECT rcSearch = { 0, 0, 0, 0 };   // the CHROME rect, larger than the EDIT inside it
    RECT rcList   = { 0, 0, 0, 0 };

    bool searchEmpty   = true;        // last state the chrome was painted for
    bool searchFocused = false;

    int  dpi = 96;
    bool done = false;
    std::wstring result;
    std::wstring resultName;
};

void SetButtonKind(HWND h, theme::ButtonKind k) {
    if (h) SetWindowLongPtrW(h, GWLP_USERDATA,
                             static_cast<LONG_PTR>(static_cast<int>(k)) + 1);
}

theme::ButtonKind ButtonKindOf(HWND h) {
    LONG_PTR v = h ? GetWindowLongPtrW(h, GWLP_USERDATA) : 0;
    if (v <= 0) return theme::ButtonKind::Secondary;
    return static_cast<theme::ButtonKind>(static_cast<int>(v) - 1);
}

HWND Mk(HWND parent, const wchar_t* cls, const wchar_t* text, DWORD style, int id) {
    return CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 10, 10,
                           parent, reinterpret_cast<HMENU>(static_cast<UINT_PTR>(id)),
                           reinterpret_cast<HINSTANCE>(
                               GetWindowLongPtrW(parent, GWLP_HINSTANCE)),
                           nullptr);
}

std::wstring GetText(HWND h) {
    if (!h) return std::wstring();
    int n = GetWindowTextLengthW(h);
    if (n <= 0) return std::wstring();
    std::vector<wchar_t> buf(static_cast<size_t>(n) + 1, L'\0');
    GetWindowTextW(h, &buf[0], n + 1);
    return std::wstring(&buf[0]);
}

// Adds every entry of `extra` whose exe basename is not already present. Used to keep the
// bundled names visible after the real scan lands, so a game the launchers do not claim does
// not vanish from the list the user was already looking at.
void MergeInto(std::vector<GameEntry>& into, const std::vector<GameEntry>& extra) {
    std::vector<std::wstring> have;
    have.reserve(into.size());
    for (size_t i = 0; i < into.size(); ++i) have.push_back(ToLower(into[i].exe));
    for (size_t i = 0; i < extra.size(); ++i) {
        const std::wstring key = ToLower(extra[i].exe);
        bool dup = false;
        for (size_t j = 0; j < have.size(); ++j) {
            if (have[j] == key) { dup = true; break; }
        }
        if (!dup) {
            into.push_back(extra[i]);
            have.push_back(key);
        }
    }
}

void PickFill(PickState* st) {
    // Remember what was selected so a refill - a keystroke, or the scan landing - does not
    // move the highlight out from under the user.
    std::wstring keep;
    const int prev = static_cast<int>(SendMessageW(st->hList, LB_GETCURSEL, 0, 0));
    if (prev >= 0 && prev < static_cast<int>(st->shown.size())) {
        keep = st->all[st->shown[static_cast<size_t>(prev)]].exe;
    }

    const std::vector<GameEntry> filtered = FilterGames(st->all, st->query);

    // FilterGames returns copies; map them back to indices in `all` so the row painter can
    // read the source tag and the install state from one place.
    st->shown.clear();
    st->shown.reserve(filtered.size());
    size_t from = 0;
    for (size_t i = 0; i < filtered.size(); ++i) {
        for (size_t j = from; j < st->all.size(); ++j) {
            if (IEquals(st->all[j].exe, filtered[i].exe) &&
                IEquals(st->all[j].name, filtered[i].name)) {
                st->shown.push_back(j);
                from = j + 1;
                break;
            }
        }
    }

    SendMessageW(st->hList, WM_SETREDRAW, FALSE, 0);
    SendMessageW(st->hList, LB_RESETCONTENT, 0, 0);
    int restore = -1;
    for (size_t i = 0; i < st->shown.size(); ++i) {
        const GameEntry& g = st->all[st->shown[i]];
        SendMessageW(st->hList, LB_ADDSTRING, 0,
                     reinterpret_cast<LPARAM>(g.name.c_str()));
        if (!keep.empty() && IEquals(g.exe, keep)) restore = static_cast<int>(i);
    }
    if (restore < 0 && !st->shown.empty()) restore = 0;
    if (restore >= 0) SendMessageW(st->hList, LB_SETCURSEL,
                                   static_cast<WPARAM>(restore), 0);
    SendMessageW(st->hList, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(st->hList, nullptr, TRUE);
}

void PickLayout(PickState* st, HWND hwnd) {
    RECT rc;
    GetClientRect(hwnd, &rc);
    const int cw = rc.right - rc.left;
    const int ch = rc.bottom - rc.top;
    const int dpi = st->dpi;
    const int M   = Dp(theme::metric::kGap, dpi);
    const int btnH = Dp(theme::metric::kButtonH, dpi);
    const int btnW = Dp(theme::metric::kButtonW, dpi);
    const int searchH = Dp(32, dpi);

    st->rcSearch.left   = M;
    st->rcSearch.top    = M;
    st->rcSearch.right  = cw - M;
    st->rcSearch.bottom = M + searchH;

    // The EDIT sits INSIDE the chrome, clear of the magnifier gutter on the left.
    MoveWindow(st->hSearch,
               st->rcSearch.left + Dp(24, dpi),
               st->rcSearch.top + Dp(6, dpi),
               (st->rcSearch.right - Dp(8, dpi)) - (st->rcSearch.left + Dp(24, dpi)),
               searchH - Dp(12, dpi), TRUE);

    const int listTop = st->rcSearch.bottom + Dp(10, dpi);
    int listBottom = ch - M - btnH - Dp(10, dpi);
    if (listBottom < listTop + Dp(60, dpi)) listBottom = listTop + Dp(60, dpi);
    st->rcList.left   = M;
    st->rcList.top    = listTop;
    st->rcList.right  = cw - M;
    st->rcList.bottom = listBottom;
    MoveWindow(st->hList, st->rcList.left, st->rcList.top,
               st->rcList.right - st->rcList.left,
               st->rcList.bottom - st->rcList.top, TRUE);

    const int by = ch - M - btnH;
    MoveWindow(st->hCancel, cw - M - btnW, by, btnW, btnH, TRUE);
    MoveWindow(st->hOk, cw - M - 2 * btnW - Dp(theme::metric::kGapTight, dpi), by,
               btnW, btnH, TRUE);
}

// The search chrome is painted on a GetDCEx DC WITHOUT DCX_CLIPCHILDREN, because the window
// is WS_CLIPCHILDREN and a BeginPaint DC has the EDIT's rect clipped out of it - the same
// measured trap settings.cpp documents for the combo frames. Painting unclipped means the
// fill also lands on the EDIT's own pixels, so the EDIT is invalidated afterwards whenever it
// has something to redraw. Called only on a state change, never per keystroke, so typing
// does not flicker.
void PaintSearchChrome(PickState* st, HWND hwnd) {
    if (!st || !st->hSearch) return;
    HDC dc = GetDCEx(hwnd, nullptr, DCX_CACHE);
    if (!dc) return;
    theme::DrawSearchChrome(dc, st->rcSearch, st->dpi, st->searchFocused,
                            st->searchEmpty, L"Search games...");
    ReleaseDC(hwnd, dc);
    if (!st->searchEmpty || st->searchFocused) {
        InvalidateRect(st->hSearch, nullptr, TRUE);
        UpdateWindow(st->hSearch);
    }
}

void PickPaint(PickState* st, HWND hwnd, HDC dc) {
    RECT client;
    GetClientRect(hwnd, &client);
    theme::FillBackground(dc, client);

    const theme::Palette& p = theme::P();

    // A 1px themed ring around the list. The list's own rect is clipped out of this DC by
    // WS_CLIPCHILDREN, so only the ring lands.
    RECT ring = st->rcList;
    InflateRect(&ring, Dp(1, st->dpi), Dp(1, st->dpi));
    theme::DrawRoundRect(dc, ring, Dp(6, st->dpi), p.inputBg, p.border);

    // The count, on the button row, left aligned.
    wchar_t line[128];
    wsprintfW(line, L"%d of %d games", static_cast<int>(st->shown.size()),
              static_cast<int>(st->all.size()));
    RECT rcCount;
    rcCount.left   = Dp(theme::metric::kGap, st->dpi);
    rcCount.right  = client.right / 2;
    rcCount.bottom = client.bottom - Dp(theme::metric::kGap, st->dpi);
    rcCount.top    = rcCount.bottom - Dp(theme::metric::kButtonH, st->dpi);
    theme::DrawText(dc, rcCount, line, theme::Font::UiSmall, st->dpi, p.textDim,
                    DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
}

// One list row: display name in UiBody, exe in MonoSmall secondary, source tag as a pill on
// the right. Drawn here rather than through theme::DrawListBoxItem because that helper draws
// one line of the control's own string and this row carries three fields.
BOOL PickDrawRow(PickState* st, const DRAWITEMSTRUCT* di) {
    if (!di || !di->hDC) return FALSE;
    const theme::Palette& p = theme::P();
    RECT rc = di->rcItem;
    if (rc.right <= rc.left || rc.bottom <= rc.top) return FALSE;

    const bool selected = (di->itemState & ODS_SELECTED) != 0;
    HBRUSH bg = CreateSolidBrush(selected ? p.cardBgAlt : p.inputBg);
    if (bg) { FillRect(di->hDC, &rc, bg); DeleteObject(bg); }

    if (di->itemID == static_cast<UINT>(-1)) return TRUE;
    if (di->itemID >= st->shown.size()) return TRUE;
    const GameEntry& g = st->all[st->shown[di->itemID]];

    const int barW = Dp(3, st->dpi);
    if (selected) {
        RECT bar = rc;
        bar.right = bar.left + barW;
        HBRUSH accent = CreateSolidBrush(p.accent);
        if (accent) { FillRect(di->hDC, &bar, accent); DeleteObject(accent); }
    }

    // Source pill, right aligned and vertically centred.
    const std::wstring tag = GameSourceName(g.source);
    RECT pill = rc;
    if (!tag.empty()) {
        const SIZE sz = theme::MeasureText(di->hDC, tag, theme::Font::UiSmall, st->dpi);
        const int pillH = sz.cy + Dp(6, st->dpi);
        const int pillW = sz.cx + Dp(18, st->dpi);
        pill.right  = rc.right - Dp(10, st->dpi);
        pill.left   = pill.right - pillW;
        pill.top    = rc.top + ((rc.bottom - rc.top) - pillH) / 2;
        pill.bottom = pill.top + pillH;
        if (pill.left > rc.left + Dp(80, st->dpi)) {
            theme::DrawPill(di->hDC, pill, tag, st->dpi,
                            g.installed ? p.cardBgAlt : p.appBg,
                            g.installed ? p.textSecondary : p.textDim);
        } else {
            pill.left = rc.right;   // no room; the text columns take the whole width
        }
    } else {
        pill.left = rc.right;
    }

    RECT t = rc;
    t.left  = rc.left + barW + Dp(10, st->dpi);
    t.right = pill.left - Dp(8, st->dpi);
    if (t.right < t.left) t.right = t.left;

    const int half = (rc.bottom - rc.top) / 2;
    RECT rn = t;
    rn.top    = rc.top + Dp(3, st->dpi);
    rn.bottom = rc.top + half + Dp(2, st->dpi);
    theme::DrawText(di->hDC, rn, g.name, theme::Font::UiBody, st->dpi, p.textPrimary,
                    DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);

    RECT re = t;
    re.top    = rn.bottom;
    re.bottom = rc.bottom - Dp(3, st->dpi);
    theme::DrawText(di->hDC, re, g.exe, theme::Font::MonoSmall, st->dpi, p.textSecondary,
                    DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
    return TRUE;
}

void PickAccept(PickState* st) {
    const int sel = static_cast<int>(SendMessageW(st->hList, LB_GETCURSEL, 0, 0));
    if (sel < 0 || sel >= static_cast<int>(st->shown.size())) return;
    const GameEntry& g = st->all[st->shown[static_cast<size_t>(sel)]];
    st->result     = g.exe;
    st->resultName = g.name;
    st->done = true;
}

LRESULT CALLBACK PickProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    PickState* st = reinterpret_cast<PickState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    if (msg == kMsgScanDone) {
        if (st && !st->scanMerged) {
            st->scanMerged = true;
            // Discovered entries first - they carry real paths - then whatever bundled
            // names the scan did not already account for.
            std::vector<GameEntry> merged = st->scanned;
            MergeInto(merged, BundledGames());
            if (!merged.empty()) st->all.swap(merged);
            PickFill(st);
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    }

    switch (msg) {
        case WM_NCCREATE: {
            CREATESTRUCTW* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                              reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
            break;
        }
        case WM_CREATE: {
            st = reinterpret_cast<PickState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
            if (!st) return -1;
            st->dpi = DpiOf(hwnd);
            theme::ApplyDarkFrame(hwnd);

            st->hSearch = Mk(hwnd, L"EDIT", L"", ES_AUTOHSCROLL | WS_TABSTOP,
                             IDC_GP_SEARCH);
            st->hList   = Mk(hwnd, L"LISTBOX", L"",
                             LBS_NOTIFY | LBS_HASSTRINGS | LBS_OWNERDRAWFIXED |
                                 WS_VSCROLL | WS_TABSTOP,
                             IDC_GP_LIST);
            st->hOk     = Mk(hwnd, L"BUTTON", L"Select", BS_OWNERDRAW | WS_TABSTOP,
                             IDC_GP_OK);
            st->hCancel = Mk(hwnd, L"BUTTON", L"Cancel", BS_OWNERDRAW | WS_TABSTOP,
                             IDC_GP_CANCEL);
            SetButtonKind(st->hOk, theme::ButtonKind::Primary);
            SetButtonKind(st->hCancel, theme::ButtonKind::Secondary);

            // Something to look at and search IMMEDIATELY. The real scan is on its way.
            st->all = BundledGames();
            PickFill(st);
            PickLayout(st, hwnd);
            SetFocus(st->hSearch);

            // The scan. games.h measures DiscoverGames at tens to hundreds of milliseconds,
            // which is a visible freeze if it runs here. The thread is joined in WM_DESTROY,
            // so `st` cannot die under it.
            st->scan = std::thread([hwnd, st]() {
                std::vector<GameEntry> found;
                found = DiscoverGames();
                st->scanned.swap(found);
                PostMessageW(hwnd, kMsgScanDone, 0, 0);
            });
            return 0;
        }

        case WM_SIZE:
            if (st) { PickLayout(st, hwnd); InvalidateRect(hwnd, nullptr, TRUE); }
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
            if (dc && st) PickPaint(st, hwnd, dc);
            EndPaint(hwnd, &ps);
            if (st) PaintSearchChrome(st, hwnd);
            return 0;
        }

        case WM_MEASUREITEM: {
            MEASUREITEMSTRUCT* mi = reinterpret_cast<MEASUREITEMSTRUCT*>(lp);
            if (!mi || !st) break;
            if (mi->CtlType == ODT_LISTBOX) {
                mi->itemHeight = static_cast<UINT>(Dp(40, st->dpi));
                return TRUE;
            }
            break;
        }

        case WM_DRAWITEM: {
            const DRAWITEMSTRUCT* di = reinterpret_cast<const DRAWITEMSTRUCT*>(lp);
            if (!di || !st) break;
            if (di->CtlType == ODT_BUTTON)
                return theme::DrawButton(di, ButtonKindOf(di->hwndItem), st->dpi);
            if (di->CtlType == ODT_LISTBOX)
                return PickDrawRow(st, di);
            break;
        }

        case WM_CTLCOLORSTATIC:
        case WM_CTLCOLOREDIT:
        case WM_CTLCOLORLISTBOX:
        case WM_CTLCOLORBTN: {
            HBRUSH b = theme::OnCtlColor(msg, reinterpret_cast<HDC>(wp),
                                         reinterpret_cast<HWND>(lp));
            if (b) return reinterpret_cast<LRESULT>(b);
            break;
        }

        case WM_COMMAND: {
            if (!st) break;
            const int id   = LOWORD(wp);
            const int code = HIWORD(wp);
            if (id == IDC_GP_SEARCH && code == EN_CHANGE) {
                st->query = Trim(GetText(st->hSearch));
                PickFill(st);
                const bool empty = st->query.empty();
                if (empty != st->searchEmpty) {
                    st->searchEmpty = empty;
                    PaintSearchChrome(st, hwnd);
                }
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            if (id == IDC_GP_SEARCH && (code == EN_SETFOCUS || code == EN_KILLFOCUS)) {
                st->searchFocused = (code == EN_SETFOCUS);
                PaintSearchChrome(st, hwnd);
                return 0;
            }
            if (id == IDC_GP_LIST && code == LBN_DBLCLK) { PickAccept(st); return 0; }
            if (id == IDC_GP_OK)     { PickAccept(st); return 0; }
            if (id == IDC_GP_CANCEL) { st->result.clear(); st->done = true; return 0; }
            break;
        }

        case WM_CLOSE:
            if (st) { st->result.clear(); st->done = true; }
            return 0;

        case WM_DESTROY:
            // Join BEFORE this window stops existing. This is what makes the worker's writes
            // through `st` safe, and DiscoverGames is bounded so the wait is bounded.
            if (st && st->scan.joinable()) st->scan.join();
            return 0;

        case WM_NCDESTROY:
            // PickState is a STACK object owned by PickGame, not a heap one owned by the
            // window: the caller has to read result/resultName out of it after the modal loop
            // has destroyed the window, which a window-owned allocation would already have
            // freed. Only the back-pointer is cleared here.
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
            break;

        default:
            break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void RegisterPickClass() {
    static bool done = false;
    if (done) return;
    WNDCLASSEXW wc;
    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = PickProc;
    wc.hInstance     = GetModuleHandleW(nullptr);
    wc.hCursor       = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;
    wc.lpszClassName = kPickClass;
    RegisterClassExW(&wc);
    done = true;
}

// Enter accepts and Escape cancels without a default push button: every button here is
// BS_OWNERDRAW, so IsDialogMessageW has no DEFID to dispatch to. Up/Down are forwarded to
// the list while the search box has focus, which is what makes type-then-arrow work.
void RunPickModalLoop(HWND hwnd, HWND owner, PickState* st) {
    if (owner) EnableWindow(owner, FALSE);
    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);
    MSG msg;
    while (!st->done && IsWindow(hwnd)) {
        BOOL got = GetMessageW(&msg, nullptr, 0, 0);
        if (got == 0) { PostQuitMessage(static_cast<int>(msg.wParam)); break; }
        if (got == -1) break;
        if (msg.message == WM_KEYDOWN) {
            if (msg.wParam == VK_RETURN) { PickAccept(st); continue; }
            if (msg.wParam == VK_ESCAPE) { st->result.clear(); st->done = true; continue; }
            if ((msg.wParam == VK_UP || msg.wParam == VK_DOWN) &&
                GetFocus() == st->hSearch && st->hList) {
                const int n = static_cast<int>(st->shown.size());
                if (n > 0) {
                    int cur = static_cast<int>(SendMessageW(st->hList, LB_GETCURSEL, 0, 0));
                    cur += (msg.wParam == VK_DOWN) ? 1 : -1;
                    if (cur < 0) cur = 0;
                    if (cur >= n) cur = n - 1;
                    SendMessageW(st->hList, LB_SETCURSEL, static_cast<WPARAM>(cur), 0);
                }
                continue;
            }
        }
        if (!IsDialogMessageW(hwnd, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    if (owner) EnableWindow(owner, TRUE);
    if (IsWindow(hwnd)) DestroyWindow(hwnd);
    if (owner) SetActiveWindow(owner);
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

std::wstring PickGame(HWND owner, std::wstring* outDisplayName) {
    if (outDisplayName) outDisplayName->clear();
    RegisterPickClass();

    PickState st;
    const int dpi = DpiOf(owner ? owner : GetDesktopWindow());
    st.dpi = dpi;

    RECT want = { 0, 0, Dp(580, dpi), Dp(520, dpi) };
    AdjustWindowRectEx(&want, WS_CAPTION | WS_SYSMENU | WS_SIZEBOX, FALSE,
                       WS_EX_DLGMODALFRAME);
    const int w = want.right - want.left;
    const int h = want.bottom - want.top;
    RECT orc = { 0, 0, 0, 0 };
    if (owner) GetWindowRect(owner, &orc);
    else SystemParametersInfoW(SPI_GETWORKAREA, 0, &orc, 0);
    const int x = orc.left + ((orc.right - orc.left) - w) / 2;
    const int y = orc.top + ((orc.bottom - orc.top) - h) / 2;

    HWND hwnd = CreateWindowExW(WS_EX_DLGMODALFRAME, kPickClass, L"Pick a game",
                                WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_SIZEBOX |
                                    WS_CLIPCHILDREN,
                                x, y, w, h, owner, nullptr,
                                GetModuleHandleW(nullptr), &st);
    if (!hwnd) return std::wstring();

    RunPickModalLoop(hwnd, owner, &st);

    if (outDisplayName) *outDisplayName = st.resultName;
    return st.result;
}

}  // namespace cd
