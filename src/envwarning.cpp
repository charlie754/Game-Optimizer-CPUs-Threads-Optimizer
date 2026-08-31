// Game Optimizer - the startup environment warning window. See envwarning.h for the
// contract; this file is the window only.
//
// VISUALS: modelled on the first-run wizard's page 2, deliberately and not by coincidence -
// same cards, same status dots, same fonts, same dark title bar, same per-monitor DPI
// handling, every colour from theme.h. A user who has seen the wizard is looking at the same
// two facts in the same shape.
//
// MODAL, AND THAT IS THE POINT: this window is a GATE, not a notification. The operator
// tested a modeless build and reported it "hiding behind the main window" - it returned at
// once and the caller's next window took the foreground. ShowEnvironmentWarning now runs its
// own GetMessageW loop, shaped after RunFirstRunWizard in firstrun.cpp, and does not return
// until the window is destroyed. That loop calls IsDialogMessageW itself, so Tab, Enter and
// Escape behave with NO message hook at all: the modeless version had to install a
// thread-local get-message hook for exactly that and nothing else, and going modal deleted
// it rather than patching around it. Nothing in this file hooks the message queue any more,
// which is checkable by grep and deliberately so.
//
// EVERY WRAPPING BODY IS MEASURED, NEVER RESERVED. Both section bodies change with the
// machine - the CPU brand string, which Game Mode branch applies, which V-Cache state was
// found - so a fixed line count is a clipped sentence on some machine nobody tested on.
// MeasureBodyHeight below is the single place a body height is decided, and it asks GDI with
// DT_CALCRECT | DT_WORDBREAK against the control's real width and real font.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <string>

#include "config.h"
#include "envwarning.h"
#include "envwarning_text.h"
#include "firstrun_text.h"
#include "settings_environment.h"
#include "startup_warning.h"
#include "theme.h"
#include "topology.h"
#include "ui.h"
#include "util.h"

namespace cd {

namespace {

const wchar_t kEnvWarnClass[] = L"GameOptimizerEnvWarning";

enum : int {
    IDC_EW_OPENGM = 3101,
    IDC_EW_CLOSE,
    IDC_EW_VCDISABLE
};

// The only string in this window that names a change this app can make to the machine, and
// it says STOP because that is what the button does: it stops amd3dvcacheSvc, which stops the
// per-session agent amd3dvcacheUser.exe that is the actual policy engine. It does NOT touch
// the kernel driver amd3dvcache, which is PnP-loaded onto the ACPI device node, runs whatever
// the service does, and is inert without the agent. A caption saying "disable" would promise
// a machine-wide change this button no longer makes.
const wchar_t kVCacheStopCaption[] = L"Stop AMD 3D V-Cache optimizer";

// EXACTLY ONE OF THESE WINDOWS AT A TIME, AND THAT IS NOT SUPPRESSION. The single call site
// runs once per launch and the window always appears; this only stops a second identical
// window stacking on the first. Since the window became modal the guard can no longer fire -
// the call does not return while a window is up - and it is kept because a guard that cannot
// fire is cheaper than a comment explaining why it was removed. It doubles as the modal
// loop's "this window is gone" flag: WM_NCDESTROY clears it.
HWND g_hEnvWarn = nullptr;

int DpiOf(HWND h) {
    UINT d = h ? GetDpiForWindow(h) : 0;
    if (d == 0) d = GetDpiForSystem();
    if (d == 0) d = 96;
    return static_cast<int>(d);
}

// One text format for every single-line label drawn by this file.
const UINT kLineFmt = DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS;

// theme::GetFont hands back a cached handle that must never be deleted, so this only ever
// assigns - there is no matching teardown and none is wanted.
void SetCtlFont(HWND h, theme::Font f, int dpi) {
    if (!h) return;
    SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(theme::GetFont(f, dpi)), TRUE);
}

// The content rect of a card. Mirrors what theme::DrawCard returns, so layout (which has no
// DC) and painting (which does) agree on where a card's contents start.
RECT CardInner(const RECT& card, int dpi) {
    const int p = theme::Dp(theme::metric::kCardPad, dpi);
    RECT r = card;
    r.left += p;
    r.top += p;
    r.right -= p;
    r.bottom -= p;
    if (r.right < r.left) r.right = r.left;
    if (r.bottom < r.top) r.bottom = r.top;
    return r;
}

HWND Mk(HWND parent, const wchar_t* cls, const wchar_t* text, DWORD style, int id) {
    return CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 10, 10, parent,
                           reinterpret_cast<HMENU>(static_cast<UINT_PTR>(id)),
                           GetModuleHandleW(nullptr), nullptr);
}

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------

struct EnvWarnState {
    StartupWarningDecision decision;
    // decision.gameModeTone == WarningTone::Actionable, cached as a bool because it is what
    // the visual treatment is pointed at. It means "Game Mode is ON and this is a multi-CCD
    // AMD part", which is the only case where Game Mode has a CCD preference to express.
    bool gameModeWarn = false;
    std::wstring gmBody;
    std::wstring vcBody;
    // Copied out of EnvironmentInfo at construction. This state is heap-owned and freed by
    // WM_NCDESTROY rather than by the caller's stack frame, so it copies what it needs out of
    // EnvironmentInfo and the Topology and holds no pointer into either.
    bool vcServicePresent = false;
    bool vcServiceRunning = false;

    // THE ONE BORROW IN THIS STRUCT, and it is sound only because this window is MODAL:
    // ShowEnvironmentWarning does not return while the window exists, so the caller's Config
    // outlives every message this state will ever see. NOTHING IN THIS WINDOW WRITES THROUGH
    // IT ANY MORE: the V-Cache button stops a service and records nothing, so there is no
    // config.ini value for this process to go stale on. The borrow is kept so the window can
    // read the live Config without re-loading it, and any future write has a sound route.
    Config* cfg = nullptr;

    // Whether the V-Cache card carries its stop button. Decided once, before the window is
    // sized, because the button occupies a row that the card's measured height must include.
    bool vcStopAvailable = false;
    // The elevated call blocks this thread without pumping messages, so a second click lands
    // in the queue and is dispatched the moment it returns. The second call would be a stop
    // of an already-stopped service - harmless in itself, but it would fire a second UAC
    // prompt the user never asked for, so the button is disabled before the call rather than
    // after it, and this flag makes the guard independent of how the click arrived - mouse,
    // Space, or Enter on a focused button.
    bool vcDisableBusy = false;

    int dpi = 96;
    HBRUSH bgBrush = nullptr;   // WM_CTLCOLOR* fallback; freed in WM_NCDESTROY

    RECT gmCard = { 0, 0, 0, 0 };
    RECT vcCard = { 0, 0, 0, 0 };

    HWND hGm = nullptr, hVc = nullptr, hOpen = nullptr, hClose = nullptr;
    HWND hVcDisable = nullptr;
};

// A read-only EDIT sends WM_CTLCOLORSTATIC, not WM_CTLCOLOREDIT. Left alone it would be
// painted as a label on the app background instead of as an input surface, so the message is
// re-pointed for exactly these two controls before it reaches theme::OnCtlColor.
bool IsReadOnlyEdit(const EnvWarnState* st, HWND ctl) {
    if (!st || !ctl) return false;
    return ctl == st->hGm || ctl == st->hVc;
}

// ---------------------------------------------------------------------------
// Measurement - the whole reason this window is not a MessageBox with a guessed size
// ---------------------------------------------------------------------------

// A multiline EDIT insets its text by its own margins, so the width the text actually wraps
// at is narrower than the control. Measuring at the narrower width is the safe direction of
// the two: it can only over-estimate the height, never clip a line.
int BodyTextWidth(int controlW, int dpi) {
    const int w = controlW - theme::Dp(8, dpi);
    return w > 1 ? w : 1;
}

// THE ONE PLACE A BODY HEIGHT IS DECIDED. No caller reserves a line count.
//
// `control` is the EDIT that will show the text, and is null only on the first call - the one
// that sizes the window before it exists. When it is present its own font is used, so the
// measurement is against the real control rather than against a font it merely ought to have.
int MeasureBodyHeight(const std::wstring& text, HDC dc, HWND control, int textW, int dpi) {
    const int floorH = theme::Dp(40, dpi);
    if (dc == nullptr || text.empty() || textW <= 0) return floorH;

    HFONT font = control ? reinterpret_cast<HFONT>(SendMessageW(control, WM_GETFONT, 0, 0))
                         : nullptr;
    if (!font) font = theme::GetFont(theme::Font::UiBody, dpi);

    RECT measured = { 0, 0, textW, 0 };
    HGDIOBJ oldFont = SelectObject(dc, font);
    // DT_EDITCONTROL makes DrawTextW break lines the way the multiline EDIT that shows this
    // text breaks them, so the number that comes back is the height the control needs.
    const int wrappedH = ::DrawTextW(dc, text.c_str(), static_cast<int>(text.size()),
                                     &measured,
                                     DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX |
                                         DT_EDITCONTROL);
    SelectObject(dc, oldFont);
    return wrappedH > floorH ? wrappedH : floorH;
}

struct EnvWarnMetrics {
    int gmCardH = 0;
    int vcCardH = 0;
    int contentH = 0;   // the client height the measured content wants, top pad to bottom pad
};

// Used twice with the same arithmetic: once before the window exists, to choose its height,
// and once per layout pass afterwards. One function, so the two can never disagree.
EnvWarnMetrics MeasureContent(EnvWarnState* st, HDC dc, int clientW, int dpi) {
    const int PAD  = theme::Dp(theme::metric::kCardPad, dpi);
    const int GAP  = theme::Dp(theme::metric::kGap, dpi);
    const int GAPT = theme::Dp(theme::metric::kGapTight, dpi);
    const int BH   = theme::Dp(theme::metric::kButtonH, dpi);
    const int headH = theme::Dp(20, dpi);

    int cardW = clientW - 2 * PAD;
    if (cardW < theme::Dp(200, dpi)) cardW = theme::Dp(200, dpi);
    int innerW = cardW - 2 * PAD;
    if (innerW < 1) innerW = 1;
    const int textW = BodyTextWidth(innerW, dpi);

    EnvWarnMetrics m;
    int h = PAD;
    if (st->decision.showGameMode) {
        const int bodyH = MeasureBodyHeight(st->gmBody, dc, st->hGm, textW, dpi);
        // ...plus a button row: this is the only section with an action.
        m.gmCardH = PAD + headH + GAPT + bodyH + GAPT + BH + PAD;
        h += m.gmCardH + GAP;
    }
    if (st->decision.showVCache) {
        const int bodyH = MeasureBodyHeight(st->vcBody, dc, st->hVc, textW, dpi);
        m.vcCardH = PAD + headH + GAPT + bodyH + PAD;
        // ...plus a button row, and ONLY when there is a button to put in it. Reserving the
        // row unconditionally would leave an empty strip at the bottom of this card on every
        // machine whose optimizer agent is not running, which is every machine once anyone
        // has used this button.
        if (st->vcStopAvailable) m.vcCardH += GAPT + BH;
        h += m.vcCardH + GAP;
    }
    h += BH + PAD;   // the footer row that carries Close
    m.contentH = h;
    return m;
}

// ---------------------------------------------------------------------------
// Layout
// ---------------------------------------------------------------------------

void EnvWarnLayout(EnvWarnState* st, HWND hwnd) {
    RECT rc;
    GetClientRect(hwnd, &rc);
    const int d = st->dpi;
    const int cw = rc.right - rc.left;
    const int ch = rc.bottom - rc.top;

    const int PAD  = theme::Dp(theme::metric::kCardPad, d);
    const int GAP  = theme::Dp(theme::metric::kGap, d);
    const int GAPT = theme::Dp(theme::metric::kGapTight, d);
    const int BH   = theme::Dp(theme::metric::kButtonH, d);
    const int BW   = theme::Dp(theme::metric::kButtonW, d);
    const int headH  = theme::Dp(20, d);
    const int minTxt = theme::Dp(40, d);

    HDC dc = GetDC(hwnd);
    EnvWarnMetrics m = MeasureContent(st, dc, cw, d);
    // The one button width here that cannot be a constant. This caption is far longer than
    // "Close", and a fixed logical width would clip it under a larger UI font or a translated
    // string - so it is measured against the font the control actually uses, the way the game
    // prompt sizes its buttons. The fallback is only reached when there is no DC to ask.
    int vcBtnW = theme::Dp(240, d);
    if (dc && st->hVcDisable) {
        const SIZE sz = theme::MeasureText(dc, kVCacheStopCaption, theme::Font::UiBody, d);
        vcBtnW = sz.cx + theme::Dp(28, d);
    }
    if (dc) ReleaseDC(hwnd, dc);

    // The window can be shorter than the measured content wants - a small work area, or the
    // user dragged it smaller. Give the deficit back in proportion rather than letting a card
    // run off the bottom. Each card keeps a floor, and every body is a WS_VSCROLL EDIT, so
    // the text stays reachable however tight it gets.
    const int over = m.contentH - ch;
    if (over > 0) {
        const int gmFloor = st->decision.showGameMode
                                ? PAD + headH + GAPT + minTxt + GAPT + BH + PAD
                                : 0;
        int vcFloor = st->decision.showVCache ? PAD + headH + GAPT + minTxt + PAD : 0;
        // The button row is not compressible: the floor has to reserve it, or squeezing the
        // window would give the deficit back out of a row that still has to be drawn.
        if (st->decision.showVCache && st->vcStopAvailable) vcFloor += GAPT + BH;
        int gmRoom = m.gmCardH - gmFloor;
        int vcRoom = m.vcCardH - vcFloor;
        if (gmRoom < 0) gmRoom = 0;
        if (vcRoom < 0) vcRoom = 0;
        const int room = gmRoom + vcRoom;
        if (room > 0) {
            const int take = over < room ? over : room;
            int fromGm = gmRoom > 0 ? take * gmRoom / room : 0;
            int fromVc = take - fromGm;
            if (fromVc > vcRoom) { fromVc = vcRoom; fromGm = take - fromVc; }
            if (fromGm > gmRoom) fromGm = gmRoom;
            m.gmCardH -= fromGm;
            m.vcCardH -= fromVc;
        }
    }

    int y = PAD;
    if (st->decision.showGameMode) {
        SetRect(&st->gmCard, PAD, y, cw - PAD, y + m.gmCardH);
        const RECT in = CardInner(st->gmCard, d);
        const int bt = in.top + headH + GAPT;
        int bb = in.bottom - GAPT - BH;
        if (bb < bt + minTxt) bb = bt + minTxt;
        MoveWindow(st->hGm, in.left, bt, in.right - in.left, bb - bt, TRUE);
        MoveWindow(st->hOpen, in.left, in.bottom - BH, theme::Dp(210, d), BH, TRUE);
        y = st->gmCard.bottom + GAP;
    } else {
        SetRectEmpty(&st->gmCard);
    }

    if (st->decision.showVCache) {
        SetRect(&st->vcCard, PAD, y, cw - PAD, y + m.vcCardH);
        const RECT in = CardInner(st->vcCard, d);
        const int bt = in.top + headH + GAPT;
        int bb = in.bottom;
        // The body stops above the button row when there is one, exactly as the Game Mode
        // card's body stops above its own.
        if (st->hVcDisable) bb -= GAPT + BH;
        if (bb < bt + minTxt) bb = bt + minTxt;
        MoveWindow(st->hVc, in.left, bt, in.right - in.left, bb - bt, TRUE);
        if (st->hVcDisable) {
            int bw = vcBtnW;
            const int avail = in.right - in.left;
            // A caption wider than the card ellipsises inside the button rather than running
            // off the card edge.
            if (bw > avail) bw = avail;
            MoveWindow(st->hVcDisable, in.left, in.bottom - BH, bw, BH, TRUE);
        }
        y = st->vcCard.bottom + GAP;
    } else {
        SetRectEmpty(&st->vcCard);
    }

    int footerTop = ch - PAD - BH;
    if (footerTop < y) footerTop = y;
    MoveWindow(st->hClose, cw - PAD - BW, footerTop, BW, BH, TRUE);
}

// ---------------------------------------------------------------------------
// Painting
// ---------------------------------------------------------------------------

// One section heading: a status dot and the section's name, in the same shape and with the
// same dot convention the wizard's page 2 uses.
void PaintHeading(HDC dc, const RECT& row, int dpi, COLORREF dot, const std::wstring& label) {
    const theme::Palette& pal = theme::P();
    const int dotR = theme::Dp(4, dpi);
    const int cy = (row.top + row.bottom) / 2;
    theme::DrawStatusDot(dc, row.left + dotR, cy, dotR, dot);

    RECT r = row;
    r.left += 2 * dotR + theme::Dp(8, dpi);
    if (r.left > r.right) r.left = r.right;
    if (r.right > r.left) {
        theme::DrawText(dc, r, label, theme::Font::UiStrong, dpi, pal.textPrimary, kLineFmt);
    }
}

void PaintEnvWarn(EnvWarnState* st, HDC dc) {
    const theme::Palette& pal = theme::P();
    const int d = st->dpi;
    const int headH = theme::Dp(20, d);
    const int GAPT = theme::Dp(theme::metric::kGapTight, d);

    if (st->decision.showGameMode && !IsRectEmpty(&st->gmCard)) {
        theme::DrawCard(dc, st->gmCard, d);
        const RECT in = CardInner(st->gmCard, d);
        RECT head = in;
        head.bottom = head.top + headH;

        // THE OPERATOR'S TONE DECISION MADE VISIBLE. On a machine that is not a multi-CCD AMD
        // part the wizard already says, correctly, that Game Mode has no CCD preference to
        // express there. This window reports the same fact in the same tone rather than
        // contradicting it, so both screens stay true.
        const bool actionable = st->gameModeWarn;
        const std::wstring pillText = actionable ? L"Needs attention" : L"For information";
        const COLORREF pillBg = actionable ? pal.warn : pal.cardBgAlt;
        const COLORREF pillFg = actionable ? pal.appBg : pal.textSecondary;

        const SIZE sz = theme::MeasureText(dc, pillText, theme::Font::UiSmall, d);
        RECT pill = head;
        pill.left = head.right - (sz.cx + 2 * theme::Dp(10, d));
        if (pill.left < head.left) pill.left = head.left;
        theme::DrawPill(dc, pill, pillText, d, pillBg, pillFg);

        RECT label = head;
        label.right = pill.left - GAPT;
        PaintHeading(dc, label, d, actionable ? pal.warn : pal.good, L"Windows Game Mode");
    }

    if (st->decision.showVCache && !IsRectEmpty(&st->vcCard)) {
        theme::DrawCard(dc, st->vcCard, d);
        const RECT in = CardInner(st->vcCard, d);
        RECT head = in;
        head.bottom = head.top + headH;

        // Same dot convention as the wizard: present AND running is the state that actually
        // moves threads, so it is the only one drawn as a warning.
        COLORREF vcDot = pal.good;
        if (!st->vcServicePresent) vcDot = pal.textDim;
        else if (st->vcServiceRunning) vcDot = pal.warn;
        PaintHeading(dc, head, d, vcDot, L"AMD 3D V-Cache");
    }
}

// ---------------------------------------------------------------------------
// The V-Cache action
// ---------------------------------------------------------------------------

// THE ONLY THING IN THIS WINDOW THAT CHANGES THE MACHINE, and it changes it through the
// elevated helper `--vcache-run 0` on this same exe, under the `runas` verb - declared in
// ui.h as LaunchVCacheRunElevated. That child sets amd3dvcacheSvc's start type to Disabled
// and stops it, which stops the per-session agent amd3dvcacheUser.exe immediately.
//
// WHAT IT DELIBERATELY DOES NOT DO: it does not go near amd3dvcache, the kernel driver. The
// button used to call `--vcache-set 1`, which wrote Start=4 to BOTH, so a user who pressed a
// button about the optimizer silently had their kernel driver configured not to load on the
// next boot. That was never what this button offered. The driver is PnP-loaded onto the ACPI
// device node and runs regardless of the service; it is inert without the agent, so stopping
// the agent is the whole of the effect and there is nothing to record for an undo and no
// restart to ask for.
void OnVCacheDisable(EnvWarnState* st, HWND hwnd) {
    if (!st || !st->hVcDisable || st->vcDisableBusy) return;

    // Down before the call, not after it. LaunchVCacheRunElevated blocks on the child without
    // pumping messages, so a second click is queued and would otherwise be dispatched into a
    // second elevation prompt for a service that is already stopped.
    st->vcDisableBusy = true;
    EnableWindow(st->hVcDisable, FALSE);

    DWORD error = ERROR_SUCCESS;
    const bool ok = LaunchVCacheRunElevated(false, error);
    if (!ok) {
        st->vcDisableBusy = false;
        EnableWindow(st->hVcDisable, TRUE);
        SetFocus(st->hVcDisable);
        if (error == ERROR_CANCELLED) {
            // THE USER ANSWERED NO. Nothing was changed, so nothing is said: a message box
            // here would argue with a decision they had just made deliberately, and this
            // window advises. The log keeps the record.
            LogLine(L"[envwarn] AMD V-Cache stop declined at the elevation prompt; "
                    L"nothing changed");
            return;
        }
        LogLine(L"[envwarn] AMD V-Cache stop failed, child/error=%lu", error);
        // Deliberately does NOT claim nothing changed: the child may have set the start type
        // and then failed to stop the running service, and it says which in the log.
        MessageBoxW(hwnd,
                    L"The AMD 3D V-Cache optimizer change did not complete. See the log for "
                    L"details.",
                    L"Game Optimizer", MB_OK | MB_ICONWARNING);
        return;
    }

    // NOTHING IS SHOWN ON SUCCESS, and there is no "Restart required" box any more. Stopping
    // the service stops the agent immediately; there is nothing for a restart to complete and
    // saying otherwise was simply false. The greyed button IS the feedback - it stays visible
    // so the window still shows what was done, rather than quietly losing the control that
    // did it, and it is not re-enabled because the service is now stopped and a second press
    // would buy the user another UAC prompt for no effect.
    LogLine(L"[envwarn] AMD 3D V-Cache optimizer service stopped from the startup warning; "
            L"the kernel driver amd3dvcache was not touched");
    if (GetFocus() == st->hVcDisable || GetFocus() == nullptr) SetFocus(st->hClose);
}

// ---------------------------------------------------------------------------
// Window procedure
// ---------------------------------------------------------------------------

LRESULT CALLBACK EnvWarnProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    EnvWarnState* st =
        reinterpret_cast<EnvWarnState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg) {
        case WM_NCCREATE: {
            CREATESTRUCTW* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                              reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
            break;
        }
        case WM_CREATE: {
            // NEVER RETURNS -1, and that is load-bearing: the creator deletes the state when
            // CreateWindowExW returns null, so a path that both destroyed the window and
            // reported failure would free it twice. A control that could not be created is
            // tolerated instead - MoveWindow and SetCtlFont are both null-safe here.
            st = reinterpret_cast<EnvWarnState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
            if (!st) return 0;
            st->dpi = DpiOf(hwnd);
            theme::ApplyDarkFrame(hwnd);

            // No WS_EX_CLIENTEDGE: the sunken 3D edge is light-theme chrome that cannot be
            // recoloured. The card border draws the boundary instead. WS_VSCROLL is the
            // backstop for a body that still does not fit after the measure.
            const DWORD kRoText =
                ES_MULTILINE | ES_READONLY | WS_VSCROLL | ES_AUTOVSCROLL | WS_TABSTOP;

            if (st->decision.showGameMode) {
                st->hGm = Mk(hwnd, L"EDIT", st->gmBody.c_str(), kRoText, -1);
                st->hOpen = Mk(hwnd, L"BUTTON", L"Open Game Mode settings",
                               BS_OWNERDRAW | WS_TABSTOP, IDC_EW_OPENGM);
            }
            if (st->decision.showVCache) {
                st->hVc = Mk(hwnd, L"EDIT", st->vcBody.c_str(), kRoText, -1);
                // NO BUTTON WHEN THERE IS NOTHING FOR IT TO DO - see vcStopAvailable. A
                // control that would be a no-op still implies something will happen when it
                // is pressed, and this window's whole job is to be honest about the machine.
                if (st->vcStopAvailable) {
                    st->hVcDisable = Mk(hwnd, L"BUTTON", kVCacheStopCaption,
                                        BS_OWNERDRAW | WS_TABSTOP, IDC_EW_VCDISABLE);
                }
            }
            st->hClose = Mk(hwnd, L"BUTTON", L"Close", BS_OWNERDRAW | WS_TABSTOP,
                            IDC_EW_CLOSE);

            SetCtlFont(st->hGm, theme::Font::UiBody, st->dpi);
            SetCtlFont(st->hVc, theme::Font::UiBody, st->dpi);
            SetCtlFont(st->hOpen, theme::Font::UiBody, st->dpi);
            SetCtlFont(st->hVcDisable, theme::Font::UiBody, st->dpi);
            SetCtlFont(st->hClose, theme::Font::UiBody, st->dpi);

            EnvWarnLayout(st, hwnd);
            if (st->hClose) SetFocus(st->hClose);
            return 0;
        }
        case WM_SIZE:
            if (st) {
                EnvWarnLayout(st, hwnd);
                InvalidateRect(hwnd, nullptr, TRUE);
            }
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
            if (dc && st) PaintEnvWarn(st, dc);
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_DRAWITEM: {
            const DRAWITEMSTRUCT* di = reinterpret_cast<const DRAWITEMSTRUCT*>(lp);
            if (di && di->CtlType == ODT_BUTTON) {
                // Close is the primary action. This window advises; it does not demand that
                // anything be changed.
                const theme::ButtonKind kind = di->CtlID == IDC_EW_CLOSE
                                                   ? theme::ButtonKind::Primary
                                                   : theme::ButtonKind::Secondary;
                if (theme::DrawButton(di, kind, st ? st->dpi : DpiOf(hwnd))) return TRUE;
            }
            break;
        }
        case WM_CTLCOLORSTATIC:
        case WM_CTLCOLOREDIT:
        case WM_CTLCOLORLISTBOX:
        case WM_CTLCOLORBTN: {
            HDC dc = reinterpret_cast<HDC>(wp);
            HWND ctl = reinterpret_cast<HWND>(lp);
            UINT m = msg;
            if (msg == WM_CTLCOLORSTATIC && IsReadOnlyEdit(st, ctl)) m = WM_CTLCOLOREDIT;
            HBRUSH b = theme::OnCtlColor(m, dc, ctl);
            if (b) return reinterpret_cast<LRESULT>(b);
            // Unhandled: still never fall back to the system's light brush.
            const theme::Palette& pal = theme::P();
            SetBkMode(dc, TRANSPARENT);
            SetTextColor(dc, pal.textSecondary);
            SetBkColor(dc, pal.appBg);
            if (st) {
                if (!st->bgBrush) st->bgBrush = CreateSolidBrush(pal.appBg);
                if (st->bgBrush) return reinterpret_cast<LRESULT>(st->bgBrush);
            }
            return reinterpret_cast<LRESULT>(GetStockObject(BLACK_BRUSH));
        }
        case WM_DPICHANGED: {
            if (!st) break;
            st->dpi = static_cast<int>(HIWORD(wp));
            SetCtlFont(st->hGm, theme::Font::UiBody, st->dpi);
            SetCtlFont(st->hVc, theme::Font::UiBody, st->dpi);
            SetCtlFont(st->hOpen, theme::Font::UiBody, st->dpi);
            SetCtlFont(st->hVcDisable, theme::Font::UiBody, st->dpi);
            SetCtlFont(st->hClose, theme::Font::UiBody, st->dpi);
            const RECT* nr = reinterpret_cast<const RECT*>(lp);
            SetWindowPos(hwnd, nullptr, nr->left, nr->top, nr->right - nr->left,
                         nr->bottom - nr->top, SWP_NOZORDER | SWP_NOACTIVATE);
            // Re-MEASURED, not re-scaled: the font changed, so the wrap points did too.
            EnvWarnLayout(st, hwnd);
            InvalidateRect(hwnd, nullptr, TRUE);
            return 0;
        }
        case WM_COMMAND: {
            switch (LOWORD(wp)) {
                case IDC_EW_OPENGM:
                    // Advises, never enforces: this opens the Windows page and the user
                    // decides. Nothing here writes the setting.
                    OpenGameModeSettings();
                    return 0;
                case IDC_EW_VCDISABLE:
                    // The user asks for it, Windows asks them again, and the change still
                    // needs their restart. Three deliberate steps, none of them automatic.
                    if (st) OnVCacheDisable(st, hwnd);
                    return 0;
                case IDC_EW_CLOSE:
                case IDOK:
                case IDCANCEL:
                    DestroyWindow(hwnd);
                    return 0;
                default:
                    break;
            }
            break;
        }
        case WM_CLOSE:
            // Destroys this window and nothing else. No PostQuitMessage: the tray, the engine
            // and the host message loop all outlive it.
            DestroyWindow(hwnd);
            return 0;
        case WM_NCDESTROY:
            if (st) {
                if (st->bgBrush) { DeleteObject(st->bgBrush); st->bgBrush = nullptr; }
                delete st;
            }
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
            // Clearing this is what ends the modal loop in ShowEnvironmentWarning. It is a
            // statement about THIS window, which IsWindow alone is not: an HWND value the
            // system has already handed to some other window would still answer TRUE.
            if (g_hEnvWarn == hwnd) g_hEnvWarn = nullptr;
            break;
        default:
            break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void RegisterEnvWarnClass() {
    static bool done = false;
    if (done) return;
    WNDCLASSEXW wc;
    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = EnvWarnProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;   // WM_ERASEBKGND paints appBg; no light flash on show
    wc.lpszClassName = kEnvWarnClass;
    RegisterClassExW(&wc);
    done = true;
}

}  // namespace

void ShowEnvironmentWarning(HWND owner, Config& cfg, const EnvironmentInfo& env,
                            const Topology& topo) {
    const StartupWarningDecision decision = DecideStartupWarning(env, topo);
    // Nothing to report, so nothing is built and NOTHING IS BLOCKED: no window, no window
    // class, no controls, no modal loop. This is the ordinary case on a machine with Game
    // Mode off and no AMD V-Cache optimizer installed, and the caller carries straight on.
    if (!decision.Any()) return;

    if (g_hEnvWarn && IsWindow(g_hEnvWarn)) {
        if (IsIconic(g_hEnvWarn)) ShowWindow(g_hEnvWarn, SW_RESTORE);
        SetForegroundWindow(g_hEnvWarn);
        return;
    }

    RegisterEnvWarnClass();

    EnvWarnState* st = new EnvWarnState();
    st->decision = decision;
    if (decision.showGameMode) {
        // THE TONE COMES FROM DecideStartupWarning, NOT FROM THE WIZARD'S TEXT.
        // Page2GameModeText is no longer called here at all - it does return a warn flag,
        // but harvesting a flag out of a function whose wording this window has stopped
        // using is a seam waiting to rot. gameModeTone is what startup_warning.h owns and
        // what group N already pins.
        st->gameModeWarn = decision.gameModeTone == WarningTone::Actionable;
        // The CPU name is composed HERE and deliberately not inside PopupGameModeText, so
        // the two wordings in envwarning_text.h stay matchable character for character in
        // a test. Omitted entirely when the brand is unknown: the wizard prints "(not
        // reported by the registry)" because a wizard page explains itself, and a window
        // that opened by itself should not spend a line apologising for a string it could
        // not read.
        if (!env.cpuBrand.empty())
            st->gmBody = L"CPU: " + env.cpuBrand + L"\r\n\r\n";
        st->gmBody += PopupGameModeText(st->gameModeWarn);
    }
    // THE SECOND ARGUMENT IS decision.showGameMode AND NOTHING ELSE. It is what adds the
    // lead-in "Separately, and regardless of the Game Mode setting above" - a sentence that is
    // only true when a Game Mode section really is above this one. A literal true here would
    // print it on a window that has nothing above it.
    if (decision.showVCache) {
        st->vcBody = Page2VCacheText(env, decision.showGameMode);
    }
    st->vcServicePresent = env.amdVCacheServicePresent;
    st->vcServiceRunning = env.amdVCacheServiceRunning;

    // BORROWED FOR THE LIFETIME OF A MODAL WINDOW, which is this call. See EnvWarnState::cfg.
    st->cfg = &cfg;

    // WHETHER THE STOP BUTTON EXISTS AT ALL, decided here because the measurement below needs
    // it and the answer cannot change while the window is up: the agent is only stopped by the
    // elevated child this window launches, and after that the button is disabled anyway.
    //
    // THE QUESTION IS "IS THERE ANYTHING RUNNING TO STOP", NOT "IS A REGISTRY VALUE SET".
    // amd3dvcacheUser.exe is the per-session agent that carries the policy; the service exists
    // only to launch it. The kernel driver's Start value is deliberately NOT consulted: it is
    // PnP-loaded onto the ACPI device node and runs whether or not the service does - measured
    // live with the service Disabled and Stopped and the driver still Running - so its state
    // is noise here, and it is not something this button changes.
    if (decision.showVCache) {
        st->vcStopAvailable = env.amdVCacheAgentRunning;
    }

    const int dpi = DpiOf(owner ? owner : GetDesktopWindow());
    st->dpi = dpi;

    // The height comes from the text, through the same MeasureContent the layout uses. A
    // guessed height would be wrong on the first machine whose CPU brand string wraps.
    const int clientW = theme::Dp(620, dpi);
    int clientH = theme::Dp(200, dpi);
    {
        HDC dc = GetDC(nullptr);
        if (dc) {
            clientH = MeasureContent(st, dc, clientW, dpi).contentH;
            ReleaseDC(nullptr, dc);
        }
    }

    RECT want = { 0, 0, clientW, clientH };
    AdjustWindowRectEx(&want, WS_OVERLAPPEDWINDOW, FALSE, WS_EX_APPWINDOW);
    int w = want.right - want.left;
    int h = want.bottom - want.top;
    RECT work = { 0, 0, 0, 0 };
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    const int availW = work.right - work.left;
    const int availH = work.bottom - work.top;
    if (availW > 0 && w > availW) w = availW;
    if (availH > 0 && h > availH) h = availH;
    int x = work.left + (availW - w) / 2;
    int y = work.top + (availH - h) / 2;
    if (x < work.left) x = work.left;
    if (y < work.top) y = work.top;

    // WS_EX_APPWINDOW because an owned window is otherwise excluded from the taskbar and
    // Alt+Tab, and on the --tray autostart path this is the only visible window the app has:
    // a user who clicked away from it would have no route back.
    HWND hwnd = CreateWindowExW(WS_EX_APPWINDOW, kEnvWarnClass,
                                L"Game Optimizer - Environment",
                                WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, x, y, w, h, owner,
                                nullptr, GetModuleHandleW(nullptr), st);
    if (!hwnd) {
        // The window never reached a procedure that could adopt the state, so this is its
        // only owner. See the WM_CREATE comment about never returning -1.
        LogLine(L"[envwarn] the environment warning window could not be created, gle=%lu",
                GetLastError());
        delete st;
        return;
    }

    g_hEnvWarn = hwnd;

    // ---- THE GATE ----------------------------------------------------------------------
    // Shaped after RunFirstRunWizard in firstrun.cpp, deliberately and not by coincidence:
    // one modal idiom in this codebase, not two. Disabling the owner is what makes the window
    // modal; running our own loop is what makes it a GATE, because the caller's next line
    // cannot run until this window has been destroyed.
    if (owner) EnableWindow(owner, FALSE);
    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);
    SetForegroundWindow(hwnd);

    MSG msg;
    while (g_hEnvWarn == hwnd && IsWindow(hwnd)) {
        BOOL got = GetMessageW(&msg, nullptr, 0, 0);
        // GETMESSAGEW RETURNING 0 IS WM_QUIT, AND IT IS NOT OURS TO SWALLOW. On this path the
        // host loop in main.cpp has not started yet, so a shutdown asked for while this
        // window is up would be lost outright. Re-post it and leave, exactly as the wizard
        // does; the host loop then sees it and shuts down in its usual order.
        if (got == 0) { PostQuitMessage(static_cast<int>(msg.wParam)); break; }
        if (got == -1) break;   // a bad hwnd would otherwise spin here forever
        // The call the modeless version could not make, because the host loop never made it
        // on this window's behalf. It is the whole reason the message hook is gone.
        if (!IsDialogMessageW(hwnd, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }

    // EVERY EXIT FROM THE LOOP LANDS HERE - ordinary close, WM_QUIT, and GetMessageW failure
    // alike - and re-enabling the owner is the one thing that must never be skipped. That
    // owner is the app's own message window, the one that owns the tray icon and receives the
    // engine's status posts; leaving it disabled would freeze the product, not just a dialog.
    if (owner) EnableWindow(owner, TRUE);
    if (IsWindow(hwnd)) DestroyWindow(hwnd);
    if (owner) SetActiveWindow(owner);
}

}  // namespace cd
