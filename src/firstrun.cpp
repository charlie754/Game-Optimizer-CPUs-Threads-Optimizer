// Game Optimizer - the three-page first-run wizard.
//
// Pages: what we found / Windows Game Mode / pick your first game. One window, the pages
// are shown and hidden control sets rather than three windows, so Back and Next keep every
// answer without any state marshalling.
//
// Two product rules are load-bearing here and are NOT style choices:
//   * The wizard ADVISES and never ENFORCES. It does not toggle Windows Game Mode, it does
//     not stop or start the AMD V-Cache service, and neither one can block Next.
//   * Windows Game Mode and the AMD 3D V-Cache Performance Optimizer service are TWO
//     separate global scheduling influences and are reported on two separate lines. On the
//     reference machine Game Mode is OFF while that service is RUNNING, so collapsing them
//     into one "is anything else interfering" line would report the machine wrongly. The
//     dark restyle gives each of them its OWN CARD and its OWN STATUS DOT for exactly that
//     reason - the visual grouping now states the separation instead of merely allowing it.
//
// firstRunDone is set on EVERY exit path, including the close button, so a user who dismisses
// the wizard is not asked again on every launch.
//
// VISUALS: every colour, radius, font and spacing value comes from theme.h. Nothing in this
// file invents a colour. The window paints its own background (WM_ERASEBKGND), its own cards
// and captions (WM_PAINT), and its own buttons (BS_OWNERDRAW + WM_DRAWITEM); the standard
// controls that remain - the read-only EDITs and the process ListView - are recoloured
// through WM_CTLCOLOR* and the ListView colour messages. That is the whole dark-mode
// mechanism, and it is the same one settings.cpp uses.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commctrl.h>
#include <tlhelp32.h>

#include <algorithm>
#include <map>
#include <string>
#include <vector>

#include "ui.h"
#include "util.h"
#include "config.h"
#include "topology.h"
#include "theme.h"

namespace cd {

namespace {

// ---------------------------------------------------------------------------
// Local plumbing (internal linkage - settings.cpp has its own copies on purpose,
// so neither translation unit depends on the other's private helpers).
// ---------------------------------------------------------------------------

void EnsureCommonControls() {
    static bool done = false;
    if (done) return;
    INITCOMMONCONTROLSEX icc;
    icc.dwSize = sizeof(icc);
    icc.dwICC = ICC_LISTVIEW_CLASSES | ICC_STANDARD_CLASSES;
    InitCommonControlsEx(&icc);
    done = true;
}

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

// A flat fill. Created and destroyed in the same breath so nothing can leak out of a paint.
void FillSolid(HDC dc, const RECT& rc, COLORREF colour) {
    HBRUSH b = CreateSolidBrush(colour);
    if (!b) return;
    FillRect(dc, &rc, b);
    DeleteObject(b);
}

HWND Mk(HWND parent, const wchar_t* cls, const wchar_t* text, DWORD style, int id,
        DWORD exStyle = 0) {
    return CreateWindowExW(exStyle, cls, text, WS_CHILD | style, 0, 0, 10, 10, parent,
                           reinterpret_cast<HMENU>(static_cast<UINT_PTR>(id)),
                           GetModuleHandleW(nullptr), nullptr);
}

std::wstring GetText(HWND h) {
    if (!h) return std::wstring();
    int n = GetWindowTextLengthW(h);
    if (n <= 0) return std::wstring();
    std::vector<wchar_t> buf(static_cast<size_t>(n) + 1, L'\0');
    GetWindowTextW(h, buf.data(), n + 1);
    return std::wstring(buf.data());
}

// ---------------------------------------------------------------------------
// Running process enumeration for page 3
// ---------------------------------------------------------------------------

struct ProcRow {
    DWORD pid = 0;
    std::wstring name;
    std::wstring title;
    bool hasWindow = false;
};

BOOL CALLBACK CollectTitlesProc(HWND h, LPARAM lp) {
    if (!IsWindowVisible(h)) return TRUE;
    if (GetWindow(h, GW_OWNER) != nullptr) return TRUE;
    wchar_t buf[256];
    int n = GetWindowTextW(h, buf, 256);
    if (n <= 0) return TRUE;
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    if (pid == 0) return TRUE;
    std::map<DWORD, std::wstring>* m = reinterpret_cast<std::map<DWORD, std::wstring>*>(lp);
    if (m->find(pid) == m->end()) (*m)[pid] = buf;
    return TRUE;
}

bool ProcRowLess(const ProcRow& a, const ProcRow& b) {
    if (a.hasWindow != b.hasWindow) return a.hasWindow;
    std::wstring an = ToLower(a.name), bn = ToLower(b.name);
    if (an != bn) return an < bn;
    return a.pid < b.pid;
}

std::vector<ProcRow> EnumerateProcesses() {
    std::map<DWORD, std::wstring> titles;
    EnumWindows(CollectTitlesProc, reinterpret_cast<LPARAM>(&titles));

    std::vector<ProcRow> out;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap != INVALID_HANDLE_VALUE) {
        PROCESSENTRY32W pe;
        ZeroMemory(&pe, sizeof(pe));
        pe.dwSize = sizeof(pe);
        if (Process32FirstW(snap, &pe)) {
            do {
                if (pe.th32ProcessID == 0 || pe.th32ProcessID == 4) continue;
                ProcRow r;
                r.pid = pe.th32ProcessID;
                r.name = pe.szExeFile;
                std::map<DWORD, std::wstring>::const_iterator it = titles.find(r.pid);
                if (it != titles.end()) {
                    r.title = it->second;
                    r.hasWindow = true;
                }
                out.push_back(r);
            } while (Process32NextW(snap, &pe));
        }
        CloseHandle(snap);
    }
    std::sort(out.begin(), out.end(), ProcRowLess);
    return out;
}

// ---------------------------------------------------------------------------
// Text builders
// ---------------------------------------------------------------------------

std::wstring FormatLps(std::vector<ULONG> lps) {
    std::sort(lps.begin(), lps.end());
    std::wstring s;
    size_t i = 0;
    while (i < lps.size()) {
        size_t j = i;
        while (j + 1 < lps.size() && lps[j + 1] == lps[j] + 1) ++j;
        if (!s.empty()) s += L", ";
        s += std::to_wstring(lps[i]);
        if (j > i) {
            s += L"-";
            s += std::to_wstring(lps[j]);
        }
        i = j + 1;
    }
    return s;
}

std::wstring Page1Summary(const Topology& t) {
    std::wstring s = t.summary;
    if (s.empty()) {
        s = std::wstring(L"Detected ") + KindName(t.kind) + L": " +
            std::to_wstring(t.domains.size()) + L" cache domain(s) across " +
            std::to_wstring(t.totalLogicalProcessors) + L" logical processors.";
    }
    s += L"\r\n\r\n";

    if (t.confidence == Confidence::High) {
        s += L"Detection confidence: High. The cache domains are clearly distinguishable, "
             L"so the masks below should be right as they stand.";
    } else if (t.confidence == Confidence::Medium) {
        s += L"Detection confidence: MEDIUM. The cache domains report the same L3 size, so "
             L"which one is called \"CCD0\" is a guess and not a measurement. Look at the "
             L"map before you trust it, and edit it if the wrong cores are highlighted.";
    } else {
        s += L"Detection confidence: NONE.";
    }

    if (t.kind == TopologyKind::SingleDomain || t.domains.size() <= 1) {
        s += L"\r\n\r\nNo CCD split was found on this CPU: every logical processor shares "
             L"one last-level cache. There is no second cache domain to move background "
             L"work onto, so this tool cannot give a game an isolated CCD here. Only the "
             L"SMT masks (\"All no SMT\") will change anything, and the effect of those is "
             L"much smaller. Nothing below will do more than that on this machine.";
    }
    return s;
}

std::wstring Page1Masks(const Topology& t) {
    std::vector<Mask> masks = DeriveMasks(t);
    std::wstring s;
    for (size_t i = 0; i < masks.size(); ++i) {
        const Mask& m = masks[i];
        s += m.name;
        s += L"   -   ";
        s += std::to_wstring(m.ids.size());
        s += L" logical processors: ";
        s += FormatLps(LpsForIds(t, m.ids));
        s += L"\r\n";
    }
    if (!t.defaultGameMask.empty()) {
        s += L"\r\nRecommended for the game: ";
        s += t.defaultGameMask;
        s += L"\r\nRecommended for background apps: ";
        s += t.defaultHeavyMask;
        s += L"\r\n";
    }
    return s;
}

std::wstring Page2GameModeText(const EnvironmentInfo& env, const Topology& t, bool& warnOut) {
    const bool multiDomain = t.domains.size() > 1;
    warnOut = env.gameModeState == GameModeState::On && multiDomain && env.isAmd;

    std::wstring s = L"CPU: ";
    s += env.cpuBrand.empty() ? std::wstring(L"(not reported by the registry)") : env.cpuBrand;
    s += L"\r\n\r\n";

    if (env.gameModeState == GameModeState::NotDeterminable) {
        s += L"Windows Game Mode is not determinable because the AutoGameModeEnabled "
             L"registry value could not be read for this account.";
    } else if (warnOut) {
        s += L"Windows Game Mode is ON "
             L"(HKCU\\Software\\Microsoft\\GameBar\\AutoGameModeEnabled = 1).\r\n\r\n"
             L"On a multi-CCD AMD part Game Mode applies its own GLOBAL CCD preference to "
             L"whatever it decides is the game. That is machine-wide, it is not per-game, "
             L"you do not choose which processes it covers, and it fights the per-game CPU "
             L"Sets this tool applies. If your game ends up on the wrong CCD anyway, this "
             L"is the first thing to check.\r\n\r\n"
             L"Game Optimizer will not change this setting for you. The button below just "
             L"opens the Windows page so you can decide.";
    } else if (env.gameModeState == GameModeState::Off) {
        s += L"Windows Game Mode is OFF "
             L"(HKCU\\Software\\Microsoft\\GameBar\\AutoGameModeEnabled = 0). It is not "
             L"applying a global CCD preference, so it is not competing with the per-game "
             L"CPU Sets this tool applies.";
    } else {
        s += L"Windows Game Mode is ON "
             L"(HKCU\\Software\\Microsoft\\GameBar\\AutoGameModeEnabled = 1), but this "
             L"machine is not a multi-CCD AMD part, so there is no CCD preference for it "
             L"to express. Nothing on this page needs your attention.";
    }
    return s;
}

std::wstring Page2VCacheText(const EnvironmentInfo& env) {
    std::wstring s = L"Separately, and regardless of the Game Mode setting above:\r\n\r\n";
    if (env.amdVCacheServiceState == AmdVCacheServiceState::NotDeterminable) {
        s += L"The AMD 3D V-Cache Performance Optimizer service state is not determinable.";
        return s;
    }
    if (env.amdVCacheServiceState == AmdVCacheServiceState::NotInstalled) {
        s += L"The AMD 3D V-Cache Performance Optimizer service (amd3dvcacheSvc) is NOT "
             L"installed on this machine, so it is not influencing scheduling here.";
        return s;
    }
    if (env.amdVCacheServiceState == AmdVCacheServiceState::Running) {
        s += L"The AMD 3D V-Cache Performance Optimizer service (amd3dvcacheSvc) is "
             L"INSTALLED and RUNNING.\r\n\r\n"
             L"This is a second, independent global influence on where threads are "
             L"scheduled - it is not the same thing as Windows Game Mode and it is not "
             L"turned off by turning Game Mode off. Both facts on this page can be true at "
             L"once, which is why they are two lines and not one. On a machine like this "
             L"one a whole cache domain has been observed flagged Parked; if the core map "
             L"shows a parked CCD, this service is the likeliest reason.\r\n\r\n"
             L"Game Optimizer does not stop, start or configure this service.";
    } else {
        s += L"The AMD 3D V-Cache Performance Optimizer service (amd3dvcacheSvc) is "
             L"INSTALLED but NOT RUNNING. It is a separate global scheduling influence from "
             L"Windows Game Mode; while it is stopped it is not expressing a preference.";
    }
    return s;
}

// The one-line technical tag drawn in monospace on the right of each page-2 fact row. It
// restates, in the raw form, exactly the value the prose below it describes.
std::wstring Page2GameModeTag(const EnvironmentInfo& env) {
    if (env.gameModeState == GameModeState::NotDeterminable)
        return L"AutoGameModeEnabled: not determinable";
    return env.gameModeState == GameModeState::On ? L"AutoGameModeEnabled = 1"
                                                   : L"AutoGameModeEnabled = 0";
}

std::wstring Page2VCacheTag(const EnvironmentInfo& env) {
    switch (env.amdVCacheServiceState) {
        case AmdVCacheServiceState::NotInstalled:
            return L"amd3dvcacheSvc: not installed";
        case AmdVCacheServiceState::InstalledButStopped:
            return L"amd3dvcacheSvc: stopped";
        case AmdVCacheServiceState::Running:
            return L"amd3dvcacheSvc: running";
        default:
            return L"amd3dvcacheSvc: not determinable";
    }
}

// ---------------------------------------------------------------------------
// Wizard window
// ---------------------------------------------------------------------------

enum : int {
    IDC_FR_MAP = 1500,
    IDC_FR_LOOKS,
    IDC_FR_EDIT,
    IDC_FR_OPENGM,
    IDC_FR_FILTER,
    IDC_FR_LIST,
    IDC_FR_BROWSE,
    IDC_FR_EXAMPLE,
    IDC_FR_SKIP,
    IDC_FR_BACK,
    IDC_FR_NEXT
};

const wchar_t kWizardClass[] = L"GameOptimizerFirstRun";

// Rectangles owned by the layout pass and consumed by the paint pass. Keeping them in one
// place is what stops a card and the control sitting inside it from drifting apart.
struct WizLayout {
    RECT title;
    RECT subtitle;
    RECT steps;
    RECT p1Card;
    RECT p1MasksCard;
    RECT p2GmCard;
    RECT p2VcCard;
    RECT p2Note;
    RECT p3Card;
    RECT p3FilterLbl;
};

struct WizardState {
    Config* cfg = nullptr;
    const Topology* topo = nullptr;
    EnvironmentInfo env;
    bool gameModeWarn = false;

    int page = 0;
    int dpi = 96;
    HBRUSH bgBrush = nullptr;   // WM_CTLCOLOR* fallback; freed in WM_NCDESTROY

    bool done = false;
    bool completed = false;    // false only when the user closed the wizard outright
    bool wantEdit = false;     // "Let me edit" - jump to the Settings core map afterwards
    bool useExample = false;
    std::wstring pickedGame;

    std::vector<ProcRow> procs;
    std::vector<size_t>  shown;

    WizLayout lay = {};

    // page 1
    HWND hP1Sum = nullptr, hP1Map = nullptr, hP1Masks = nullptr;
    // Stand-in for the core map's slot when the control could not be created. Exactly one of
    // hP1Map / hP1MapFail is ever non-null, and they occupy the same rectangle.
    HWND hP1MapFail = nullptr;
    HWND hP1Looks = nullptr, hP1Edit = nullptr;
    // page 2
    HWND hP2Gm = nullptr, hP2Open = nullptr, hP2VCache = nullptr;
    // page 3
    HWND hP3Filter = nullptr, hP3List = nullptr;
    HWND hP3Browse = nullptr, hP3Example = nullptr, hP3Skip = nullptr, hP3Sel = nullptr;
    // footer
    HWND hBack = nullptr, hNext = nullptr;
};

// A read-only EDIT sends WM_CTLCOLORSTATIC, not WM_CTLCOLOREDIT. Left alone it would be
// painted as a label on the app background instead of as an input surface, so the message is
// re-pointed for exactly these four controls before it reaches theme::OnCtlColor.
bool IsReadOnlyEdit(const WizardState* st, HWND ctl) {
    if (!ctl) return false;
    return ctl == st->hP1Sum || ctl == st->hP1Masks || ctl == st->hP2Gm || ctl == st->hP2VCache;
}

void ApplyFonts(WizardState* st) {
    const int d = st->dpi;
    // Prose in the proportional face; anything columnar or technical in the mono face.
    SetCtlFont(st->hP1Sum, theme::Font::UiBody, d);
    SetCtlFont(st->hP1Masks, theme::Font::MonoBody, d);
    SetCtlFont(st->hP1MapFail, theme::Font::UiBody, d);
    SetCtlFont(st->hP1Looks, theme::Font::UiBody, d);
    SetCtlFont(st->hP1Edit, theme::Font::UiBody, d);
    SetCtlFont(st->hP2Gm, theme::Font::UiBody, d);
    SetCtlFont(st->hP2VCache, theme::Font::UiBody, d);
    SetCtlFont(st->hP2Open, theme::Font::UiBody, d);
    SetCtlFont(st->hP3Filter, theme::Font::UiBody, d);
    SetCtlFont(st->hP3List, theme::Font::MonoSmall, d);   // PIDs and exe names are columnar
    SetCtlFont(st->hP3Browse, theme::Font::UiBody, d);
    SetCtlFont(st->hP3Example, theme::Font::UiBody, d);
    SetCtlFont(st->hP3Skip, theme::Font::UiBody, d);
    SetCtlFont(st->hP3Sel, theme::Font::UiSmall, d);
    SetCtlFont(st->hBack, theme::Font::UiBody, d);
    SetCtlFont(st->hNext, theme::Font::UiBody, d);
}

void FillProcList(WizardState* st) {
    if (!st->hP3List) return;
    std::wstring flt = ToLower(Trim(GetText(st->hP3Filter)));
    SendMessageW(st->hP3List, WM_SETREDRAW, FALSE, 0);
    ListView_DeleteAllItems(st->hP3List);
    st->shown.clear();
    wchar_t cell[512];
    for (size_t i = 0; i < st->procs.size(); ++i) {
        const ProcRow& r = st->procs[i];
        if (!flt.empty()) {
            std::wstring hay = ToLower(r.name) + L" " + ToLower(r.title);
            if (hay.find(flt) == std::wstring::npos) continue;
        }
        int row = static_cast<int>(st->shown.size());
        wsprintfW(cell, L"%lu", static_cast<unsigned long>(r.pid));
        LVITEMW it;
        ZeroMemory(&it, sizeof(it));
        it.mask = LVIF_TEXT;
        it.iItem = row;
        it.iSubItem = 0;
        it.pszText = cell;
        ListView_InsertItem(st->hP3List, &it);
        lstrcpynW(cell, r.name.c_str(), 512);
        ListView_SetItemText(st->hP3List, row, 1, cell);
        lstrcpynW(cell, r.title.c_str(), 512);
        ListView_SetItemText(st->hP3List, row, 2, cell);
        st->shown.push_back(i);
    }
    SendMessageW(st->hP3List, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(st->hP3List, nullptr, TRUE);
}

void UpdateSelectionLine(WizardState* st) {
    std::wstring s;
    if (st->useExample) {
        s = L"Selected: the shipped \"Overwatch\" example profile will be enabled. You can "
            L"edit or delete it in Settings.";
    } else if (!st->pickedGame.empty()) {
        s = L"Selected: " + st->pickedGame +
            L"   (a profile will be created for it, using the recommended masks)";
    } else {
        s = L"Nothing selected. Skip is fine - the tray works with zero profiles and simply "
            L"reads idle until you add one.";
    }
    SetWindowTextW(st->hP3Sel, s.c_str());
}

void ShowPage(WizardState* st, HWND hwnd) {
    HWND p1[] = { st->hP1Sum, st->hP1Map, st->hP1MapFail, st->hP1Masks,
                  st->hP1Looks, st->hP1Edit };
    HWND p2[] = { st->hP2Gm, st->hP2Open, st->hP2VCache };
    HWND p3[] = { st->hP3Filter, st->hP3List, st->hP3Browse,
                  st->hP3Example, st->hP3Skip, st->hP3Sel };

    for (HWND h : p1) if (h) ShowWindow(h, st->page == 0 ? SW_SHOW : SW_HIDE);
    for (HWND h : p2) if (h) ShowWindow(h, st->page == 1 ? SW_SHOW : SW_HIDE);
    for (HWND h : p3) if (h) ShowWindow(h, st->page == 2 ? SW_SHOW : SW_HIDE);

    // The Game Mode button exists only when there is something to warn about.
    if (st->hP2Open && !st->gameModeWarn) ShowWindow(st->hP2Open, SW_HIDE);

    EnableWindow(st->hBack, st->page > 0);
    SetWindowTextW(st->hNext, st->page == 2 ? L"Finish" : L"Next >");

    static const wchar_t* kTitles[3] = {
        L"Game Optimizer - 1 of 3: What we found",
        L"Game Optimizer - 2 of 3: Windows Game Mode",
        L"Game Optimizer - 3 of 3: Pick your first game"
    };
    SetWindowTextW(hwnd, kTitles[st->page]);
    InvalidateRect(hwnd, nullptr, TRUE);
}

void WizardLayout(WizardState* st, HWND hwnd) {
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
    const int RH   = theme::Dp(theme::metric::kRowH, d);
    const int titleH = theme::Dp(28, d);
    const int subH   = theme::Dp(16, d);
    const int capH   = theme::Dp(18, d);   // the muted caption row at the top of a card
    const int rowH   = theme::Dp(20, d);   // a status-dot fact row
    const int minTxt = theme::Dp(40, d);

    const int fullW = cw - 2 * PAD;
    const int contentTop = PAD + titleH + subH + GAP;
    const int footerTop = ch - PAD - BH;
    const int bodyBottom = footerTop - GAP;

    WizLayout& L = st->lay;
    SetRect(&L.title, PAD, PAD, cw - PAD, PAD + titleH);
    SetRect(&L.subtitle, PAD, PAD + titleH, cw - PAD, PAD + titleH + subH);
    SetRect(&L.steps, PAD, footerTop, PAD + theme::Dp(64, d), footerTop + BH);

    // ---- footer ----
    MoveWindow(st->hNext, cw - PAD - BW, footerTop, BW, BH, TRUE);
    MoveWindow(st->hBack, cw - PAD - 2 * BW - GAPT, footerTop, BW, BH, TRUE);

    // ---- page 1 ----
    {
        int y = contentTop;
        const int btnY = bodyBottom - BH;
        const int sumH = theme::Dp(104, d);
        const int mapH = theme::Dp(112, d);

        SetRect(&L.p1Card, PAD, y, cw - PAD, y + PAD + capH + GAPT + sumH + PAD);
        RECT in = CardInner(L.p1Card, d);
        MoveWindow(st->hP1Sum, in.left, in.top + capH + GAPT, in.right - in.left, sumH, TRUE);
        y = L.p1Card.bottom + GAP;

        // The core map paints its own surface, so it sits directly on the app background
        // rather than inside a card - two nested panels would only add noise.
        if (st->hP1Map) MoveWindow(st->hP1Map, PAD, y, fullW, mapH, TRUE);
        if (st->hP1MapFail) MoveWindow(st->hP1MapFail, PAD, y, fullW, mapH, TRUE);
        y += mapH + GAP;

        const int minCard = PAD + capH + GAPT + minTxt + PAD;
        int mcBottom = btnY - GAP;
        if (mcBottom - y < minCard) mcBottom = y + minCard;
        SetRect(&L.p1MasksCard, PAD, y, cw - PAD, mcBottom);
        RECT in2 = CardInner(L.p1MasksCard, d);
        int mt = in2.top + capH + GAPT;
        int mh = in2.bottom - mt;
        if (mh < minTxt) mh = minTxt;
        MoveWindow(st->hP1Masks, in2.left, mt, in2.right - in2.left, mh, TRUE);

        const int pbW = theme::Dp(120, d);
        MoveWindow(st->hP1Looks, PAD, btnY, pbW, BH, TRUE);
        MoveWindow(st->hP1Edit, PAD + pbW + GAPT, btnY, pbW, BH, TRUE);
    }

    // ---- page 2 ----
    // Two cards, never one: the Game Mode registry value and the AMD service are separate
    // facts about the machine and the layout says so.
    {
        int y = contentTop;
        const int noteH = theme::Dp(30, d);
        const int noteTop = bodyBottom - noteH;
        SetRect(&L.p2Note, PAD, noteTop, cw - PAD, noteTop + noteH);

        const int gmExtra = st->gameModeWarn ? (GAPT + BH) : 0;
        const int minGm = PAD + rowH + GAPT + theme::Dp(60, d) + gmExtra + PAD;
        const int minVc = PAD + rowH + GAPT + theme::Dp(60, d) + PAD;

        int avail = (noteTop - GAP) - y - GAP;   // both cards plus the gap between them
        if (avail < minGm + minVc) avail = minGm + minVc;
        int gmH = avail * 55 / 100;
        if (gmH < minGm) gmH = minGm;
        int vcH = avail - gmH;
        if (vcH < minVc) vcH = minVc;

        SetRect(&L.p2GmCard, PAD, y, cw - PAD, y + gmH);
        RECT in = CardInner(L.p2GmCard, d);
        int et = in.top + rowH + GAPT;
        int eb = in.bottom - gmExtra;
        if (eb - et < minTxt) eb = et + minTxt;
        MoveWindow(st->hP2Gm, in.left, et, in.right - in.left, eb - et, TRUE);
        MoveWindow(st->hP2Open, in.left, in.bottom - BH, theme::Dp(210, d), BH, TRUE);
        y = L.p2GmCard.bottom + GAP;

        SetRect(&L.p2VcCard, PAD, y, cw - PAD, y + vcH);
        RECT in2 = CardInner(L.p2VcCard, d);
        int vt = in2.top + rowH + GAPT;
        int vh = in2.bottom - vt;
        if (vh < minTxt) vh = minTxt;
        MoveWindow(st->hP2VCache, in2.left, vt, in2.right - in2.left, vh, TRUE);
    }

    // ---- page 3 ----
    {
        int y = contentTop;
        const int btnY = bodyBottom - BH;
        const int selH = theme::Dp(34, d);
        const int selY = btnY - GAP - selH;
        const int minCard = PAD + RH + GAPT + theme::Dp(80, d) + PAD;
        int cardBottom = selY - GAP;
        if (cardBottom - y < minCard) cardBottom = y + minCard;
        SetRect(&L.p3Card, PAD, y, cw - PAD, cardBottom);

        RECT in = CardInner(L.p3Card, d);
        const int lblW = theme::Dp(46, d);
        SetRect(&L.p3FilterLbl, in.left, in.top, in.left + lblW, in.top + RH);
        MoveWindow(st->hP3Filter, in.left + lblW, in.top, in.right - in.left - lblW, RH, TRUE);
        int lt = in.top + RH + GAPT;
        int lh = in.bottom - lt;
        if (lh < theme::Dp(80, d)) lh = theme::Dp(80, d);
        const int listW = in.right - in.left;
        MoveWindow(st->hP3List, in.left, lt, listW, lh, TRUE);

        MoveWindow(st->hP3Sel, PAD, selY, fullW, selH, TRUE);

        int bx = PAD;
        MoveWindow(st->hP3Browse, bx, btnY, theme::Dp(104, d), BH, TRUE);
        bx += theme::Dp(104, d) + GAPT;
        MoveWindow(st->hP3Example, bx, btnY, theme::Dp(196, d), BH, TRUE);
        bx += theme::Dp(196, d) + GAPT;
        MoveWindow(st->hP3Skip, bx, btnY, theme::Dp(92, d), BH, TRUE);

        int w0 = theme::Dp(60, d);
        int w1 = theme::Dp(190, d);
        int total = listW - GetSystemMetrics(SM_CXVSCROLL) - theme::Dp(4, d);
        int w2 = total - w0 - w1;
        if (w2 < theme::Dp(80, d)) w2 = theme::Dp(80, d);
        ListView_SetColumnWidth(st->hP3List, 0, w0);
        ListView_SetColumnWidth(st->hP3List, 1, w1);
        ListView_SetColumnWidth(st->hP3List, 2, w2);
    }
}

// ---------------------------------------------------------------------------
// Painting
// ---------------------------------------------------------------------------

// One page-2 fact: a status dot, the fact's name, and the raw value in monospace, right
// aligned. Two of these, never merged.
void PaintFactRow(HDC dc, const RECT& row, int dpi, COLORREF dot, const std::wstring& label,
                  const std::wstring& tag) {
    const theme::Palette& pal = theme::P();
    const int dotR = theme::Dp(4, dpi);
    const int cy = (row.top + row.bottom) / 2;
    theme::DrawStatusDot(dc, row.left + dotR, cy, dotR, dot);

    RECT r = row;
    r.left += 2 * dotR + theme::Dp(8, dpi);
    if (r.left > r.right) r.left = r.right;

    SIZE sz = theme::MeasureText(dc, tag, theme::Font::MonoSmall, dpi);
    RECT tagR = r;
    tagR.left = r.right - sz.cx;
    if (tagR.left < r.left) tagR.left = r.left;
    theme::DrawText(dc, tagR, tag, theme::Font::MonoSmall, dpi, pal.textSecondary,
                    DT_RIGHT | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);

    RECT lbl = r;
    lbl.right = tagR.left - theme::Dp(8, dpi);
    if (lbl.right > lbl.left) {
        theme::DrawText(dc, lbl, label, theme::Font::UiStrong, dpi, pal.textPrimary, kLineFmt);
    }
}

void PaintWizard(WizardState* st, HDC dc) {
    const theme::Palette& pal = theme::P();
    const int d = st->dpi;
    const WizLayout& L = st->lay;
    const int capH = theme::Dp(18, d);
    const int rowH = theme::Dp(20, d);
    const int dotR = theme::Dp(4, d);
    const int GAPT = theme::Dp(theme::metric::kGapTight, d);
    const int page = (st->page >= 0 && st->page <= 2) ? st->page : 0;

    static const wchar_t* kHead[3] = {
        L"What we found", L"Windows Game Mode", L"Pick your first game"
    };
    static const wchar_t* kStep[3] = { L"Step 1 of 3", L"Step 2 of 3", L"Step 3 of 3" };

    theme::DrawText(dc, L.title, kHead[page], theme::Font::UiTitle, d, pal.textPrimary,
                    kLineFmt);
    theme::DrawText(dc, L.subtitle, kStep[page], theme::Font::UiSmall, d, pal.textDim,
                    kLineFmt);

    if (page == 0) {
        theme::DrawCard(dc, L.p1Card, d);
        RECT in = CardInner(L.p1Card, d);
        RECT cap = in;
        cap.bottom = cap.top + capH;

        // Detection confidence as a pill: the single fact that decides whether the masks
        // below can be trusted as they stand.
        std::wstring pillText = L"Confidence: None";
        COLORREF pillBg = pal.danger;
        COLORREF pillFg = pal.textOnAccent;
        if (st->topo->confidence == Confidence::High) {
            pillText = L"Confidence: High";
            pillBg = pal.good;
            pillFg = pal.appBg;
        } else if (st->topo->confidence == Confidence::Medium) {
            pillText = L"Confidence: Medium";
            pillBg = pal.warn;
            pillFg = pal.appBg;
        }
        SIZE sz = theme::MeasureText(dc, pillText, theme::Font::UiSmall, d);
        RECT pill = cap;
        pill.left = cap.right - (sz.cx + 2 * theme::Dp(10, d));
        if (pill.left < cap.left) pill.left = cap.left;
        theme::DrawPill(dc, pill, pillText, d, pillBg, pillFg);

        RECT capText = cap;
        capText.right = pill.left - GAPT;
        if (capText.right > capText.left) {
            theme::DrawText(dc, capText, L"Detection summary", theme::Font::UiSmall, d,
                            pal.textSecondary, kLineFmt);
        }

        theme::DrawCard(dc, L.p1MasksCard, d);
        RECT in2 = CardInner(L.p1MasksCard, d);
        RECT cap2 = in2;
        cap2.bottom = cap2.top + capH;
        theme::DrawText(dc, cap2, L"Masks derived for this CPU", theme::Font::UiSmall, d,
                        pal.textSecondary, kLineFmt);
    } else if (page == 1) {
        theme::DrawCard(dc, L.p2GmCard, d);
        RECT in = CardInner(L.p2GmCard, d);
        RECT row = in;
        row.bottom = row.top + rowH;
        COLORREF gmDot = pal.good;
        if (st->gameModeWarn) gmDot = pal.warn;
        else if (!st->env.gameModeKeyPresent) gmDot = pal.textDim;
        PaintFactRow(dc, row, d, gmDot, L"Windows Game Mode", Page2GameModeTag(st->env));

        theme::DrawCard(dc, L.p2VcCard, d);
        RECT in2 = CardInner(L.p2VcCard, d);
        RECT row2 = in2;
        row2.bottom = row2.top + rowH;
        COLORREF vcDot = pal.good;
        if (!st->env.amdVCacheServicePresent) vcDot = pal.textDim;
        else if (st->env.amdVCacheServiceRunning) vcDot = pal.warn;
        PaintFactRow(dc, row2, d, vcDot, L"AMD 3D V-Cache Performance Optimizer service",
                     Page2VCacheTag(st->env));

        theme::DrawText(dc, L.p2Note,
                        L"Game Optimizer never changes either setting, and neither one stops "
                        L"you continuing.",
                        theme::Font::UiSmall, d, pal.textDim,
                        DT_LEFT | DT_WORDBREAK | DT_NOPREFIX);
    } else {
        theme::DrawCard(dc, L.p3Card, d);
        theme::DrawText(dc, L.p3FilterLbl, L"Filter:", theme::Font::UiSmall, d,
                        pal.textSecondary, kLineFmt);
    }

    // Step indicator, bottom left.
    int cx = L.steps.left + dotR;
    const int cy = (L.steps.top + L.steps.bottom) / 2;
    for (int i = 0; i < 3; ++i) {
        theme::DrawStatusDot(dc, cx, cy, dotR, i == page ? pal.accent : pal.borderStrong);
        cx += theme::Dp(14, d);
    }
}

// The ListView header is system-drawn light chrome and there is no colour message for it, so
// it is custom drawn. Item text comes back out of the control, so the column captions stay
// wherever they were set.
LRESULT HeaderCustomDraw(WizardState* st, NMCUSTOMDRAW* cd) {
    if (cd->dwDrawStage == CDDS_PREPAINT) return CDRF_NOTIFYITEMDRAW;
    if (cd->dwDrawStage != CDDS_ITEMPREPAINT) return CDRF_DODEFAULT;

    const theme::Palette& pal = theme::P();
    RECT rc = cd->rc;
    FillSolid(cd->hdc, rc, pal.cardBgAlt);
    RECT line = rc;
    line.top = line.bottom - 1;
    FillSolid(cd->hdc, line, pal.border);

    HWND hdr = ListView_GetHeader(st->hP3List);
    wchar_t buf[128];
    buf[0] = L'\0';
    HDITEMW hdi;
    ZeroMemory(&hdi, sizeof(hdi));
    hdi.mask = HDI_TEXT;
    hdi.pszText = buf;
    hdi.cchTextMax = 128;
    if (hdr && Header_GetItem(hdr, static_cast<int>(cd->dwItemSpec), &hdi)) {
        RECT tr = rc;
        tr.left += theme::Dp(8, st->dpi);
        tr.right -= theme::Dp(6, st->dpi);
        if (tr.right > tr.left) {
            theme::DrawText(cd->hdc, tr, buf, theme::Font::UiSmall, st->dpi,
                            pal.textSecondary, kLineFmt);
        }
    }
    return CDRF_SKIPDEFAULT;
}

// Applies the page-3 choice into the config. Called once, on Finish.
void CommitChoice(WizardState* st) {
    Config& cfg = *st->cfg;
    if (st->useExample) {
        for (size_t i = 0; i < cfg.profiles.size(); ++i) {
            if (IEquals(cfg.profiles[i].name, L"Overwatch")) {
                cfg.profiles[i].enabled = true;
                return;
            }
        }
        Profile p;
        p.name = L"Overwatch";
        p.enabled = true;
        p.game = L"Overwatch.exe";
        p.gameMask = st->topo->defaultGameMask;
        p.heavyMask = st->topo->defaultHeavyMask;
        cfg.profiles.push_back(p);
        return;
    }
    if (st->pickedGame.empty()) return;

    for (size_t i = 0; i < cfg.profiles.size(); ++i) {
        if (IEquals(cfg.profiles[i].game, st->pickedGame)) {
            cfg.profiles[i].enabled = true;
            return;
        }
    }
    std::wstring base = BaseName(st->pickedGame);
    std::wstring name = base;
    size_t dot = name.find_last_of(L'.');
    if (dot != std::wstring::npos && dot > 0) name = name.substr(0, dot);
    if (name.empty()) name = L"Game";

    Profile p;
    p.name = name;
    p.enabled = true;
    p.game = st->pickedGame;
    p.gameMask = st->topo->defaultGameMask;
    p.heavyMask = st->topo->defaultHeavyMask;
    cfg.profiles.push_back(p);
}

void FinishWizard(WizardState* st, bool completed) {
    st->cfg->firstRunDone = true;   // every exit path, including the close button
    if (completed) CommitChoice(st);
    st->completed = completed;
    st->done = true;
}

LRESULT CALLBACK WizardProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    WizardState* st = reinterpret_cast<WizardState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg) {
        case WM_NCCREATE: {
            CREATESTRUCTW* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                              reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
            break;
        }
        case WM_CREATE: {
            st = reinterpret_cast<WizardState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
            st->dpi = DpiOf(hwnd);
            theme::ApplyDarkFrame(hwnd);

            const DWORD kRoText = ES_MULTILINE | ES_READONLY | WS_VSCROLL | ES_AUTOVSCROLL;
            // No WS_EX_CLIENTEDGE anywhere: the sunken 3D edge is light-theme chrome that
            // cannot be recoloured. The card border draws the boundary instead.

            // ---- page 1 ----
            st->hP1Sum = Mk(hwnd, L"EDIT", Page1Summary(*st->topo).c_str(),
                            kRoText | WS_TABSTOP, -1);
            st->hP1Map = CreateWindowExW(0, kCoreMapClass, L"", WS_CHILD, 0, 0, 10, 10,
                                         hwnd,
                                         reinterpret_cast<HMENU>(
                                             static_cast<UINT_PTR>(IDC_FR_MAP)),
                                         GetModuleHandleW(nullptr), nullptr);
            if (st->hP1Map) {
                CoreMapSetTopology(st->hP1Map, st->topo);
                CoreMapSetEditable(st->hP1Map, false);
            } else {
                // GetLastError first: nothing may run between the failed create and this read.
                // Page 1 asks the user to confirm the detected topology, so a blank rectangle
                // where the map should be is worse here than anywhere else in the app.
                const DWORD gle = GetLastError();
                LogLine(L"firstrun: the core map control could not be created "
                        L"(class %s), gle=%lu", kCoreMapClass, gle);
                st->hP1MapFail = Mk(hwnd, L"STATIC",
                                    L"The core map could not be created, so the per-core view "
                                    L"is not shown. The summary and mask text below still "
                                    L"describe what was detected. See GameOptimizer.log in the "
                                    L"config folder.",
                                    SS_LEFT, -1);
            }
            st->hP1Masks = Mk(hwnd, L"EDIT", Page1Masks(*st->topo).c_str(),
                              kRoText | WS_TABSTOP, -1);
            st->hP1Looks = Mk(hwnd, L"BUTTON", L"Looks right",
                              BS_OWNERDRAW | WS_TABSTOP, IDC_FR_LOOKS);
            st->hP1Edit = Mk(hwnd, L"BUTTON", L"Let me edit",
                             BS_OWNERDRAW | WS_TABSTOP, IDC_FR_EDIT);

            // ---- page 2 ----
            st->env = ProbeEnvironment();
            st->hP2Gm = Mk(hwnd, L"EDIT",
                           Page2GameModeText(st->env, *st->topo, st->gameModeWarn).c_str(),
                           kRoText | WS_TABSTOP, -1);
            st->hP2Open = Mk(hwnd, L"BUTTON", L"Open Game Mode settings",
                             BS_OWNERDRAW | WS_TABSTOP, IDC_FR_OPENGM);
            st->hP2VCache = Mk(hwnd, L"EDIT", Page2VCacheText(st->env).c_str(),
                               kRoText | WS_TABSTOP, -1);

            // ---- page 3 ----
            st->hP3Filter = Mk(hwnd, L"EDIT", L"", ES_AUTOHSCROLL | WS_TABSTOP,
                               IDC_FR_FILTER);
            st->hP3List = Mk(hwnd, WC_LISTVIEWW, L"",
                             LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS | WS_TABSTOP,
                             IDC_FR_LIST);
            st->hP3Browse = Mk(hwnd, L"BUTTON", L"Browse...", BS_OWNERDRAW | WS_TABSTOP,
                               IDC_FR_BROWSE);
            st->hP3Example = Mk(hwnd, L"BUTTON", L"Use the Overwatch example",
                                BS_OWNERDRAW | WS_TABSTOP, IDC_FR_EXAMPLE);
            st->hP3Skip = Mk(hwnd, L"BUTTON", L"Skip", BS_OWNERDRAW | WS_TABSTOP,
                             IDC_FR_SKIP);
            st->hP3Sel = Mk(hwnd, L"STATIC", L"", SS_LEFT, -1);

            ListView_SetExtendedListViewStyle(st->hP3List,
                                              LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
            // The three colour messages are the whole of the ListView's dark mode; its
            // scrollbar stays system-drawn, which theme.h states as an accepted trade.
            ListView_SetBkColor(st->hP3List, theme::P().inputBg);
            ListView_SetTextBkColor(st->hP3List, theme::P().inputBg);
            ListView_SetTextColor(st->hP3List, theme::P().textPrimary);
            LVCOLUMNW col;
            ZeroMemory(&col, sizeof(col));
            col.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
            wchar_t h0[] = L"PID";
            col.pszText = h0; col.cx = 60; col.iSubItem = 0;
            ListView_InsertColumn(st->hP3List, 0, &col);
            wchar_t h1[] = L"Process";
            col.pszText = h1; col.cx = 180; col.iSubItem = 1;
            ListView_InsertColumn(st->hP3List, 1, &col);
            wchar_t h2[] = L"Window title";
            col.pszText = h2; col.cx = 240; col.iSubItem = 2;
            ListView_InsertColumn(st->hP3List, 2, &col);

            // ---- footer ----
            st->hBack = Mk(hwnd, L"BUTTON", L"< Back", BS_OWNERDRAW | WS_TABSTOP,
                           IDC_FR_BACK);
            st->hNext = Mk(hwnd, L"BUTTON", L"Next >", BS_OWNERDRAW | WS_TABSTOP,
                           IDC_FR_NEXT);
            ShowWindow(st->hBack, SW_SHOW);
            ShowWindow(st->hNext, SW_SHOW);

            ApplyFonts(st);

            st->procs = EnumerateProcesses();
            FillProcList(st);
            UpdateSelectionLine(st);
            WizardLayout(st, hwnd);
            ShowPage(st, hwnd);
            return 0;
        }
        case WM_SIZE:
            if (st) { WizardLayout(st, hwnd); ShowPage(st, hwnd); }
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
            if (dc && st) PaintWizard(st, dc);
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_DRAWITEM: {
            const DRAWITEMSTRUCT* di = reinterpret_cast<const DRAWITEMSTRUCT*>(lp);
            if (di && di->CtlType == ODT_BUTTON) {
                // Exactly one primary per page: the footer's Next / Finish.
                theme::ButtonKind kind = theme::ButtonKind::Secondary;
                if (di->CtlID == IDC_FR_NEXT) {
                    kind = theme::ButtonKind::Primary;
                } else if (di->CtlID == IDC_FR_BACK || di->CtlID == IDC_FR_SKIP ||
                           di->CtlID == IDC_FR_EDIT) {
                    kind = theme::ButtonKind::Ghost;
                }
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
            if (msg == WM_CTLCOLORSTATIC && st && IsReadOnlyEdit(st, ctl)) m = WM_CTLCOLOREDIT;
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
            ApplyFonts(st);
            const RECT* nr = reinterpret_cast<const RECT*>(lp);
            SetWindowPos(hwnd, nullptr, nr->left, nr->top, nr->right - nr->left,
                         nr->bottom - nr->top, SWP_NOZORDER | SWP_NOACTIVATE);
            WizardLayout(st, hwnd);
            ShowPage(st, hwnd);
            return 0;
        }
        case WM_NOTIFY: {
            if (!st) break;
            const NMHDR* nh = reinterpret_cast<const NMHDR*>(lp);
            if (st->hP3List && nh->code == NM_CUSTOMDRAW &&
                nh->hwndFrom == ListView_GetHeader(st->hP3List)) {
                return HeaderCustomDraw(st, reinterpret_cast<NMCUSTOMDRAW*>(lp));
            }
            if (nh->idFrom == IDC_FR_LIST &&
                (nh->code == NM_DBLCLK || nh->code == LVN_ITEMCHANGED)) {
                int sel = ListView_GetNextItem(st->hP3List, -1, LVNI_SELECTED);
                if (sel >= 0 && sel < static_cast<int>(st->shown.size())) {
                    st->pickedGame = st->procs[st->shown[static_cast<size_t>(sel)]].name;
                    st->useExample = false;
                    UpdateSelectionLine(st);
                }
                if (nh->code == NM_DBLCLK) {
                    FinishWizard(st, true);
                    return 0;
                }
            }
            break;
        }
        case WM_COMMAND: {
            if (!st) break;
            const int id = LOWORD(wp);
            const int code = HIWORD(wp);
            switch (id) {
                case IDC_FR_LOOKS:
                    st->wantEdit = false;
                    st->page = 1;
                    ShowPage(st, hwnd);
                    return 0;
                case IDC_FR_EDIT:
                    // Remembered, not acted on: the core map opens in Settings once the
                    // wizard has closed, so the user still sees pages 2 and 3.
                    st->wantEdit = true;
                    st->page = 1;
                    ShowPage(st, hwnd);
                    return 0;
                case IDC_FR_OPENGM:
                    OpenGameModeSettings();
                    return 0;
                case IDC_FR_FILTER:
                    if (code == EN_CHANGE) FillProcList(st);
                    return 0;
                case IDC_FR_BROWSE: {
                    std::wstring path = BrowseForExe(hwnd);
                    if (!path.empty()) {
                        st->pickedGame = path;
                        st->useExample = false;
                        UpdateSelectionLine(st);
                    }
                    return 0;
                }
                case IDC_FR_EXAMPLE:
                    st->useExample = true;
                    st->pickedGame.clear();
                    UpdateSelectionLine(st);
                    return 0;
                case IDC_FR_SKIP:
                    st->useExample = false;
                    st->pickedGame.clear();
                    FinishWizard(st, true);
                    return 0;
                case IDC_FR_BACK:
                    if (st->page > 0) { --st->page; ShowPage(st, hwnd); }
                    return 0;
                case IDC_FR_NEXT:
                case IDOK:
                    if (st->page < 2) { ++st->page; ShowPage(st, hwnd); }
                    else FinishWizard(st, true);
                    return 0;
                case IDCANCEL:
                    FinishWizard(st, false);
                    return 0;
                default:
                    break;
            }
            break;
        }
        case WM_CLOSE:
            if (st) FinishWizard(st, false);
            return 0;
        case WM_NCDESTROY:
            if (st) {
                if (st->bgBrush) { DeleteObject(st->bgBrush); st->bgBrush = nullptr; }
            }
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
            break;
        default:
            break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void RegisterWizardClass() {
    static bool done = false;
    if (done) return;
    WNDCLASSEXW wc;
    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WizardProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;   // WM_ERASEBKGND paints appBg; no light flash on show
    wc.lpszClassName = kWizardClass;
    RegisterClassExW(&wc);
    done = true;
}

}  // namespace

bool RunFirstRunWizard(HWND owner, Config& cfg, const Topology& topo) {
    EnsureCommonControls();
    RegisterWizardClass();

    WizardState st;
    st.cfg = &cfg;
    st.topo = &topo;

    int dpi = DpiOf(owner ? owner : GetDesktopWindow());
    RECT want = { 0, 0, theme::Dp(700, dpi), theme::Dp(620, dpi) };
    AdjustWindowRectEx(&want, WS_OVERLAPPEDWINDOW, FALSE, 0);
    int w = want.right - want.left;
    int h = want.bottom - want.top;
    RECT work = { 0, 0, 0, 0 };
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    int availH = work.bottom - work.top;
    if (availH > 0 && h > availH) h = availH;
    int x = work.left + ((work.right - work.left) - w) / 2;
    int y = work.top + ((work.bottom - work.top) - h) / 2;
    if (x < work.left) x = work.left;
    if (y < work.top) y = work.top;

    HWND hwnd = CreateWindowExW(0, kWizardClass, L"Game Optimizer - 1 of 3: What we found",
                                WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, x, y, w, h,
                                owner, nullptr, GetModuleHandleW(nullptr), &st);
    if (!hwnd) {
        // The window could not be created, so the user was never asked. Setting the flag
        // anyway is deliberate: an unshowable wizard must not block every future launch.
        cfg.firstRunDone = true;
        return false;
    }

    if (owner) EnableWindow(owner, FALSE);
    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);
    SetForegroundWindow(hwnd);

    MSG msg;
    while (!st.done && IsWindow(hwnd)) {
        BOOL got = GetMessageW(&msg, nullptr, 0, 0);
        if (got == 0) { PostQuitMessage(static_cast<int>(msg.wParam)); break; }
        if (got == -1) break;
        if (!IsDialogMessageW(hwnd, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    if (owner) EnableWindow(owner, TRUE);
    if (IsWindow(hwnd)) DestroyWindow(hwnd);
    if (owner) SetActiveWindow(owner);

    cfg.firstRunDone = true;   // belt and braces: true on every path out of here

    std::wstring err;
    if (!SaveConfig(GetConfigPath(), cfg, &err)) {
        LogLine(L"firstrun: SaveConfig failed: %s", err.c_str());
    }

    // "Let me edit" on page 1: ask the host to open Settings, where the core map is
    // editable. Done by posting the tray's own Settings command so this file does not
    // need an Engine reference it was never given.
    if (st.wantEdit && owner && IsWindow(owner)) {
        PostMessageW(owner, WM_COMMAND, MAKEWPARAM(IDM_SETTINGS, 0), 0);
    }
    return st.completed;
}

}  // namespace cd
