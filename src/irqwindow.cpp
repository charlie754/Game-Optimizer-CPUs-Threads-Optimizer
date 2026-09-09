// Game Optimizer - the interrupt and DPC bench window. See ui.h for the contract and
// irq_policy.h for every sentence this window can put on screen.
//
// READ-ONLY, ALL OF IT. There is no Apply button because there is nothing to apply: this
// window samples two per-processor counters and reads three registry subkeys per PCI device,
// and that is the whole of it. No elevation is asked for, no value is written, and no device
// is restarted.
//
// MODAL, and hand-rolled, like envwarning.cpp and firstrun.cpp before it. settings.cpp has a
// RunModalLoop but it is file-local inside that file's anonymous namespace with three callers
// all in that translation unit and external linkage nowhere, so it cannot be reached from
// here. A third hand-rolled loop is house style; exporting a function out of settings.cpp's
// anonymous namespace would not be. The shape is copied from envwarning.cpp: disable the
// owner, GetMessageW, re-post WM_QUIT rather than swallow it, IsDialogMessageW, and
// unconditionally re-enable the owner on every exit from the loop.
//
// Mk and DpiOf are file-local here, as they already are in settings.cpp, envwarning.cpp,
// firstrun.cpp and gameprompt.cpp. Per-translation-unit duplication of those two is the
// established pattern in this tree, not an oversight in this file.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <algorithm>
#include <string>
#include <vector>

#include "config.h"
#include "engine.h"
#include "irq_policy.h"
#include "irq_probe.h"
#include "theme.h"
#include "topology.h"
#include "ui.h"
#include "util.h"

namespace cd {

namespace {

const wchar_t kIrqClass[] = L"GameOptimizerIrq";

enum : int {
    IDC_IRQ_CORES = 3201,
    IDC_IRQ_DEVICES,
    IDC_IRQ_AGAIN,
    IDC_IRQ_COPY,
    IDC_IRQ_CLOSE
};

// Doubles as the modal loop's "this window is gone" flag: WM_NCDESTROY clears it. IsWindow
// alone cannot say that - an HWND value the system has already handed to some other window
// would still answer TRUE.
HWND g_hIrq = nullptr;

const UINT_PTR kSampleTimer = 1;
const UINT     kSampleMs    = 1000;

int DpiOf(HWND h) {
    UINT d = h ? GetDpiForWindow(h) : 0;
    if (d == 0) d = GetDpiForSystem();
    if (d == 0) d = 96;
    return static_cast<int>(d);
}

const UINT kLineFmt = DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS;

void SetCtlFont(HWND h, theme::Font f, int dpi) {
    if (!h) return;
    SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(theme::GetFont(f, dpi)), TRUE);
}

HWND Mk(HWND parent, const wchar_t* cls, const wchar_t* text, DWORD style, int id) {
    return CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 10, 10, parent,
                           reinterpret_cast<HMENU>(static_cast<UINT_PTR>(id)),
                           GetModuleHandleW(nullptr), nullptr);
}

std::wstring GetText(HWND h) {
    if (!h) return std::wstring();
    const int n = GetWindowTextLengthW(h);
    if (n <= 0) return std::wstring();
    std::wstring s((size_t)n, L'\0');
    GetWindowTextW(h, &s[0], n + 1);
    return s;
}

RECT CardInner(const RECT& card, int dpi) {
    const int p = theme::Dp(theme::metric::kCardPad, dpi);
    RECT r = card;
    r.left += p; r.top += p; r.right -= p; r.bottom -= p;
    if (r.right < r.left) r.right = r.left;
    if (r.bottom < r.top) r.bottom = r.top;
    return r;
}

// EVERY WRAPPING BODY IS MEASURED, NEVER RESERVED - the rule envwarning.cpp records and the
// reason it does not use a MessageBox. The sentences on this page carry machine-dependent
// numbers and a mask name that is "Cache" on one part and "CCD0" on another, so a fixed line
// count is a clipped sentence on some machine nobody tested on.
int MeasureWrapped(HWND control, HDC dc, const std::wstring& text, int textW, int dpi,
                   theme::Font fallback, UINT extraFlags) {
    const int floorH = theme::Dp(18, dpi);
    if (!dc || text.empty() || textW <= 0) return floorH;
    HFONT font = control ? reinterpret_cast<HFONT>(SendMessageW(control, WM_GETFONT, 0, 0))
                         : nullptr;
    if (!font) font = theme::GetFont(fallback, dpi);
    RECT measured = { 0, 0, textW, 0 };
    HGDIOBJ oldFont = SelectObject(dc, font);
    const int h = ::DrawTextW(dc, text.c_str(), (int)text.size(), &measured,
                              DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX | extraFlags);
    SelectObject(dc, oldFont);
    return h > floorH ? h : floorH;
}

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------

struct IrqState {
    int dpi = 96;
    const Topology* topo = nullptr;   // BORROWED, and sound only because this window is modal

    IrqRefusal   refusal = IrqRefusal::None;
    std::wstring counterError;        // the sampler could not be opened at all
    std::wstring deviceError;

    IrqSampler* sampler = nullptr;
    bool capturing = false;
    std::vector<CoreDpcStat> cores;   // ascending by lp; only measured processors appear
    std::vector<ULONG> allLps;        // every logical processor, so "not measured" can show

    std::vector<IrqDevice> devices;

    GameGroupSource groupSource = GameGroupSource::Unknown;
    std::wstring    groupMaskName;
    std::vector<ULONG> groupLps;
    ULONG_PTR groupMask = 0;
    bool      groupMaskValid = false;

    HWND hIntro = nullptr, hBanner = nullptr, hGroup = nullptr, hVerdict = nullptr;
    HWND hRepro = nullptr, hCores = nullptr, hDevHdr = nullptr, hDevices = nullptr;
    HWND hDevDetail = nullptr;
    HWND hAgain = nullptr, hCopy = nullptr, hClose = nullptr;

    RECT introCard = { 0, 0, 0, 0 };
    RECT coreCard = { 0, 0, 0, 0 };
    RECT devCard = { 0, 0, 0, 0 };

    // Owned, created lazily, deleted in WM_NCDESTROY. The two row brushes are CACHED rather
    // than made per row: WM_DRAWITEM runs once per logical processor per capture tick, and a
    // CreateSolidBrush/DeleteObject pair in that path is GDI churn for no gain.
    HBRUSH cardBrush = nullptr;
    HBRUSH rowBrush = nullptr;
    HBRUSH rowAltBrush = nullptr;
};

// The row surface. inputBg is what theme::OnCtlColor gives the LISTBOX itself, so an ordinary
// row must erase to the same colour or the strip under the last row is a different shade;
// cardBgAlt is the raised surface used for a row this page is drawing attention to.
HBRUSH RowBrush(IrqState* st, bool raised) {
    const theme::Palette& pal = theme::P();
    HBRUSH& slot = raised ? st->rowAltBrush : st->rowBrush;
    if (!slot) slot = CreateSolidBrush(raised ? pal.cardBgAlt : pal.inputBg);
    return slot;
}

// The stat for one logical processor, or nullptr when it contributed no sample at all.
const CoreDpcStat* StatFor(const IrqState* st, ULONG lp) {
    for (size_t i = 0; i < st->cores.size(); ++i)
        if (st->cores[i].lp == lp) return &st->cores[i];
    return nullptr;
}

// ---------------------------------------------------------------------------
// The sentences, assembled from the pure layer
// ---------------------------------------------------------------------------

std::wstring GroupSentence(const IrqState* st) {
    if (!st->groupMaskValid) {
        return L"This page could not work out which processors your game's group holds, so it "
               L"cannot say whether anything below is inside it.";
    }
    return IrqGameGroupSentence(st->groupSource, st->groupMaskName, st->groupMask);
}

// THE ONE SENTENCE THIS PAGE EXISTS TO PRODUCE. Every branch of it is a different claim and
// none of them is allowed to be silent: a page that says nothing when it measured nothing
// reads as a page that measured quiet.
std::wstring VerdictSentence(const IrqState* st) {
    if (!st->counterError.empty()) return st->counterError;
    if (st->cores.empty()) return IrqNotStartedText();

    const int inGroup = IrqHottestInGroupIndex(st->cores);
    if (inGroup >= 0) {
        const CoreDpcStat& c = st->cores[(size_t)inGroup];
        if (IrqCoreIsHot(c)) {
            // M IS THE GROUP'S OWN SIZE, counted from allLps against groupLps. Counting
            // it from st->cores would drop every group processor PDH never returned, and the
            // sentence would then name a smaller group than the mask printed above it.
            return IrqHotCoreSentence(c, IrqQuietCoresInGroup(st->cores),
                                      IrqCoresInGroup(st->allLps, st->groupLps),
                                      st->groupSource, st->groupMaskName);
        }
        // Nothing in the group is hot. Say so, and then say whether something OUTSIDE it is,
        // because "your group is quiet" and "this machine is quiet" are different facts.
        std::wstring s = IrqNoHotCoreSentence(c);
        const int overall = IrqHottestOverallIndex(st->cores);
        if (overall >= 0 && !st->cores[(size_t)overall].inGameGroup &&
            IrqCoreIsHot(st->cores[(size_t)overall])) {
            s += L" ";
            s += IrqHotCoreOutsideGroupSentence(st->cores[(size_t)overall], st->groupSource,
                                                st->groupMaskName);
        }
        return s;
    }

    // No processor in the group returned enough samples. That is not the same as quiet.
    std::wstring s = IrqNoGroupMeasuredSentence(st->groupSource, st->groupMaskName);
    const int overall = IrqHottestOverallIndex(st->cores);
    if (overall >= 0 && IrqCoreIsHot(st->cores[(size_t)overall])) {
        s += L" ";
        s += IrqHotCoreOutsideGroupSentence(st->cores[(size_t)overall], st->groupSource,
                                            st->groupMaskName);
    }
    return s;
}

// The static policy column for one device, as a state and a list. THE DECISION IS NOT TAKEN
// HERE: it is IrqPolicyStateOf in the pure half, where a test can reach it and where the
// column, the detail sentence and the agreement classifier all read the same answer. This
// used to test d.overrideDecoded directly, so a policy that is present and undecodable
// reported as "Windows chooses" - the unread-as-absent fold the header forbids.
IrqPolicyState PolicyStateOf(const IrqDevice& d) { return IrqPolicyStateOf(d.Readout()); }

std::wstring PolicyColumn(const IrqDevice& d) {
    switch (PolicyStateOf(d)) {
        case IrqPolicyState::Unreadable:    return IrqUnreadableColumnText();
        case IrqPolicyState::Configured:    return FormatCpuList(d.overrideMask);
        case IrqPolicyState::NotConfigured: return IrqWindowsChoosesText();
    }
    // A state added later without a column reads as unread, never as "Windows chooses".
    return IrqUnreadableColumnText();
}

std::wstring TargetColumn(const IrqDevice& d) {
    if (d.keyReadFailed) return IrqUnreadableColumnText();
    if (d.targetSetDecoded) return FormatCpuList(d.targetSetMask);
    return IrqNotRecordedText();
}

std::wstring LimitColumn(const IrqDevice& d) {
    if (d.keyReadFailed) return IrqUnreadableColumnText();
    if (!d.messageNumberLimitPresent) return IrqNotSetText();
    return std::to_wstring(d.messageNumberLimit);
}

std::wstring DeviceDetail(const IrqDevice& d) {
    const IrqPolicyReadout r = d.Readout();
    std::wstring s = d.friendlyName.empty() ? d.instanceId : d.friendlyName;
    s += L"\r\n";
    s += IrqPolicyStateText(PolicyStateOf(d), FormatCpuListWithCount(d.overrideMask));
    s += L" ";
    s += IrqAgreementText(IrqClassifyAgreement(r), r);
    if (d.hasProblemCode) {
        s += L" Windows reports a problem with this device (code " +
             std::to_wstring(d.problemCode) + L"), so its readings may not describe a device "
             L"that is running.";
    }
    return s;
}

// ---------------------------------------------------------------------------
// The clipboard report - the same numbers, in a form that can be pasted into a bug report
// ---------------------------------------------------------------------------

std::wstring BuildReport(const IrqState* st) {
    std::wstring s;
    s += L"Interrupt and DPC readout\r\n";
    s += L"=========================\r\n\r\n";
    if (IrqRefusalIsHard(st->refusal)) {
        s += IrqRefusalReason(st->refusal);
        s += L"\r\n";
        return s;
    }
    s += GroupSentence(st);
    s += L"\r\n";
    s += VerdictSentence(st);
    s += L"\r\n\r\n";
    s += IrqReproductionCommand();
    s += L"\r\n\r\n";
    for (size_t i = 0; i < st->allLps.size(); ++i) {
        const ULONG lp = st->allLps[i];
        const CoreDpcStat* c = StatFor(st, lp);
        // MEMBERSHIP COMES FROM THE GROUP'S LIST, not from whether a sample arrived, so an
        // unmeasured processor in the game's group is still reported as being in it.
        const bool inGroup = IrqLpIsInGameGroup(lp, st->groupLps);
        if (!c || !IrqCoreIsMeasured(*c)) {
            s += IrqNotMeasuredSentence(lp);
            if (inGroup) s += L"\tin the game's group";
            s += L"\r\n";
            continue;
        }
        s += L"CPU " + std::to_wstring(lp) + L"\tISR " + IrqFormatPct(c->meanIsrPct) +
             L"%\tDPC " + IrqFormatPct(c->meanDpcPct) + L"%\tDPC range " +
             IrqFormatPct(c->minDpcPct) + L"-" + IrqFormatPct(c->maxDpcPct) + L"%\t" +
             std::to_wstring(c->samples) + L" samples";
        if (inGroup) s += L"\tin the game's group";
        s += L"\r\n";
    }
    s += L"\r\n";
    s += IrqDeviceHeadingText();
    s += L"\r\n";
    if (st->devices.empty()) {
        s += st->deviceError.empty() ? IrqNoDevicesText() : st->deviceError.c_str();
        s += L"\r\n";
        return s;
    }
    for (size_t i = 0; i < st->devices.size(); ++i) {
        const IrqDevice& d = st->devices[i];
        s += (d.friendlyName.empty() ? d.instanceId : d.friendlyName);
        s += L"\r\n\t" + d.instanceId;
        s += L"\r\n\tMessageNumberLimit: " + LimitColumn(d);
        s += L"\r\n\tAffinity Policy: " + PolicyColumn(d);
        if (d.devicePolicyPresent)
            s += L" (DevicePolicy " + std::to_wstring(d.devicePolicy) + L")";
        s += L"\r\n\tAffinity Policy - Temporal: " + TargetColumn(d);
        if (d.targetGroupPresent)
            s += L" (TargetGroup " + std::to_wstring(d.targetGroup) + L")";
        s += L"\r\n";
    }
    return s;
}

// One place, so the failure paths cannot each invent their own. Returns false without
// touching the clipboard when any step fails; the caller says so rather than claiming a copy.
bool CopyToClipboard(HWND owner, const std::wstring& text) {
    if (text.empty()) return false;
    if (!OpenClipboard(owner)) return false;
    bool ok = false;
    if (EmptyClipboard()) {
        const size_t bytes = (text.size() + 1) * sizeof(wchar_t);
        HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, bytes);
        if (h) {
            void* p = GlobalLock(h);
            if (p) {
                memcpy(p, text.c_str(), bytes);
                GlobalUnlock(h);
                if (SetClipboardData(CF_UNICODETEXT, h)) {
                    ok = true;   // the clipboard owns the block from here
                } else {
                    GlobalFree(h);
                }
            } else {
                GlobalFree(h);
            }
        }
    }
    CloseClipboard();
    return ok;
}

// ---------------------------------------------------------------------------
// Layout
// ---------------------------------------------------------------------------

void IrqLayout(IrqState* st, HWND hwnd) {
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
    const int headH = theme::Dp(20, d);

    int cardW = cw - 2 * PAD;
    if (cardW < theme::Dp(320, d)) cardW = theme::Dp(320, d);
    int innerW = cardW - 2 * PAD;
    if (innerW < 1) innerW = 1;

    HDC dc = GetDC(hwnd);

    const int introH =
        MeasureWrapped(st->hIntro, dc, GetText(st->hIntro), innerW - theme::Dp(8, d), d,
                       theme::Font::UiBody, DT_EDITCONTROL);
    const bool hasBanner = st->hBanner && GetWindowTextLengthW(st->hBanner) > 0;
    const int bannerH = hasBanner
        ? MeasureWrapped(st->hBanner, dc, GetText(st->hBanner), innerW, d,
                         theme::Font::UiBody, 0)
        : 0;
    const int groupH = MeasureWrapped(st->hGroup, dc, GetText(st->hGroup), innerW, d,
                                      theme::Font::UiSmall, 0);
    const int verdictH = MeasureWrapped(st->hVerdict, dc, GetText(st->hVerdict), innerW, d,
                                        theme::Font::UiBody, 0);
    const int reproH = MeasureWrapped(st->hRepro, dc, GetText(st->hRepro), innerW, d,
                                      theme::Font::UiSmall, 0);
    const int devHdrH = MeasureWrapped(st->hDevHdr, dc, GetText(st->hDevHdr), innerW, d,
                                       theme::Font::UiSmall, 0);
    const int devDetailH = MeasureWrapped(st->hDevDetail, dc, GetText(st->hDevDetail), innerW,
                                          d, theme::Font::UiSmall, 0);
    if (dc) ReleaseDC(hwnd, dc);

    const int introCardH = 2 * PAD + introH + (hasBanner ? GAPT + bannerH : 0);
    const int coreFixed = 2 * PAD + headH + GAPT + groupH + GAPT + verdictH + GAPT + reproH +
                          GAPT;
    const int devFixed = 2 * PAD + headH + GAPT + devHdrH + GAPT + GAPT + devDetailH;

    // THE TWO LISTS ARE THE ELASTIC PARTS. Everything above is measured text that must not be
    // clipped, so the height left over after the measured content is what the lists share -
    // never the other way round.
    const int minCoreList = theme::Dp(96, d);
    const int minDevList  = theme::Dp(80, d);
    int spare = ch - PAD - introCardH - GAP - coreFixed - GAP - devFixed - GAP - BH - PAD;
    int coreListH = minCoreList;
    int devListH = minDevList;
    if (spare > minCoreList + minDevList) {
        // Two thirds to the processors, one third to the devices: the processor list has one
        // row per logical processor and is the one that overflows first.
        coreListH = (spare * 2) / 3;
        devListH = spare - coreListH;
    }

    int y = PAD;
    SetRect(&st->introCard, PAD, y, PAD + cardW, y + introCardH);
    {
        RECT in = CardInner(st->introCard, d);
        int iy = in.top;
        MoveWindow(st->hIntro, in.left, iy, innerW, introH, TRUE);
        iy += introH;
        if (hasBanner) {
            iy += GAPT;
            MoveWindow(st->hBanner, in.left, iy, innerW, bannerH, TRUE);
        }
        // Hidden rather than left at a stale position with no text: an empty static still
        // erases its rectangle, which would punch a hole in the card.
        ShowWindow(st->hBanner, hasBanner ? SW_SHOW : SW_HIDE);
    }
    y = st->introCard.bottom + GAP;

    SetRect(&st->coreCard, PAD, y, PAD + cardW, y + coreFixed + coreListH);
    {
        RECT in = CardInner(st->coreCard, d);
        int iy = in.top + headH + GAPT;
        MoveWindow(st->hGroup, in.left, iy, innerW, groupH, TRUE);
        iy += groupH + GAPT;
        MoveWindow(st->hVerdict, in.left, iy, innerW, verdictH, TRUE);
        iy += verdictH + GAPT;
        MoveWindow(st->hCores, in.left, iy, innerW, coreListH, TRUE);
        iy += coreListH + GAPT;
        MoveWindow(st->hRepro, in.left, iy, innerW, reproH, TRUE);
    }
    y = st->coreCard.bottom + GAP;

    SetRect(&st->devCard, PAD, y, PAD + cardW, y + devFixed + devListH);
    {
        RECT in = CardInner(st->devCard, d);
        int iy = in.top + headH + GAPT;
        MoveWindow(st->hDevHdr, in.left, iy, innerW, devHdrH, TRUE);
        iy += devHdrH + GAPT;
        MoveWindow(st->hDevices, in.left, iy, innerW, devListH, TRUE);
        iy += devListH + GAPT;
        MoveWindow(st->hDevDetail, in.left, iy, innerW, devDetailH, TRUE);
    }

    const int fy = ch - PAD - BH;
    int fx = cw - PAD - BW;
    MoveWindow(st->hClose, fx, fy, BW, BH, TRUE);
    const int wideBw = theme::Dp(150, d);
    fx -= GAPT + wideBw;
    MoveWindow(st->hCopy, fx, fy, wideBw, BH, TRUE);
    fx -= GAPT + wideBw;
    MoveWindow(st->hAgain, fx, fy, wideBw, BH, TRUE);
}

// ---------------------------------------------------------------------------
// Painting
// ---------------------------------------------------------------------------

void PaintHeading(HDC dc, const RECT& row, int dpi, COLORREF dot, const std::wstring& label) {
    const theme::Palette& pal = theme::P();
    const int dotR = theme::Dp(4, dpi);
    const int cy = (row.top + row.bottom) / 2;
    theme::DrawStatusDot(dc, row.left + dotR, cy, dotR, dot);
    RECT r = row;
    r.left += 2 * dotR + theme::Dp(8, dpi);
    if (r.left > r.right) r.left = r.right;
    if (r.right > r.left)
        theme::DrawText(dc, r, label, theme::Font::UiStrong, dpi, pal.textPrimary, kLineFmt);
}

void PaintIrq(IrqState* st, HDC dc) {
    const theme::Palette& pal = theme::P();
    const int d = st->dpi;
    const int headH = theme::Dp(20, d);

    if (!IsRectEmpty(&st->introCard)) theme::DrawCard(dc, st->introCard, d);

    if (!IsRectEmpty(&st->coreCard)) {
        theme::DrawCard(dc, st->coreCard, d);
        RECT head = CardInner(st->coreCard, d);
        head.bottom = head.top + headH;
        // The dot is the warning, and it is warn ONLY for the case this page exists for: a
        // loaded processor inside the group the game was put on. Everything else is neutral.
        const bool hot = IrqHotCoreIsInGameGroup(st->cores);
        const COLORREF dot = !st->counterError.empty() ? pal.danger
                                                       : (hot ? pal.warn : pal.good);
        PaintHeading(dc, head, d, dot, L"Interrupt and DPC time, per processor");
    }

    if (!IsRectEmpty(&st->devCard)) {
        theme::DrawCard(dc, st->devCard, d);
        RECT head = CardInner(st->devCard, d);
        head.bottom = head.top + headH;
        PaintHeading(dc, head, d, pal.textDim, L"PCI devices");
    }
}

// One text run inside a row, clipped to its own column so a long device name cannot push the
// numbers off the right-hand side.
void DrawRun(HDC dc, RECT row, int x, int w, const std::wstring& s, theme::Font f, int dpi,
             COLORREF col) {
    if (w <= 0 || s.empty()) return;
    RECT r = row;
    r.left = x;
    r.right = x + w;
    if (r.right > row.right) r.right = row.right;
    if (r.left >= r.right) return;
    theme::DrawText(dc, r, s, f, dpi, col, kLineFmt);
}

void DrawCoreRow(IrqState* st, const DRAWITEMSTRUCT* di) {
    const theme::Palette& pal = theme::P();
    const int d = st->dpi;
    if (di->itemID == (UINT)-1) return;
    if ((size_t)di->itemID >= st->allLps.size()) return;
    const ULONG lp = st->allLps[di->itemID];
    const CoreDpcStat* c = StatFor(st, lp);

    RECT row = di->rcItem;
    // FROM THE GROUP'S OWN LIST, NEVER FROM StatFor. A processor PDH did not return has no
    // CoreDpcStat at all, and asking the stat painted every unmeasured member of the game's
    // group as though it were outside the group. [M] Council review 2026-09-08, C3/H8.
    const bool inGroup = IrqLpIsInGameGroup(lp, st->groupLps);
    // The group's own processors get the raised surface, so which half of the machine the
    // game is on is visible without reading a word.
    HBRUSH bg = RowBrush(st, inGroup);
    if (bg) FillRect(di->hDC, &row, bg);

    const int pad = theme::Dp(6, d);
    const int w = row.right - row.left - 2 * pad;
    if (w <= 0) return;
    const int x0 = row.left + pad;
    const int cName = theme::Dp(84, d);
    const int cIsr  = theme::Dp(110, d);
    const int cDpc  = theme::Dp(110, d);
    const int cRange = theme::Dp(170, d);

    const bool measured = c && IrqCoreIsMeasured(*c);
    const bool hot = c && IrqCoreIsHot(*c);
    const COLORREF valueCol = hot ? (inGroup ? pal.warn : pal.textPrimary) : pal.textSecondary;
    const int used = cName + cIsr + cDpc + cRange;

    DrawRun(di->hDC, row, x0, cName, L"CPU " + std::to_wstring(lp), theme::Font::MonoSmall, d,
            pal.textPrimary);
    if (!measured) {
        // NEVER 0.00% FOR A COUNTER THAT WAS NOT READ. This row is the whole reason the
        // sample count is carried around rather than a mean alone. It still carries the group
        // label: not measured and not in the group are different facts about a processor.
        DrawRun(di->hDC, row, x0 + cName, cIsr + cDpc + cRange, L"not measured",
                theme::Font::UiSmall, d, pal.textDim);
        if (inGroup && w > used) {
            DrawRun(di->hDC, row, x0 + used, w - used, L"in the game's group",
                    theme::Font::UiSmall, d, pal.textSecondary);
        }
        return;
    }
    DrawRun(di->hDC, row, x0 + cName, cIsr, L"ISR " + IrqFormatPct(c->meanIsrPct) + L"%",
            theme::Font::MonoSmall, d, valueCol);
    DrawRun(di->hDC, row, x0 + cName + cIsr, cDpc,
            L"DPC " + IrqFormatPct(c->meanDpcPct) + L"%", theme::Font::MonoSmall, d, valueCol);
    DrawRun(di->hDC, row, x0 + cName + cIsr + cDpc, cRange,
            IrqFormatPct(c->minDpcPct) + L"-" + IrqFormatPct(c->maxDpcPct) + L"% / " +
                std::to_wstring(c->samples) + L"n",
            theme::Font::MonoSmall, d, pal.textDim);
    if (inGroup && w > used) {
        DrawRun(di->hDC, row, x0 + used, w - used, L"in the game's group",
                theme::Font::UiSmall, d, pal.textSecondary);
    }
}

void DrawDeviceRow(IrqState* st, const DRAWITEMSTRUCT* di) {
    const theme::Palette& pal = theme::P();
    const int d = st->dpi;
    if (di->itemID == (UINT)-1) return;
    if ((size_t)di->itemID >= st->devices.size()) return;
    const IrqDevice& dev = st->devices[di->itemID];

    RECT row = di->rcItem;
    const bool sel = (di->itemState & ODS_SELECTED) != 0;
    HBRUSH bg = RowBrush(st, sel);
    if (bg) FillRect(di->hDC, &row, bg);

    const int pad = theme::Dp(6, d);
    const int w = row.right - row.left - 2 * pad;
    if (w <= 0) return;
    const int x0 = row.left + pad;
    const int cName = (w * 34) / 100;
    const int cLimit = (w * 10) / 100;
    const int cPolicy = (w * 28) / 100;
    const int cTarget = w - cName - cLimit - cPolicy;

    const IrqPolicyReadout r = dev.Readout();
    const bool disagree = IrqClassifyAgreement(r) == IrqAgreement::Disagree;

    DrawRun(di->hDC, row, x0, cName,
            dev.friendlyName.empty() ? dev.instanceId : dev.friendlyName,
            theme::Font::UiSmall, d, pal.textPrimary);
    DrawRun(di->hDC, row, x0 + cName, cLimit, LimitColumn(dev), theme::Font::MonoSmall, d,
            pal.textSecondary);
    DrawRun(di->hDC, row, x0 + cName + cLimit, cPolicy, PolicyColumn(dev),
            theme::Font::MonoSmall, d, pal.textSecondary);
    DrawRun(di->hDC, row, x0 + cName + cLimit + cPolicy, cTarget, TargetColumn(dev),
            theme::Font::MonoSmall, d, disagree ? pal.warn : pal.textSecondary);
}

// ---------------------------------------------------------------------------
// Capture
// ---------------------------------------------------------------------------

void RefreshVerdict(IrqState* st, HWND hwnd) {
    const std::wstring before = GetText(st->hVerdict);
    const std::wstring after = VerdictSentence(st);
    SetWindowTextW(st->hVerdict, after.c_str());
    if (st->hCores) InvalidateRect(st->hCores, nullptr, TRUE);
    // Only re-lay-out when the sentence really changed: the wrap point moves with it and a
    // layout pass every second would fight the user's scrollbar.
    if (before != after) {
        IrqLayout(st, hwnd);
        InvalidateRect(hwnd, nullptr, TRUE);
    }
}

void StartCapture(IrqState* st, HWND hwnd) {
    if (st->refusal != IrqRefusal::None) return;
    if (st->sampler) { IrqSamplerClose(st->sampler); st->sampler = nullptr; }
    st->cores.clear();
    st->counterError.clear();
    // The Copy button reports its own outcome in its caption, so a new capture puts the
    // caption back rather than leaving "Copied" over numbers that have since been replaced.
    if (st->hCopy) SetWindowTextW(st->hCopy, L"Copy measurement");
    std::wstring err;
    st->sampler = IrqSamplerOpen(&err);
    if (!st->sampler) {
        st->refusal = IrqRefusal::CountersUnavailable;
        st->counterError = err.empty() ? IrqRefusalReason(IrqRefusal::CountersUnavailable) : err;
        if (st->hBanner) SetWindowTextW(st->hBanner,
                                        IrqRefusalReason(IrqRefusal::CountersUnavailable));
        st->capturing = false;
        if (st->hAgain) EnableWindow(st->hAgain, TRUE);
        RefreshVerdict(st, hwnd);
        return;
    }
    st->capturing = true;
    if (st->hAgain) EnableWindow(st->hAgain, FALSE);
    // The priming collection happens now rather than on the first tick, so the first tick a
    // second from now already produces a usable sample.
    IrqSamplerCollect(st->sampler);
    SetTimer(hwnd, kSampleTimer, kSampleMs, nullptr);
    RefreshVerdict(st, hwnd);
}

void OnSampleTick(IrqState* st, HWND hwnd) {
    if (!st->sampler || !st->capturing) return;
    if (!IrqSamplerCollect(st->sampler)) {
        const std::wstring status = IrqSamplerStatus(st->sampler);
        if (!status.empty()) {
            st->counterError = status;
            st->capturing = false;
            KillTimer(hwnd, kSampleTimer);
            if (st->hAgain) EnableWindow(st->hAgain, TRUE);
            RefreshVerdict(st, hwnd);
            return;
        }
    }
    IrqSamplerResults(st->sampler, st->cores);
    IrqMarkGameGroup(st->cores, st->groupLps);
    if (IrqSamplerSampleCount(st->sampler) >= kIrqCollects - 1) {
        // THE CAPTURE IS A FIXED LENGTH, and that is what lets the sentence say "over 7
        // samples" and mean it. A meter that averaged forever would keep quoting a figure
        // from a load that had already stopped.
        st->capturing = false;
        KillTimer(hwnd, kSampleTimer);
        if (st->hAgain) EnableWindow(st->hAgain, TRUE);
    }
    RefreshVerdict(st, hwnd);
}

void OnDeviceSelectionChanged(IrqState* st, HWND hwnd) {
    if (!st->hDevices || !st->hDevDetail) return;
    const LRESULT sel = SendMessageW(st->hDevices, LB_GETCURSEL, 0, 0);
    std::wstring text;
    if (sel != LB_ERR && (size_t)sel < st->devices.size())
        text = DeviceDetail(st->devices[(size_t)sel]);
    const std::wstring before = GetText(st->hDevDetail);
    if (before == text) return;
    SetWindowTextW(st->hDevDetail, text.c_str());
    IrqLayout(st, hwnd);
    InvalidateRect(hwnd, nullptr, TRUE);
}

// ---------------------------------------------------------------------------
// Window procedure
// ---------------------------------------------------------------------------

LRESULT CALLBACK IrqProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    IrqState* st = reinterpret_cast<IrqState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg) {
        case WM_NCCREATE: {
            CREATESTRUCTW* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                              reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
            break;
        }
        case WM_CREATE: {
            // NEVER RETURNS -1: the creator deletes the state when CreateWindowExW returns
            // null, so a path that both destroyed the window and reported failure would free
            // it twice. A control that could not be created is tolerated instead.
            st = reinterpret_cast<IrqState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
            if (!st) return 0;
            st->dpi = DpiOf(hwnd);
            theme::ApplyDarkFrame(hwnd);

            const DWORD kRoText =
                ES_MULTILINE | ES_READONLY | WS_VSCROLL | ES_AUTOVSCROLL | WS_TABSTOP;
            st->hIntro = Mk(hwnd, L"EDIT", IrqBenchIntroText(), kRoText, -1);
            st->hBanner = Mk(hwnd, L"STATIC",
                             IrqRefusalIsHard(st->refusal) ? IrqRefusalReason(st->refusal) : L"",
                             SS_LEFT, -1);
            st->hGroup = Mk(hwnd, L"STATIC", L"", SS_LEFT, -1);
            st->hVerdict = Mk(hwnd, L"STATIC", L"", SS_LEFT, -1);
            st->hRepro = Mk(hwnd, L"STATIC", IrqReproductionCommand().c_str(), SS_LEFT, -1);
            st->hCores = Mk(hwnd, L"LISTBOX", L"",
                            LBS_NOSEL | LBS_HASSTRINGS | LBS_OWNERDRAWFIXED | WS_VSCROLL |
                                WS_TABSTOP,
                            IDC_IRQ_CORES);
            st->hDevHdr = Mk(hwnd, L"STATIC", IrqDeviceHeadingText(), SS_LEFT, -1);
            st->hDevices = Mk(hwnd, L"LISTBOX", L"",
                              LBS_NOTIFY | LBS_HASSTRINGS | LBS_OWNERDRAWFIXED | WS_VSCROLL |
                                  WS_TABSTOP,
                              IDC_IRQ_DEVICES);
            st->hDevDetail = Mk(hwnd, L"STATIC", L"", SS_LEFT, -1);
            st->hAgain = Mk(hwnd, L"BUTTON", L"Measure again", BS_OWNERDRAW | WS_TABSTOP,
                            IDC_IRQ_AGAIN);
            st->hCopy = Mk(hwnd, L"BUTTON", L"Copy measurement", BS_OWNERDRAW | WS_TABSTOP,
                           IDC_IRQ_COPY);
            st->hClose = Mk(hwnd, L"BUTTON", L"Close", BS_OWNERDRAW | WS_TABSTOP,
                            IDC_IRQ_CLOSE);

            SetCtlFont(st->hIntro, theme::Font::UiBody, st->dpi);
            SetCtlFont(st->hBanner, theme::Font::UiBody, st->dpi);
            SetCtlFont(st->hGroup, theme::Font::UiSmall, st->dpi);
            SetCtlFont(st->hVerdict, theme::Font::UiBody, st->dpi);
            SetCtlFont(st->hRepro, theme::Font::UiSmall, st->dpi);
            SetCtlFont(st->hCores, theme::Font::MonoSmall, st->dpi);
            SetCtlFont(st->hDevHdr, theme::Font::UiSmall, st->dpi);
            SetCtlFont(st->hDevices, theme::Font::MonoSmall, st->dpi);
            SetCtlFont(st->hDevDetail, theme::Font::UiSmall, st->dpi);
            SetCtlFont(st->hAgain, theme::Font::UiBody, st->dpi);
            SetCtlFont(st->hCopy, theme::Font::UiBody, st->dpi);
            SetCtlFont(st->hClose, theme::Font::UiBody, st->dpi);

            SetWindowTextW(st->hGroup, GroupSentence(st).c_str());

            // ONE ROW PER LOGICAL PROCESSOR, added once and never rebuilt. The stats behind
            // them change every second; the rows do not, so the list is invalidated rather
            // than repopulated and the user's scroll position survives the capture.
            if (st->hCores) {
                for (size_t i = 0; i < st->allLps.size(); ++i)
                    SendMessageW(st->hCores, LB_ADDSTRING, 0,
                                 reinterpret_cast<LPARAM>(L""));
            }
            if (st->hDevices) {
                for (size_t i = 0; i < st->devices.size(); ++i)
                    SendMessageW(st->hDevices, LB_ADDSTRING, 0,
                                 reinterpret_cast<LPARAM>(L""));
                if (!st->devices.empty()) SendMessageW(st->hDevices, LB_SETCURSEL, 0, 0);
            }
            if (st->devices.empty()) {
                SetWindowTextW(st->hDevDetail,
                               st->deviceError.empty() ? IrqNoDevicesText()
                                                       : st->deviceError.c_str());
            } else {
                SetWindowTextW(st->hDevDetail, DeviceDetail(st->devices[0]).c_str());
            }

            if (IrqRefusalIsHard(st->refusal)) {
                // A refused machine gets the reason and nothing that would pretend to
                // describe it. The lists stay empty rather than showing a partial machine.
                SetWindowTextW(st->hVerdict, IrqRefusalReason(st->refusal));
                if (st->hAgain) EnableWindow(st->hAgain, FALSE);
            } else {
                StartCapture(st, hwnd);
            }

            IrqLayout(st, hwnd);
            if (st->hClose) SetFocus(st->hClose);
            return 0;
        }
        case WM_SIZE:
            if (st) {
                IrqLayout(st, hwnd);
                InvalidateRect(hwnd, nullptr, TRUE);
            }
            return 0;
        case WM_GETMINMAXINFO: {
            MINMAXINFO* mm = reinterpret_cast<MINMAXINFO*>(lp);
            if (!mm) break;
            const int d = st ? st->dpi : DpiOf(hwnd);
            mm->ptMinTrackSize.x = theme::Dp(720, d);
            mm->ptMinTrackSize.y = theme::Dp(520, d);
            return 0;
        }
        case WM_ERASEBKGND: {
            RECT rc;
            GetClientRect(hwnd, &rc);
            theme::FillBackground(reinterpret_cast<HDC>(wp), rc);
            return 1;
        }
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(hwnd, &ps);
            if (dc && st) PaintIrq(st, dc);
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_TIMER:
            if (st && wp == kSampleTimer) OnSampleTick(st, hwnd);
            return 0;
        case WM_MEASUREITEM: {
            MEASUREITEMSTRUCT* mi = reinterpret_cast<MEASUREITEMSTRUCT*>(lp);
            if (!mi || !st) break;
            if (mi->CtlType == ODT_LISTBOX) {
                mi->itemHeight = (UINT)theme::Dp(theme::metric::kRowH, st->dpi);
                return TRUE;
            }
            break;
        }
        case WM_DRAWITEM: {
            const DRAWITEMSTRUCT* di = reinterpret_cast<const DRAWITEMSTRUCT*>(lp);
            if (!di || !st) break;
            if (di->CtlType == ODT_BUTTON) {
                const theme::ButtonKind kind = di->CtlID == (UINT)IDC_IRQ_CLOSE
                                                   ? theme::ButtonKind::Primary
                                                   : theme::ButtonKind::Secondary;
                if (theme::DrawButton(di, kind, st->dpi)) return TRUE;
                break;
            }
            if (di->CtlType == ODT_LISTBOX) {
                if (di->CtlID == (UINT)IDC_IRQ_CORES) { DrawCoreRow(st, di); return TRUE; }
                if (di->CtlID == (UINT)IDC_IRQ_DEVICES) { DrawDeviceRow(st, di); return TRUE; }
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
            // A READ-ONLY MULTILINE EDIT SENDS WM_CTLCOLORSTATIC, NOT WM_CTLCOLOREDIT.
            // envwarning.cpp records the same trap. The intro is one, and it wants the input
            // surface it would have had if Windows had asked the obvious question.
            if (msg == WM_CTLCOLORSTATIC && st && ctl == st->hIntro) m = WM_CTLCOLOREDIT;
            HBRUSH b = theme::OnCtlColor(m, dc, ctl);
            const theme::Palette& pal = theme::P();
            if (st && (m == WM_CTLCOLORSTATIC || m == WM_CTLCOLORBTN)) {
                // The generic helper assumes a static sits on appBg. Every static in this
                // window sits ON A CARD, so it must erase to cardBg or it leaves a
                // mismatched strip behind a sentence that rewrites itself every second.
                if (!st->cardBrush) st->cardBrush = CreateSolidBrush(pal.cardBg);
                if (st->cardBrush) {
                    SetBkColor(dc, pal.cardBg);
                    b = st->cardBrush;
                }
                COLORREF fg = pal.textSecondary;
                if (ctl == st->hVerdict) {
                    // The one sentence that can be a warning gets the warning colour, and
                    // ONLY for the case this page exists for.
                    fg = IrqHotCoreIsInGameGroup(st->cores) ? pal.warn : pal.textPrimary;
                    if (!st->counterError.empty()) fg = pal.danger;
                } else if (ctl == st->hBanner) {
                    fg = pal.warn;
                } else if (ctl == st->hRepro || ctl == st->hDevHdr || ctl == st->hGroup) {
                    fg = pal.textDim;
                }
                SetTextColor(dc, fg);
            }
            if (b) return reinterpret_cast<LRESULT>(b);
            SetBkMode(dc, TRANSPARENT);
            SetTextColor(dc, pal.textSecondary);
            SetBkColor(dc, pal.cardBg);
            if (st && st->cardBrush) return reinterpret_cast<LRESULT>(st->cardBrush);
            return reinterpret_cast<LRESULT>(GetStockObject(BLACK_BRUSH));
        }
        case WM_DPICHANGED: {
            if (!st) break;
            st->dpi = static_cast<int>(HIWORD(wp));
            SetCtlFont(st->hIntro, theme::Font::UiBody, st->dpi);
            SetCtlFont(st->hBanner, theme::Font::UiBody, st->dpi);
            SetCtlFont(st->hGroup, theme::Font::UiSmall, st->dpi);
            SetCtlFont(st->hVerdict, theme::Font::UiBody, st->dpi);
            SetCtlFont(st->hRepro, theme::Font::UiSmall, st->dpi);
            SetCtlFont(st->hCores, theme::Font::MonoSmall, st->dpi);
            SetCtlFont(st->hDevHdr, theme::Font::UiSmall, st->dpi);
            SetCtlFont(st->hDevices, theme::Font::MonoSmall, st->dpi);
            SetCtlFont(st->hDevDetail, theme::Font::UiSmall, st->dpi);
            SetCtlFont(st->hAgain, theme::Font::UiBody, st->dpi);
            SetCtlFont(st->hCopy, theme::Font::UiBody, st->dpi);
            SetCtlFont(st->hClose, theme::Font::UiBody, st->dpi);
            {
                const RECT* nr = reinterpret_cast<const RECT*>(lp);
                if (nr)
                    SetWindowPos(hwnd, nullptr, nr->left, nr->top, nr->right - nr->left,
                                 nr->bottom - nr->top, SWP_NOZORDER | SWP_NOACTIVATE);
            }
            // Re-MEASURED, not re-scaled: the font changed, so the wrap points did too.
            IrqLayout(st, hwnd);
            InvalidateRect(hwnd, nullptr, TRUE);
            return 0;
        }
        case WM_COMMAND: {
            const int id = LOWORD(wp);
            const int code = HIWORD(wp);
            if (id == IDC_IRQ_DEVICES) {
                if (code == LBN_SELCHANGE && st) OnDeviceSelectionChanged(st, hwnd);
                return 0;
            }
            switch (id) {
                case IDC_IRQ_AGAIN:
                    if (st && code == BN_CLICKED) StartCapture(st, hwnd);
                    return 0;
                case IDC_IRQ_COPY:
                    if (st && code == BN_CLICKED) {
                        // Says what happened either way. A button that silently did nothing
                        // is indistinguishable from one that worked.
                        const bool ok = CopyToClipboard(hwnd, BuildReport(st));
                        SetWindowTextW(st->hCopy,
                                       ok ? L"Copied" : L"Clipboard refused");
                    }
                    return 0;
                case IDC_IRQ_CLOSE:
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
            DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY:
            KillTimer(hwnd, kSampleTimer);
            if (st && st->sampler) { IrqSamplerClose(st->sampler); st->sampler = nullptr; }
            break;
        case WM_NCDESTROY:
            if (st) {
                if (st->sampler) { IrqSamplerClose(st->sampler); st->sampler = nullptr; }
                if (st->cardBrush) { DeleteObject(st->cardBrush); st->cardBrush = nullptr; }
                if (st->rowBrush) { DeleteObject(st->rowBrush); st->rowBrush = nullptr; }
                if (st->rowAltBrush) { DeleteObject(st->rowAltBrush); st->rowAltBrush = nullptr; }
                delete st;
            }
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
            if (g_hIrq == hwnd) g_hIrq = nullptr;
            break;
        default:
            break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void RegisterIrqClass() {
    static bool done = false;
    if (done) return;
    WNDCLASSEXW wc;
    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = IrqProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;   // WM_ERASEBKGND paints appBg; no light flash on show
    wc.lpszClassName = kIrqClass;
    RegisterClassExW(&wc);
    done = true;
}

// THE GATE. Copied in shape from envwarning.cpp: disabling the owner is what makes this
// window modal; running our own loop is what makes it a GATE, because the caller's next line
// cannot run until this window has been destroyed.
void RunIrqModalLoop(HWND hwnd, HWND owner) {
    if (owner) EnableWindow(owner, FALSE);
    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);
    SetForegroundWindow(hwnd);

    MSG msg;
    while (g_hIrq == hwnd && IsWindow(hwnd)) {
        BOOL got = GetMessageW(&msg, nullptr, 0, 0);
        // GETMESSAGEW RETURNING 0 IS WM_QUIT, AND IT IS NOT OURS TO SWALLOW. Re-post it and
        // leave; the host loop then sees it and shuts down in its usual order.
        if (got == 0) { PostQuitMessage(static_cast<int>(msg.wParam)); break; }
        if (got == -1) break;   // a bad hwnd would otherwise spin here forever
        if (!IsDialogMessageW(hwnd, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }

    // EVERY EXIT FROM THE LOOP LANDS HERE, and re-enabling the owner is the one thing that
    // must never be skipped: leaving the Settings window disabled would freeze the product,
    // not just a dialog.
    if (owner) EnableWindow(owner, TRUE);
    if (IsWindow(hwnd)) DestroyWindow(hwnd);
    if (owner) SetActiveWindow(owner);
}

// Which processors the game's group holds, and how honestly this page can name it. The
// LIVE profile wins when a game is actually running; otherwise the machine's own default is
// named as a default, in as many words, rather than passed off as the running answer.
void ResolveGameGroup(IrqState* st, const Config& cfg, const Topology& topo, Engine* engine) {
    const Mask* m = nullptr;
    if (engine) {
        const EngineStatus s = engine->GetStatus();
        if (s.active && !s.gameMaskName.empty()) {
            m = cfg.FindMask(s.gameMaskName);
            if (m) st->groupSource = GameGroupSource::LiveProfile;
        }
    }
    if (!m && !topo.defaultGameMask.empty()) {
        m = cfg.FindMask(topo.defaultGameMask);
        if (m) st->groupSource = GameGroupSource::MachineDefault;
    }
    if (!m) {
        st->groupSource = GameGroupSource::Unknown;
        return;
    }
    st->groupMaskName = m->name;
    st->groupLps = LpsForIds(topo, m->ids);
    st->groupMaskValid = BuildIrqMask(topo, st->groupLps, st->groupMask);
}

}  // namespace

void ShowInterruptBench(HWND owner, const Config& cfg, const Topology& topo, Engine* engine) {
    if (g_hIrq && IsWindow(g_hIrq)) {
        if (IsIconic(g_hIrq)) ShowWindow(g_hIrq, SW_RESTORE);
        SetForegroundWindow(g_hIrq);
        return;
    }
    RegisterIrqClass();

    IrqState* st = new IrqState();
    st->topo = &topo;
    st->refusal = IrqEvaluateMachine(topo);

    if (!IrqRefusalIsHard(st->refusal)) {
        for (size_t i = 0; i < topo.entries.size(); ++i)
            st->allLps.push_back(topo.entries[i].LogicalProcessorIndex);
        std::sort(st->allLps.begin(), st->allLps.end());
        ResolveGameGroup(st, cfg, topo, engine);
        std::wstring devErr;
        if (!IrqEnumerateDevices(st->devices, &devErr)) st->deviceError = devErr;
    } else {
        LogLine(L"[irq] machine out of scope for the readout, refusal=%d", (int)st->refusal);
    }

    const int dpi = owner ? DpiOf(owner) : DpiOf(nullptr);
    int w = theme::Dp(960, dpi);
    int h = theme::Dp(820, dpi);

    RECT work = { 0, 0, 0, 0 };
    HMONITOR mon = MonitorFromWindow(owner ? owner : GetDesktopWindow(),
                                     MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi;
    ZeroMemory(&mi, sizeof(mi));
    mi.cbSize = sizeof(mi);
    if (GetMonitorInfoW(mon, &mi)) work = mi.rcWork;
    if (work.right > work.left && work.bottom > work.top) {
        const int maxW = work.right - work.left - theme::Dp(40, dpi);
        const int maxH = work.bottom - work.top - theme::Dp(40, dpi);
        if (w > maxW) w = maxW;
        if (h > maxH) h = maxH;
    }
    int x = work.left + ((work.right - work.left) - w) / 2;
    int y = work.top + ((work.bottom - work.top) - h) / 2;
    if (x < work.left) x = work.left;
    if (y < work.top) y = work.top;

    HWND hwnd = CreateWindowExW(0, kIrqClass, L"Game Optimizer - interrupt and DPC readout",
                                WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, x, y, w, h, owner,
                                nullptr, GetModuleHandleW(nullptr), st);
    if (!hwnd) {
        // The window never reached a procedure that could adopt the state, so this is its
        // only owner. See the WM_CREATE comment about never returning -1.
        LogLine(L"[irq] the readout window could not be created, gle=%lu", GetLastError());
        delete st;
        return;
    }
    g_hIrq = hwnd;
    RunIrqModalLoop(hwnd, owner);
}

}  // namespace cd
