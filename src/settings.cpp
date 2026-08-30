// Game Optimizer - the Settings window, the running-process picker and the exe browser.
//
// No .rc dialog templates: every window here is built with CreateWindowExW out of the
// stock control classes, every coordinate is scaled by GetDpiForWindow()/96 and the font
// comes from SPI_GETNONCLIENTMETRICS, so 150% desktop scaling stays legible.
//
// The Settings window is MODELESS and single-instance. Because it is modeless the host
// message loop owns dispatch, and this file cannot assume the host calls
// IsDialogMessageW for it - so it installs a thread-local WH_GETMESSAGE hook that does
// exactly that for its own children and nothing else. Tab/Enter/Escape then behave.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <tlhelp32.h>

#include <algorithm>
#include <map>
#include <string>
#include <vector>

#include "ui.h"
#include "util.h"
#include "applier.h"
#include "config.h"
#include "engine.h"
#include "procwatch.h"
#include "settings_environment.h"
#include "settings_heavy_order.h"
#include "settings_merge.h"
#include "settings_warning.h"
#include "sponsor.h"
#include "theme.h"
#include "topology.h"
#include "webview_host.h"

namespace cd {

namespace {

// ---------------------------------------------------------------------------
// Shared plumbing
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

HFONT MakeUiFont(int dpi, bool bold) {
    LOGFONTW lf;
    ZeroMemory(&lf, sizeof(lf));
    NONCLIENTMETRICSW ncm;
    ZeroMemory(&ncm, sizeof(ncm));
    ncm.cbSize = sizeof(ncm);
    if (SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0)) {
        lf = ncm.lfMessageFont;
    } else {
        lf.lfHeight = -12;
        lf.lfCharSet = DEFAULT_CHARSET;
        lstrcpynW(lf.lfFaceName, L"Segoe UI", LF_FACESIZE);
    }
    UINT sys = GetDpiForSystem();
    if (sys == 0) sys = 96;
    lf.lfHeight = MulDiv(lf.lfHeight, dpi, static_cast<int>(sys));
    if (bold) lf.lfWeight = FW_SEMIBOLD;
    return CreateFontIndirectW(&lf);
}

struct FontApply { HFONT f; };

BOOL CALLBACK ApplyFontProc(HWND child, LPARAM lp) {
    const FontApply* fa = reinterpret_cast<const FontApply*>(lp);
    SendMessageW(child, WM_SETFONT, reinterpret_cast<WPARAM>(fa->f), TRUE);
    return TRUE;
}

HWND Mk(HWND parent, const wchar_t* cls, const wchar_t* text, DWORD style, int id,
        DWORD exStyle = 0) {
    return CreateWindowExW(exStyle, cls, text, WS_CHILD | WS_VISIBLE | style,
                           0, 0, 10, 10, parent,
                           reinterpret_cast<HMENU>(static_cast<UINT_PTR>(id)),
                           reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(parent, GWLP_HINSTANCE)),
                           nullptr);
}

std::wstring GetText(HWND h) {
    if (!h) return std::wstring();
    int n = GetWindowTextLengthW(h);
    if (n <= 0) return std::wstring();
    std::vector<wchar_t> buf(static_cast<size_t>(n) + 1, L'\0');
    GetWindowTextW(h, buf.data(), n + 1);
    return std::wstring(buf.data());
}

bool IsChecked(HWND h) {
    return h && SendMessageW(h, BM_GETCHECK, 0, 0) == BST_CHECKED;
}

void SetChecked(HWND h, bool on) {
    if (h) SendMessageW(h, BM_SETCHECK, on ? BST_CHECKED : BST_UNCHECKED, 0);
}

// SplitLines / JoinLines were removed with the heavy-apps EDIT they existed for. The heavy
// list is a LISTBOX now, so its two directions are HeavyItems / SetHeavyItems and there is no
// CRLF blob to split any more.

// ---------------------------------------------------------------------------
// Running process enumeration (shared by the picker)
// ---------------------------------------------------------------------------

struct ProcRow {
    DWORD pid = 0;
    std::wstring name;
    std::wstring title;
    bool hasWindow = false;
    // Machine-wide CPU%, i.e. Task Manager's convention: 100.0 is every logical processor.
    // Filled from a ProcessSnapshot DELTA - it is 0 for every row until a second snapshot
    // exists, which is why the picker prints "-" rather than "0%" until then.
    double cpu = 0.0;
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

// CPU DESCENDING is the primary key now: the processes a user opens this dialog to find are
// the busy ones, and burying them under an alphabetical list of services is what made the
// list hard to use. Everything else is unchanged and becomes the tie-break - which also means
// the FIRST second, when no delta has been measured yet and every figure is exactly 0, still
// orders the list windowed-first exactly as it did before.
bool ProcRowLess(const ProcRow& a, const ProcRow& b) {
    if (a.cpu != b.cpu) return a.cpu > b.cpu;
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
// PickRunningProcess - a modal LISTVIEW with a live filter
// ---------------------------------------------------------------------------

enum : int {
    IDC_PK_FILTER = 1200,
    IDC_PK_LIST,
    IDC_PK_HINT
};

struct PickerState {
    std::vector<ProcRow> all;
    std::vector<size_t>  shown;
    HWND hFilterLabel = nullptr;
    HWND hFilter = nullptr;
    HWND hList = nullptr;
    HWND hHint = nullptr;
    HWND hOk = nullptr;
    HWND hCancel = nullptr;
    HFONT font = nullptr;
    int dpi = 96;
    std::wstring result;
    bool done = false;

    // ---- Live CPU% -----------------------------------------------------------
    // The SAME path the Heavy apps meters use in the Settings window
    // (SettingsState::cpuSnap / RefreshCpuTable): one ProcessSnapshot per interval, and the
    // percentage is the CPU-time delta between consecutive snapshots. That path opens
    // processes with PROCESS_QUERY_LIMITED_INFORMATION and nothing wider.
    ProcessSnapshot snap;
    bool tookOne = false;   // a snapshot exists, so the next Take has a predecessor
    bool haveCpu = false;   // a DELTA exists, so the figures mean something
};

const wchar_t kPickerClass[] = L"GameOptimizerPicker";
const UINT_PTR kPickerTimer = 1;
const int kPickerSampleMs = 1000;

enum : int { PK_COL_PID = 0, PK_COL_NAME, PK_COL_CPU, PK_COL_TITLE, PK_COL_COUNT };

// The pid currently selected, or 0. Read BEFORE re-sampling: `shown` indexes the CURRENT
// `all`, and re-sampling replaces `all` wholesale.
DWORD PickerSelectedPid(const PickerState* st) {
    if (!st->hList) return 0;
    const int sel = ListView_GetNextItem(st->hList, -1, LVNI_SELECTED);
    if (sel < 0 || sel >= static_cast<int>(st->shown.size())) return 0;
    const size_t i = st->shown[static_cast<size_t>(sel)];
    if (i >= st->all.size()) return 0;
    return st->all[i].pid;
}

// Re-enumerates the process list and re-samples CPU. The FIRST call has no predecessor, so
// every cpuPercent in it is 0 BY CONSTRUCTION - that is a real "not measured yet", not a
// claim that nothing is busy, which is why haveCpu only becomes true on the second call and
// the column prints "-" until then.
void PickerSample(PickerState* st) {
    ProcessSnapshot next;
    // The last argument is the auto-pin threshold, which only drives ProcInfo::
    // aboveThresholdTicks. Nothing here reads that, so 100 is passed rather than a figure
    // this dialog would have to invent.
    if (!next.Take(st->tookOne ? &st->snap : nullptr, kPickerSampleMs,
                   GetTotalLogicalProcessors(), 100)) {
        return;   // transient enumeration failure; keep the previous table rather than zeroing
    }
    const bool hadPrev = st->tookOne;
    st->snap = next;
    st->tookOne = true;
    if (hadPrev) st->haveCpu = true;

    // The window titles still come from EnumWindows, which a ProcessSnapshot does not carry.
    std::vector<ProcRow> rows = EnumerateProcesses();
    if (rows.empty()) return;
    const std::map<DWORD, ProcInfo>& all = st->snap.All();
    for (size_t i = 0; i < rows.size(); ++i) {
        std::map<DWORD, ProcInfo>::const_iterator it = all.find(rows[i].pid);
        if (it == all.end()) continue;
        double v = it->second.cpuPercent;
        if (v < 0.0) v = 0.0;
        if (v > 100.0) v = 100.0;
        rows[i].cpu = v;
    }
    std::sort(rows.begin(), rows.end(), ProcRowLess);
    st->all.swap(rows);
}

// `keepPid` is the process to re-select afterwards, from PickerSelectedPid. The list re-sorts
// by CPU on every tick, so a row index means something different each second and restoring
// the ROW would move the selection onto a stranger.
void PickerFill(PickerState* st, DWORD keepPid) {
    std::wstring flt = ToLower(Trim(GetText(st->hFilter)));
    SendMessageW(st->hList, WM_SETREDRAW, FALSE, 0);
    ListView_DeleteAllItems(st->hList);
    st->shown.clear();
    wchar_t cell[512];
    int reselect = -1;
    for (size_t i = 0; i < st->all.size(); ++i) {
        const ProcRow& r = st->all[i];
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
        it.iSubItem = PK_COL_PID;
        it.pszText = cell;
        ListView_InsertItem(st->hList, &it);
        lstrcpynW(cell, r.name.c_str(), 512);
        ListView_SetItemText(st->hList, row, PK_COL_NAME, cell);
        // NOT MEASURED YET AND IDLE ARE DIFFERENT STATES. Before the second snapshot exists
        // there is no delta at all, and printing "0%" would assert every process on the
        // machine is quiet - which is a claim, not an observation.
        if (st->haveCpu)
            wsprintfW(cell, L"%d%%", static_cast<int>(r.cpu + 0.5));
        else
            lstrcpynW(cell, L"-", 512);
        ListView_SetItemText(st->hList, row, PK_COL_CPU, cell);
        lstrcpynW(cell, r.title.c_str(), 512);
        ListView_SetItemText(st->hList, row, PK_COL_TITLE, cell);
        if (keepPid != 0 && r.pid == keepPid) reselect = row;
        st->shown.push_back(i);
    }
    if (reselect >= 0) {
        ListView_SetItemState(st->hList, reselect, LVIS_SELECTED | LVIS_FOCUSED,
                              LVIS_SELECTED | LVIS_FOCUSED);
        ListView_EnsureVisible(st->hList, reselect, FALSE);
    }
    SendMessageW(st->hList, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(st->hList, nullptr, TRUE);

    wchar_t hint[160];
    wsprintfW(hint,
              st->haveCpu
                  ? L"%d of %d processes shown, busiest first. CPU%% is machine-wide."
                  : L"%d of %d processes shown. CPU%% appears after the first refresh.",
              static_cast<int>(st->shown.size()), static_cast<int>(st->all.size()));
    SetWindowTextW(st->hHint, hint);
}

void PickerLayout(PickerState* st, HWND hwnd) {
    RECT rc;
    GetClientRect(hwnd, &rc);
    const int cw = rc.right - rc.left;
    const int ch = rc.bottom - rc.top;
    const int dpi = st->dpi;
    const int M = MulDiv(10, dpi, 96);
    const int RH = MulDiv(23, dpi, 96);
    const int BW = MulDiv(90, dpi, 96);
    const int LBLW = MulDiv(44, dpi, 96);

    int y = M;
    MoveWindow(st->hFilterLabel, M, y + MulDiv(4, dpi, 96), LBLW, RH, TRUE);
    MoveWindow(st->hFilter, M + LBLW, y, cw - 2 * M - LBLW, RH, TRUE);
    y += RH + MulDiv(6, dpi, 96);

    int footer = RH + M;
    int hintH = MulDiv(18, dpi, 96);
    int listH = ch - y - footer - hintH - MulDiv(6, dpi, 96) - M;
    if (listH < MulDiv(60, dpi, 96)) listH = MulDiv(60, dpi, 96);
    MoveWindow(st->hList, M, y, cw - 2 * M, listH, TRUE);
    y += listH + MulDiv(3, dpi, 96);
    MoveWindow(st->hHint, M, y, cw - 2 * M, hintH, TRUE);

    int by = ch - M - RH;
    MoveWindow(st->hCancel, cw - M - BW, by, BW, RH, TRUE);
    MoveWindow(st->hOk, cw - M - 2 * BW - MulDiv(6, dpi, 96), by, BW, RH, TRUE);

    // Give the title column whatever is left over. CPU% is a fixed narrow right-aligned
    // column: it holds at most "100%", and letting it stretch would push the title off.
    int w0 = MulDiv(64, dpi, 96);
    int w1 = MulDiv(190, dpi, 96);
    int wc = MulDiv(64, dpi, 96);
    int total = cw - 2 * M - GetSystemMetrics(SM_CXVSCROLL) - MulDiv(4, dpi, 96);
    int w2 = total - w0 - w1 - wc;
    if (w2 < MulDiv(80, dpi, 96)) w2 = MulDiv(80, dpi, 96);
    ListView_SetColumnWidth(st->hList, PK_COL_PID, w0);
    ListView_SetColumnWidth(st->hList, PK_COL_NAME, w1);
    ListView_SetColumnWidth(st->hList, PK_COL_CPU, wc);
    ListView_SetColumnWidth(st->hList, PK_COL_TITLE, w2);
}

void PickerAccept(PickerState* st) {
    int sel = ListView_GetNextItem(st->hList, -1, LVNI_SELECTED);
    if (sel < 0 || sel >= static_cast<int>(st->shown.size())) return;
    st->result = st->all[st->shown[static_cast<size_t>(sel)]].name;
    st->done = true;
}

// ---------------------------------------------------------------------------
// Dark theming for the picker
// ---------------------------------------------------------------------------
// Everything else in this app goes dark through WM_CTLCOLOR* and owner-draw. The LISTVIEW
// answers NEITHER of those: it ignores WM_CTLCOLOR entirely, and its HEADER is a separate
// child window parented by the LIST VIEW, not by this dialog, so a notification the header
// sends never reaches here on its own. Three documented mechanisms cover it:
//
//   1. LVM_SETBKCOLOR / LVM_SETTEXTBKCOLOR / LVM_SETTEXTCOLOR for the list's own surface,
//      including the empty band below the last row.
//   2. NM_CUSTOMDRAW on the LIST for the rows. Colour messages alone are not enough: the
//      theme engine draws a SELECTED row as a light bar and pays no attention to the text
//      background colour, so near-white text on it is unreadable. The row is drawn here and
//      the default is skipped.
//   3. NM_CUSTOMDRAW on the HEADER, reached by subclassing the list view purely to intercept
//      that one notification on its way to the list's own window procedure. Everything else
//      passes straight through, so column sizing, sorting clicks and HDN_* are untouched.
//
// The undocumented uxtheme ordinals (SetPreferredAppMode / AllowDarkModeForWindow) are NOT
// used, for the reason recorded at the top of src\theme.h: they shift between Windows builds.
// The accepted cost is that the list's SCROLL BAR stays light - nothing documented recolours
// it - as does the report-mode "unfolding" tooltip for a truncated cell.
constexpr UINT_PTR kPickerListSubclassId = 3;

// CPU% is a number and is right-aligned in the header and in the rows alike. This mirrors the
// LVCFMT_RIGHT the column was created with; the custom draw does the alignment itself now, so
// the two have to agree.
bool PickerColumnIsRight(int col) { return col == PK_COL_CPU; }

// Fills rc with one solid colour. Every fill here is a flat colour, so a cached pen/brush
// layer would buy nothing over the two calls.
void PickerFillRect(HDC dc, const RECT& rc, COLORREF colour) {
    HBRUSH b = CreateSolidBrush(colour);
    if (!b) return;
    FillRect(dc, &rc, b);
    DeleteObject(b);
}

void PickerDrawHeaderItem(HWND hHdr, const NMCUSTOMDRAW* cd, int dpi) {
    if (!hHdr || !cd || !cd->hdc) return;
    const RECT rc = cd->rc;
    if (rc.right <= rc.left || rc.bottom <= rc.top) return;

    const theme::Palette& p = theme::P();
    const bool pressed = (cd->uItemState & CDIS_SELECTED) != 0;
    const bool hot     = (cd->uItemState & CDIS_HOT) != 0;
    PickerFillRect(cd->hdc, rc, pressed ? p.border : (hot ? p.borderStrong : p.cardBgAlt));

    // A hairline on the right edge so the columns still read as columns once the system's
    // own divider is gone with the default drawing.
    const int inset = theme::Dp(6, dpi);
    if (rc.bottom - rc.top > 2 * inset) {
        RECT d = { rc.right - 1, rc.top + inset, rc.right, rc.bottom - inset };
        PickerFillRect(cd->hdc, d, p.border);
    }

    wchar_t buf[128];
    buf[0] = L'\0';
    HDITEMW hi;
    ZeroMemory(&hi, sizeof(hi));
    hi.mask = HDI_TEXT | HDI_FORMAT;
    hi.pszText = buf;
    hi.cchTextMax = 128;
    UINT fmt = DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS;
    if (Header_GetItem(hHdr, static_cast<int>(cd->dwItemSpec), &hi)) {
        const int just = hi.fmt & HDF_JUSTIFYMASK;
        if (just == HDF_RIGHT) fmt |= DT_RIGHT;
        else if (just == HDF_CENTER) fmt |= DT_CENTER;
    }
    if (buf[0] == L'\0') return;

    const int pad = theme::Dp(6, dpi);
    RECT tr = rc;
    tr.left += pad;
    tr.right -= pad;
    if (tr.right <= tr.left) return;
    const int saved = SaveDC(cd->hdc);
    HFONT f = reinterpret_cast<HFONT>(SendMessageW(hHdr, WM_GETFONT, 0, 0));
    if (f) SelectObject(cd->hdc, f);
    SetBkMode(cd->hdc, TRANSPARENT);
    SetTextColor(cd->hdc, p.textSecondary);
    DrawTextW(cd->hdc, buf, -1, &tr, fmt);
    if (saved) RestoreDC(cd->hdc, saved);
}

// Subclass on the LIST VIEW. Its only job is the header's NM_CUSTOMDRAW; every other message
// goes to DefSubclassProc untouched.
LRESULT CALLBACK PickerListProc(HWND h, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR id,
                                DWORD_PTR ref) {
    (void)ref;
    if (msg == WM_NOTIFY) {
        const NMHDR* nh = reinterpret_cast<const NMHDR*>(lp);
        HWND hHdr = ListView_GetHeader(h);
        if (nh && hHdr && nh->hwndFrom == hHdr && nh->code == NM_CUSTOMDRAW) {
            const NMCUSTOMDRAW* cd = reinterpret_cast<const NMCUSTOMDRAW*>(lp);
            const theme::Palette& p = theme::P();
            RECT client;
            GetClientRect(hHdr, &client);
            switch (cd->dwDrawStage) {
                case CDDS_PREPAINT:
                    // The band to the RIGHT of the last column belongs to no item, so no
                    // item-level draw can ever reach it. The whole control is painted first
                    // and the items then paint over their own share.
                    if (cd->hdc) PickerFillRect(cd->hdc, client, p.cardBgAlt);
                    return CDRF_NOTIFYITEMDRAW | CDRF_NOTIFYPOSTPAINT;
                case CDDS_ITEMPREPAINT:
                    PickerDrawHeaderItem(hHdr, cd, DpiOf(h));
                    return CDRF_SKIPDEFAULT;
                case CDDS_POSTPAINT: {
                    if (!cd->hdc) return CDRF_DODEFAULT;
                    // Re-cover the right-hand band AFTER the items, because the default
                    // painting of that area happens between the two stages, and lay a 1px
                    // rule under the whole strip.
                    const int n = Header_GetItemCount(hHdr);
                    RECT last = { 0, 0, 0, 0 };
                    if (n > 0 && Header_GetItemRect(hHdr, n - 1, &last) &&
                        last.right < client.right) {
                        RECT gap = { last.right, client.top, client.right, client.bottom };
                        PickerFillRect(cd->hdc, gap, p.cardBgAlt);
                    }
                    RECT rule = { client.left, client.bottom - 1, client.right,
                                  client.bottom };
                    PickerFillRect(cd->hdc, rule, p.border);
                    return CDRF_DODEFAULT;
                }
                default:
                    return CDRF_DODEFAULT;
            }
        }
    }
    if (msg == WM_NCDESTROY) RemoveWindowSubclass(h, PickerListProc, id);
    return DefSubclassProc(h, msg, wp, lp);
}

// One row, all four columns. The row rectangle and the per-column rectangles come from the
// control itself, so a column the user has resized or a horizontally scrolled list stays
// correct without this code tracking either.
void PickerDrawRow(PickerState* st, const NMLVCUSTOMDRAW* cd) {
    if (!st || !st->hList || !cd || !cd->nmcd.hdc) return;
    HWND hList = st->hList;
    HDC dc = cd->nmcd.hdc;
    const int item = static_cast<int>(cd->nmcd.dwItemSpec);
    RECT row;
    if (!ListView_GetItemRect(hList, item, &row, LVIR_BOUNDS)) return;

    const theme::Palette& p = theme::P();
    const UINT state = ListView_GetItemState(hList, item, LVIS_SELECTED | LVIS_FOCUSED);
    const bool selected = (state & LVIS_SELECTED) != 0;
    PickerFillRect(dc, row, selected ? p.cardBgAlt : p.cardBg);

    const int pad = theme::Dp(6, st->dpi);
    const int saved = SaveDC(dc);
    HFONT f = reinterpret_cast<HFONT>(SendMessageW(hList, WM_GETFONT, 0, 0));
    if (f) SelectObject(dc, f);
    SetBkMode(dc, TRANSPARENT);
    wchar_t buf[512];
    for (int c = 0; c < PK_COL_COUNT; ++c) {
        RECT cr;
        if (c == 0) {
            // LVM_GETSUBITEMRECT with iSubItem 0 answers with the WHOLE row, so column 0 is
            // the row rect clipped to its own width instead.
            cr = row;
            cr.right = cr.left + ListView_GetColumnWidth(hList, 0);
        } else if (!ListView_GetSubItemRect(hList, item, c, LVIR_BOUNDS, &cr)) {
            continue;
        }
        if (cr.right - cr.left <= 2 * pad) continue;
        buf[0] = L'\0';
        ListView_GetItemText(hList, item, c, buf, 512);
        if (buf[0] == L'\0') continue;
        RECT tr = cr;
        tr.left += pad;
        tr.right -= pad;
        // The window title is context, not the identity the user is picking, so it stays a
        // step quieter - except on the selected row, where one colour reads best.
        SetTextColor(dc, (!selected && c == PK_COL_TITLE) ? p.textSecondary : p.textPrimary);
        DrawTextW(dc, buf, -1, &tr,
                  DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS |
                      (PickerColumnIsRight(c) ? DT_RIGHT : DT_LEFT));
    }
    if (saved) RestoreDC(dc, saved);

    // The default focus rectangle went with the default drawing, and keyboard users need to
    // see which row the arrow keys are on.
    if ((state & LVIS_FOCUSED) != 0 && GetFocus() == hList) {
        HBRUSH fr = CreateSolidBrush(p.borderStrong);
        if (fr) {
            FrameRect(dc, &row, fr);
            DeleteObject(fr);
        }
    }
}

// A 1px themed outline just OUTSIDE a child, drawn by the parent. It replaces the
// WS_EX_CLIENTEDGE the filter box and the list used to carry: that edge is drawn by the theme
// engine in the user's OS light/dark preference and cannot be recoloured, which left a pale
// rectangle around both dark controls.
void PickerFrameChild(HDC dc, HWND parent, HWND child, COLORREF colour) {
    if (!parent || !child || !IsWindowVisible(child)) return;
    RECT rc;
    GetWindowRect(child, &rc);
    MapWindowPoints(nullptr, parent, reinterpret_cast<POINT*>(&rc), 2);
    InflateRect(&rc, 1, 1);
    HBRUSH b = CreateSolidBrush(colour);
    if (!b) return;
    FrameRect(dc, &rc, b);
    DeleteObject(b);
}

LRESULT CALLBACK PickerProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    PickerState* st =
        reinterpret_cast<PickerState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg) {
        case WM_NCCREATE: {
            CREATESTRUCTW* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                              reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
            break;
        }
        case WM_CREATE: {
            st = reinterpret_cast<PickerState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
            st->dpi = DpiOf(hwnd);
            st->font = MakeUiFont(st->dpi, false);
            // The title bar and the frame, by the same documented DWM route the Settings
            // window uses. Everything inside is painted below.
            theme::ApplyDarkFrame(hwnd);

            st->hFilterLabel = Mk(hwnd, L"STATIC", L"Filter:", SS_LEFT, -1);
            // No WS_EX_CLIENTEDGE on either of these: that edge is drawn by the theme engine
            // in the OS light/dark preference and no documented call recolours it, so it left
            // a pale rectangle around a dark control. PickerFrameChild draws ours instead.
            st->hFilter = Mk(hwnd, L"EDIT", L"", ES_AUTOHSCROLL, IDC_PK_FILTER);
            st->hList = Mk(hwnd, WC_LISTVIEWW, L"",
                           LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS | WS_TABSTOP,
                           IDC_PK_LIST);
            st->hHint = Mk(hwnd, L"STATIC", L"", SS_LEFT, IDC_PK_HINT);
            // BS_OWNERDRAW and BS_DEFPUSHBUTTON share the low style nibble, so Select cannot
            // be both - the same trade the Settings window's OK button makes. Enter still
            // reaches IDOK: IsDialogMessageW falls back to it when the window reports no
            // default id, and RunModalLoop calls IsDialogMessageW on every message.
            st->hOk = Mk(hwnd, L"BUTTON", L"Select", BS_OWNERDRAW | WS_TABSTOP, IDOK);
            st->hCancel = Mk(hwnd, L"BUTTON", L"Cancel", BS_OWNERDRAW | WS_TABSTOP,
                             IDCANCEL);

            ListView_SetExtendedListViewStyle(st->hList,
                                              LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
            // The list view's own surface. These three are the ONLY documented route to it:
            // it does not answer WM_CTLCOLOR*. The rows are drawn in NM_CUSTOMDRAW, but these
            // still matter for the empty band under the last row and while a filter matches
            // nothing at all.
            ListView_SetBkColor(st->hList, theme::P().cardBg);
            ListView_SetTextBkColor(st->hList, theme::P().cardBg);
            ListView_SetTextColor(st->hList, theme::P().textPrimary);
            LVCOLUMNW col;
            ZeroMemory(&col, sizeof(col));
            // LVCF_FMT has to be in the mask or LVCOLUMN::fmt is ignored, and column 0 of a
            // report-mode list view is always left-aligned whatever is asked for - which is
            // why CPU% is column 2 and not column 0.
            col.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM | LVCF_FMT;
            wchar_t h0[] = L"PID";
            col.pszText = h0; col.cx = 60; col.iSubItem = PK_COL_PID;
            col.fmt = LVCFMT_LEFT;
            ListView_InsertColumn(st->hList, PK_COL_PID, &col);
            wchar_t h1[] = L"Process";
            col.pszText = h1; col.cx = 180; col.iSubItem = PK_COL_NAME;
            col.fmt = LVCFMT_LEFT;
            ListView_InsertColumn(st->hList, PK_COL_NAME, &col);
            wchar_t hc[] = L"CPU";
            col.pszText = hc; col.cx = 64; col.iSubItem = PK_COL_CPU;
            col.fmt = LVCFMT_RIGHT;
            ListView_InsertColumn(st->hList, PK_COL_CPU, &col);
            wchar_t h2[] = L"Window title";
            col.pszText = h2; col.cx = 260; col.iSubItem = PK_COL_TITLE;
            col.fmt = LVCFMT_LEFT;
            ListView_InsertColumn(st->hList, PK_COL_TITLE, &col);

            // AFTER the columns exist, because that is when the header control does. The
            // subclass exists only to catch the header's NM_CUSTOMDRAW, which the header
            // sends to the LIST VIEW and never to this dialog.
            SetWindowSubclass(st->hList, PickerListProc, kPickerListSubclassId, 0);

            FontApply fa; fa.f = st->font;
            EnumChildWindows(hwnd, ApplyFontProc, reinterpret_cast<LPARAM>(&fa));

            // First snapshot: it has no predecessor, so every percentage is 0 and the column
            // reads "-". The delta arrives on the first timer tick.
            PickerSample(st);
            if (st->all.empty()) st->all = EnumerateProcesses();
            PickerFill(st, 0);
            PickerLayout(st, hwnd);
            SetTimer(hwnd, kPickerTimer, kPickerSampleMs, nullptr);
            SetFocus(st->hFilter);
            return 0;
        }
        case WM_TIMER:
            if (st && wp == kPickerTimer) {
                const DWORD keep = PickerSelectedPid(st);
                PickerSample(st);
                PickerFill(st, keep);
            }
            return 0;
        case WM_DESTROY:
            KillTimer(hwnd, kPickerTimer);
            return 0;
        case WM_SIZE:
            if (st) PickerLayout(st, hwnd);
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
            if (dc && st) {
                const theme::Palette& p = theme::P();
                PickerFrameChild(dc, hwnd, st->hFilter, p.border);
                PickerFrameChild(dc, hwnd, st->hList, p.border);
            }
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_DRAWITEM: {
            const DRAWITEMSTRUCT* di = reinterpret_cast<const DRAWITEMSTRUCT*>(lp);
            if (!di || !st || di->CtlType != ODT_BUTTON) break;
            // Select is the primary action; Cancel is the quiet one.
            return theme::DrawButton(di,
                                     di->CtlID == IDOK ? theme::ButtonKind::Primary
                                                       : theme::ButtonKind::Secondary,
                                     st->dpi);
        }
        // The documented route for the controls that are not owner-drawn. Each one here sits
        // directly on the window background, which is exactly what the generic helper assumes,
        // so only the footer's colour needs an opinion of its own.
        case WM_CTLCOLORSTATIC:
        case WM_CTLCOLOREDIT:
        case WM_CTLCOLORBTN: {
            HDC dc = reinterpret_cast<HDC>(wp);
            HWND ctl = reinterpret_cast<HWND>(lp);
            HBRUSH b = theme::OnCtlColor(msg, dc, ctl);
            if (st && dc && msg == WM_CTLCOLORSTATIC && ctl == st->hHint) {
                // The footer is a caption about the list, not a heading of its own.
                SetTextColor(dc, theme::P().textSecondary);
            }
            if (b) return reinterpret_cast<LRESULT>(b);
            break;
        }
        case WM_COMMAND: {
            if (!st) break;
            const int id = LOWORD(wp);
            const int code = HIWORD(wp);
            if (id == IDC_PK_FILTER && code == EN_CHANGE) {
                PickerFill(st, PickerSelectedPid(st));
                return 0;
            }
            if (id == IDOK) { PickerAccept(st); return 0; }
            if (id == IDCANCEL) { st->result.clear(); st->done = true; return 0; }
            break;
        }
        case WM_NOTIFY: {
            if (!st) break;
            const NMHDR* nh = reinterpret_cast<const NMHDR*>(lp);
            if (nh->idFrom == IDC_PK_LIST && nh->code == NM_DBLCLK) {
                PickerAccept(st);
                return 0;
            }
            if (nh->hwndFrom == st->hList && nh->code == NM_CUSTOMDRAW) {
                const NMLVCUSTOMDRAW* cd = reinterpret_cast<const NMLVCUSTOMDRAW*>(lp);
                if (cd->nmcd.dwDrawStage == CDDS_PREPAINT) return CDRF_NOTIFYITEMDRAW;
                if (cd->nmcd.dwDrawStage == CDDS_ITEMPREPAINT) {
                    PickerDrawRow(st, cd);
                    return CDRF_SKIPDEFAULT;
                }
                return CDRF_DODEFAULT;
            }
            break;
        }
        case WM_CLOSE:
            if (st) { st->result.clear(); st->done = true; }
            return 0;
        default:
            break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void RegisterPickerClass() {
    static bool done = false;
    if (done) return;
    WNDCLASSEXW wc;
    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = PickerProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;   // WM_ERASEBKGND paints appBg; no light flash
    wc.lpszClassName = kPickerClass;
    RegisterClassExW(&wc);
    done = true;
}

// ---------------------------------------------------------------------------
// Settings window
// ---------------------------------------------------------------------------

enum : int {
    IDC_PROFLIST = 1300,
    IDC_ADD, IDC_DUP, IDC_REMOVE, IDC_RENAME,
    IDC_ENABLED,
    IDC_GAME, IDC_GAMEPICK, IDC_GAMEBROWSE, IDC_GAMEMASK,
    IDC_HEAVY, IDC_HEAVYPICK, IDC_HEAVYMASK,
    IDC_AUTOPIN, IDC_PCT, IDC_SECS,
    IDC_MAPMASK, IDC_MAPRESET, IDC_MAP,
    IDC_STARTUP, IDC_POLL, IDC_NOTIFY,
    IDC_INSPECT,
    IDC_APPLY
};

// The page switcher and its pages. These ids are NEW - no existing id changed value - and
// they sit well above IDC_APPLY so the two blocks cannot collide.
//
// IDC_SECS above is deliberately still in the enum even though the seconds field is gone:
// removing it would silently renumber IDC_MAPMASK and everything after it, and those values
// are what WM_COMMAND dispatches on.
//
// IDC_NAV_RULES IS GONE. The Rules page no longer exists - the auto-pin rule is per-profile
// and now lives on the Profiles page - and these three ids must stay contiguous and in page
// order, so the dead id could not simply be left in the middle.
enum : int {
    IDC_NAV = 1400,
    IDC_NAV_PROFILES,     // page ids must stay contiguous and in page order
    IDC_NAV_COREMAP,
    IDC_NAV_GENERAL
};

// Controls added this round. A separate block so nothing above it can shift.
enum : int {
    IDC_SEARCH = 1500,    // profile search EDIT
    IDC_ADDGAME,          // "Add game..." -> cd::PickGame
    IDC_HEAVYADD,         // type a heavy exe name
    IDC_HEAVYREM          // drop the selected heavy exe
};

enum : int { PAGE_PROFILES = 0, PAGE_COREMAP, PAGE_GENERAL, PAGE_COUNT };

const wchar_t kSettingsClass[] = L"GameOptimizerSettings";
const UINT_PTR kStatusTimer = 1;

// ---- the sponsor strip's patience timer -------------------------------------------------
// MEASURED 2026-08-29, and it is the reason this exists rather than a comment saying the
// swap is instant. WebView2 creation is asynchronous. On the success path here it completed
// fast enough that a capture three seconds after the window opened already showed the page.
// On a FAILING path - a user-data folder that could not be used - the controller callback
// took over ten seconds to arrive, and for that whole time the band was EMPTY: no web strip
// and no GDI strip, because the GDI one is created hidden.
//
// An empty band breaks the one rule this feature is not allowed to break. So the GDI strip is
// shown unconditionally once this fires, and hidden again only if the web strip turns up.
// Long enough that a normal success never flashes; short enough that a failure never leaves a
// hole anybody would notice.
const UINT_PTR kSponsorFallbackTimer = 2;
const UINT     kSponsorFallbackMs    = 700;

// ---------------------------------------------------------------------------
// WHY THE AUTO-PIN RULE IS OR IS NOT RUNNING RIGHT NOW.
//
// MEASURED, and it is the defect this exists for: the operator set the threshold to 1%, saw
// claude.exe sitting at 4% with a RED meter, and nothing ever happened. The rule was working
// exactly as designed. engine.cpp ComputeDesired rule 4 admits a new qualifier only when
//     (a) an ENABLED profile's game is a live process - otherwise gameSet is empty; AND
//     (b) the foreground pid is in that gameSet.
// Neither precondition appeared anywhere in this window, while the red meter actively implied
// the rule was about to fire. A red bar that means nothing is a lie told in colour.
//
// Every state below is DERIVED FROM EngineStatus, which the engine publishes and this window
// already reads once a second. The rule is not re-implemented here and nothing is guessed.
enum class AutoPinState {
    Hidden,    // no profile is selected, so there is nothing to describe
    Off,       // the rule is switched off for this profile
    Waiting,   // the rule CANNOT fire at all right now
    Paused,    // the game is running but does not own the foreground
    Active     // the game is running AND owns the foreground
};

// A heavy row is one executable but auto-pin admits by pid. Keep the counts so a grouped row
// can say all, none, or only some instances were applied without disturbing the independent
// Windows readback column (whose "Mixed" result is deliberately left as-is).
struct AutoApplySummary {
    size_t applied = 0;
    size_t accessDenied = 0;
    size_t failed = 0;
};

struct SettingsState {
    Config* out = nullptr;
    const Topology* topo = nullptr;
    Engine* engine = nullptr;
    Config work;
    Config baseline;  // Exact config at open: distinguishes tray additions from deletions here,
                      // so reconciling the measured stale snapshot cannot resurrect a profile.

    // A freshly detected copy of the machine, refreshed on the same 1 s timer that repaints
    // the core map. `topo` above is the topology the app STARTED with; Parked flags move
    // under load, so every parked warning and the Inspect action read this one instead.
    Topology live;
    bool haveLive = false;

    // CPU identity/elevation come from the full probe once. The two mutable scheduling
    // influences inside this value are refreshed query-only on the existing 1 s timer.
    EnvironmentInfo env;

    // The blocked-processes line grows by a paragraph when the engine reports a stale
    // topology, so the layout has to reserve more height for it. Tracked rather than
    // measured because the text is set from a timer and re-measuring every second is waste.
    bool blockedTall = false;
    // Drives the status dot beside the blocked line. Display only - derived from the same
    // EngineStatus the sentence itself is built from, never from a second source.
    bool blockedBad = false;

    int selProfile = -1;
    int dpi = 96;
    bool loading = false;

    // The page switcher and the page it currently shows. There is no scroll offset any more:
    // one page is visible at a time and each page is laid out to fit the content area.
    //
    // hNav KEEPS ITS NAME and is now the theme::kTabBarClass strip across the TOP of the
    // window rather than the left rail. Operator decision: menu on top, no side panel. The
    // contract is identical apart from the notification code, so nothing else here changed.
    HWND hNav = nullptr;
    HWND hNavBtn[PAGE_COUNT] = { nullptr, nullptr, nullptr };
    int  page = PAGE_PROFILES;

    // ---- Profile list ordering and filtering --------------------------------
    // One entry per VISIBLE listbox row, holding the index into work.profiles. The list is
    // ordered by Config::ProfilesForDisplay and then filtered by the search box, so a row
    // index is NOT a profile index and the two must never be confused. The same value is
    // also stored on the item with LB_SETITEMDATA so the owner-draw handler can reach the
    // profile without this vector.
    std::vector<size_t> rows;
    // Draw the 1px divider under this row (the last recently-used one). -1 = no divider.
    int sepRow = -1;

    // ---- Live CPU% for the Heavy apps meters ---------------------------------
    // EngineStatus carries which processes are governed and which are blocked, but it has no
    // per-process CPU figure and engine.h is frozen this round - so the percentages are
    // sampled here, on the settings window's own 1 s timer, through the same ProcessSnapshot
    // the watcher uses. That path opens processes with PROCESS_QUERY_LIMITED_INFORMATION and
    // nothing wider, so it adds no new access right to this app.
    ProcessSnapshot cpuSnap;
    bool haveCpu = false;
    std::map<std::wstring, double> cpuByExe;   // lowercased basename -> machine-wide %
    std::vector<std::wstring> heavyCanonical;   // user's order, independent of display order

    // ---- Which mask each of those processes is ON, read back from Windows ----
    // Refreshed on the SAME 1 s beat and from the SAME snapshot as the percentages above -
    // one timer, one process enumeration - because a second timer would double the cost of a
    // window whose entire purpose is to save CPU.
    //
    // NOT DERIVED FROM ENGINESTATUS, and that is the whole point of the feature. EngineStatus
    // publishes the mask this app INTENDED for each pid; these two fields hold what
    // GetProcessDefaultCpuSets actually reports. Where they disagree the user sees the
    // readback, because the disagreement is the interesting case: Windows can accept an
    // assignment and ignore it, and other software writes this API too.
    std::map<std::wstring, CpuSetStageInfo> stageByHeavy;  // heavy list ENTRY (lowercased)
    std::map<std::wstring, AutoApplySummary> autoApplyByExe; // auto row basename (lowercased)
    CpuSetStageInfo targetStage;                           // the profile's game
    // Last drawn target label. The target's stage is painted by the PARENT, which repaints
    // only when something changed, so the previous string is what says whether it did.
    std::wstring targetStageText;

    // Which of the five auto-pin states the edited profile is in. Recomputed on the same 1 s
    // timer as everything else and cached here because BOTH the parent's paint (the status
    // dot) and the heavy list's owner-draw (the meter's colour ramp) read it.
    AutoPinState autoState = AutoPinState::Hidden;

    // ---- What auto-pin has actually MOVED, as rows in the heavy list ---------
    // The defect this exists for: nothing in this window ever said which processes the rule
    // picked, so a working auto-pin and a broken one looked identical and the operator
    // reported the working one as broken. These are the row TEXTS currently appended to
    // hHeavy - including the trailing "+N more" caption when there is one - cached so the
    // list is only rebuilt when the set really moved. Rebuilding it every second would reset
    // the selection and the scroll position under the user's hand.
    //
    // THEY ARE NOT THE USER'S CONFIG AND MUST NEVER BECOME IT. Profile::heavy is written from
    // HeavyItems(), which reads back only rows stamped kHeavyRowManual; see the enum there.
    std::vector<std::wstring> autoRows;
    // Distinct auto-pinned executables BEFORE the display cap, for the "+N more" row and for
    // the status sentence's count.
    size_t autoTotal = 0;

    // Owned. Returned from WM_CTLCOLOR* so a static/checkbox erases to the card it sits on
    // and an edit/listbox to the input surface. Deleted in WM_NCDESTROY.
    HBRUSH cardBrush = nullptr;
    HBRUSH inputBrush = nullptr;

    HWND hProfHdr = nullptr, hProfList = nullptr;
    HWND hSearch = nullptr, hAddGame = nullptr;
    HWND hAdd = nullptr, hDup = nullptr, hRem = nullptr, hRen = nullptr;
    HWND hEditHdr = nullptr, hEnabled = nullptr;
    HWND hGameLbl = nullptr, hGame = nullptr, hGamePick = nullptr, hGameBrowse = nullptr;
    HWND hGameMaskLbl = nullptr, hGameMask = nullptr, hGameMaskWarn = nullptr;
    // hHeavy is now an owner-drawn LISTBOX, not the old multi-line EDIT: each row carries the
    // exe name plus a live CPU meter, which a text box cannot show.
    HWND hHeavyLbl = nullptr, hHeavy = nullptr, hHeavyPick = nullptr;
    HWND hHeavyAdd = nullptr, hHeavyRem = nullptr;
    HWND hHeavyMaskLbl = nullptr, hHeavyMask = nullptr, hHeavyMaskWarn = nullptr;
    HWND hAutoPin = nullptr, hAutoDesc = nullptr;
    // The live "why is nothing happening" line under the percent field. See AutoPinState.
    HWND hAutoStatus = nullptr;
    // The seconds field and its two captions are DELETED, not hidden - see Profile::
    // autoPinSeconds in config.h for why the model keeps the value the UI no longer edits.
    HWND hPctLbl = nullptr, hPct = nullptr;
    HWND hMapHdr = nullptr, hTopoText = nullptr;
    HWND hMapMaskLbl = nullptr, hMapMask = nullptr, hMapReset = nullptr, hMap = nullptr;
    // Stand-in shown in the core map's slot when the control could not be created. Exactly
    // one of hMap / hMapFail is ever non-null, and they occupy the same rectangle.
    HWND hMapFail = nullptr;
    HWND hGenHdr = nullptr, hStartup = nullptr, hNotify = nullptr;
    HWND hPollLbl = nullptr, hPoll = nullptr;
    HWND hGameModeStatus = nullptr, hVCacheStatus = nullptr, hVCacheEffect = nullptr;
    HWND hBlocked = nullptr, hInspect = nullptr;
    // The sponsor strip. It belongs to the WINDOW, not to a page - it sits directly above the
    // footer on every page - so it is deliberately absent from PageControls.
    HWND hSponsor = nullptr;
    int  sponsorW = 0, sponsorH = 0;   // from cd::SponsorMeasure; 0 when the strip is absent
    // The same strip, rendered by an embedded WebView2 from the plugin's own HTML and CSS.
    // EXACTLY ONE of the two is ever visible. This one is created with the window and
    // destroyed with it - nothing web-related exists while the app sits in the tray - and
    // whenever it cannot be had, hSponsor above is shown in its place. See webview_host.h.
    WebSponsor* web = nullptr;
    bool webShowing = false;
    // The patience timer fired before WebView2 reported in, so the GDI control was shown to
    // keep the band from sitting empty. It matters to the LAYOUT as well as to visibility:
    // the two renderings are different shapes, and a GDI row laid out inside the web panel's
    // tall narrow rectangle is squeezed and clipped. While this is set the band is sized for
    // the control that is actually on screen. SponsorWebReady clears it if the page turns up
    // after all.
    bool webLate = false;
    HWND hOk = nullptr, hCancel = nullptr, hApply = nullptr;
};

HWND  g_hSettings = nullptr;
HHOOK g_msgHook = nullptr;

// Defined further down; declared here because the warning labels change the layout and the
// profile loader has to be able to trigger a re-layout when it swaps the mask selection.
void SettingsLayout(SettingsState* st, HWND hwnd);

// ---- how much room the sponsor panel needs ----------------------------------------------
// THE TWO RENDERINGS ARE DIFFERENT SHAPES, so the band cannot be one number.
//
//   WebView2  ONE ROW OF THREE GROUPS - Ko-fi, Star-on-Github, and the disease-research copy
//             beside the GOATPROJECT lockup - ~76 logical px tall and as wide as the window
//             gives it. It was a 272 x 261 vertical stack, then a fixed 831 x 65 row; the
//             numbers here are measured, see tools\measure-panel.py.
//   GDI       the fallback in sponsor.cpp - a short horizontal ROW, ~45 logical px tall.
//
// Reserving the row's height for the stack clips three quarters of the panel; reserving the
// stack's height for the row opens 200px of dead space above the footer. So the band follows
// whichever rendering is actually in play, and `st->web` is the discriminator: non-null means
// WebView2 is live or still being built, null means we are on the GDI path - either because
// creation refused synchronously or because SponsorWebReady reported failure, and that
// function re-runs the layout precisely so this answer changes with it.
//
// THE cx IT RETURNS FOR THE WEBVIEW2 PANEL IS A FLOOR, NOT A WIDTH, and the two callers use it
// differently on purpose. WebSponsorMinSize hands back kSponsorCssMinWidth - the narrowest host
// the three groups fit in - because the panel itself has no width of its own any more: it fills
// whatever it is given. SettingsLayout therefore OVERRIDES cx with the full content row and uses
// this only to ask "is there a panel at all"; WM_GETMINMAXINFO is the one that has to respect it,
// and it does so through the window minimum. For the GDI strip cx really is its width.
//
// A zero comes back when there is nothing to show at all, and the caller collapses the band.
SIZE SponsorBandSize(const SettingsState* st, int dpi) {
    SIZE z;
    z.cx = 0;
    z.cy = 0;
    if (st == nullptr) return z;
    if (st->web != nullptr && !st->webLate) return WebSponsorMinSize(dpi);
    // The GDI control's own measure, cached when the control was created. Guarded on BOTH
    // axes: a measure with no width is not a strip, and must reserve no band.
    if (st->sponsorW > 0 && st->sponsorH > 0) {
        z.cx = st->sponsorW;
        z.cy = st->sponsorH;
    }
    return z;
}

// ---- which sponsor strip the user actually sees -----------------------------------------
// Delivered on the UI thread once WebView2 creation has finished or failed. This is the ONE
// place that decides between the two renderings, and its failure branch is the whole reason
// the GDI control in sponsor.cpp was kept rather than deleted: a machine without the WebView2
// runtime, or with a policy that blocks it, still gets a sponsor strip and a healthy window.
void SponsorWebReady(void* user, bool ok) {
    HWND hwnd = reinterpret_cast<HWND>(user);
    if (hwnd == nullptr || !IsWindow(hwnd)) return;
    SettingsState* st =
        reinterpret_cast<SettingsState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (st == nullptr) return;

    KillTimer(hwnd, kSponsorFallbackTimer);

    if (ok) {
        st->webShowing = true;
        // The patience timer may already have shown the GDI strip. Exactly one of the two is
        // ever visible, so it goes away now that the real one is rendering.
        if (st->hSponsor != nullptr) ShowWindow(st->hSponsor, SW_HIDE);
        st->webLate = false;
        LogLine(L"[settings] sponsor strip: WebView2 (the plugin's own markup)");
        // The band was sized for the GDI row while the page was late; it is the panel's
        // again now, and the panel is four times as tall. Without this the page renders
        // correctly into a rectangle a quarter of its height and is clipped.
        SettingsLayout(st, hwnd);
        InvalidateRect(hwnd, nullptr, TRUE);
        return;
    }

    LogLine(L"[settings] sponsor strip: WebView2 unavailable - showing the GDI strip");
    WebSponsorDestroy(st->web);
    st->web = nullptr;
    st->webShowing = false;
    st->webLate = false;
    if (st->hSponsor != nullptr) {
        ShowWindow(st->hSponsor, SW_SHOWNA);
        InvalidateRect(st->hSponsor, nullptr, TRUE);
    }
    // THE BAND MUST SHRINK. It was reserving the web panel's 150 logical px for a control
    // that draws a 45px row; leaving it would strand 200px of dead space above the footer on
    // exactly the machines that never get the web panel at all. SponsorBandSize now answers
    // with the GDI measure, so one re-layout moves the page content back down into the room
    // the panel gave up.
    SettingsLayout(st, hwnd);
    InvalidateRect(hwnd, nullptr, TRUE);
}
bool UpdateMaskWarnings(SettingsState* st);
void RefreshLiveTopology(SettingsState* st);
void ShowInspectReport(SettingsState* st, HWND owner);
void ApplyPageVisibility(SettingsState* st);
void OverdrawSearchChrome(SettingsState* st, HWND hwnd);
bool PromptName(HWND owner, const wchar_t* prompt, std::wstring& io);
// Returns true when the state or the sentence changed, i.e. the caller must repaint.
bool RefreshAutoPinStatus(SettingsState* st);
// Returns true when the auto-pin rows in the heavy list were rebuilt.
bool SyncAutoPinRows(SettingsState* st);

// The owner-draw button kind, parked on the control itself. GWLP_USERDATA is zero for a
// control nobody stamped, so the stored value is kind+1 and 0 reads back as Secondary -
// a button that lost its kind is drawn quietly rather than as the primary action.
void SetButtonKind(HWND h, theme::ButtonKind k) {
    if (h) SetWindowLongPtrW(h, GWLP_USERDATA, static_cast<LONG_PTR>(static_cast<int>(k)) + 1);
}

theme::ButtonKind ButtonKindOf(HWND h) {
    LONG_PTR v = h ? GetWindowLongPtrW(h, GWLP_USERDATA) : 0;
    if (v <= 0) return theme::ButtonKind::Secondary;
    return static_cast<theme::ButtonKind>(static_cast<int>(v) - 1);
}

// A themed BS_AUTOCHECKBOX draws its own label with the visual style's text colour - near
// black - and ignores the colour set in WM_CTLCOLORBTN entirely. On a dark card that is an
// invisible label, and there is no way to recolour it while the control stays themed.
//
// SetWindowTheme(h, L"", L"") is the DOCUMENTED way off that path: the control falls back to
// classic drawing, which does honour WM_CTLCOLORBTN, so the label becomes readable. The cost
// is that the tick box itself is drawn classic (a light box) rather than themed. That is a
// deliberate, stated trade - the alternative routes are BS_OWNERDRAW, which silently breaks
// BM_SETCHECK/BM_GETCHECK on a checkbox and so would change behaviour, and the undocumented
// uxtheme ordinals that theme.h refuses on purpose.
//
// Bound at runtime rather than linked, so no new import is added to the exe.
void UseClassicChrome(HWND h) {
    typedef HRESULT(WINAPI * SetWindowThemeFn)(HWND, LPCWSTR, LPCWSTR);
    static SetWindowThemeFn fn = nullptr;
    static bool tried = false;
    if (!tried) {
        tried = true;
        HMODULE m = LoadLibraryW(L"uxtheme.dll");   // kept for process life, as comctl32 does
        if (m) fn = reinterpret_cast<SetWindowThemeFn>(
                        reinterpret_cast<void*>(GetProcAddress(m, "SetWindowTheme")));
    }
    if (fn && h) fn(h, L"", L"");
}

// ---------------------------------------------------------------------------
// Owner-drawn check boxes and the combo frame cover-up
// ---------------------------------------------------------------------------
// WHY A SUBCLASS RATHER THAN BS_OWNERDRAW, AND THIS IS MEASURED, NOT ASSUMED:
// BS_CHECKBOX/BS_AUTOCHECKBOX and BS_OWNERDRAW share the low nibble of the button style, so
// `BS_AUTOCHECKBOX | BS_OWNERDRAW` is 0x03 | 0x0B == 0x0B - the auto-checkbox semantics are
// not combined, they are erased. A probe on this machine created all three variants and read
// the style back plus the check state:
//     BS_AUTOCHECKBOX               actual_low=0x03  set1_get=1  afterclick=1
//     BS_OWNERDRAW                  actual_low=0x0B  set1_get=0  afterclick=0
//     BS_AUTOCHECKBOX|BS_OWNERDRAW  actual_low=0x0B  set1_get=0  afterclick=0
// On a BS_OWNERDRAW button BM_SETCHECK is a no-op and BM_GETCHECK always answers 0, so there
// would be NO check state on the control to read - and these four boxes drive real settings
// (start-with-Windows, notifications, auto-pin, profile enabled). Drawing from a cached bool
// instead would let the picture disagree with what the app is actually configured to do.
//
// So the control keeps BS_AUTOCHECKBOX and every behaviour with it - click and space-bar
// toggling, BM_GETCHECK/BM_SETCHECK, tab order, BN_CLICKED - and only its PIXELS are taken
// over, in WM_PAINT, with the state read back out of the control on every paint.
//
// The one complication: a button repaints itself DIRECTLY, outside WM_PAINT, on BM_SETCHECK,
// clicks, focus changes and enable changes. Measured: a plain subclass that owns WM_PAINT
// still ends up with 169 system-drawn pixels on its surface after one BM_SETCHECK. Clearing
// WS_VISIBLE across the default handling gives that direct draw an empty visible region, and
// the InvalidateRect afterwards routes the repaint back through our WM_PAINT; with that in
// place the same probe reports 0 foreign pixels through set/click/space/focus/enable/text
// while the check state still reads 1/0/1/0 exactly as the stock control does. The style bit
// is restored immediately and never touched by ShowWindow, so nothing is actually hidden.
constexpr UINT_PTR kCheckSubclassId = 1;
constexpr UINT_PTR kComboSubclassId = 2;

LRESULT SilentDefault(HWND h, UINT msg, WPARAM wp, LPARAM lp, bool repaint) {
    const LONG_PTR style = GetWindowLongPtrW(h, GWL_STYLE);
    const bool visible = (style & WS_VISIBLE) != 0;
    if (visible) SetWindowLongPtrW(h, GWL_STYLE, style & ~static_cast<LONG_PTR>(WS_VISIBLE));
    const LRESULT r = DefSubclassProc(h, msg, wp, lp);
    if (visible) SetWindowLongPtrW(h, GWL_STYLE, style);
    if (repaint) InvalidateRect(h, nullptr, FALSE);
    return r;
}

LRESULT CALLBACK CheckBoxProc(HWND h, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR id,
                              DWORD_PTR ref) {
    (void)ref;
    switch (msg) {
        case WM_ERASEBKGND:
            return 1;                     // the whole surface is painted in WM_PAINT
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(h, &ps);
            if (dc) {
                RECT rc;
                GetClientRect(h, &rc);
                HWND parent = GetParent(h);

                // The card under this control is drawn by the PARENT, and WS_CLIPCHILDREN
                // keeps the parent's paint out of this rect, so there is nothing behind us to
                // preserve. Asking the parent for its WM_CTLCOLORBTN brush reproduces exactly
                // what the classic check box already did with that brush, so the background
                // colour is unchanged from before this owner-draw.
                HBRUSH bg = nullptr;
                if (parent)
                    bg = reinterpret_cast<HBRUSH>(
                        SendMessageW(parent, WM_CTLCOLORBTN, reinterpret_cast<WPARAM>(dc),
                                     reinterpret_cast<LPARAM>(h)));
                if (bg) FillRect(dc, &rc, bg);

                DRAWITEMSTRUCT di;
                ZeroMemory(&di, sizeof(di));
                di.CtlType    = ODT_BUTTON;
                di.CtlID      = static_cast<UINT>(GetDlgCtrlID(h));
                di.itemAction = ODA_DRAWENTIRE;
                di.hwndItem   = h;
                di.hDC        = dc;
                di.rcItem     = rc;
                if (!IsWindowEnabled(h)) di.itemState |= ODS_DISABLED;
                if (GetFocus() == h)     di.itemState |= ODS_FOCUS;

                // READ FROM THE CONTROL, every single paint - never from a cached bool.
                const bool checked = SendMessageW(h, BM_GETCHECK, 0, 0) == BST_CHECKED;
                if (checked) di.itemState |= ODS_CHECKED;

                theme::DrawCheckBox(&di, checked, DpiOf(parent ? parent : h));
            }
            EndPaint(h, &ps);
            return 0;
        }
        // Everything that makes the stock control draw itself directly.
        case BM_SETCHECK:
        case BM_SETSTATE:
        case WM_ENABLE:
        case WM_SETTEXT:
        case WM_LBUTTONDOWN:
        case WM_LBUTTONUP:
        case WM_LBUTTONDBLCLK:
        case WM_KEYDOWN:
        case WM_KEYUP:
        case WM_CHAR:
        case WM_SETFOCUS:
        case WM_KILLFOCUS:
        case WM_CAPTURECHANGED:
            return SilentDefault(h, msg, wp, lp, true);
        // Suppressed as well, but without forcing a repaint: this control has no hover state
        // to show, and invalidating on every mouse move would repaint it dozens of times a
        // second for nothing.
        case WM_MOUSEMOVE:
        case WM_MOUSELEAVE:
            return SilentDefault(h, msg, wp, lp, false);
        case WM_NCDESTROY:
            RemoveWindowSubclass(h, CheckBoxProc, id);
            break;
        default:
            break;
    }
    return DefSubclassProc(h, msg, wp, lp);
}

// Paints the themed 1px frame for ONE combo over the light edge the system draws.
//
// MEASURED, and it is the whole reason this is not simply done with the WM_PAINT DC: the
// settings window is WS_CLIPCHILDREN, so its BeginPaint DC has every child's rect clipped
// out and stroking the combo's bounds on it is a silent no-op - a probe counted 0 frame
// pixels on screen. GetDCEx WITHOUT DCX_CLIPCHILDREN is the DC that reaches the child; the
// same probe counted 464. The frame then survives until the combo repaints itself, which is
// what the subclass below is for.
void OverdrawOneCombo(HWND parent, HWND combo) {
    if (!parent || !combo || !IsWindowVisible(combo)) return;
    RECT rc;
    GetWindowRect(combo, &rc);
    MapWindowPoints(nullptr, parent, reinterpret_cast<POINT*>(&rc), 2);
    HDC dc = GetDCEx(parent, nullptr, DCX_CACHE);
    if (!dc) return;
    theme::OverdrawComboFrame(dc, rc, DpiOf(parent), GetFocus() == combo);
    ReleaseDC(parent, dc);
}

LRESULT CALLBACK ComboProc(HWND h, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR id,
                           DWORD_PTR ref) {
    (void)ref;
    switch (msg) {
        case WM_PAINT:
        case WM_NCPAINT:
        case WM_SETFOCUS:
        case WM_KILLFOCUS: {
            const LRESULT r = DefSubclassProc(h, msg, wp, lp);
            OverdrawOneCombo(GetParent(h), h);   // after the control drew, never before
            return r;
        }
        case WM_NCDESTROY:
            RemoveWindowSubclass(h, ComboProc, id);
            break;
        default:
            break;
    }
    return DefSubclassProc(h, msg, wp, lp);
}

// Repaints only the chrome the PARENT draws - cards, headings, stat numbers, the ring
// gauge. The window has WS_CLIPCHILDREN, so no child is disturbed and nothing flickers.
// Use this when a drawn value changed but no control moved.
void RepaintChrome(HWND hwnd) {
    if (hwnd) InvalidateRect(hwnd, nullptr, TRUE);
}

// Repaint after a re-layout.
//
// InvalidateRect(hwnd, ...) marks only the PARENT's client area dirty. The scrolling children
// were just moved by SetWindowPos and keep whatever pixels they had, so statics smear and the
// owner-drawn core map never receives WM_PAINT at all. RDW_ALLCHILDREN is the part that fixes
// it - it pushes the invalidation down into every child - and RDW_UPDATENOW forces the paint
// out now rather than leaving a half-scrolled frame on screen until the next idle.
void RedrawSettings(HWND hwnd) {
    if (!hwnd) return;
    RedrawWindow(hwnd, nullptr, nullptr,
                 RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_UPDATENOW);
}

// Re-runs the layout when a warning appeared or disappeared. The parent is fetched from a
// child rather than passed in, so callers that only hold the state can use it too.
void RelayoutIfWarningsChanged(SettingsState* st) {
    if (!UpdateMaskWarnings(st)) return;
    HWND parent = st->hGameMask ? GetParent(st->hGameMask) : nullptr;
    if (!parent) return;
    SettingsLayout(st, parent);
    RedrawSettings(parent);
}

LRESULT CALLBACK SettingsMsgHook(int code, WPARAM wp, LPARAM lp) {
    if (code == HC_ACTION && wp == PM_REMOVE && g_hSettings) {
        MSG* m = reinterpret_cast<MSG*>(lp);
        if (m->message >= WM_KEYFIRST && m->message <= WM_KEYLAST &&
            (m->hwnd == g_hSettings || IsChild(g_hSettings, m->hwnd))) {
            if (IsDialogMessageW(g_hSettings, m)) {
                m->message = WM_NULL;
                m->wParam = 0;
                m->lParam = 0;
            }
        }
    }
    return CallNextHookEx(g_msgHook, code, wp, lp);
}

std::wstring ConfidenceSentence(const Topology& t) {
    switch (t.confidence) {
        case Confidence::High:
            return L"Detection confidence: High - the cache domains are clearly "
                   L"distinguishable, so the derived masks should be correct as they stand.";
        case Confidence::Medium:
            return L"Detection confidence: MEDIUM. The cache domains report the same L3 "
                   L"size, so which one is \"CCD0\" is a guess, not a measurement. Check the "
                   L"map below and edit it if the wrong cores are selected.";
        default:
            return L"Detection confidence: NONE. Only one cache domain was found, so there "
                   L"is no CCD split for this tool to use. Only the SMT masks will change "
                   L"anything on this machine.";
    }
}

std::wstring TopologyBlock(const Topology& t) {
    std::wstring s = t.summary;
    if (s.empty()) {
        s = std::wstring(L"Topology: ") + KindName(t.kind) + L", " +
            std::to_wstring(t.domains.size()) + L" cache domain(s), " +
            std::to_wstring(t.totalLogicalProcessors) + L" logical processors.";
    }
    s += L"\r\n";
    s += ConfidenceSentence(t);
    return s;
}

void FillMaskCombo(HWND combo, const Config& c, const std::wstring& select) {
    if (!combo) return;
    SendMessageW(combo, CB_RESETCONTENT, 0, 0);
    int sel = -1;
    for (size_t i = 0; i < c.masks.size(); ++i) {
        SendMessageW(combo, CB_ADDSTRING, 0,
                     reinterpret_cast<LPARAM>(c.masks[i].name.c_str()));
        if (!select.empty() && IEquals(c.masks[i].name, select)) sel = static_cast<int>(i);
    }
    SendMessageW(combo, CB_SETCURSEL, static_cast<WPARAM>(sel), 0);
}

std::wstring ComboText(HWND combo) {
    if (!combo) return std::wstring();
    LRESULT sel = SendMessageW(combo, CB_GETCURSEL, 0, 0);
    if (sel == CB_ERR) return std::wstring();
    LRESULT len = SendMessageW(combo, CB_GETLBTEXTLEN, static_cast<WPARAM>(sel), 0);
    if (len <= 0) return std::wstring();
    std::vector<wchar_t> buf(static_cast<size_t>(len) + 1, L'\0');
    SendMessageW(combo, CB_GETLBTEXT, static_cast<WPARAM>(sel),
                 reinterpret_cast<LPARAM>(buf.data()));
    return std::wstring(buf.data());
}

// ---------------------------------------------------------------------------
// The heavy-apps LISTBOX. It replaced a multi-line EDIT, so the two directions that used to
// be GetText/SetWindowText live here instead. Everything else - what a heavy entry MEANS,
// and Profile::heavy itself - is unchanged.
//
// IT NOW CARRIES TWO KINDS OF ROW, and the difference is load-bearing rather than cosmetic.
// The user's own entries are their configuration and are saved. The auto-pin rows are a LIVE
// READBACK of what rule 4 has moved this session; they are transient, they belong to no
// profile, and writing one into heavy= would silently turn a momentary observation into a
// permanent setting the user never asked for. The row's item data is what tells them apart,
// and HeavyItems - the one and only function that feeds Profile::heavy - filters on it.
// ---------------------------------------------------------------------------

// LB_GETITEMDATA on a row nobody stamped returns 0, so kHeavyRowManual MUST be 0: a row that
// somehow escaped its stamp is then treated as the user's, which is the harmless direction.
// The dangerous direction - an auto row read back as configuration - needs a positive value
// that only this file ever writes.
enum : LPARAM {
    kHeavyRowManual = 0,   // the user's entry; the ONLY kind that reaches Profile::heavy
    kHeavyRowAuto   = 1,   // auto-pin moved this executable; never saved
    kHeavyRowMore   = 2    // the "+N more" caption; a sentence, not a process
};

// Row texts. manualOnly is the guard described above, not an optimisation.
std::vector<std::wstring> HeavyRows(const SettingsState* st, bool manualOnly) {
    std::vector<std::wstring> out;
    if (!st->hHeavy) return out;
    const LRESULT n = SendMessageW(st->hHeavy, LB_GETCOUNT, 0, 0);
    for (LRESULT i = 0; i < n; ++i) {
        const LRESULT origin = SendMessageW(st->hHeavy, LB_GETITEMDATA,
                                            static_cast<WPARAM>(i), 0);
        if (manualOnly && origin != kHeavyRowManual) continue;
        if (origin == kHeavyRowMore) continue;      // a caption is never a process name
        const LRESULT len = SendMessageW(st->hHeavy, LB_GETTEXTLEN,
                                         static_cast<WPARAM>(i), 0);
        if (len <= 0) continue;
        std::vector<wchar_t> buf(static_cast<size_t>(len) + 1, L'\0');
        const LRESULT got = SendMessageW(st->hHeavy, LB_GETTEXT, static_cast<WPARAM>(i),
                                         reinterpret_cast<LPARAM>(buf.data()));
        if (got <= 0) continue;
        std::wstring s = Trim(std::wstring(buf.data(), static_cast<size_t>(got)));
        if (!s.empty()) out.push_back(s);
    }
    return out;
}

// THE CONFIG-FACING DIRECTION. StoreUiToProfile writes Profile::heavy from this and from
// nothing else, so this is the single place the auto-pin rows have to be kept out of - and
// they are kept out by construction rather than by every caller remembering to.
std::vector<std::wstring> HeavyItems(const SettingsState* st) {
    return HeavyRows(st, true);
}

// How many rows at the FRONT of the list are the user's. The auto-pin rows are always a
// contiguous tail, which is what lets SyncAutoPinRows delete them without disturbing a single
// manual index, and what makes this a count rather than a search.
int ManualRowCount(const SettingsState* st) {
    if (!st->hHeavy) return 0;
    const LRESULT n = SendMessageW(st->hHeavy, LB_GETCOUNT, 0, 0);
    LRESULT i = 0;
    while (i < n &&
           SendMessageW(st->hHeavy, LB_GETITEMDATA, static_cast<WPARAM>(i), 0)
               == kHeavyRowManual) {
        ++i;
    }
    return static_cast<int>(i);
}

void SetHeavyItems(SettingsState* st, const std::vector<std::wstring>& v) {
    st->heavyCanonical = v;
    if (!st->hHeavy) return;
    std::set<std::wstring> running;
    for (const auto& entry : st->cpuByExe) running.insert(entry.first);
    std::vector<std::wstring> itemKeys;
    itemKeys.reserve(v.size());
    for (const auto& entry : v) itemKeys.push_back(ToLower(BaseName(Trim(entry))));
    // Profile load is the one safe settling point. Re-sorting on the status timer would make
    // rows dance when processes start or stop and could move one out from under a click.
    const std::vector<std::wstring> ordered = OrderHeavyByActivity(v, itemKeys, running);
    SendMessageW(st->hHeavy, LB_RESETCONTENT, 0, 0);
    for (size_t i = 0; i < ordered.size(); ++i) {
        const LRESULT row = SendMessageW(st->hHeavy, LB_ADDSTRING, 0,
                                         reinterpret_cast<LPARAM>(ordered[i].c_str()));
        if (row >= 0)
            SendMessageW(st->hHeavy, LB_SETITEMDATA, static_cast<WPARAM>(row),
                         kHeavyRowManual);
    }
    // The reset took the auto rows with it, so the cache no longer describes the control.
    // Left stale, the next sync would compare equal and never put them back.
    st->autoRows.clear();
    st->autoTotal = 0;
}

// Appends one entry, case-insensitively de-duplicated, and selects it.
void HeavyAppend(SettingsState* st, const std::wstring& raw) {
    std::wstring name = Trim(raw);
    if (name.empty() || !st->hHeavy) return;
    std::vector<std::wstring> v = HeavyItems(st);
    for (size_t i = 0; i < v.size(); ++i)
        if (IEquals(v[i], name)) return;
    // INSERTED at the end of the manual block, not appended to the list: the auto rows are a
    // tail and every other function here relies on that. LB_ADDSTRING would put the user's
    // new entry after them and break the invariant on the very first use of this button.
    const LRESULT row = SendMessageW(st->hHeavy, LB_INSERTSTRING,
                                     static_cast<WPARAM>(ManualRowCount(st)),
                                     reinterpret_cast<LPARAM>(name.c_str()));
    if (row < 0) return;
    st->heavyCanonical.push_back(name);
    SendMessageW(st->hHeavy, LB_SETITEMDATA, static_cast<WPARAM>(row), kHeavyRowManual);
    SendMessageW(st->hHeavy, LB_SETCURSEL, static_cast<WPARAM>(row), 0);
    // The executable the user has just named themselves must stop being reported as one the
    // app chose, or it sits in the list twice with two different explanations.
    SyncAutoPinRows(st);
}

// ---------------------------------------------------------------------------
// Profile list: ordering, filtering, and the row <-> profile mapping
// ---------------------------------------------------------------------------

// The listbox row currently showing profile `profileIdx`, or -1 when the search box has
// filtered it out. Callers must never assume a row index IS a profile index.
int RowForProfile(const SettingsState* st, int profileIdx) {
    if (profileIdx < 0) return -1;
    for (size_t i = 0; i < st->rows.size(); ++i)
        if (st->rows[i] == static_cast<size_t>(profileIdx)) return static_cast<int>(i);
    return -1;
}

int ProfileForRow(const SettingsState* st, int row) {
    if (row < 0 || row >= static_cast<int>(st->rows.size())) return -1;
    return static_cast<int>(st->rows[static_cast<size_t>(row)]);
}

// Case-insensitive substring match over the profile NAME and its game exe - the two things a
// user would type to find a profile.
bool ProfileMatchesFilter(const Profile& p, const std::wstring& lowerFilter) {
    if (lowerFilter.empty()) return true;
    std::wstring hay = ToLower(p.name) + L" " + ToLower(p.game);
    return hay.find(lowerFilter) != std::wstring::npos;
}

void RefreshProfileList(SettingsState* st) {
    if (!st->hProfList) return;
    st->loading = true;
    SendMessageW(st->hProfList, LB_RESETCONTENT, 0, 0);
    st->rows.clear();
    st->sepRow = -1;

    // ORDER COMES FROM THE MODEL, not from vector order: recently-used profiles first, newest
    // at the top, then everything else. `sepAfter` is how many of those leading entries were
    // recently used, i.e. where the divider belongs - or -1 for "no divider".
    int sepAfter = -1;
    const std::vector<size_t> order = st->work.ProfilesForDisplay(&sepAfter);
    const std::wstring flt = ToLower(Trim(GetText(st->hSearch)));

    int keptRecent = 0;
    for (size_t k = 0; k < order.size(); ++k) {
        const size_t pi = order[k];
        if (pi >= st->work.profiles.size()) continue;
        const Profile& p = st->work.profiles[pi];
        if (!ProfileMatchesFilter(p, flt)) continue;
        const std::wstring label = (p.enabled ? L"[x] " : L"[ ] ") + p.name;
        const LRESULT row = SendMessageW(st->hProfList, LB_ADDSTRING, 0,
                                         reinterpret_cast<LPARAM>(label.c_str()));
        if (row < 0) continue;                       // LB_ERR / LB_ERRSPACE
        SendMessageW(st->hProfList, LB_SETITEMDATA, static_cast<WPARAM>(row),
                     static_cast<LPARAM>(pi));
        st->rows.push_back(pi);
        if (sepAfter >= 0 && static_cast<int>(k) < sepAfter) ++keptRecent;
    }
    // A divider under the last visible row would be a line with nothing under it, and a
    // divider above the first would have nothing over it, so both are suppressed.
    if (keptRecent > 0 && keptRecent < static_cast<int>(st->rows.size()))
        st->sepRow = keptRecent;

    if (st->selProfile >= static_cast<int>(st->work.profiles.size()))
        st->selProfile = static_cast<int>(st->work.profiles.size()) - 1;
    if (st->selProfile < 0 && !st->rows.empty())
        st->selProfile = static_cast<int>(st->rows[0]);
    // A filtered-out selection keeps the editor on the profile the user was editing rather
    // than silently jumping to another one; the list simply shows nothing selected.
    SendMessageW(st->hProfList, LB_SETCURSEL,
                 static_cast<WPARAM>(RowForProfile(st, st->selProfile)), 0);
    st->loading = false;
    InvalidateRect(st->hProfList, nullptr, TRUE);

    // The tab carries the count so it is readable without opening the page.
    if (st->hNav) {
        theme::TabBarSetBadge(st->hNav, IDC_NAV_PROFILES,
                              st->work.profiles.empty()
                                  ? std::wstring()
                                  : std::to_wstring(st->work.profiles.size()));
    }
}

// The one STATIC caption left beside the auto-pin percent field. It was three; the seconds
// field and its two captions are gone. The test is HWND identity, never a control-id range
// and never a walk over the children.
bool IsAutoPinLabel(const SettingsState* st, HWND ctl) {
    if (!st || !ctl) return false;
    return ctl == st->hPctLbl;
}

// True while the auto-pin rule is off, i.e. while that caption describes a field that has no
// effect. Derived from the check box itself so the colour cannot drift out of step with the
// enabled state of the EDIT field.
bool AutoPinLabelsAreDim(const SettingsState* st) {
    return st && !IsChecked(st->hAutoPin);
}

void SyncAutoPinEnable(SettingsState* st) {
    bool on = IsChecked(st->hAutoPin);
    // The EDIT field stays genuinely disabled: refusing keystrokes into a field that has no
    // effect is real interaction semantics, not decoration.
    EnableWindow(st->hPct, on);

    // The STATIC caption is NOT disabled. A disabled STATIC paints its caption twice - once
    // in COLOR_3DHILIGHT offset by one pixel, once in COLOR_GRAYTEXT - and that emboss happens
    // inside the control's own paint, where WM_CTLCOLORSTATIC cannot reach it. On a light
    // dialog it reads as "greyed out"; on this dark card the highlight pass is a near-white
    // ghost one pixel from every glyph, so the text looks smeared. It is left enabled and
    // dimmed to textDim in WM_CTLCOLORSTATIC instead. Nothing else repaints it when the rule
    // is toggled - its enabled state no longer changes - so ask for the repaint here.
    if (st->hPctLbl) InvalidateRect(st->hPctLbl, nullptr, TRUE);
    // Turning the rule on or off changes which of the five states it is in, and the status
    // line has to say so on the click rather than up to a second later.
    RefreshAutoPinStatus(st);
    // The "% CPU" unit and the status dot are both drawn by the PARENT, so they need the
    // parent's repaint rather than a child's.
    HWND parent = st->hAutoPin ? GetParent(st->hAutoPin) : nullptr;
    if (parent) InvalidateRect(parent, nullptr, TRUE);
    // The meters lose their colour ramp when the rule cannot fire.
    if (st->hHeavy) InvalidateRect(st->hHeavy, nullptr, TRUE);
}

void LoadProfileToUi(SettingsState* st) {
    st->loading = true;
    const bool has = st->selProfile >= 0 &&
                     st->selProfile < static_cast<int>(st->work.profiles.size());
    Profile empty;
    const Profile& p = has ? st->work.profiles[static_cast<size_t>(st->selProfile)] : empty;

    SetChecked(st->hEnabled, p.enabled);
    SetWindowTextW(st->hGame, p.game.c_str());
    FillMaskCombo(st->hGameMask, st->work, p.gameMask);
    SetHeavyItems(st, p.heavy);
    FillMaskCombo(st->hHeavyMask, st->work, p.heavyMask);
    // SetHeavyItems has just dropped the previous profile's readback rows along with its
    // entries. Refilled here rather than left to the timer: a second of a profile's heavy
    // list with the auto rows missing reads as "the rule has stopped", which is the exact
    // misreading this feature exists to prevent.
    SyncAutoPinRows(st);
    SetChecked(st->hAutoPin, p.autoPin);
    SetWindowTextW(st->hPct, std::to_wstring(p.autoPinPercent).c_str());
    // p.autoPinSeconds is deliberately NOT loaded: the seconds control is gone and this UI
    // neither reads nor writes that field any more.

    HWND editable[] = { st->hEnabled, st->hGame, st->hGamePick, st->hGameBrowse,
                        st->hGameMask, st->hHeavy, st->hHeavyPick, st->hHeavyAdd,
                        st->hHeavyRem, st->hHeavyMask, st->hAutoPin, st->hDup, st->hRem,
                        st->hRen };
    for (HWND h : editable) EnableWindow(h, has ? TRUE : FALSE);
    st->loading = false;
    SyncAutoPinEnable(st);
    if (!has) EnableWindow(st->hPct, FALSE);
    // The readback belongs to the profile that was on screen a moment ago. Dropped rather
    // than left to expire on the next tick: a stage label sitting under a DIFFERENT game for
    // up to a second is a wrong answer, and this feature exists precisely so the user does
    // not have to wonder whether what they are reading is current.
    st->stageByHeavy.clear();
    st->targetStage = CpuSetStageInfo();
    st->targetStageText.clear();
    // Switching profile switches both mask selections, so the parked warnings belong to a
    // different pair of masks now.
    RelayoutIfWarningsChanged(st);
}

void StoreUiToProfile(SettingsState* st) {
    if (st->selProfile < 0 ||
        st->selProfile >= static_cast<int>(st->work.profiles.size())) return;
    Profile& p = st->work.profiles[static_cast<size_t>(st->selProfile)];
    p.enabled = IsChecked(st->hEnabled);
    p.game = Trim(GetText(st->hGame));
    std::wstring gm = ComboText(st->hGameMask);
    if (!gm.empty()) p.gameMask = gm;
    // Membership still comes from the listbox so adds and removes work. Order comes from the
    // user's canonical list so clicking between profiles can no longer silently rewrite it.
    p.heavy = RestoreCanonicalOrder(HeavyItems(st), st->heavyCanonical);
    std::wstring hm = ComboText(st->hHeavyMask);
    if (!hm.empty()) p.heavyMask = hm;
    p.autoPin = IsChecked(st->hAutoPin);
    int v = 0;
    if (ParseIntW(Trim(GetText(st->hPct)), v)) p.autoPinPercent = v;
    // p.autoPinSeconds is left exactly as it was. ValidateAndRepair owns that field now.
}

// ---------------------------------------------------------------------------
// Live CPU% for the Heavy apps meters
// ---------------------------------------------------------------------------

// The threshold the meter marks. Read from the FIELD rather than the stored profile so the
// mark tracks what the user is typing, which is the whole point of showing it beside the
// live figure.
double AutoPinThreshold(const SettingsState* st) {
    int v = 0;
    if (st->hPct && ParseIntW(Trim(GetText(st->hPct)), v) && v > 0 && v <= 100)
        return static_cast<double>(v);
    if (st->selProfile >= 0 && st->selProfile < static_cast<int>(st->work.profiles.size()))
        return static_cast<double>(
            st->work.profiles[static_cast<size_t>(st->selProfile)].autoPinPercent);
    return 8.0;
}

// One snapshot per timer tick. The FIRST one has no predecessor, so every cpuPercent in it is
// 0 by construction and the meters stay empty until the second tick - that is a real "not
// measured yet", not a claim that nothing is busy.
void RefreshCpuTable(SettingsState* st) {
    ProcessSnapshot next;
    const int threshold = static_cast<int>(AutoPinThreshold(st) + 0.5);
    if (!next.Take(st->haveCpu ? &st->cpuSnap : nullptr, 1000,
                   GetTotalLogicalProcessors(), threshold)) {
        return;   // transient enumeration failure; keep the previous table rather than zeroing
    }
    st->cpuSnap = next;
    st->haveCpu = true;

    st->cpuByExe.clear();
    const std::map<DWORD, ProcInfo>& all = st->cpuSnap.All();
    for (std::map<DWORD, ProcInfo>::const_iterator it = all.begin(); it != all.end(); ++it) {
        // Summed across every instance of one executable, which is what a user reading
        // "chrome.exe 22%" expects and what the auto-pin rule effectively sees.
        st->cpuByExe[ToLower(it->second.name)] += it->second.cpuPercent;
    }
}

// `running` distinguishes "0% right now" from "not running at all" - the second draws the
// name dimmed, because a heavy entry naming an exe that is not on the machine is worth
// noticing rather than reporting as an idle process.
double CpuForExe(const SettingsState* st, const std::wstring& exe, bool& running) {
    running = false;
    const std::wstring key = ToLower(BaseName(Trim(exe)));
    if (key.empty()) return 0.0;
    std::map<std::wstring, double>::const_iterator it = st->cpuByExe.find(key);
    if (it == st->cpuByExe.end()) return 0.0;
    running = true;
    double v = it->second;
    if (v < 0.0) v = 0.0;
    if (v > 100.0) v = 100.0;
    return v;
}

// ---------------------------------------------------------------------------
// Which mask the target and each heavy app are CURRENTLY on
// ---------------------------------------------------------------------------
//
// READ BACK FROM WINDOWS, NEVER FROM WHAT WE MEANT TO APPLY. EngineStatus already publishes
// one mask name per governed pid and it would have been free to print - but that field is
// INTENT, and this app has measured Windows accepting an assignment and then ignoring it, and
// has measured other software holding CPU Set assignments it did not make. A row that showed
// intent would be right exactly when nobody needed to look at it.

// Defined further down with the rest of the auto-pin state; declared here because an All
// Games profile names no executable, and the only honest target it can have is the game the
// engine actually matched - which is worth reporting only while that match is THIS profile.
bool StatusDescribesProfile(const EngineStatus& s, const Profile& p);

// How many instances of one list entry are read back per tick.
//
// NOT A CPU BUDGET. MEASURED 2026-08-29 on this machine: one full readback - OpenProcess with
// PROCESS_QUERY_LIMITED_INFORMATION, the two-call GetProcessDefaultCpuSets, CloseHandle -
// costs 1.43-1.72 us, and every process on a 377-process desktop reads in about 0.6 ms. This
// is a ceiling on the pathological case (a browser or a build tool with a hundred children),
// and sixteen instances is far more than enough to notice that they disagree.
const size_t kStageProbePerEntry = 16;

// The pids paired with the creation time THIS snapshot recorded for each of them, which is
// what makes the readback pid-reuse-safe: the applier re-checks that creation time through
// the same handle it reads the mask through, and refuses to report on a pid that has been
// recycled since. It costs nothing here - the creation time is already in the snapshot the
// CPU meters just took, so there is no second enumeration and no extra syscall.
//
// A pid the snapshot does not hold is DROPPED rather than passed on with a zero creation
// time. Nothing here can vouch for it, and this row exists to answer a question about our
// process; the only such pid is the engine's game below, and one tick later our own snapshot
// has it.
std::vector<ObservedProc> ObservedFromSnapshot(const ProcessSnapshot& snap,
                                               const std::vector<DWORD>& pids) {
    std::vector<ObservedProc> out;
    out.reserve(pids.size());
    for (size_t i = 0; i < pids.size(); ++i) {
        const ProcInfo* pi = snap.Find(pids[i]);
        if (pi == nullptr) continue;
        ObservedProc o;
        o.pid = pi->pid;
        o.creationTime = pi->creationTime;   // 0 when the snapshot could not open it
        out.push_back(o);
    }
    return out;
}

// Re-reads which mask the target and every visible heavy entry are on. Costs one snapshot's
// worth of nothing extra: it reuses the ProcessSnapshot RefreshCpuTable just took, so no
// second enumeration and no second timer.
//
// Returns true when the TARGET's label changed. The heavy rows need no such answer - they are
// owner-drawn and the list is invalidated on every tick anyway - but the target's is painted
// by the parent, which only repaints when told something moved.
bool RefreshCpuSetStages(SettingsState* st) {
    const std::wstring was = st->targetStageText;
    st->stageByHeavy.clear();
    st->targetStage = CpuSetStageInfo();
    st->targetStageText.clear();
    if (!st->haveCpu) return !was.empty();   // no snapshot yet: there is nothing to ask

    // The masks the user has NAMES for, taken from the working config rather than from the
    // topology, so the answer is drawn from the same list the two combo boxes offer. A mask
    // the user hand-edited on the core map page is still their mask and still has their name.
    const std::vector<Mask>& masks = st->work.masks;

    // EVERY row, the auto-pin ones included: "which mask is it on RIGHT NOW" is the single
    // strongest piece of evidence that the rule did what it says, and a row that showed the
    // tag but no readback would be asking the user to take our word for it. HeavyRows(false)
    // rather than HeavyItems() - this direction only READS, so the config guard does not
    // apply, and applying it here would leave exactly the new rows blank.
    const std::vector<std::wstring> heavy = HeavyRows(st, false);
    for (size_t i = 0; i < heavy.size(); ++i) {
        const std::wstring key = ToLower(heavy[i]);
        if (st->stageByHeavy.find(key) != st->stageByHeavy.end()) continue;   // duplicate row
        // FindBySpec, not a basename lookup: it is the matcher the ENGINE uses, so a heavy
        // entry written as a full path is resolved here exactly as it is resolved when the
        // mask is applied, and the two cannot report different processes.
        st->stageByHeavy[key] = ReadCpuSetStage(
            ObservedFromSnapshot(st->cpuSnap, st->cpuSnap.FindBySpec(heavy[i])),
            masks, kStageProbePerEntry);
    }

    // NO PROFILE SELECTED LEAVES THE LABEL EMPTY, and LayoutPage then draws nothing. "-" is
    // reserved for a target we looked for and did not find; with no profile there is no
    // target to look for, and the two are not the same statement.
    const bool has = st->selProfile >= 0 &&
                     st->selProfile < static_cast<int>(st->work.profiles.size());
    if (!has) return was != st->targetStageText;

    const Profile& p = st->work.profiles[static_cast<size_t>(st->selProfile)];
    std::vector<DWORD> gamePids;
    const std::wstring game = Trim(GetText(st->hGame));
    if (!game.empty()) {
        // The FIELD, not the stored profile - the same rule the CPU meters follow. What the
        // user is looking at is what the row has to describe.
        //
        // The GAME ONLY. Its descendants carry the game mask too, but this row is captioned
        // "Game:" and reporting a family under that caption would be answering a question
        // nobody asked with a value that can legitimately differ.
        gamePids = st->cpuSnap.FindBySpec(game);
    } else if (p.isAllGames && st->engine) {
        // An All Games profile names no executable, so the only target it HAS is the one the
        // engine matched - and only while that match is this profile's doing, or the row
        // would describe a game some other profile is governing.
        //
        // This pid comes from the ENGINE's snapshot, not ours, so it is looked up in ours
        // below to pick up a creation time we recorded ourselves. A game that started in the
        // few milliseconds between the two snapshots is not in ours yet and waits for the
        // next tick, which is a beat of "-" rather than a mask read off a pid nothing here
        // can vouch for.
        const EngineStatus s = st->engine->GetStatus();
        if (s.gamePid != 0 && StatusDescribesProfile(s, p)) gamePids.push_back(s.gamePid);
    }
    st->targetStage = ReadCpuSetStage(ObservedFromSnapshot(st->cpuSnap, gamePids),
                                      masks, kStageProbePerEntry);
    st->targetStageText = CpuSetStageLabel(st->targetStage);
    return was != st->targetStageText;
}

// ---------------------------------------------------------------------------
// The auto-pin status line
// ---------------------------------------------------------------------------

// Is the profile the engine matched the same one this editor is showing?
//
// The name is the primary key because that is what EngineStatus publishes. The game
// executable is a SECOND key, so a profile the user renamed but has not applied yet is still
// recognised as the running one instead of silently reporting "another profile is active".
bool StatusDescribesProfile(const EngineStatus& s, const Profile& p) {
    if (!s.active) return false;
    if (!s.profileName.empty() && IEquals(s.profileName, p.name)) return true;
    if (s.gamePid != 0 && !p.game.empty()) {
        const std::wstring want = ToLower(BaseName(Trim(p.game)));
        for (size_t i = 0; i < s.governed.size(); ++i) {
            if (s.governed[i].pid != s.gamePid) continue;
            return !want.empty() && IEquals(s.governed[i].name, want);
        }
    }
    return false;
}

// Does the game own the foreground? This is precondition (b) of ComputeDesired rule 4,
// evaluated against exactly what EngineStatus publishes.
//
// The game SET is the game pid plus its non-excluded descendants, and EngineStatus does not
// publish that set as a set - it publishes one mask name per governed pid. So a descendant is
// recognised by its mask name, and ONLY when the two mask names differ: if a profile points
// its game mask and its heavy mask at the same mask, a governed pid's mask name cannot say
// which set it came from, and the conservative answer there is "not the game", which
// under-claims rather than over-claims.
bool GameOwnsForeground(const EngineStatus& s) {
    const DWORD fg = GetForegroundPid();
    if (fg == 0) return false;
    if (s.gamePid != 0 && fg == s.gamePid) return true;
    if (s.gameMaskName.empty()) return false;
    if (IEquals(s.gameMaskName, s.heavyMaskName)) return false;
    for (size_t i = 0; i < s.governed.size(); ++i) {
        if (s.governed[i].pid != fg) continue;
        return IEquals(s.governed[i].maskName, s.gameMaskName);
    }
    return false;
}

// The exe this profile's rule waits on, for the sentence. An All Games profile carries a
// pipe-separated candidate list rather than one executable, so it is described rather than
// quoted - printing that list would be worse than useless.
std::wstring AutoPinGameLabel(const Profile& p) {
    if (p.isAllGames) return L"a detected game";
    const std::wstring g = BaseName(Trim(p.game));
    return g.empty() ? std::wstring(L"the game") : g;
}

// The mask the rule moves processes ONTO. Read from the combo so it tracks the editor, with
// the stored value as the fallback for the moment before the combo has a selection.
std::wstring AutoPinTargetMask(const SettingsState* st, const Profile& p) {
    std::wstring m = ComboText(st->hHeavyMask);
    if (m.empty()) m = p.heavyMask;
    return m.empty() ? std::wstring(L"the background mask") : m;
}

// ---------------------------------------------------------------------------
// The auto-pin rows
// ---------------------------------------------------------------------------

// HOW MANY AUTO ROWS ARE SHOWN AT ONCE, and why there is a limit at all.
//
// The set is unbounded in principle: rule 4 admits every process over the threshold and the
// user may set that threshold to 1%. The list has a minimum of Dp(52) and grows with the window -
// and the user's OWN entries are the editable half of it. Letting the readback grow without
// limit would push the rows they came here to edit out of view behind a scrollbar, so the
// feature that explains auto-pin would break the control it was added to.
//
// Scrolling was the alternative and it is worse: the manual rows stay reachable but only by
// scrolling past a readout, and the row a user is looking for moves every time the auto set
// changes. Capping keeps the editable rows where they were and costs one honest caption.
//
// Eight, and the caption, rather than three: the measured case had SEVEN governed processes,
// so a cap that hides the normal case would report the feature as busier than it is.
const size_t kAutoRowsShown = 8;

// Rebuilds the auto-pin rows at the END of the heavy list from what the ENGINE published.
// Returns true when the rows actually moved.
//
// Called from the one 1 s status timer this window already runs - no second timer; this app
// exists to save CPU and a timer that fires four times a minute to redraw a list nobody is
// looking at would be the wrong kind of feature. Costs one GetStatus (a mutex and a copy)
// plus one pass over the governed vector, and touches the control at all only when the set
// differs from the cached one - LB_DELETESTRING/LB_ADDSTRING churn once a second would reset
// the selection and the scroll position while the user was reading the list.
bool SyncAutoPinRows(SettingsState* st) {
    if (!st || !st->hHeavy) return false;

    std::vector<std::wstring> rows;
    st->autoApplyByExe.clear();
    size_t total = 0;
    bool haveMore = false;

    const bool has = st->selProfile >= 0 &&
                     st->selProfile < static_cast<int>(st->work.profiles.size());
    if (has && st->engine) {
        const Profile& p = st->work.profiles[static_cast<size_t>(st->selProfile)];
        const EngineStatus s = st->engine->GetStatus();
        // ONLY while the engine is actually running THIS profile. Rule 1 picks one profile
        // and the rest are inert, so listing another profile's auto-pinned processes under
        // this editor would attribute them to a rule that is not running. Paused governs
        // nothing at all, and the status sentence says so on its own line.
        if (!s.paused && StatusDescribesProfile(s, p)) {
            for (size_t i = 0; i < s.governed.size(); ++i) {
                const GovernedProcess& g = s.governed[i];
                if (!g.autoPinned) continue;
                const std::wstring key = ToLower(BaseName(Trim(g.name)));
                if (key.empty()) continue;
                AutoApplySummary& summary = st->autoApplyByExe[key];
                if (g.applyResult == ApplyResult::Ok) {
                    ++summary.applied;
                } else if (g.applyResult == ApplyResult::AccessDenied) {
                    ++summary.accessDenied;
                } else {
                    ++summary.failed;
                }
            }
            // The set comes from the ENGINE, collapsed to executables by the pure helper in
            // engine.h. Nothing about rule 4 is re-decided here: a second implementation in
            // the window would be free to disagree with the first, and the disagreement
            // would look exactly like the bug this feature exists to rule out.
            std::vector<std::wstring> names = AutoPinnedExeNames(s, HeavyItems(st));
            total = names.size();
            if (names.size() > kAutoRowsShown) {
                haveMore = true;
                names.resize(kAutoRowsShown);
            }
            rows.swap(names);
            if (haveMore) {
                rows.push_back(L"+" + std::to_wstring(total - kAutoRowsShown) +
                               L" more auto-pinned");
            }
        }
    }

    st->autoTotal = total;
    if (rows == st->autoRows) return false;
    st->autoRows = rows;

    const int sel = static_cast<int>(SendMessageW(st->hHeavy, LB_GETCURSEL, 0, 0));
    const int top = static_cast<int>(SendMessageW(st->hHeavy, LB_GETTOPINDEX, 0, 0));

    // Drop the old readback. It is a contiguous TAIL - HeavyAppend inserts the user's entries
    // in front of it - so this walks back from the end and stops at the first manual row, and
    // no manual index moves.
    for (int i = static_cast<int>(SendMessageW(st->hHeavy, LB_GETCOUNT, 0, 0)) - 1;
         i >= 0; --i) {
        if (SendMessageW(st->hHeavy, LB_GETITEMDATA, static_cast<WPARAM>(i), 0)
                == kHeavyRowManual) {
            break;
        }
        SendMessageW(st->hHeavy, LB_DELETESTRING, static_cast<WPARAM>(i), 0);
    }
    const int manual = static_cast<int>(SendMessageW(st->hHeavy, LB_GETCOUNT, 0, 0));

    for (size_t i = 0; i < rows.size(); ++i) {
        const LRESULT row = SendMessageW(st->hHeavy, LB_ADDSTRING, 0,
                                         reinterpret_cast<LPARAM>(rows[i].c_str()));
        if (row < 0) continue;                       // LB_ERR / LB_ERRSPACE
        SendMessageW(st->hHeavy, LB_SETITEMDATA, static_cast<WPARAM>(row),
                     (haveMore && i + 1 == rows.size()) ? kHeavyRowMore : kHeavyRowAuto);
    }

    // A selection on one of the user's rows is exactly where they left it. A selection that
    // was on an auto row is CLEARED rather than moved to whatever now occupies that index -
    // the Remove button acts on the selection, and silently sliding it onto a different
    // process is how a user deletes an entry they never selected.
    SendMessageW(st->hHeavy, LB_SETCURSEL,
                 static_cast<WPARAM>(sel >= 0 && sel < manual ? sel : -1), 0);
    if (top >= 0) SendMessageW(st->hHeavy, LB_SETTOPINDEX, static_cast<WPARAM>(top), 0);
    return true;
}

// Recomputes the state and the sentence. Returns true when either changed - the status dot is
// drawn by the PARENT, so a colour change needs the parent's repaint, not the label's.
bool RefreshAutoPinStatus(SettingsState* st) {
    if (!st->hAutoStatus) return false;

    AutoPinState want = AutoPinState::Hidden;
    std::wstring line;

    const bool has = st->selProfile >= 0 &&
                     st->selProfile < static_cast<int>(st->work.profiles.size());
    if (has) {
        const Profile& p = st->work.profiles[static_cast<size_t>(st->selProfile)];
        const std::wstring game = AutoPinGameLabel(p);
        const int pct = static_cast<int>(AutoPinThreshold(st) + 0.5);
        const std::wstring pctText = std::to_wstring(pct);

        // The check box, not the stored field: the sentence has to describe what the user is
        // looking at, and the box is what they just clicked.
        if (!IsChecked(st->hAutoPin)) {
            want = AutoPinState::Off;
            line = L"Auto-pin is off for this profile, so nothing is moved automatically.";
        } else if (!IsChecked(st->hEnabled)) {
            want = AutoPinState::Waiting;
            line = L"Waiting - this profile is turned off, so the rule never runs.";
        } else if (!p.isAllGames && Trim(p.game).empty()) {
            want = AutoPinState::Waiting;
            line = L"Waiting - no game executable is set for this profile, so the rule can "
                   L"never match anything.";
        } else {
            EngineStatus s;
            if (st->engine) s = st->engine->GetStatus();
            if (s.paused) {
                want = AutoPinState::Paused;
                line = L"Paused - Game Optimizer is paused, so no masks are being applied.";
            } else if (StatusDescribesProfile(s, p)) {
                if (GameOwnsForeground(s)) {
                    want = AutoPinState::Active;
                    // WITH THE COUNT, once there is one. "processes above 1% are being moved"
                    // is a description of the rule; "3 apps above 1% are on Freq" is a report
                    // of what happened, and the operator's complaint was that the window only
                    // ever offered the first. The rows carry the names; this carries the
                    // number and teaches the tag those rows are marked with.
                    if (st->autoTotal > 0) {
                        line = L"Active - " + std::to_wstring(st->autoTotal) +
                               L" app" + (st->autoTotal == 1 ? L"" : L"s") + L" above " +
                               pctText + L"% moved to " + AutoPinTargetMask(st, p) +
                               L", tagged AUTO above.";
                    } else {
                        line = L"Active - processes above " + pctText +
                               L"% are being moved to " + AutoPinTargetMask(st, p) + L".";
                    }
                } else {
                    want = AutoPinState::Paused;
                    line = L"Paused - " + game +
                           L" is running but not in the foreground.";
                }
            } else if (s.active) {
                // Rule 1: the FIRST enabled profile whose game is live wins, so a different
                // winner means this rule cannot fire even if its own game is running too.
                want = AutoPinState::Waiting;
                line = L"Waiting - \"" + s.profileName +
                       L"\" is the profile Game Optimizer matched, so this rule is not the "
                       L"one running.";
            } else {
                want = AutoPinState::Waiting;
                line = L"Waiting - this rule only runs while " + game + L" is running.";
            }
        }
    }

    const bool changed = (want != st->autoState) || (GetText(st->hAutoStatus) != line);
    if (!changed) return false;
    st->autoState = want;
    SetWindowTextW(st->hAutoStatus, line.c_str());
    // Same rule as the parked warnings: a Profiles-page control may only become visible while
    // that page is the one on screen, or it floats over whatever page actually is.
    ShowWindow(st->hAutoStatus,
               (!line.empty() && st->page == PAGE_PROFILES) ? SW_SHOW : SW_HIDE);
    return true;
}

// The colour of the status dot beside that sentence. Display only, derived from the same
// state the sentence itself is built from - never from a second source.
COLORREF AutoPinDotColour(const SettingsState* st) {
    const theme::Palette& pal = theme::P();
    switch (st->autoState) {
        case AutoPinState::Active: return pal.good;
        case AutoPinState::Paused: return pal.warn;
        default:                   return pal.textDim;
    }
}

// True while the rule could actually fire - now, or the moment the user alt-tabs back into
// the game. When this is FALSE the CPU meters drop their good/warn/danger ramp for a flat
// textDim, because a red bar next to a rule that cannot fire is a promise the app will not
// keep. Off and Waiting are both "cannot fire at all"; Paused is one alt-tab away, so it
// keeps the ramp.
bool AutoPinCanFire(const SettingsState* st) {
    return st->autoState == AutoPinState::Active || st->autoState == AutoPinState::Paused;
}

// theme::DrawCpuMeter with the ramp replaced by one flat colour. It is a local copy rather
// than a new theme entry point because theme.h is frozen this round; the geometry - track
// height, radius, 1px outline, the fill inside that outline, the tick spanning the track -
// is kept identical to theme.cpp's so the two cannot drift into different-looking meters.
void DrawCpuMeterDim(HDC dc, const RECT& rc, double pct, int dpi, double threshold) {
    if (!dc) return;
    const int availW = rc.right - rc.left;
    const int availH = rc.bottom - rc.top;
    if (availW <= 1 || availH <= 0) return;

    const theme::Palette& pal = theme::P();
    if (pct < 0.0) pct = 0.0;
    if (pct > 100.0) pct = 100.0;
    if (threshold < 1.0) threshold = 1.0;
    if (threshold > 100.0) threshold = 100.0;

    int trackH = theme::Dp(6, dpi);
    if (trackH < 3) trackH = 3;
    if (trackH > availH) trackH = availH;

    RECT track;
    track.left = rc.left;
    track.right = rc.right;
    track.top = rc.top + (availH - trackH) / 2;
    track.bottom = track.top + trackH;
    const int trackW = track.right - track.left;
    const int radius = trackH / 2;

    theme::DrawRoundRect(dc, track, radius, pal.inputBg, pal.border);

    RECT inner = track;
    InflateRect(&inner, -1, -1);
    const int innerW = inner.right - inner.left;
    const int innerH = inner.bottom - inner.top;
    if (innerW > 0 && innerH > 0) {
        int fillW = static_cast<int>((pct / 100.0) * static_cast<double>(innerW) + 0.5);
        if (fillW > innerW) fillW = innerW;
        if (pct > 0.0 && fillW < innerH) fillW = innerW < innerH ? innerW : innerH;
        if (fillW > 0) {
            RECT f = inner;
            f.right = f.left + fillW;
            theme::FillRoundRect(dc, f, innerH / 2, pal.textDim);
        }
    }

    int tickW = theme::Dp(1, dpi);
    if (tickW < 1) tickW = 1;
    int tx = track.left +
             static_cast<int>((threshold / 100.0) * static_cast<double>(trackW) + 0.5);
    if (tx > track.right - tickW) tx = track.right - tickW;
    if (tx < track.left) tx = track.left;
    RECT tick;
    tick.left = tx;
    tick.right = tx + tickW;
    tick.top = track.top;
    tick.bottom = track.bottom;
    if (tick.right > tick.left && tick.bottom > tick.top) {
        HBRUSH tb = CreateSolidBrush(pal.textDim);
        if (tb) {
            FillRect(dc, &tick, tb);
            DeleteObject(tb);
        }
    }
}

void StoreGeneralToWork(SettingsState* st) {
    st->work.startWithWindows = IsChecked(st->hStartup);
    st->work.notifications = IsChecked(st->hNotify);
    int v = 0;
    if (ParseIntW(Trim(GetText(st->hPoll)), v)) st->work.pollMs = v;
}

void SelectMapMask(SettingsState* st) {
    if (!st->hMap) return;
    std::wstring name = ComboText(st->hMapMask);
    const Mask* m = st->work.FindMask(name);
    std::vector<ULONG> ids;
    if (m) ids = m->ids;
    st->loading = true;
    CoreMapSetSelection(st->hMap, ids);
    st->loading = false;
}

// ---------------------------------------------------------------------------
// Parked-mask warnings.
//
// docs\spec\03-risks.md 2a: an assignment can be accepted and then ignored, and the setter
// and the getter both report success when it is. Parked processors are one of the two
// routes into that state we can actually SEE, so the warning is raised at the moment the
// user picks the mask rather than after they wonder why nothing changed.
//
// These strings say what is parked and that Windows MAY ignore the assignment. None of them
// says a mask is working, active or verified, because nothing here can establish that.
// ---------------------------------------------------------------------------

// Re-detect the machine. A failure is transient (the enumeration can fail under memory
// pressure); the previous snapshot is kept rather than reporting a machine with no parked
// processors, which would be a silently WRONG all-clear.
void RefreshLiveTopology(SettingsState* st) {
    Topology fresh;
    std::wstring err;
    if (!DetectTopology(fresh, &err)) return;
    st->live = fresh;
    st->haveLive = true;
}

// Empty string means "nothing worth saying". `maskName` is looked up in the WORKING config
// because this warns about the choice being made in the editor, not about what is applied.
std::wstring MaskParkedWarning(const SettingsState* st, const std::wstring& maskName) {
    if (!st->haveLive || maskName.empty()) return std::wstring();
    const Mask* m = st->work.FindMask(maskName);
    if (m == nullptr || m->ids.empty()) return std::wstring();

    int total = 0, parked = 0;
    for (size_t i = 0; i < m->ids.size(); ++i) {
        const CpuSetEntry* e = FindById(st->live, m->ids[i]);
        if (e == nullptr) continue;      // an id this machine does not have; not a park issue
        ++total;
        if (e->Parked) ++parked;
    }
    if (total == 0 || parked == 0) return std::wstring();

    const std::wstring n = std::to_wstring(total);
    const std::wstring p = std::to_wstring(parked);
    if (parked == total) {
        return FormatFullyParkedMaskWarning(maskName, total,
                                            st->env.amdVCacheServiceRunning);
    }
    if (parked * 2 > total) {
        return p + L" of " + n + L" processors in \"" + maskName +
               L"\" are currently parked - Windows may ignore this assignment.";
    }
    return std::wstring();
}

// Recomputes both labels. Returns true when either label's text changed, which is the
// caller's cue to re-run the layout: these rows take no vertical space when empty.
bool UpdateMaskWarnings(SettingsState* st) {
    bool changed = false;
    HWND pair[2] = { st->hGameMaskWarn, st->hHeavyMaskWarn };
    HWND combo[2] = { st->hGameMask, st->hHeavyMask };
    for (int i = 0; i < 2; ++i) {
        if (!pair[i]) continue;
        std::wstring want = MaskParkedWarning(st, ComboText(combo[i]));
        if (GetText(pair[i]) == want) continue;
        SetWindowTextW(pair[i], want.c_str());
        // A warning belongs to the Profiles page. It may only become visible while that page
        // is the one on screen, or it would float over whatever page actually is.
        ShowWindow(pair[i],
                   (!want.empty() && st->page == PAGE_PROFILES) ? SW_SHOW : SW_HIDE);
        changed = true;
    }
    return changed;
}

// Returns true when the line's height requirement changed, i.e. the caller must re-layout.
bool RefreshBlockedLine(SettingsState* st) {
    if (!st->hBlocked || !st->engine) return false;
    EngineStatus s = st->engine->GetStatus();
    std::vector<std::wstring> names;
    for (size_t i = 0; i < s.governed.size(); ++i) {
        if (s.governed[i].blocked) names.push_back(s.governed[i].name);
    }
    std::wstring line;
    if (names.empty()) {
        line = L"No processes are currently blocked. Game Optimizer runs unelevated on "
               L"purpose; anything it cannot touch will be named here rather than skipped "
               L"silently.";
    } else {
        line = L"Blocked (access denied), " + std::to_wstring(names.size()) + L": ";
        for (size_t i = 0; i < names.size(); ++i) {
            if (i) line += L", ";
            line += names[i];
        }
        line += L". These are elevated or protected processes; no mask was applied to them.";
    }
    // EngineStatus::staleTopology - the watcher saw an apply refused as an invalid CPU Set
    // Id. It deliberately does not re-detect on its own, so this is the only place the user
    // finds out that the stored ids stopped describing this machine.
    if (s.staleTopology) {
        line += L"\r\nA CPU Set assignment was refused as INVALID, which means the stored "
                L"CPU Set Ids no longer match this machine. Reset the masks to detected, or "
                L"restart Game Optimizer, so the topology is read again.";
    }
    SetWindowTextW(st->hBlocked, line.c_str());

    const bool tall = s.staleTopology;
    const bool bad = !names.empty();
    const bool changed = (tall != st->blockedTall) || (bad != st->blockedBad);
    st->blockedTall = tall;
    st->blockedBad = bad;
    return changed;
}

// Updates the General page's environment card from the already-probed value. Returns true
// only when the running-service explanation appeared or disappeared, because that is the
// only change that alters the card's height and therefore requires a layout pass.
bool UpdateEnvironmentSection(SettingsState* st) {
    if (!st) return false;

    const std::wstring game = FormatGameModeEnvironmentStatus(st->env.gameModeState);
    const std::wstring service =
        FormatAmdVCacheEnvironmentStatus(st->env.amdVCacheServiceState);
    const std::wstring effect =
        st->env.amdVCacheServiceState == AmdVCacheServiceState::Running
            ? AmdVCacheRunningEffectText()
            : std::wstring();

    const bool hadEffect = st->hVCacheEffect &&
                           GetWindowTextLengthW(st->hVCacheEffect) > 0;
    const bool hasEffect = !effect.empty();

    if (st->hGameModeStatus && GetText(st->hGameModeStatus) != game)
        SetWindowTextW(st->hGameModeStatus, game.c_str());
    if (st->hVCacheStatus && GetText(st->hVCacheStatus) != service)
        SetWindowTextW(st->hVCacheStatus, service.c_str());
    if (st->hVCacheEffect && GetText(st->hVCacheEffect) != effect)
        SetWindowTextW(st->hVCacheEffect, effect.c_str());
    if (st->hVCacheEffect) {
        ShowWindow(st->hVCacheEffect,
                   hasEffect && st->page == PAGE_GENERAL ? SW_SHOW : SW_HIDE);
    }
    return hadEffect != hasEffect;
}

// ---------------------------------------------------------------------------
// Layout
//
// WHY THERE IS NO SCROLLBAR ANY MORE. This window used to be one tall column with
// WS_VSCROLL, and every child was positioned in content space with the scroll offset
// subtracted. Moving forty-odd children on every wheel tick is what produced the repaint
// corruption in the bug report: the copied bits of a moved window are stale, and a child
// that never receives WM_PAINT keeps whatever pixels it had. The sidebar removes the
// failure mode instead of patching it - one page is visible at a time, each page is laid
// out to fit the content area, and nothing moves unless the window is resized or the page
// is switched.
//
// ONE function computes the geometry for both consumers: SettingsLayout (which moves the
// children) and PaintSettings (which draws the cards behind them). They cannot drift apart
// because there is only one set of maths.
// ---------------------------------------------------------------------------

// Every child position for one pass, applied in ONE DeferWindowPos batch by Flush().
// Moving ~45 controls with 45 separate SetWindowPos calls lets the user see each
// intermediate state; one batch does not.
//
// SWP_NOCOPYBITS is what stops the ghosting. Without it SetWindowPos bit-blits a moved
// window's old pixels to its new position and only invalidates the difference, so when many
// controls move at once the copied bits are stale and leave doubled buttons and smeared
// labels.
struct PosBatch {
    enum { kMax = 96 };
    struct Move { HWND h; int x, y, w, h2; };
    int n;
    Move items[kMax];

    void operator()(HWND h, int x, int yy, int w, int hh) {
        if (!h) return;
        if (n >= kMax) {   // cannot happen with the current control count; stay correct
            SetWindowPos(h, nullptr, x, yy, w, hh, Flags());
            return;
        }
        Move m = { h, x, yy, w, hh };
        items[n++] = m;
    }
    static UINT Flags() { return SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOCOPYBITS; }
    void Flush() {
        const UINT f = Flags();
        HDWP d = BeginDeferWindowPos(n);
        if (d) {
            for (int i = 0; i < n && d; ++i)
                d = DeferWindowPos(d, items[i].h, nullptr, items[i].x, items[i].y,
                                   items[i].w, items[i].h2, f);
            // A failed DeferWindowPos destroys the batch without having moved anything, so
            // the fallback below is still a complete layout rather than a partial one.
            if (d) {
                EndDeferWindowPos(d);
                n = 0;
                return;
            }
        }
        for (int i = 0; i < n; ++i)
            SetWindowPos(items[i].h, nullptr, items[i].x, items[i].y,
                         items[i].w, items[i].h2, f);
        n = 0;
    }
};

// The display list the parent paints behind its children: cards, drawn strings, status
// dots, pills and the one ring gauge. Built by LayoutPage, consumed by PaintSettings.
struct TextItem {
    RECT rc;
    std::wstring s;
    theme::Font f;
    COLORREF col;
    UINT fmt;
};
struct DotItem { int x, y, r; COLORREF col; };
struct PillItem { RECT rc; std::wstring s; COLORREF bg, fg; };

struct Geom {
    // `sidebar` KEEPS ITS NAME and now holds the top tab bar's strip. Nothing paints it - the
    // bar is its own window - so it is geometry the two consumers agree on, nothing more.
    RECT sidebar, content, footer;
    int  nCard;   RECT card[8];
    int  nStat;   RECT stat[3];
    int  nTxt;    TextItem txt[28];
    int  nDot;    DotItem  dot[4];
    int  nPill;   PillItem pill[4];
    bool hasGauge;
    RECT gauge;
    double pct;
    std::wstring gaugeCentre, gaugeUnit;
    Geom() : nCard(0), nStat(0), nTxt(0), nDot(0), nPill(0), hasGauge(false), pct(0.0) {
        SetRectEmpty(&sidebar);
        SetRectEmpty(&content);
        SetRectEmpty(&footer);
        SetRectEmpty(&gauge);
    }
};

struct ProfileColumns {
    int leftW;
    int rightW;
};

ProfileColumns MeasureProfileColumns(int clientW, int dpi) {
    const int gap = theme::Dp(theme::metric::kGap, dpi);
    int contentW = clientW - 2 * gap;
    if (contentW < theme::Dp(240, dpi)) contentW = theme::Dp(240, dpi);

    ProfileColumns columns;
    columns.leftW = contentW * 38 / 100;
    if (columns.leftW < theme::Dp(260, dpi)) columns.leftW = theme::Dp(260, dpi);
    if (columns.leftW > theme::Dp(400, dpi)) columns.leftW = theme::Dp(400, dpi);
    if (columns.leftW > contentW - theme::Dp(320, dpi))
        columns.leftW = contentW - theme::Dp(320, dpi);
    if (columns.leftW < theme::Dp(200, dpi)) columns.leftW = theme::Dp(200, dpi);

    columns.rightW = contentW - columns.leftW - gap;
    if (columns.rightW < theme::Dp(240, dpi)) columns.rightW = theme::Dp(240, dpi);
    return columns;
}

struct WarningLayout {
    bool game;
    bool heavy;
    int gameH;
    int heavyH;
    int textW;
    int cardH;
};

// The warning statics use UiSmall and SS_LEFT, whose default drawing path word-wraps.
// Dp(36) preserves the old comfortable minimum; Dp(96) bounds pathological narrow-width
// growth while leaving room for roughly six lines at the supported window widths.
int MeasureWarningRow(HWND control, HDC dc, int textW, int dpi) {
    const int floorH = theme::Dp(36, dpi);
    const int capH = theme::Dp(96, dpi);
    int height = floorH;

    if (control && dc && textW > 0) {
        const std::wstring text = GetText(control);
        RECT measured = { 0, 0, textW, 0 };
        HFONT font = reinterpret_cast<HFONT>(
            SendMessageW(control, WM_GETFONT, 0, 0));
        if (!font) font = theme::GetFont(theme::Font::UiSmall, dpi);
        HGDIOBJ oldFont = SelectObject(dc, font);
        const int wrappedH = ::DrawTextW(dc, text.c_str(), static_cast<int>(text.size()),
                                         &measured, DT_CALCRECT | DT_WORDBREAK);
        SelectObject(dc, oldFont);
        if (wrappedH > height) height = wrappedH;
    }

    if (height > capH) height = capH;
    return height;
}

WarningLayout MeasureWarningLayout(SettingsState* st, HDC dc, int cardW, int dpi) {
    WarningLayout warning = {};
    if (!st) return warning;

    warning.game = st->hGameMaskWarn && GetWindowTextLengthW(st->hGameMaskWarn) > 0;
    warning.heavy = st->hHeavyMaskWarn && GetWindowTextLengthW(st->hHeavyMaskWarn) > 0;

    const int pad = theme::Dp(theme::metric::kCardPad, dpi);
    const int gapTight = theme::Dp(theme::metric::kGapTight, dpi);
    warning.textW = cardW - 2 * pad - theme::Dp(18, dpi);
    if (warning.textW < 1) warning.textW = 1;

    if (warning.game)
        warning.gameH = MeasureWarningRow(st->hGameMaskWarn, dc, warning.textW, dpi);
    if (warning.heavy)
        warning.heavyH = MeasureWarningRow(st->hHeavyMaskWarn, dc, warning.textW, dpi);

    const int rows = (warning.game ? 1 : 0) + (warning.heavy ? 1 : 0);
    if (rows > 0)
        warning.cardH = 2 * pad + warning.gameH + warning.heavyH + (rows - 1) * gapTight;
    return warning;
}

// `put` may be null: PaintSettings needs the geometry without moving anything.
void LayoutPage(SettingsState* st, HWND hwnd, Geom& g, PosBatch* put, HDC measureDc) {
    RECT rc;
    GetClientRect(hwnd, &rc);
    const int dpi = st->dpi;
    const theme::Palette& pal = theme::P();

    const int cw = rc.right - rc.left;
    const int ch = rc.bottom - rc.top;

    const int TABH  = theme::Dp(theme::metric::kTabBarH, dpi);
    const int PAD   = theme::Dp(theme::metric::kCardPad, dpi);
    const int GAP   = theme::Dp(theme::metric::kGap, dpi);
    const int GT    = theme::Dp(theme::metric::kGapTight, dpi);
    const int RH    = theme::Dp(theme::metric::kRowH, dpi);
    const int BH    = theme::Dp(theme::metric::kButtonH, dpi);
    const int BW    = theme::Dp(theme::metric::kButtonW, dpi);
    const int ROW   = BH;                       // one input row: tallest thing in it
    const int HH    = theme::Dp(22, dpi);       // a section heading line
    const int LH    = theme::Dp(18, dpi);       // one caption line
    const int LBL   = theme::Dp(104, dpi);
    const int CBW   = theme::Dp(200, dpi);      // combo width
    const int CBDROP= theme::Dp(240, dpi);      // combo height incl. its dropped list
    const int footerH = BH + 2 * GAP;

    const UINT kL   = DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX;
    const UINT kBig = DT_LEFT | DT_SINGLELINE | DT_BOTTOM | DT_NOPREFIX;

    // Menu on TOP. The bar spans the full width; the content column below it and the footer
    // now both start at x = 0, so a page gets the whole window width.
    SetRect(&g.sidebar, 0, 0, cw, TABH);
    SetRect(&g.footer, 0, ch - footerH, cw, ch);
    SetRect(&g.content, 0, TABH, cw, ch - footerH);

    auto Put = [&](HWND h, int x, int yy, int w, int hh) { if (put) (*put)(h, x, yy, w, hh); };
    auto Say = [&](int l, int t, int r, int b, const std::wstring& s, theme::Font f,
                   COLORREF c, UINT fmt) {
        if (g.nTxt >= 28) return;
        TextItem ti;
        SetRect(&ti.rc, l, t, r, b);
        ti.s = s; ti.f = f; ti.col = c; ti.fmt = fmt;
        g.txt[g.nTxt++] = ti;
    };
    auto AddCard = [&](int top, int height, int left, int width) -> RECT {
        RECT c;
        SetRect(&c, left, top, left + width, top + height);
        if (g.nCard < 8) g.card[g.nCard++] = c;
        return c;
    };

    // The tab bar spans the full width across the top; the footer spans the full width too.
    Put(st->hNav, 0, 0, cw, TABH);
    if (!st->hNav) {
        // Fallback switcher, used only when the bar class could not be created. Without it
        // two of the three pages would be unreachable. Laid out horizontally so the fallback
        // occupies the same strip the bar would have.
        int bx = GAP;
        const int fbw = theme::Dp(120, dpi);
        for (int i = 0; i < PAGE_COUNT; ++i) {
            Put(st->hNavBtn[i], bx, (TABH - BH) / 2, fbw, BH);
            bx += fbw + GT;
        }
    }

    const int x0 = GAP;
    int W = cw - 2 * GAP;
    if (W < theme::Dp(240, dpi)) W = theme::Dp(240, dpi);

    // ---- the sponsor panel -------------------------------------------------------------
    // NO HEIGHT for it is written down in this file, and there is no natural width left to
    // write down. SponsorBandSize above returns whichever rendering is actually in play - the
    // WebView2 panel's measured floor and height, or the GDI control's own cd::SponsorMeasure -
    // so when either changes SIZE, this layout follows it instead of going stale silently. That
    // held through 272x261 -> 462x150 -> 831x65 without one line here being edited.
    //
    // IT DID NOT HOLD THIS TIME, and the reason is worth recording rather than glossing: the
    // panel did not change size, it changed KIND. It stopped having a width of its own, which is
    // a question about PLACEMENT, and placement has always lived here. A layout that reads a
    // measurement cannot follow a change in what the measurement means.
    //
    // THE TWO RENDERINGS ARE NOW PLACED DIFFERENTLY, AND THAT IS NOT AN INCONSISTENCY.
    //
    //   WebView2  FULL WIDTH, one kGap a side. Its centre line is the window's centre line by
    //             construction rather than by arithmetic - it spans the whole content row, so
    //             it cannot be anywhere else. The page is `.shell.is-open { width: 100% }` and
    //             spreads its three groups across whatever host it is given with
    //             `justify-content: space-between`, so the gaps between the groups grow with
    //             the window. Handing it anything narrower would shrink those gaps for no
    //             reason; handing it a right-anchored fixed width - which is what this did
    //             until 2026-08-29 - pins the whole panel back into the bottom-right corner.
    //             band.cx is that rendering's FLOOR and is DELIBERATELY UNUSED on this path -
    //             it is overwritten below. The floor's one consumer is WM_GETMINMAXINFO, which
    //             keeps the window wide enough that the full-width host is never narrower than
    //             it. band.cy is what this path takes from the measurement.
    //   GDI       ITS OWN NATURAL WIDTH, right-anchored, exactly as before: its right edge
    //             lines up with the right edge of the Apply button, the same cw - GAP margin
    //             the footer row below uses. It is a DIFFERENT CONTROL WITH A DIFFERENT DESIGN
    //             - sponsor.cpp packs its buttons from its own left edge and paints nothing
    //             behind them - so stretching it across the window would leave a wide empty
    //             strip with three buttons huddled at one end.
    //
    // When there is nothing to show the size is zero and the band collapses entirely.
    const SIZE band = SponsorBandSize(st, dpi);
    int spW = static_cast<int>(band.cx);
    int spH = static_cast<int>(band.cy);

    // WHICH RENDERING THE BAND WAS MEASURED FOR. This is the SAME expression SponsorBandSize
    // uses to choose, and it has to stay that way: if the two ever disagree the panel is laid
    // out to the other one's shape. `webLate` means the patience timer put the GDI strip on
    // screen because the page had not reported in, so the band is the GDI row's and so is the
    // placement.
    const bool webBand = (st->web != nullptr && !st->webLate);

    // IF IT DOES NOT FIT, IT CLIPS - it never pushes the footer or the page content. The footer
    // row and the gap above it are fixed points; the panel is decoration plus three links and is
    // the thing that gives. `avail` is everything between the tab bar and that gap, so at a DPI
    // or window size where it cannot fit, the user loses part of the panel rather than the OK
    // button.
    //
    // WHICH part has changed with the placement, and it is worth saying plainly. The old
    // fixed-width panel clipped at its own LEFT edge, so the lockup in the bottom-right survived.
    // A `space-between` row that runs out of room overflows its END edge, so what disappears
    // behind `.shell { overflow: hidden }` is the RIGHT-hand group - the GOATPROJECT lockup.
    // Nothing degrades gracefully any more; what prevents it is the window minimum staying above
    // kSponsorCssMinWidth, which is what WM_GETMINMAXINFO below is for.
    const int avail = (ch - footerH - GAP) - (TABH + GAP);
    if (spH > avail) spH = avail > 0 ? avail : 0;

    int spLeft;
    if (webBand) {
        // THE HOST WINDOW IS THE CONTENT ROW.
        spW = cw - 2 * GAP;
        spLeft = GAP;
    } else {
        if (spW > cw - 2 * GAP) spW = cw - 2 * GAP;
        spLeft = cw - GAP - spW;
    }
    if (spW < 0) {
        spW = 0;
        spLeft = GAP;
    }

    const int spTop = ch - footerH - GAP - spH;
    if (spH > 0 && spW > 0 && st->web != nullptr) {
        // The host window IS the panel - same rectangle, no margin, transparent behind it.
        // It is moved even when the GDI strip is the one on screen (webLate): the host is
        // hidden then, and this only stops it sitting where it was created for the one frame
        // before SponsorWebReady re-runs this layout.
        RECT wr;
        ::SetRect(&wr, spLeft, spTop, spLeft + spW, spTop + spH);
        WebSponsorMove(st->web, wr);
    }
    // THE FALLBACK IS PARKED AT ITS OWN RECTANGLE, NEVER THE PANEL'S. When the GDI strip is the
    // band, this is the same rectangle the block above computed, so nothing moved. When the
    // WebView2 panel is up the strip is hidden - but SponsorWebReady and the patience timer both
    // call ShowWindow BEFORE they re-run this layout, so whatever position it is holding is what
    // the user sees for one frame. Until 2026-08-29 that was the panel's rectangle, which was at
    // least a strip-shaped one; the panel is now the whole content row and a 45px strip stretched
    // across it is not. Parking it where it actually belongs costs one SetWindowPos per layout.
    if (st->sponsorW > 0 && st->sponsorH > 0) {
        int gW = st->sponsorW;
        int gH = st->sponsorH;
        if (gW > cw - 2 * GAP) gW = cw - 2 * GAP;
        if (gH > avail) gH = avail > 0 ? avail : 0;
        if (gW > 0 && gH > 0)
            Put(st->hSponsor, cw - GAP - gW, ch - footerH - GAP - gH, gW, gH);
    }
    // The page content stops a gap above the panel; with no panel this is the old value
    // (ch - footerH - GAP) unchanged.
    const int bottom = spH > 0 ? spTop - GAP : ch - footerH - GAP;
    int y = TABH + GAP;

    {
        const int by = g.footer.top + GAP;
        const int right = cw - GAP;
        Put(st->hApply,  right - BW,              by, BW, BH);
        Put(st->hCancel, right - 2 * BW - GT,     by, BW, BH);
        Put(st->hOk,     right - 3 * BW - 2 * GT, by, BW, BH);
    }

    if (st->page == PAGE_PROFILES) {
        // TWO COLUMNS, because the page is now the full window width and it carries strictly
        // more than it used to: the search box, the game picker, the heavy-apps CPU list and
        // the auto-pin rule that used to have a page of its own. Stacked in one column that
        // does not fit the minimum window height; side by side it does, and the horizontal
        // space the rail used to occupy is what pays for it.
        const ProfileColumns columns = MeasureProfileColumns(cw, dpi);
        const int LW = columns.leftW;
        const int RX = x0 + LW + GAP;
        const int RW = columns.rightW;

        const WarningLayout warning = MeasureWarningLayout(st, measureDc, RW, dpi);
        const bool warnGame = warning.game;
        const bool warnHeavy = warning.heavy;
        const int warnRows = (warnGame ? 1 : 0) + (warnHeavy ? 1 : 0);
        const int warnCardH = warning.cardH;

        // ---- left column: the profile list ----------------------------------
        // Header row, search box, list, four buttons. The list takes every pixel the column
        // can spare, which is what the extra width buys us.
        {
            const int fixedL = 2 * PAD + BH + GT + RH + GT + GT + BH;
            int listH = bottom - y - fixedL;
            if (listH < theme::Dp(90, dpi)) listH = theme::Dp(90, dpi);

            RECT c = AddCard(y, fixedL + listH, x0, LW);
            int ix = c.left + PAD, iy = c.top + PAD, iw = LW - 2 * PAD;

            // "Profiles" and the "Add game..." action share the header row.
            int agw = theme::Dp(112, dpi);
            if (agw > iw / 2) agw = iw / 2;
            Put(st->hProfHdr, ix, iy + (BH - HH) / 2, iw - agw - GT, HH);
            Put(st->hAddGame, ix + iw - agw, iy, agw, BH);
            iy += BH + GT;

            Put(st->hSearch, ix, iy, iw, RH);
            iy += RH + GT;
            Put(st->hProfList, ix, iy, iw, listH);
            iy += listH + GT;
            {
                int bw = BW;
                if (bw > (iw - 3 * GT) / 4) bw = (iw - 3 * GT) / 4;
                if (bw < theme::Dp(48, dpi)) bw = theme::Dp(48, dpi);
                int bx = ix;
                Put(st->hAdd, bx, iy, bw, BH); bx += bw + GT;
                Put(st->hDup, bx, iy, bw, BH); bx += bw + GT;
                Put(st->hRem, bx, iy, bw, BH); bx += bw + GT;
                Put(st->hRen, bx, iy, bw, BH);
            }
        }

        // ---- right column: the selected profile ------------------------------
        int ry = y;
        {
            const int descH = theme::Dp(34, dpi);
            // The live status line. Two lines of UiSmall at the narrowest column this page
            // allows, because the longest sentence names an executable and a mask name.
            const int statusH = theme::Dp(34, dpi);
            const int minHeavy = theme::Dp(52, dpi);
            // header, enabled, game, game mask, heavy label, [heavy], heavy mask,
            // auto-pin, description, percent row, status row.
            const int fixedR = 2 * PAD + HH + GT + ROW + GT + ROW + GT + ROW + GT +
                               ROW + GT + GT + ROW + GT + GT + ROW + GT + descH + GT + ROW +
                               GT + statusH;
            int heavyH = minHeavy;
            int slack = bottom - ry - fixedR - minHeavy -
                        (warnRows > 0 ? warnCardH + GAP : 0);
            if (slack > 0) {
                int take = slack;
                heavyH += take;
            }

            RECT c = AddCard(ry, fixedR + heavyH, RX, RW);
            int ix = c.left + PAD, iy = c.top + PAD, iw = RW - 2 * PAD;
            Put(st->hEditHdr, ix, iy, iw, HH);
            iy += HH + GT;
            Put(st->hEnabled, ix, iy, iw, ROW);
            iy += ROW + GT;

            Put(st->hGameLbl, ix, iy + (ROW - LH) / 2, LBL, LH);
            {
                int ex = ix + LBL;
                int ew = iw - LBL - 2 * BW - 2 * GT;
                if (ew < theme::Dp(90, dpi)) ew = theme::Dp(90, dpi);
                Put(st->hGame, ex, iy + (ROW - RH) / 2, ew, RH);
                Put(st->hGamePick, ex + ew + GT, iy, BW, BH);
                Put(st->hGameBrowse, ex + ew + GT + BW + GT, iy, BW, BH);
            }
            iy += ROW + GT;

            Put(st->hGameMaskLbl, ix, iy + (ROW - LH) / 2, LBL, LH);
            Put(st->hGameMask, ix + LBL, iy + (ROW - RH) / 2, CBW, CBDROP);
            // THE COMBO IS THE INTENT; THIS IS WHAT WINDOWS SAYS IS ACTUALLY IN FORCE. Drawn
            // by the parent rather than given a control of its own: it is one short word that
            // changes about as often as the game starts and stops, and a STATIC would have to
            // be created, themed, page-scoped and hidden on every other page to say it.
            //
            // Skipped entirely when the column is too narrow for it. The combo already
            // overruns this card at the minimum window width, and adding a second thing to
            // overrun it would turn a tight layout into an unreadable one.
            if (!st->targetStageText.empty()) {
                const int sx = ix + LBL + CBW + GT;
                const int sw = ix + iw - sx;
                // Dp(110) fits "Now: Cache no SMT", the longest label a derived mask can
                // produce on the reference machine. At the window's own minimum width this
                // column measures ~165dp, so the skip is for a layout we do not ship.
                if (sw >= theme::Dp(110, dpi)) {
                    Say(sx, iy + (ROW - LH) / 2, sx + sw, iy + (ROW - LH) / 2 + LH,
                        L"Now: " + st->targetStageText, theme::Font::UiSmall,
                        pal.textSecondary, kL | DT_END_ELLIPSIS);
                }
            }
            iy += ROW + GT;

            // Heavy apps: caption plus the three affordances that keep the list editable.
            {
                int bw = theme::Dp(96, dpi);
                if (bw > (iw - 2 * GT) / 4) bw = (iw - 2 * GT) / 4;
                if (bw < theme::Dp(52, dpi)) bw = theme::Dp(52, dpi);
                const int bandW = 3 * bw + 2 * GT;
                int capW = iw - bandW - GT;
                if (capW < theme::Dp(60, dpi)) capW = theme::Dp(60, dpi);
                Put(st->hHeavyLbl, ix, iy + (ROW - LH) / 2, capW, LH);
                int bx = ix + iw - bandW;
                Put(st->hHeavyPick, bx, iy, bw, BH); bx += bw + GT;
                Put(st->hHeavyAdd, bx, iy, bw, BH);  bx += bw + GT;
                Put(st->hHeavyRem, bx, iy, bw, BH);
            }
            iy += ROW + GT;
            Put(st->hHeavy, ix, iy, iw, heavyH);
            iy += heavyH + GT;

            Put(st->hHeavyMaskLbl, ix, iy + (ROW - LH) / 2, LBL, LH);
            Put(st->hHeavyMask, ix + LBL, iy + (ROW - RH) / 2, CBW, CBDROP);
            iy += ROW + GT;

            // ---- the auto-pin rule, which is PER PROFILE and so lives here now ----
            iy += GT;
            Put(st->hAutoPin, ix, iy, iw, ROW);
            iy += ROW + GT;
            const int indent = theme::Dp(20, dpi);
            Put(st->hAutoDesc, ix + indent, iy, iw - indent, descH);
            iy += descH + GT;
            {
                int x = ix + indent;
                const int w1 = theme::Dp(120, dpi), w2 = theme::Dp(58, dpi);
                const int w3 = theme::Dp(56, dpi);
                Put(st->hPctLbl, x, iy + (ROW - LH) / 2, w1, LH); x += w1 + GT;
                Put(st->hPct, x, iy + (ROW - RH) / 2, w2, RH);    x += w2 + GT;
                // The unit is parent-drawn rather than a fourth STATIC: it never changes and
                // a control that only ever says "% CPU" is a control to keep in step for
                // nothing. It dims with the rule, exactly as hPctLbl does.
                Say(x, iy + (ROW - LH) / 2, x + w3, iy + (ROW - LH) / 2 + LH, L"% CPU",
                    theme::Font::UiSmall,
                    AutoPinLabelsAreDim(st) ? pal.textDim : pal.textSecondary, kL);
            }
            iy += ROW + GT;

            // ---- the live status line --------------------------------------
            // The two preconditions in ComputeDesired rule 4 are invisible from this window
            // otherwise, and a user who cannot see them reasonably concludes the feature is
            // broken. The dot is drawn by the parent, exactly like the blocked-processes dot
            // on the General page, so its colour comes from the cached state rather than from
            // a second evaluation of the rule.
            {
                const int dotR = theme::Dp(4, dpi);
                const int sx = ix + indent;
                DotItem d = { sx + dotR, iy + theme::Dp(9, dpi), dotR, AutoPinDotColour(st) };
                if (g.nDot < 4) g.dot[g.nDot++] = d;
                const int tx = sx + theme::Dp(18, dpi);
                int tw = ix + iw - tx;
                if (tw < 0) tw = 0;
                Put(st->hAutoStatus, tx, iy, tw, statusH);
            }
            ry = c.bottom + GAP;
        }

        // The parked-mask warnings, in their own card behind a warn-coloured dot, under the
        // selected-profile card. The wording is untouched - it is a measured hazard, not
        // decoration.
        if (warnRows > 0) {
            RECT wc = AddCard(ry, warnCardH, RX, RW);
            const int dotR = theme::Dp(4, dpi);
            const int tx = wc.left + PAD + theme::Dp(18, dpi);
            int wy = wc.top + PAD;
            if (warnGame) {
                DotItem d = { wc.left + PAD + dotR, wy + theme::Dp(9, dpi), dotR, pal.warn };
                if (g.nDot < 4) g.dot[g.nDot++] = d;
                Put(st->hGameMaskWarn, tx, wy, warning.textW, warning.gameH);
                wy += warning.gameH + GT;
            }
            if (warnHeavy) {
                DotItem d = { wc.left + PAD + dotR, wy + theme::Dp(9, dpi), dotR, pal.warn };
                if (g.nDot < 4) g.dot[g.nDot++] = d;
                Put(st->hHeavyMaskWarn, tx, wy, warning.textW, warning.heavyH);
            }
            ry = wc.bottom + GAP;
        }
        y = ry;
    } else if (st->page == PAGE_COREMAP) {
        // --- the stat row: two numbers and the ring gauge -----------------------
        // The third card carries the ring, and the ring is what sets the row's height:
        // caption band, then a SQUARE for the gauge, then the card padding. Deriving the
        // height from the square rather than hardcoding it is what stops the ring being
        // squashed into a letterbox and clipped by the card edge.
        const int gaugeSide = theme::Dp(84, dpi);
        const int statH = 2 * PAD + LH + GT + gaugeSide;
        const int sw = (W - 2 * GAP) / 3;
        for (int i = 0; i < 3; ++i) {
            RECT s;
            SetRect(&s, x0 + i * (sw + GAP), y,
                    (i == 2 ? x0 + W : x0 + i * (sw + GAP) + sw), y + statH);
            g.stat[g.nStat++] = s;
        }
        {
            const RECT& r = g.stat[0];
            Say(r.left + PAD, r.top + PAD, r.right - PAD, r.top + PAD + LH,
                L"Logical processors", theme::Font::UiSmall, pal.textSecondary, kL);
            Say(r.left + PAD, r.top + PAD + LH, r.right - PAD, r.bottom - PAD - LH,
                std::to_wstring(st->topo->totalLogicalProcessors),
                theme::Font::MonoDisplay, pal.textPrimary, kBig);
            Say(r.left + PAD, r.bottom - PAD - LH, r.right - PAD, r.bottom - PAD,
                L"on this machine", theme::Font::UiSmall, pal.textDim, kL);
        }
        {
            const RECT& r = g.stat[1];
            Say(r.left + PAD, r.top + PAD, r.right - PAD, r.top + PAD + LH,
                L"Cache domains", theme::Font::UiSmall, pal.textSecondary, kL);
            Say(r.left + PAD, r.top + PAD + LH, r.right - PAD, r.bottom - PAD - LH,
                std::to_wstring(static_cast<unsigned long long>(st->topo->domains.size())),
                theme::Font::MonoDisplay, pal.good, kBig);
            Say(r.left + PAD, r.bottom - PAD - LH, r.right - PAD, r.bottom - PAD,
                KindName(st->topo->kind), theme::Font::MonoSmall, pal.textDim, kL);
        }
        {
            const RECT& r = g.stat[2];
            const std::wstring mname = ComboText(st->hMapMask);
            const Mask* m = st->work.FindMask(mname);
            int inMask = 0;
            if (m) {
                for (size_t i = 0; i < m->ids.size(); ++i)
                    if (FindById(*st->topo, m->ids[i]) != nullptr) ++inMask;
            }
            const int total = st->topo->totalLogicalProcessors;
            const double pct = total > 0 ? static_cast<double>(inMask) /
                                           static_cast<double>(total) : 0.0;
            // Band 1: the caption and the mask-name pill share one row at the top of the
            // card, and NOTHING else is allowed into it - the ring used to be handed the
            // whole padded interior, so its arc rode up over the pill and off the card.
            const int capTop = r.top + PAD;
            const int capBot = capTop + LH;
            int capR = r.right - PAD;
            if (!mname.empty() && g.nPill < 4) {
                int pw = theme::Dp(10, dpi) * static_cast<int>(mname.size()) +
                         theme::Dp(14, dpi);
                const int maxPw = (r.right - r.left) / 2;
                if (pw > maxPw) pw = maxPw;
                PillItem p;
                SetRect(&p.rc, r.right - PAD - pw, capTop, r.right - PAD, capBot);
                p.s = mname; p.bg = pal.cardBgAlt; p.fg = pal.textSecondary;
                g.pill[g.nPill++] = p;
                capR = p.rc.left - GT;
            }
            Say(r.left + PAD, capTop, capR, capBot,
                L"In this mask", theme::Font::UiSmall, pal.textSecondary, kL);

            // Band 2: everything BELOW the caption band, inset by the card padding, is the
            // gauge's. Take the largest square that fits there and centre it. DrawRingGauge
            // sizes its radius from min(w,h) and keeps the stroke inside the rect, so a
            // square that fits the padded interior cannot reach the card border.
            const int gTop  = capBot + GT;
            const int availH = (r.bottom - PAD) - gTop;
            const int availW = (r.right - PAD) - (r.left + PAD);
            int side = availH < availW ? availH : availW;
            if (side < 0) side = 0;
            const int gLeft = (r.left + r.right) / 2 - side / 2;
            const int gTop2 = gTop + (availH - side) / 2;
            g.hasGauge = true;
            SetRect(&g.gauge, gLeft, gTop2, gLeft + side, gTop2 + side);
            g.pct = pct;
            g.gaugeCentre = std::to_wstring(static_cast<int>(pct * 100.0 + 0.5));
            g.gaugeUnit = L"%";
        }
        y = g.stat[0].bottom + GAP;

        // --- the map card -------------------------------------------------------
        const int topoH = theme::Dp(56, dpi);
        const int fixed = 2 * PAD + HH + GT + topoH + GT + LH + GT + ROW + GT;
        const int minMap = theme::Dp(110, dpi);
        // No upper cap. The card runs to the bottom of the page - `bottom` is already the
        // top of the footer minus the gap - so the map gets every pixel the page can spare
        // instead of being squeezed to a fixed height above a band of dead space. The floor
        // stays: on a window too short for even that, the map scrolls rather than vanishes.
        int mapH = bottom - y - fixed;
        if (mapH < minMap) mapH = minMap;

        RECT c = AddCard(y, fixed + mapH, x0, W);
        int ix = c.left + PAD, iy = c.top + PAD, iw = W - 2 * PAD;
        Put(st->hMapHdr, ix, iy, iw, HH);
        iy += HH + GT;
        Put(st->hTopoText, ix, iy, iw, topoH);
        iy += topoH + GT;
        // The topology signature is the string a stale-id report is judged against, so it
        // is shown verbatim and in mono rather than paraphrased.
        Say(ix, iy, ix + iw, iy + LH, L"Signature  " + st->topo->signature,
            theme::Font::MonoSmall, pal.textDim, kL);
        iy += LH + GT;
        Put(st->hMapMaskLbl, ix, iy + (ROW - LH) / 2, LBL, LH);
        Put(st->hMapMask, ix + LBL, iy + (ROW - RH) / 2, CBW, CBDROP);
        Put(st->hMapReset, ix + LBL + CBW + GT, iy, theme::Dp(150, dpi), BH);
        iy += ROW + GT;
        Put(st->hMap, ix, iy, iw, mapH);
        Put(st->hMapFail, ix, iy, iw, mapH);   // only ever one of the two exists
        y = c.bottom + GAP;
    } else {
        const int cardH = 2 * PAD + HH + GT + ROW + GT + ROW;
        RECT c = AddCard(y, cardH, x0, W);
        int ix = c.left + PAD, iy = c.top + PAD, iw = W - 2 * PAD;
        Put(st->hGenHdr, ix, iy, iw, HH);
        iy += HH + GT;
        Put(st->hStartup, ix, iy, theme::Dp(200, dpi), ROW);
        Put(st->hNotify, ix + theme::Dp(212, dpi), iy, theme::Dp(240, dpi), ROW);
        iy += ROW + GT;
        Put(st->hPollLbl, ix, iy + (ROW - LH) / 2, theme::Dp(150, dpi), LH);
        Put(st->hPoll, ix + theme::Dp(150, dpi), iy + (ROW - RH) / 2,
            theme::Dp(80, dpi), RH);
        y = c.bottom + GAP;

        // Live scheduling influences. The two state rows use the same card/row rhythm as
        // the General controls above. The explanation exists only while the AMD service is
        // RUNNING, and its pure wording is shared with the parked-mask warning.
        const bool showEffect = st->hVCacheEffect &&
                                GetWindowTextLengthW(st->hVCacheEffect) > 0;
        const int effectH = theme::Dp(34, dpi);
        int envH = 2 * PAD + HH + GT + ROW + GT + ROW;
        if (showEffect) envH += GT + effectH;
        RECT envCard = AddCard(y, envH, x0, W);
        ix = envCard.left + PAD; iy = envCard.top + PAD; iw = W - 2 * PAD;
        Say(ix, iy, ix + iw, iy + HH, L"Environment",
            theme::Font::UiHeading, pal.textPrimary, kL);
        iy += HH + GT;
        Put(st->hGameModeStatus, ix, iy + (ROW - LH) / 2, iw, LH);
        iy += ROW + GT;
        Put(st->hVCacheStatus, ix, iy + (ROW - LH) / 2, iw, LH);
        if (showEffect) {
            iy += ROW + GT;
            Put(st->hVCacheEffect, ix, iy, iw, effectH);
        }
        y = envCard.bottom + GAP;

        const int blockedH = theme::Dp(st->blockedTall ? 84 : 52, dpi);
        const int headRow = BH > HH ? BH : HH;
        RECT c2 = AddCard(y, 2 * PAD + headRow + GT + blockedH, x0, W);
        ix = c2.left + PAD; iy = c2.top + PAD; iw = W - 2 * PAD;
        const int insW = theme::Dp(160, dpi);
        Say(ix, iy, ix + iw - insW - GT, iy + headRow, L"Processes",
            theme::Font::UiHeading, pal.textPrimary, kL);
        Put(st->hInspect, ix + iw - insW, iy, insW, BH);
        iy += headRow + GT;
        {
            const int dotR = theme::Dp(4, dpi);
            const COLORREF dc = st->blockedBad ? pal.danger
                                               : (st->blockedTall ? pal.warn : pal.good);
            DotItem d = { ix + dotR, iy + theme::Dp(9, dpi), dotR, dc };
            if (g.nDot < 4) g.dot[g.nDot++] = d;
        }
        Put(st->hBlocked, ix + theme::Dp(18, dpi), iy, iw - theme::Dp(18, dpi), blockedH);
        y = c2.bottom + GAP;
    }
    (void)y;
}

void SettingsLayout(SettingsState* st, HWND hwnd) {
    PosBatch put;
    put.n = 0;
    Geom g;
    HDC dc = GetDC(hwnd);
    LayoutPage(st, hwnd, g, &put, dc);
    if (dc) ReleaseDC(hwnd, dc);
    put.Flush();
}

// The parent's own paint: the cards, the drawn headings, the stat numbers, the status dots
// and the ring gauge. WS_CLIPCHILDREN keeps every control out of this DC, so a card can be
// drawn straight under the controls that sit on it.
void PaintSettings(SettingsState* st, HWND hwnd, HDC dc) {
    Geom g;
    LayoutPage(st, hwnd, g, nullptr, dc);
    const int dpi = st->dpi;
    const theme::Palette& pal = theme::P();

    for (int i = 0; i < g.nCard; ++i) theme::DrawCard(dc, g.card[i], dpi);
    for (int i = 0; i < g.nStat; ++i) theme::DrawCard(dc, g.stat[i], dpi);
    for (int i = 0; i < g.nTxt; ++i)
        theme::DrawText(dc, g.txt[i].rc, g.txt[i].s, g.txt[i].f, dpi,
                        g.txt[i].col, g.txt[i].fmt);
    for (int i = 0; i < g.nDot; ++i)
        theme::DrawStatusDot(dc, g.dot[i].x, g.dot[i].y, g.dot[i].r, g.dot[i].col);
    for (int i = 0; i < g.nPill; ++i)
        theme::DrawPill(dc, g.pill[i].rc, g.pill[i].s, dpi, g.pill[i].bg, g.pill[i].fg);
    if (g.hasGauge)
        theme::DrawRingGauge(dc, g.gauge, g.pct, dpi, pal.accent, pal.cardBgAlt,
                             g.gaugeCentre, g.gaugeUnit);
}

// Re-frames every combo that is on the current page. Called from the parent's WM_PAINT,
// AFTER EndPaint - the BeginPaint DC cannot reach a child (see OverdrawOneCombo) - and after
// forcing each combo to finish its own paint, because a parent WM_PAINT normally runs before
// its children's and anything drawn on top first would simply be painted over.
void OverdrawPageCombos(SettingsState* st, HWND hwnd) {
    if (!st || !hwnd) return;
    HWND combos[] = { st->hGameMask, st->hHeavyMask, st->hMapMask };
    for (HWND c : combos) {
        if (!c || !IsWindowVisible(c)) continue;
        UpdateWindow(c);
        OverdrawOneCombo(hwnd, c);
    }
}

// Paints the search box's frame, magnifier and placeholder. Same route and same reason as
// OverdrawOneCombo: this window is WS_CLIPCHILDREN, so a BeginPaint DC has the edit's rect
// clipped out of it and anything stroked there is a silent no-op. GetDCEx without
// DCX_CLIPCHILDREN is the DC that reaches the child, and the edit is made to finish its own
// paint first so the chrome is not immediately painted over.
void OverdrawSearchChrome(SettingsState* st, HWND hwnd) {
    if (!st || !hwnd || !st->hSearch || !IsWindowVisible(st->hSearch)) return;
    UpdateWindow(st->hSearch);
    RECT rc;
    GetWindowRect(st->hSearch, &rc);
    MapWindowPoints(nullptr, hwnd, reinterpret_cast<POINT*>(&rc), 2);
    HDC dc = GetDCEx(hwnd, nullptr, DCX_CACHE);
    if (!dc) return;
    theme::DrawSearchChrome(dc, rc, st->dpi, GetFocus() == st->hSearch,
                            GetWindowTextLengthW(st->hSearch) == 0, L"Search games...");
    ReleaseDC(hwnd, dc);
}

// ---------------------------------------------------------------------------
// The two owner-drawn list boxes
//
// Both are drawn here rather than by theme::DrawListBoxItem because both carry more than a
// string: the profile list carries the All Games tag and the recently-used divider, and the
// heavy list carries a live CPU meter. The generic helper still handles every other list.
// ---------------------------------------------------------------------------

// Shared row background: the selection surface plus the accent bar. Returns the rect left
// for content. Every brush created here is deleted here.
RECT DrawRowBackground(const DRAWITEMSTRUCT* di, int dpi, bool selected) {
    const theme::Palette& pal = theme::P();
    RECT rc = di->rcItem;
    HBRUSH bg = CreateSolidBrush(selected ? pal.cardBgAlt : pal.inputBg);
    if (bg) {
        FillRect(di->hDC, &rc, bg);
        DeleteObject(bg);
    }
    const int barW = theme::Dp(3, dpi);
    if (selected) {
        RECT bar = rc;
        bar.right = bar.left + barW;
        HBRUSH ab = CreateSolidBrush(pal.accent);
        if (ab) {
            FillRect(di->hDC, &bar, ab);
            DeleteObject(ab);
        }
    }
    RECT t = rc;
    t.left += barW + theme::Dp(8, dpi);
    t.right -= theme::Dp(8, dpi);
    if (t.right < t.left) t.right = t.left;
    return t;
}

BOOL DrawProfileItem(SettingsState* st, const DRAWITEMSTRUCT* di) {
    if (!di || !di->hDC) return FALSE;
    if (di->rcItem.right <= di->rcItem.left || di->rcItem.bottom <= di->rcItem.top)
        return FALSE;
    const int dpi = st->dpi;
    const theme::Palette& pal = theme::P();
    const bool selected = (di->itemState & ODS_SELECTED) != 0;
    RECT t = DrawRowBackground(di, dpi, selected);
    if (di->itemID == static_cast<UINT>(-1)) return TRUE;   // empty list: background only

    const LRESULT data = SendMessageW(di->hwndItem, LB_GETITEMDATA,
                                      static_cast<WPARAM>(di->itemID), 0);
    const Profile* p = nullptr;
    if (data >= 0 && static_cast<size_t>(data) < st->work.profiles.size())
        p = &st->work.profiles[static_cast<size_t>(data)];

    // The All Games profile matches ANY game rather than one executable, so it must not read
    // as just another row in the list. A small accent tag on the right says so at a glance.
    if (p && p->isAllGames) {
        const int pw = theme::Dp(36, dpi);
        const int ph = theme::Dp(15, dpi);
        const int mid = (di->rcItem.top + di->rcItem.bottom) / 2;
        RECT pill;
        SetRect(&pill, t.right - pw, mid - ph / 2, t.right, mid - ph / 2 + ph);
        if (pill.left > t.left) {
            theme::DrawPill(di->hDC, pill, L"ALL", dpi, pal.accent, pal.textOnAccent);
            t.right = pill.left - theme::Dp(6, dpi);
        }
    }

    if (p && t.right > t.left) {
        const std::wstring label = (p->enabled ? L"[x] " : L"[ ] ") + p->name;
        theme::DrawText(di->hDC, t, label, theme::Font::MonoSmall, dpi,
                        p->enabled ? pal.textPrimary : pal.textDim,
                        DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX |
                            DT_END_ELLIPSIS);
    }

    // The recently-used divider. sepRow is a count, so the line belongs at the bottom of the
    // row before it. 1px, never scaled - a hairline rule is a hairline at any DPI.
    if (st->sepRow > 0 && static_cast<int>(di->itemID) == st->sepRow - 1) {
        RECT ln = di->rcItem;
        ln.top = ln.bottom - 1;
        HBRUSH sb = CreateSolidBrush(pal.border);
        if (sb) {
            FillRect(di->hDC, &ln, sb);
            DeleteObject(sb);
        }
    }
    return TRUE;
}

BOOL DrawHeavyItem(SettingsState* st, const DRAWITEMSTRUCT* di) {
    if (!di || !di->hDC) return FALSE;
    if (di->rcItem.right <= di->rcItem.left || di->rcItem.bottom <= di->rcItem.top)
        return FALSE;
    const int dpi = st->dpi;
    const theme::Palette& pal = theme::P();
    const bool selected = (di->itemState & ODS_SELECTED) != 0;
    RECT t = DrawRowBackground(di, dpi, selected);
    if (di->itemID == static_cast<UINT>(-1)) return TRUE;

    std::wstring name;
    const LRESULT len = SendMessageW(di->hwndItem, LB_GETTEXTLEN,
                                     static_cast<WPARAM>(di->itemID), 0);
    if (len > 0) {
        std::vector<wchar_t> buf(static_cast<size_t>(len) + 1, L'\0');
        const LRESULT got = SendMessageW(di->hwndItem, LB_GETTEXT,
                                         static_cast<WPARAM>(di->itemID),
                                         reinterpret_cast<LPARAM>(buf.data()));
        if (got > 0) name.assign(buf.data(), static_cast<size_t>(got));
    }

    const LRESULT origin = SendMessageW(di->hwndItem, LB_GETITEMDATA,
                                        static_cast<WPARAM>(di->itemID), 0);

    // The "+N more" row is a SENTENCE, not a process. It gets no meter, no readback and no
    // percentage: every one of those would be a claim about a process this row does not name,
    // and an empty meter beside a caption reads as "that process is idle".
    if (origin == kHeavyRowMore) {
        if (t.right > t.left && !name.empty()) {
            theme::DrawText(di->hDC, t, name, theme::Font::UiSmall, dpi, pal.textDim,
                            DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX |
                                DT_END_ELLIPSIS);
        }
        return TRUE;
    }

    bool running = false;
    const double pct = CpuForExe(st, name, running);

    // Right to left inside the row: Dp(12) of padding from the ROW's own right edge (not
    // from t, which has already taken Dp(8) off), then Dp(40) reserved for the percentage,
    // then the track. The rect handed to DrawCpuMeter IS the track - Dp(96) x Dp(6),
    // vertically centred - so nothing about the meter's size is decided in two places.
    //
    // A BAR WITHOUT A NUMBER IS NOT READABLE for a threshold the user is asked to type into
    // the box below this list. "Roughly two thirds along" is not a value you can compare
    // against 8%, so the number is part of the meter, not decoration beside it.
    const int rowPad = theme::Dp(12, dpi);
    const int pctW   = theme::Dp(40, dpi);
    const int meterW = theme::Dp(96, dpi);
    const int meterH = theme::Dp(6, dpi);
    const int mid = (di->rcItem.top + di->rcItem.bottom) / 2;

    RECT num;
    SetRect(&num, di->rcItem.right - rowPad - pctW, di->rcItem.top,
            di->rcItem.right - rowPad, di->rcItem.bottom);
    RECT meter;
    SetRect(&meter, num.left - meterW, mid - meterH / 2,
            num.left, mid - meterH / 2 + meterH);

    if (meter.left > t.left + theme::Dp(48, dpi)) {
        // An entry naming an executable that is not running draws an EMPTY meter, not a
        // missing one: the row still has to show where the threshold sits. It prints NO
        // number though - ABSENT AND IDLE ARE DIFFERENT STATES. "0%" would assert that the
        // process is running and quiet, which is a different and wrong claim.
        //
        // AND THE RAMP IS EARNED, NOT AUTOMATIC. The good/warn/danger colours say "this
        // process is approaching, or has passed, the point where the rule moves it". When the
        // rule CANNOT fire - it is switched off, or its game is not running, or another
        // profile is the one the engine matched - that claim is false, so the meter drops to
        // a flat textDim. The operator read a red bar as "about to happen" while the rule had
        // no way of firing at all; a red bar that means nothing is a lie told in colour.
        if (AutoPinCanFire(st))
            theme::DrawCpuMeter(di->hDC, meter, running ? pct : 0.0, dpi,
                                AutoPinThreshold(st));
        else
            DrawCpuMeterDim(di->hDC, meter, running ? pct : 0.0, dpi, AutoPinThreshold(st));
        if (running) {
            wchar_t pctText[16];
            wsprintfW(pctText, L" %d%%", static_cast<int>(pct + 0.5));
            theme::DrawText(di->hDC, num, pctText, theme::Font::MonoSmall, dpi,
                            AutoPinCanFire(st) ? pal.textSecondary : pal.textDim,
                            DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        }
        t.right = meter.left - theme::Dp(8, dpi);
        if (t.right < t.left) t.right = t.left;

        // The mask this entry's processes are on RIGHT NOW, between the name and the meter.
        // Read back from Windows on the same tick - never the heavy mask this profile is
        // configured with, which is intent and would say the same thing whether or not it
        // took. See RefreshCpuSetStages.
        //
        // textSecondary, never a ramp: this is a fact about where the process is, and giving
        // it good/warn colouring would imply a judgement the app cannot make. Dimmed only
        // when there is nothing to say - "-" for an entry that is not running.
        //
        // The column is RESERVED whether or not there is a word for it yet - for the one
        // tick after a profile switch there is not. Widening the name into it and taking it
        // back a moment later would make every row twitch once a second.
        const int stageW = theme::Dp(76, dpi);
        RECT stage;
        SetRect(&stage, t.right - stageW, di->rcItem.top, t.right, di->rcItem.bottom);
        if (stage.left > t.left + theme::Dp(56, dpi)) {
            const std::map<std::wstring, CpuSetStageInfo>::const_iterator si =
                st->stageByHeavy.find(ToLower(Trim(name)));
            if (si != st->stageByHeavy.end()) {
                const std::wstring word = CpuSetStageLabel(si->second);
                theme::DrawText(di->hDC, stage, word, theme::Font::MonoSmall, dpi,
                                si->second.stage == CpuSetStage::NotRunning ? pal.textDim
                                                                            : pal.textSecondary,
                                DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX |
                                    DT_END_ELLIPSIS);
            }
            t.right = stage.left - theme::Dp(8, dpi);
            if (t.right < t.left) t.right = t.left;
        }
    }

    // WHICH KIND OF ROW IS THIS. The two are the same shape and carry the same three columns,
    // so without a mark the user cannot tell an entry they chose from one the app chose - and
    // the whole point of showing the second kind is that they are different things. An accent
    // pill is this window's existing idiom for exactly that: the profile list marks its All
    // Games row the same way, in the same place, in the same colours.
    //
    // Anchored to the RIGHT edge of the name column, which is a fixed x for every row, so the
    // pills line up in a column and the names stay left-aligned. A leading badge would have
    // indented the auto rows' names away from the manual ones and made the list read ragged.
    // No gutter is reserved on manual rows: at the narrowest column this page allows, Dp(44)
    // taken from every name is the difference between reading an executable and reading an
    // ellipsis, and the rows that pay for the tag should be the rows that carry it.
    if (origin == kHeavyRowAuto) {
        std::wstring pillText = L"AUTO";
        COLORREF pillBg = pal.accent;
        bool failed = false;
        const std::map<std::wstring, AutoApplySummary>::const_iterator ai =
            st->autoApplyByExe.find(ToLower(BaseName(Trim(name))));
        if (ai != st->autoApplyByExe.end()) {
            const AutoApplySummary& summary = ai->second;
            if (summary.accessDenied > 0) {
                pillText = summary.applied == 0 && summary.failed == 0
                               ? L"ACCESS DENIED"
                               : L"SOME DENIED";
                pillBg = pal.danger;
                failed = true;
            } else if (summary.failed > 0) {
                pillText = summary.applied == 0 ? L"AUTO FAILED" : L"SOME FAILED";
                pillBg = pal.danger;
                failed = true;
            }
        }
        SIZE pillTextSize = theme::MeasureText(
            di->hDC, pillText, theme::Font::UiSmall, dpi);
        // DrawPill consumes Dp(8) per side; Dp(20) covers that Dp(16) plus slack.
        const int pillHorizontalPadding = theme::Dp(20, dpi);
        int pw = pillTextSize.cx + pillHorizontalPadding;
        // Failure is the important claim. If the full phrase would disappear at the
        // existing name-width floor, keep the reason visible in a shorter form instead.
        if (failed && t.right - pw <= t.left + theme::Dp(56, dpi)) {
            pillText = ai->second.accessDenied > 0 ? L"DENIED" : L"FAILED";
            pillTextSize = theme::MeasureText(
                di->hDC, pillText, theme::Font::UiSmall, dpi);
            pw = pillTextSize.cx + pillHorizontalPadding;
        }
        const int ph = theme::Dp(15, dpi);
        const int mid2 = (di->rcItem.top + di->rcItem.bottom) / 2;
        RECT pill;
        SetRect(&pill, t.right - pw, mid2 - ph / 2, t.right, mid2 - ph / 2 + ph);
        // Successful AUTO keeps the original name floor. A failure gets priority and needs
        // only enough remaining room to identify a short executable such as HYP.exe.
        const int nameFloor = theme::Dp(failed ? 32 : 56, dpi);
        if (pill.left > t.left + nameFloor) {
            theme::DrawPill(di->hDC, pill, pillText, dpi, pillBg, pal.textOnAccent);
            t.right = pill.left - theme::Dp(6, dpi);
            if (t.right < t.left) t.right = t.left;
        }
    }

    if (t.right > t.left && !name.empty()) {
        theme::DrawText(di->hDC, t, name, theme::Font::MonoBody, dpi,
                        running ? pal.textPrimary : pal.textDim,
                        DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX |
                            DT_END_ELLIPSIS);
    }
    return TRUE;
}

// The controls that belong to one page. nullptr entries are skipped by the caller, so a
// control that failed to create (the core map) simply is not in the set.
void PageControls(SettingsState* st, int page, HWND* out, int& n) {
    n = 0;
    // The auto-pin rule is on THIS page now: it is per profile, and a page of its own could
    // never say which profile it belonged to.
    HWND profiles[] = { st->hProfHdr, st->hSearch, st->hAddGame, st->hProfList, st->hAdd,
                        st->hDup, st->hRem, st->hRen, st->hEditHdr, st->hEnabled,
                        st->hGameLbl, st->hGame, st->hGamePick, st->hGameBrowse,
                        st->hGameMaskLbl, st->hGameMask, st->hGameMaskWarn, st->hHeavyLbl,
                        st->hHeavy, st->hHeavyPick, st->hHeavyAdd, st->hHeavyRem,
                        st->hHeavyMaskLbl, st->hHeavyMask, st->hHeavyMaskWarn,
                        st->hAutoPin, st->hAutoDesc, st->hPctLbl, st->hPct,
                        st->hAutoStatus };
    HWND coremap[]  = { st->hMapHdr, st->hTopoText, st->hMapMaskLbl, st->hMapMask,
                        st->hMapReset, st->hMap, st->hMapFail };
    HWND general[]  = { st->hGenHdr, st->hStartup, st->hNotify, st->hPollLbl, st->hPoll,
                        st->hGameModeStatus, st->hVCacheStatus, st->hVCacheEffect,
                        st->hBlocked, st->hInspect };

    const HWND* src = nullptr;
    int count = 0;
    switch (page) {
        case PAGE_PROFILES: src = profiles; count = ARRAYSIZE(profiles); break;
        case PAGE_COREMAP:  src = coremap;  count = ARRAYSIZE(coremap);  break;
        default:            src = general;  count = ARRAYSIZE(general);  break;
    }
    for (int i = 0; i < count; ++i)
        if (src[i]) out[n++] = src[i];
}

void ApplyPageVisibility(SettingsState* st) {
    HWND buf[48];
    for (int p = 0; p < PAGE_COUNT; ++p) {
        int n = 0;
        PageControls(st, p, buf, n);
        const bool on = (p == st->page);
        for (int i = 0; i < n; ++i) {
            HWND h = buf[i];
            bool show = on;
            // The two parked warnings and the auto-pin status line take part in the page's
            // visibility, but only when they actually have something to say.
            if (h == st->hGameMaskWarn || h == st->hHeavyMaskWarn ||
                h == st->hAutoStatus || h == st->hVCacheEffect)
                show = on && GetWindowTextLengthW(h) > 0;
            ShowWindow(h, show ? SW_SHOW : SW_HIDE);
        }
    }
}

// Fonts are the loudest half of this style: proportional sans for prose and headings,
// MONOSPACE for everything numeric or technical - exe names, mask names, poll interval,
// percentages, seconds. theme::GetFont hands back a CACHED handle per (font, dpi), so
// nothing here is owned and nothing here may be deleted.
void ApplySettingsFonts(SettingsState* st, HWND hwnd) {
    const int dpi = st->dpi;
    HFONT body = theme::GetFont(theme::Font::UiBody, dpi);
    HFONT head = theme::GetFont(theme::Font::UiHeading, dpi);
    HFONT small = theme::GetFont(theme::Font::UiSmall, dpi);
    HFONT mono = theme::GetFont(theme::Font::MonoBody, dpi);
    HFONT monoSmall = theme::GetFont(theme::Font::MonoSmall, dpi);

    FontApply fa;
    fa.f = body;
    EnumChildWindows(hwnd, ApplyFontProc, reinterpret_cast<LPARAM>(&fa));

    HWND heads[] = { st->hProfHdr, st->hEditHdr, st->hMapHdr, st->hGenHdr };
    for (HWND h : heads)
        if (h) SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(head), TRUE);

    HWND smalls[] = { st->hGameLbl, st->hGameMaskLbl, st->hHeavyLbl, st->hHeavyMaskLbl,
                      st->hGameMaskWarn, st->hHeavyMaskWarn, st->hAutoDesc, st->hPctLbl,
                      st->hAutoStatus, st->hTopoText, st->hMapMaskLbl,
                      st->hPollLbl, st->hVCacheEffect, st->hBlocked, st->hMapFail };
    for (HWND h : smalls)
        if (h) SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(small), TRUE);

    HWND monos[] = { st->hGame, st->hSearch, st->hPoll, st->hPct,
                     st->hGameMask, st->hHeavyMask, st->hMapMask };
    for (HWND h : monos)
        if (h) SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(mono), TRUE);

    // Both list boxes draw their own rows with theme fonts; the control font only decides
    // how LB_GETTEXT-sized geometry is measured, so the mono metrics keep the two in step.
    HWND lists[] = { st->hProfList, st->hHeavy };
    for (HWND h : lists)
        if (h) SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(monoSmall), TRUE);
}

void SwitchPage(SettingsState* st, HWND hwnd, int page) {
    if (page < 0 || page >= PAGE_COUNT || page == st->page) return;
    st->page = page;
    // Set BEFORE telling the bar, so the notification it sends back finds the page already
    // current and returns above rather than recursing.
    if (st->hNav) theme::TabBarSetSelected(st->hNav, IDC_NAV_PROFILES + page);
    ApplyPageVisibility(st);
    SettingsLayout(st, hwnd);
    RedrawSettings(hwnd);
    // Hiding the focused control leaves the keyboard nowhere to go; hand focus to the bar.
    HWND f = GetFocus();
    if (!f || !IsWindowVisible(f)) SetFocus(st->hNav ? st->hNav : hwnd);
}

void ApplyChanges(SettingsState* st, HWND hwnd) {
    StoreUiToProfile(st);
    StoreGeneralToWork(st);

    // Settings never edits these fields. Preserve their live values so a tray Pause or prompt
    // decline saved while this modeless window is open cannot be erased by its stale snapshot.
    st->work.paused = st->out->paused;
    st->work.unknown = st->out->unknown;

    // Compare caller-folded names against both the open baseline and the edited work copy. The
    // baseline is what keeps a profile deliberately deleted here from being resurrected.
    std::vector<std::wstring> liveProfileKeys;
    std::vector<std::wstring> baselineProfileKeys;
    std::vector<std::wstring> workProfileKeys;
    liveProfileKeys.reserve(st->out->profiles.size());
    baselineProfileKeys.reserve(st->baseline.profiles.size());
    workProfileKeys.reserve(st->work.profiles.size());
    for (size_t i = 0; i < st->out->profiles.size(); ++i)
        liveProfileKeys.push_back(ToLower(st->out->profiles[i].name));
    for (size_t i = 0; i < st->baseline.profiles.size(); ++i)
        baselineProfileKeys.push_back(ToLower(st->baseline.profiles[i].name));
    for (size_t i = 0; i < st->work.profiles.size(); ++i)
        workProfileKeys.push_back(ToLower(st->work.profiles[i].name));

    const std::vector<std::size_t> liveAdditions =
        IndicesAddedBehindTheWindow(liveProfileKeys, baselineProfileKeys, workProfileKeys);
    std::wstring recoveredProfiles;
    for (size_t i = 0; i < liveAdditions.size(); ++i) {
        const Profile& recovered = st->out->profiles[liveAdditions[i]];
        st->work.profiles.push_back(recovered);
        if (!recoveredProfiles.empty()) recoveredProfiles += L", ";
        recoveredProfiles += L"'" + recovered.name + L"'";
    }

    // Name every recovered profile in one record so another behind-window reconciliation is
    // visible in the app log instead of surfacing later as unexplained config churn.
    if (!recoveredProfiles.empty()) {
        LogLine(L"[settings] recovered live profile(s) added while Settings was open: %s",
                recoveredProfiles.c_str());
    }

    *st->out = st->work;
    std::vector<std::wstring> repairs = ValidateAndRepair(*st->out, *st->topo);
    st->work = *st->out;

    if (!repairs.empty()) {
        std::wstring m = L"Some settings were repaired before saving:\r\n\r\n";
        for (size_t i = 0; i < repairs.size(); ++i) m += L"  \x2022 " + repairs[i] + L"\r\n";
        MessageBoxW(hwnd, m.c_str(), L"Game Optimizer", MB_OK | MB_ICONINFORMATION);
    }

    if (st->engine) st->engine->SetConfig(*st->out);

    std::wstring err;
    if (!SaveConfig(GetConfigPath(), *st->out, &err)) {
        std::wstring m = L"The configuration could not be saved.\r\n\r\n" + err;
        MessageBoxW(hwnd, m.c_str(), L"Game Optimizer", MB_OK | MB_ICONWARNING);
    }

    // This also exposes recovered profiles. It retains selProfile unless that index is invalid,
    // so the refresh cannot redirect a subsequent edit to a different profile.
    RefreshProfileList(st);
    LoadProfileToUi(st);
    FillMaskCombo(st->hMapMask, st->work, ComboText(st->hMapMask));
    if (SendMessageW(st->hMapMask, CB_GETCURSEL, 0, 0) == CB_ERR && !st->work.masks.empty())
        SendMessageW(st->hMapMask, CB_SETCURSEL, 0, 0);
    SelectMapMask(st);
    SetWindowTextW(st->hPoll, std::to_wstring(st->work.pollMs).c_str());
    SetChecked(st->hNotify, st->work.notifications);
    SetChecked(st->hStartup, st->work.startWithWindows);
    RepaintChrome(hwnd);   // masks may have been repaired, so the stat row may have moved
}

// Clears the search box so a profile that was just created is actually visible in the list
// instead of being filtered out by whatever the user last typed.
void ClearProfileFilter(SettingsState* st) {
    if (st->hSearch && GetWindowTextLengthW(st->hSearch) > 0)
        SetWindowTextW(st->hSearch, L"");
}

// "Add game..." - the picker is cd::PickGame, implemented elsewhere; this only consumes it.
// A new profile from here is ENABLED with auto-pin ON, which is the operator's default: a
// user who just picked a game wants the machine managed, and the useful half switched off is
// a worse default than one unwanted pin, which a single Cancel undoes.
void OnAddGame(SettingsState* st, HWND hwnd) {
    std::wstring display;
    const std::wstring exe = PickGame(hwnd, &display);
    if (exe.empty()) return;

    StoreUiToProfile(st);
    Profile p;
    p.name = Trim(display);
    if (p.name.empty()) p.name = exe;
    p.game = exe;
    p.enabled = true;
    p.autoPin = true;
    p.gameMask = st->topo->defaultGameMask;
    p.heavyMask = st->topo->defaultHeavyMask;
    st->work.profiles.push_back(p);
    st->selProfile = static_cast<int>(st->work.profiles.size()) - 1;
    ClearProfileFilter(st);
    RefreshProfileList(st);
    LoadProfileToUi(st);
    SettingsLayout(st, hwnd);
    RedrawSettings(hwnd);
}

void OnProfileButton(SettingsState* st, HWND hwnd, int id) {
    switch (id) {
        case IDC_ADD: {
            std::wstring name = L"New profile";
            if (!PromptName(hwnd, L"Name for the new profile:", name)) return;
            if (name.empty()) return;
            StoreUiToProfile(st);
            Profile p;
            p.name = name;
            // enabled and autoPin are LEFT AT THEIR MODEL DEFAULTS - both on. This used to
            // force enabled=false here, which quietly reversed the default config.h states;
            // see Profile::autoPin for why the operator chose on.
            p.gameMask = st->topo->defaultGameMask;
            p.heavyMask = st->topo->defaultHeavyMask;
            st->work.profiles.push_back(p);
            st->selProfile = static_cast<int>(st->work.profiles.size()) - 1;
            ClearProfileFilter(st);
            RefreshProfileList(st);
            LoadProfileToUi(st);
            break;
        }
        case IDC_DUP: {
            if (st->selProfile < 0) return;
            StoreUiToProfile(st);
            Profile p = st->work.profiles[static_cast<size_t>(st->selProfile)];
            p.name += L" copy";
            // A copy stays DISABLED, unlike a fresh profile: two enabled profiles naming the
            // same game is a conflict the user did not ask for, and only the first would ever
            // match. This is not the "default to enabled" rule being reversed - it is a copy,
            // not a new profile.
            p.enabled = false;
            st->work.profiles.push_back(p);
            st->selProfile = static_cast<int>(st->work.profiles.size()) - 1;
            ClearProfileFilter(st);
            RefreshProfileList(st);
            LoadProfileToUi(st);
            break;
        }
        case IDC_REMOVE: {
            if (st->selProfile < 0) return;
            std::wstring m = L"Remove the profile \"" +
                             st->work.profiles[static_cast<size_t>(st->selProfile)].name +
                             L"\"?";
            if (MessageBoxW(hwnd, m.c_str(), L"Game Optimizer",
                            MB_YESNO | MB_ICONQUESTION) != IDYES) return;
            st->work.profiles.erase(st->work.profiles.begin() + st->selProfile);
            if (st->selProfile >= static_cast<int>(st->work.profiles.size()))
                st->selProfile = static_cast<int>(st->work.profiles.size()) - 1;
            RefreshProfileList(st);
            LoadProfileToUi(st);
            break;
        }
        case IDC_RENAME: {
            if (st->selProfile < 0) return;
            std::wstring name = st->work.profiles[static_cast<size_t>(st->selProfile)].name;
            if (!PromptName(hwnd, L"New name for this profile:", name)) return;
            if (name.empty()) return;
            st->work.profiles[static_cast<size_t>(st->selProfile)].name = name;
            RefreshProfileList(st);
            break;
        }
        default:
            break;
    }
}

void OnResetMask(SettingsState* st, HWND hwnd) {
    std::wstring name = ComboText(st->hMapMask);
    if (name.empty()) return;
    std::vector<Mask> derived = DeriveMasks(*st->topo);
    const Mask* src = nullptr;
    for (size_t i = 0; i < derived.size(); ++i) {
        if (IEquals(derived[i].name, name)) { src = &derived[i]; break; }
    }
    if (!src) {
        MessageBoxW(hwnd,
                    L"This mask is not one of the masks derived for this machine, so there "
                    L"is nothing detected to reset it to.",
                    L"Game Optimizer", MB_OK | MB_ICONINFORMATION);
        return;
    }
    Mask* dst = st->work.FindMask(name);
    if (!dst) return;
    dst->ids = src->ids;
    dst->derived = true;
    SelectMapMask(st);
    RelayoutIfWarningsChanged(st);   // the reset mask may be a profile's game or heavy mask
    RepaintChrome(hwnd);             // and the "In this mask" gauge is drawn from its ids
}

LRESULT CALLBACK SettingsProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    SettingsState* st =
        reinterpret_cast<SettingsState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg) {
        case WM_NCCREATE: {
            CREATESTRUCTW* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                              reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
            break;
        }
        case WM_CREATE: {
            st = reinterpret_cast<SettingsState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
            st->dpi = DpiOf(hwnd);
            theme::ApplyDarkFrame(hwnd);
            st->cardBrush = CreateSolidBrush(theme::P().cardBg);
            st->inputBrush = CreateSolidBrush(theme::P().inputBg);

            // The tab bar across the TOP - operator decision, menu on top, no side panel.
            // Same contract as the rail it replaced apart from the notification code, so the
            // page switching below is unchanged.
            st->hNav = CreateWindowExW(
                0, theme::kTabBarClass, L"", WS_CHILD | WS_VISIBLE, 0, 0, 10, 10, hwnd,
                reinterpret_cast<HMENU>(static_cast<UINT_PTR>(IDC_NAV)),
                GetModuleHandleW(nullptr), nullptr);
            if (st->hNav) {
                theme::TabBarAddItem(st->hNav, IDC_NAV_PROFILES, L"Profiles");
                theme::TabBarAddItem(st->hNav, IDC_NAV_COREMAP, L"Core map");
                theme::TabBarAddItem(st->hNav, IDC_NAV_GENERAL, L"General");
                theme::TabBarSetSelected(st->hNav, IDC_NAV_PROFILES);
            } else {
                // Read GetLastError before anything else can overwrite it. Without a
                // switcher two of the three pages would be unreachable, so a plain button
                // row stands in rather than the window losing most of its content.
                const DWORD gle = GetLastError();
                LogLine(L"[settings] the tab bar could not be created "
                        L"(class %s), gle=%lu", theme::kTabBarClass, gle);
                const wchar_t* names[PAGE_COUNT] = { L"Profiles", L"Core map", L"General" };
                for (int i = 0; i < PAGE_COUNT; ++i) {
                    st->hNavBtn[i] = Mk(hwnd, L"BUTTON", names[i],
                                        BS_OWNERDRAW | WS_TABSTOP, IDC_NAV_PROFILES + i);
                    SetButtonKind(st->hNavBtn[i], i == 0 ? theme::ButtonKind::Primary
                                                         : theme::ButtonKind::Ghost);
                }
            }

            st->hProfHdr  = Mk(hwnd, L"STATIC", L"Profiles", SS_LEFT, -1);
            // The search box is a PLAIN edit; its frame, magnifier and placeholder are drawn
            // by theme::DrawSearchChrome from the parent's paint - see OverdrawSearchChrome.
            st->hSearch = Mk(hwnd, L"EDIT", L"", ES_AUTOHSCROLL | WS_TABSTOP, IDC_SEARCH);
            st->hAddGame = Mk(hwnd, L"BUTTON", L"Add game...",
                              BS_OWNERDRAW | WS_TABSTOP, IDC_ADDGAME);
            SetButtonKind(st->hAddGame, theme::ButtonKind::Primary);
            st->hProfList = Mk(hwnd, L"LISTBOX", L"",
                               LBS_NOTIFY | LBS_HASSTRINGS | LBS_OWNERDRAWFIXED |
                                   WS_VSCROLL | WS_TABSTOP,
                               IDC_PROFLIST);
            st->hAdd = Mk(hwnd, L"BUTTON", L"Add", BS_OWNERDRAW | WS_TABSTOP, IDC_ADD);
            st->hDup = Mk(hwnd, L"BUTTON", L"Duplicate", BS_OWNERDRAW | WS_TABSTOP, IDC_DUP);
            st->hRem = Mk(hwnd, L"BUTTON", L"Remove", BS_OWNERDRAW | WS_TABSTOP, IDC_REMOVE);
            st->hRen = Mk(hwnd, L"BUTTON", L"Rename", BS_OWNERDRAW | WS_TABSTOP, IDC_RENAME);
            SetButtonKind(st->hAdd, theme::ButtonKind::Secondary);
            SetButtonKind(st->hDup, theme::ButtonKind::Secondary);
            SetButtonKind(st->hRem, theme::ButtonKind::Danger);
            SetButtonKind(st->hRen, theme::ButtonKind::Secondary);

            st->hEditHdr = Mk(hwnd, L"STATIC", L"Selected profile", SS_LEFT, -1);
            st->hEnabled = Mk(hwnd, L"BUTTON", L"Profile enabled",
                              BS_AUTOCHECKBOX | WS_TABSTOP, IDC_ENABLED);

            st->hGameLbl = Mk(hwnd, L"STATIC", L"Game:", SS_LEFT, -1);
            st->hGame = Mk(hwnd, L"EDIT", L"", ES_AUTOHSCROLL | WS_TABSTOP, IDC_GAME);
            st->hGamePick = Mk(hwnd, L"BUTTON", L"Pick running...",
                               BS_OWNERDRAW | WS_TABSTOP, IDC_GAMEPICK);
            st->hGameBrowse = Mk(hwnd, L"BUTTON", L"Browse...",
                                 BS_OWNERDRAW | WS_TABSTOP, IDC_GAMEBROWSE);
            SetButtonKind(st->hGamePick, theme::ButtonKind::Secondary);
            SetButtonKind(st->hGameBrowse, theme::ButtonKind::Secondary);

            st->hGameMaskLbl = Mk(hwnd, L"STATIC", L"Game mask:", SS_LEFT, -1);
            st->hGameMask = Mk(hwnd, L"COMBOBOX", L"",
                               CBS_DROPDOWNLIST | CBS_OWNERDRAWFIXED | CBS_HASSTRINGS |
                                   WS_VSCROLL | WS_TABSTOP,
                               IDC_GAMEMASK);
            st->hGameMaskWarn = Mk(hwnd, L"STATIC", L"", SS_LEFT, -1);
            if (st->hGameMaskWarn) ShowWindow(st->hGameMaskWarn, SW_HIDE);

            st->hHeavyLbl = Mk(hwnd, L"STATIC", L"Heavy apps and their CPU use:",
                               SS_LEFT, -1);
            // Was a multi-line EDIT of names. It is now an owner-drawn LISTBOX so each row
            // can carry a live CPU meter beside the name - a text box cannot show that.
            // Add / Pick running / Remove keep it every bit as editable as the box was.
            st->hHeavy = Mk(hwnd, L"LISTBOX", L"",
                            LBS_NOTIFY | LBS_HASSTRINGS | LBS_OWNERDRAWFIXED |
                                WS_VSCROLL | WS_TABSTOP,
                            IDC_HEAVY);
            st->hHeavyPick = Mk(hwnd, L"BUTTON", L"Pick running...",
                                BS_OWNERDRAW | WS_TABSTOP, IDC_HEAVYPICK);
            // "Browse..." rather than the old "Add...", and it now does what the name says:
            // it opens the exe browser. Operator's request. The id is unchanged so nothing
            // that dispatches on it moves.
            st->hHeavyAdd = Mk(hwnd, L"BUTTON", L"Browse...",
                               BS_OWNERDRAW | WS_TABSTOP, IDC_HEAVYADD);
            st->hHeavyRem = Mk(hwnd, L"BUTTON", L"Remove",
                               BS_OWNERDRAW | WS_TABSTOP, IDC_HEAVYREM);
            SetButtonKind(st->hHeavyPick, theme::ButtonKind::Secondary);
            SetButtonKind(st->hHeavyAdd, theme::ButtonKind::Secondary);
            SetButtonKind(st->hHeavyRem, theme::ButtonKind::Danger);

            st->hHeavyMaskLbl = Mk(hwnd, L"STATIC", L"Heavy mask:", SS_LEFT, -1);
            st->hHeavyMask = Mk(hwnd, L"COMBOBOX", L"",
                                CBS_DROPDOWNLIST | CBS_OWNERDRAWFIXED | CBS_HASSTRINGS |
                                    WS_VSCROLL | WS_TABSTOP,
                                IDC_HEAVYMASK);
            st->hHeavyMaskWarn = Mk(hwnd, L"STATIC", L"", SS_LEFT, -1);
            if (st->hHeavyMaskWarn) ShowWindow(st->hHeavyMaskWarn, SW_HIDE);

            st->hAutoPin = Mk(hwnd, L"BUTTON", L"Auto-pin busy background processes",
                              BS_AUTOCHECKBOX | WS_TABSTOP, IDC_AUTOPIN);
            // The sentence no longer mentions a seconds figure, because there is no longer a
            // seconds field for the user to set. The debounce still exists - it is fixed at
            // kAutoPinDebounceTicks and is not a setting - so the wording does not promise
            // that the rule fires on a single sample either.
            // The legend for the AUTO tag lives HERE rather than in the list's caption or in
            // a fourth static, and the sentence was tightened to pay for it in the same two
            // lines: descH is a fixed Dp(34) and the right-hand card has no slack to give -
            // see fixedR in SettingsLayout, which already competes with the list's own floor.
            st->hAutoDesc = Mk(hwnd, L"STATIC",
                               L"While this game is in front, processes that stay above the "
                               L"threshold move to the background mask until the game exits. "
                               L"The list above tags them AUTO.",
                               SS_LEFT, -1);
            st->hPctLbl = Mk(hwnd, L"STATIC", L"Pin a process above", SS_LEFT, -1);
            st->hPct = Mk(hwnd, L"EDIT", L"", ES_NUMBER | ES_AUTOHSCROLL | WS_TABSTOP,
                          IDC_PCT);
            // The live status line. Created empty and hidden; RefreshAutoPinStatus fills it
            // in and shows it, on the same beat as everything else on this window.
            st->hAutoStatus = Mk(hwnd, L"STATIC", L"", SS_LEFT, -1);
            if (st->hAutoStatus) ShowWindow(st->hAutoStatus, SW_HIDE);

            st->hMapHdr = Mk(hwnd, L"STATIC", L"Core map", SS_LEFT, -1);
            st->hTopoText = Mk(hwnd, L"STATIC", TopologyBlock(*st->topo).c_str(),
                               SS_LEFT, -1);
            st->hMapMaskLbl = Mk(hwnd, L"STATIC", L"Editing mask:", SS_LEFT, -1);
            st->hMapMask = Mk(hwnd, L"COMBOBOX", L"",
                              CBS_DROPDOWNLIST | CBS_OWNERDRAWFIXED | CBS_HASSTRINGS |
                                  WS_VSCROLL | WS_TABSTOP,
                              IDC_MAPMASK);
            st->hMapReset = Mk(hwnd, L"BUTTON", L"Reset to detected",
                               BS_OWNERDRAW | WS_TABSTOP, IDC_MAPRESET);
            SetButtonKind(st->hMapReset, theme::ButtonKind::Secondary);
            st->hMap = CreateWindowExW(0, kCoreMapClass, L"",
                                       WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                                       0, 0, 10, 10, hwnd,
                                       reinterpret_cast<HMENU>(static_cast<UINT_PTR>(IDC_MAP)),
                                       GetModuleHandleW(nullptr), nullptr);
            if (st->hMap) {
                CoreMapSetTopology(st->hMap, st->topo);
                CoreMapSetEditable(st->hMap, true);
            } else {
                // Read GetLastError before anything else can overwrite it. A silent skip here
                // leaves a blank rectangle that reads as a layout bug, and the core map is the
                // only place a user can check or correct the detected topology - so say so on
                // screen as well as in the log.
                const DWORD gle = GetLastError();
                LogLine(L"[settings] the core map control could not be created "
                        L"(class %s), gle=%lu", kCoreMapClass, gle);
                st->hMapFail = Mk(hwnd, L"STATIC",
                                  L"The core map could not be created, so the per-core view "
                                  L"is not available on this page. Everything else here still "
                                  L"works. See GameOptimizer.log in the config folder.",
                                  SS_LEFT, -1);
            }

            st->hGenHdr = Mk(hwnd, L"STATIC", L"General", SS_LEFT, -1);
            st->hStartup = Mk(hwnd, L"BUTTON", L"Start with Windows",
                              BS_AUTOCHECKBOX | WS_TABSTOP, IDC_STARTUP);
            st->hNotify = Mk(hwnd, L"BUTTON", L"Show notifications",
                             BS_AUTOCHECKBOX | WS_TABSTOP, IDC_NOTIFY);
            st->hPollLbl = Mk(hwnd, L"STATIC", L"Poll interval (ms):", SS_LEFT, -1);
            st->hPoll = Mk(hwnd, L"EDIT", L"", ES_NUMBER | ES_AUTOHSCROLL | WS_TABSTOP,
                           IDC_POLL);
            st->hGameModeStatus = Mk(hwnd, L"STATIC", L"", SS_LEFT, -1);
            st->hVCacheStatus = Mk(hwnd, L"STATIC", L"", SS_LEFT, -1);
            st->hVCacheEffect = Mk(hwnd, L"STATIC", L"", SS_LEFT, -1);
            if (st->hVCacheEffect) ShowWindow(st->hVCacheEffect, SW_HIDE);
            st->hBlocked = Mk(hwnd, L"STATIC", L"", SS_LEFT, -1);
            st->hInspect = Mk(hwnd, L"BUTTON", L"Inspect processes...",
                              BS_OWNERDRAW | WS_TABSTOP, IDC_INSPECT);
            SetButtonKind(st->hInspect, theme::ButtonKind::Secondary);

            // The sponsor strip, directly above the footer on every page. It owns its own
            // painting, its own hit test and its own URLs - this window neither handles its
            // clicks nor knows where it points.
            //
            // Created HIDDEN. The strip is normally rendered by a WebView2 showing the
            // plugin's own markup (below), and this control is the fallback that appears the
            // moment that cannot be had. Showing it first and swapping later would flash;
            // showing it only on failure costs nothing, because the failure is usually
            // synchronous - no loader DLL, no runtime - and the band is empty for a couple of
            // hundred milliseconds at worst when it is not.
            st->hSponsor = CreateWindowExW(
                0, kSponsorClass, L"", WS_CHILD, 0, 0, 10, 10, hwnd,
                nullptr, GetModuleHandleW(nullptr), nullptr);
            if (st->hSponsor) {
                const SIZE sz = SponsorMeasure(st->dpi);
                st->sponsorW = sz.cx > 0 ? static_cast<int>(sz.cx) : 0;
                st->sponsorH = sz.cy > 0 ? static_cast<int>(sz.cy) : 0;
                // A measure of zero would reserve a band nothing can be seen in, so the strip
                // is dropped rather than laid out at a size it did not ask for.
                if (st->sponsorH <= 0) {
                    DestroyWindow(st->hSponsor);
                    st->hSponsor = nullptr;
                    st->sponsorW = st->sponsorH = 0;
                }
            } else {
                // Read GetLastError before anything else can overwrite it. The strip is
                // decoration plus three links; losing it must not cost the window a band of
                // dead space, so sponsorH stays 0 and the layout collapses the band.
                const DWORD gle = GetLastError();
                LogLine(L"[settings] the sponsor strip could not be created "
                        L"(class %s), gle=%lu", kSponsorClass, gle);
            }

            // The WebView2 rendering of the same strip, from the plugin's own HTML and CSS.
            // It is attempted ONLY here, when the Settings window is built - never on the
            // startup path, and never while the app is sitting in the tray - and it is torn
            // down again in WM_DESTROY.
            //
            // A null return is a synchronous refusal: no loader DLL, no runtime, no host
            // window. Non-null means creation is in flight and SponsorWebReady below gets the
            // verdict. EITHER WAY the GDI control is what the user ends up looking at when
            // this does not work, which is why it was created above and not conditionally.
            //
            // It is attempted even when the GDI control above could NOT be created. The two
            // are independent renderings and either can fail on its own; gating the web panel
            // on the fallback's success would throw away the good one because the spare was
            // missing. If both fail the band collapses to nothing, which is the honest result.
            {
                RECT wr;
                SetRect(&wr, 0, 0, 10, 10);   // real bounds arrive with the first layout
                st->web = WebSponsorCreate(hwnd, wr, &SponsorWebReady, hwnd);
                if (st->web == nullptr) {
                    if (st->hSponsor != nullptr) ShowWindow(st->hSponsor, SW_SHOWNA);
                } else {
                    // Creation is asynchronous and can take a long time when it is going to
                    // fail. See kSponsorFallbackTimer: this is what guarantees the band is
                    // never empty, whatever the runtime decides to do.
                    SetTimer(hwnd, kSponsorFallbackTimer, kSponsorFallbackMs, nullptr);
                }
            }

            // BS_OWNERDRAW and BS_DEFPUSHBUTTON share the low style nibble, so OK cannot be
            // both. Enter still reaches IDOK: IsDialogMessageW falls back to IDOK when the
            // window reports no default id, which is exactly this case.
            st->hOk = Mk(hwnd, L"BUTTON", L"OK", BS_OWNERDRAW | WS_TABSTOP, IDOK);
            st->hCancel = Mk(hwnd, L"BUTTON", L"Cancel", BS_OWNERDRAW | WS_TABSTOP,
                             IDCANCEL);
            st->hApply = Mk(hwnd, L"BUTTON", L"Apply", BS_OWNERDRAW | WS_TABSTOP, IDC_APPLY);
            SetButtonKind(st->hOk, theme::ButtonKind::Primary);
            SetButtonKind(st->hCancel, theme::ButtonKind::Secondary);
            SetButtonKind(st->hApply, theme::ButtonKind::Secondary);

            ApplySettingsFonts(st, hwnd);
            {
                // UseClassicChrome stays: it keeps the control off the themed hot-track path
                // so it stops asking to repaint on every mouse move. The pixels themselves
                // are now ours - see CheckBoxProc for why this is a subclass and not
                // BS_OWNERDRAW, which would destroy BM_GETCHECK on these controls.
                HWND checks[] = { st->hEnabled, st->hAutoPin, st->hStartup, st->hNotify };
                for (HWND h : checks) {
                    UseClassicChrome(h);
                    if (h) SetWindowSubclass(h, CheckBoxProc, kCheckSubclassId, 0);
                }
                HWND combos[] = { st->hGameMask, st->hHeavyMask, st->hMapMask };
                for (HWND c : combos)
                    if (c) SetWindowSubclass(c, ComboProc, kComboSubclassId, 0);
            }

            SetChecked(st->hStartup, st->work.startWithWindows);
            SetChecked(st->hNotify, st->work.notifications);
            SetWindowTextW(st->hPoll, std::to_wstring(st->work.pollMs).c_str());

            FillMaskCombo(st->hMapMask, st->work,
                          st->topo->defaultGameMask);
            if (SendMessageW(st->hMapMask, CB_GETCURSEL, 0, 0) == CB_ERR &&
                !st->work.masks.empty())
                SendMessageW(st->hMapMask, CB_SETCURSEL, 0, 0);

            RefreshProfileList(st);
            RefreshLiveTopology(st);   // before the first warning pass, or it has no data
            // LoadProfileToUi -> SetHeavyItems needs process presence before it builds the
            // rows; sampling afterwards made the first ordering pass see everything inactive.
            // The first snapshot has no predecessor, so every percentage in it is 0. The
            // meters fill in on the next timer tick; they never show a made-up figure.
            RefreshCpuTable(st);
            LoadProfileToUi(st);
            SelectMapMask(st);
            RefreshBlockedLine(st);
            UpdateEnvironmentSection(st);
            UpdateMaskWarnings(st);
            RefreshAutoPinStatus(st);
            ApplyPageVisibility(st);
            SettingsLayout(st, hwnd);
            SetTimer(hwnd, kStatusTimer, 1000, nullptr);
            return 0;
        }
        case WM_SIZE:
            // The class deliberately has no CS_HREDRAW/CS_VREDRAW, so a resize only
            // invalidates the newly exposed strip. Every child just moved, so the repaint
            // has to be asked for explicitly here.
            if (st) {
                SettingsLayout(st, hwnd);
                RedrawSettings(hwnd);
            }
            return 0;
        case WM_GETMINMAXINFO: {
            // The pages no longer scroll, so the window has to stay large enough to show
            // one. Below this the Profiles page - the tallest, and taller still while a
            // parked warning is up - would clip its last row.
            //
            // BOTH FIGURES GREW THIS ROUND. Width, because the Profiles page is now two
            // columns and the narrower of them still has to hold a combo and two buttons.
            // Height, because the tab bar takes a strip off the top and the auto-pin rule
            // moved onto that page from a page of its own.
            //
            // AND IT GREW AGAIN THIS ROUND, TWICE. The auto-pin card carries a status row it
            // did not have, and the sponsor strip takes a band above the footer - so the
            // minimum has to cover both or the strip collides with the page content or the
            // footer.
            //
            // THE PANEL'S CONTRIBUTION IS ITS OWN NATURAL SIZE, never a constant: needH is
            // the page's own requirement PLUS whatever SponsorBandSize reports plus the one
            // gap between them. It calls the SAME function LayoutPage does, so the minimum
            // and the layout cannot drift apart - and when the panel shrinks, or is replaced
            // by the much shorter GDI row, this minimum shrinks by exactly the same number of
            // pixels instead of leaving a band of dead space above the footer.
            //
            // AND IT SHRANK AGAIN ON 2026-08-29. The panel was re-grouped from the plugin's
            // 261px VERTICAL STACK into a single row, so this minimum dropped by ~111px. needH
            // adds only the panel's HEIGHT; the minimum WIDTH is argued separately below.
            MINMAXINFO* mm = reinterpret_cast<MINMAXINFO*>(lp);
            if (!mm) break;
            const int dpi = st ? st->dpi : DpiOf(hwnd);
            // 660 + the status row (Dp 34) + its gap (kGapTight, Dp 6). That
            // existing floor already budgets the old worst case: two Dp(36)
            // warning rows. Wrapped warnings can be taller, so add only the
            // measured excess over that legacy card at the minimum client width.
            const int minClientW = theme::Dp(880, dpi);
            const ProfileColumns minColumns = MeasureProfileColumns(minClientW, dpi);
            HDC measureDc = st ? GetDC(hwnd) : nullptr;
            const WarningLayout warning =
                MeasureWarningLayout(st, measureDc, minColumns.rightW, dpi);
            if (measureDc) ReleaseDC(hwnd, measureDc);
            const int legacyWarningCardH =
                2 * theme::Dp(theme::metric::kCardPad, dpi) + 2 * theme::Dp(36, dpi) +
                theme::Dp(theme::metric::kGapTight, dpi);
            int needH = theme::Dp(700, dpi);
            if (warning.cardH > legacyWarningCardH)
                needH += warning.cardH - legacyWarningCardH;
            {
                const SIZE band = SponsorBandSize(st, dpi);
                if (band.cx > 0 && band.cy > 0)
                    needH += static_cast<int>(band.cy) + theme::Dp(theme::metric::kGap, dpi);
            }
            // THE MINIMUM WIDTH IS Dp(880), AND WHAT IT IS FOR HAS CHANGED. It used to be a
            // robustness choice with no clipping argument behind it, because the panel was a
            // fixed 831 CSS px card that could not be too wide for a window this size. THE PANEL
            // NOW FILLS THE WINDOW, so the question is the mirror image - is this minimum WIDE
            // ENOUGH for the panel? - and it is a hard requirement rather than a preference.
            //
            // The panel is one row of three groups spread with `justify-content: space-between`,
            // and it has a FLOOR: kSponsorCssMinWidth, the narrowest host at which the three
            // groups still fit. Below it the row overflows its end edge and
            // `.shell { overflow: hidden }` cuts the GOATPROJECT lockup off, silently, with no
            // error anywhere.
            //
            // RE-DERIVED 2026-08-29, and the numbers are measured, not reasoned about:
            //
            //   [M] kSponsorCssMinWidth = 849 (src\sponsor_html.h). tools\measure-panel.py
            //       renders the shipped page and sweeps the host width 1000px down to 640px in
            //       1px steps: the first break is at 848 (row-overflow), so the floor is 849.
            //       The generator's own --measure pass reports the same 848.83 intrinsic width
            //       from `width:max-content` - two independent routes, one answer.
            //   [M] The panel gets cw - 2 * theme::metric::kGap, so at this minimum it has
            //       880 - 2 * 12 = 856 CSS px. 849 fits with 7 to spare.
            //   [M] 856 is the CSS-px answer and not the guard: the three MulDiv calls round
            //       independently, so the largest floor that is safe at EVERY integer dpi from
            //       96 to 480 (custom scaling runs 100%..500%) is 855. 849 is inside it.
            //   [M] Sweeping every one of those 385 DPI values, the worst margin is 7 DEVICE px,
            //       at dpi 96; it GROWS with DPI (9 at 125%, 10 at 150%, 12 at 175%) because
            //       7 CSS px scale up while the rounding error stays at most 1px. The thin case
            //       is 100% and even there nothing rounds it away.
            //
            // COULD IT BE SMALLER? Yes: Dp(874) is the smallest minimum at which a 849px floor
            // is safe at every DPI. It is not taken. 880 costs 6 CSS px against 874, and buys:
            //   * headroom for the panel's floor to move. The floor is a MEASUREMENT of four
            //     intrinsic widths and two gaps; a font-metric change moves it, and at 874 the
            //     margin is zero, so the next re-measure would fail the generator's guard rather
            //     than absorb it.
            //   * defence in depth. What refuses an over-budget panel is a Python script's
            //     guard; a regression there is silent in a way this constant is not.
            //   * it is already the shipped minimum, so keeping it changes nothing for anyone.
            //
            // Not more than 880: at 150% scaling Dp(880) is 1320 device px and still fits a
            // 1366-wide screen with 30px spare. Dp(900) leaves exactly ZERO there; Dp(920)
            // does not fit at all.
            //
            // MIN_CLIENT_CSS_W in tools\gen-sponsor-html.py AND in tools\measure-panel.py is
            // this same 880. All three have to move together or the guard is measuring a window
            // that does not exist.
            RECT need = { 0, 0, minClientW, needH };
            AdjustWindowRectEx(&need, WS_OVERLAPPEDWINDOW, FALSE, 0);
            mm->ptMinTrackSize.x = need.right - need.left;
            mm->ptMinTrackSize.y = need.bottom - need.top;
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
            if (dc && st) PaintSettings(st, hwnd, dc);
            EndPaint(hwnd, &ps);
            // The combo frames and the search chrome go on last and outside the BeginPaint
            // DC, which is clipped out of every child rect by WS_CLIPCHILDREN.
            OverdrawPageCombos(st, hwnd);
            OverdrawSearchChrome(st, hwnd);
            return 0;
        }
        case WM_MEASUREITEM: {
            MEASUREITEMSTRUCT* mi = reinterpret_cast<MEASUREITEMSTRUCT*>(lp);
            if (!mi || !st) break;
            if (mi->CtlType == ODT_LISTBOX || mi->CtlType == ODT_COMBOBOX) {
                // The heavy list carries a meter as well as a name, so it gets a slightly
                // taller row; everything else stays on the standard one.
                const int h = (mi->CtlType == ODT_LISTBOX &&
                               mi->CtlID == static_cast<UINT>(IDC_HEAVY))
                                  ? theme::Dp(28, st->dpi)
                                  : theme::Dp(theme::metric::kRowH, st->dpi);
                mi->itemHeight = static_cast<UINT>(h);
                return TRUE;
            }
            break;
        }
        case WM_DRAWITEM: {
            const DRAWITEMSTRUCT* di = reinterpret_cast<const DRAWITEMSTRUCT*>(lp);
            if (!di || !st) break;
            if (di->CtlType == ODT_BUTTON)
                return theme::DrawButton(di, ButtonKindOf(di->hwndItem), st->dpi);
            if (di->CtlType == ODT_COMBOBOX)
                return theme::DrawComboBox(di, st->dpi);
            if (di->CtlType == ODT_LISTBOX) {
                // Two lists carry more than a string and draw themselves; the generic helper
                // still handles any other.
                if (di->hwndItem == st->hProfList) return DrawProfileItem(st, di);
                if (di->hwndItem == st->hHeavy)    return DrawHeavyItem(st, di);
                return theme::DrawListBoxItem(di, st->dpi);
            }
            break;
        }
        case WM_TIMER:
            if (st && wp == kSponsorFallbackTimer) {
                KillTimer(hwnd, kSponsorFallbackTimer);
                // The web strip has not reported in. Whether it eventually succeeds or fails,
                // the band gets a strip NOW; SponsorWebReady hides this one again if the
                // page does turn up.
                if (!st->webShowing && st->hSponsor != nullptr) {
                    // Size the band for the control that is about to be on screen. The two
                    // renderings are different shapes: leaving the band at the web panel's
                    // 462x150 rectangle would strand dead height above the 45px GDI row.
                    st->webLate = true;
                    ShowWindow(st->hSponsor, SW_SHOWNA);
                    SettingsLayout(st, hwnd);
                    InvalidateRect(hwnd, nullptr, TRUE);
                }
                return 0;
            }
            if (st && wp == kStatusTimer) {
                bool relayout = RefreshBlockedLine(st);
                // Query-only live refresh: one Game Mode registry value and exactly the
                // named AMD service. CPU brand/elevation stay on the full one-time probe;
                // no service enumeration and no second timer are introduced.
                RefreshEnvironmentStatus(st->env);
                if (UpdateEnvironmentSection(st)) relayout = true;
                // Parked state moves under load, so the warnings are re-checked on the same
                // beat as the core map's own parked refresh rather than only at selection.
                RefreshLiveTopology(st);
                if (UpdateMaskWarnings(st)) relayout = true;
                // BEFORE the status sentence, which reports the count this sets. Same timer,
                // no second one: see kAutoRowsShown for what this costs and why it is not
                // behind the visibility guard the CPU sampling is - it opens no process and
                // enumerates nothing, so gating it would save nothing and let the rows go
                // stale for a second every time the page came back.
                bool autoRowsMoved = SyncAutoPinRows(st);
                // The auto-pin state is live: the game starts, the user alt-tabs, another
                // profile takes over. The sentence and the dot both move with it, and so does
                // the meters' colour ramp - which is why the heavy list is invalidated below
                // whenever this reports a change, not only when a percentage moved.
                bool autoChanged = RefreshAutoPinStatus(st);
                if (autoRowsMoved) autoChanged = true;
                if (st->hMap) CoreMapRefreshParked(st->hMap);
                // The heavy-apps meters live on this same beat. Sampled only while their list
                // is actually on screen - a hidden page has nothing to show and the snapshot
                // is not free.
                if (st->hHeavy && IsWindowVisible(st->hHeavy)) {
                    RefreshCpuTable(st);
                    // Same beat, same snapshot, and behind the same visibility guard: no
                    // process is opened for a row that is not on screen. See
                    // RefreshCpuSetStages for the measured cost of one readback.
                    if (RefreshCpuSetStages(st)) autoChanged = true;
                    InvalidateRect(st->hHeavy, nullptr, TRUE);
                } else if (autoChanged && st->hHeavy) {
                    InvalidateRect(st->hHeavy, nullptr, TRUE);
                }
                if (relayout) {
                    SettingsLayout(st, hwnd);
                    RedrawSettings(hwnd);
                } else if (autoChanged) {
                    // The status dot and the target's "Now:" label are both drawn by the
                    // PARENT, so a state change with no layout change still needs the
                    // parent's chrome repainted.
                    RepaintChrome(hwnd);
                }
            }
            return 0;
        // Standard controls do not go dark on their own. WM_CTLCOLOR* is the documented
        // route for the ones that are not owner-drawn: STATIC, EDIT, LISTBOX and the
        // checkboxes' background.
        case WM_CTLCOLORSTATIC:
        case WM_CTLCOLOREDIT:
        case WM_CTLCOLORLISTBOX:
        case WM_CTLCOLORBTN: {
            HDC dc = reinterpret_cast<HDC>(wp);
            HWND ctl = reinterpret_cast<HWND>(lp);
            HBRUSH b = theme::OnCtlColor(msg, dc, ctl);
            if (!st) {
                if (b) return reinterpret_cast<LRESULT>(b);
                break;
            }
            const theme::Palette& pal = theme::P();
            if (msg == WM_CTLCOLORSTATIC || msg == WM_CTLCOLORBTN) {
                // Every static and checkbox in this window sits ON a card, so it must erase
                // to cardBg whichever surface the generic helper assumed. A hollow brush
                // would show the card through but stop the control erasing at all, and the
                // blocked line rewrites itself once a second - that smears.
                if (st->cardBrush) {
                    SetBkColor(dc, pal.cardBg);
                    b = st->cardBrush;
                }
                COLORREF fg = pal.textSecondary;
                if (msg == WM_CTLCOLORBTN || ctl == st->hProfHdr || ctl == st->hEditHdr ||
                    ctl == st->hMapHdr || ctl == st->hGenHdr) {
                    fg = pal.textPrimary;
                } else if (ctl == st->hGameMaskWarn || ctl == st->hHeavyMaskWarn ||
                           ctl == st->hMapFail) {
                    fg = pal.warn;
                } else if (ctl == st->hAutoStatus) {
                    // The DOT carries the state; the sentence stays readable prose. It only
                    // dims when the rule itself is switched off, matching the rest of the
                    // card going quiet.
                    fg = (st->autoState == AutoPinState::Off) ? pal.textDim
                                                              : pal.textSecondary;
                } else if (ctl == st->hVCacheEffect) {
                    fg = pal.textDim;
                } else if (IsAutoPinLabel(st, ctl) && AutoPinLabelsAreDim(st)) {
                    // These three stay enabled on purpose - see SyncAutoPinEnable - so the
                    // "off" state has to be carried by the colour rather than by the control's
                    // own disabled painting, which embosses.
                    fg = pal.textDim;
                }
                SetTextColor(dc, fg);
            } else if (!b) {
                SetBkColor(dc, pal.inputBg);
                SetTextColor(dc, pal.textPrimary);
                b = st->inputBrush;
            }
            if (b) return reinterpret_cast<LRESULT>(b);
            break;
        }
        case WM_DPICHANGED: {
            if (!st) break;
            st->dpi = static_cast<int>(HIWORD(wp));
            // theme::GetFont caches per (font, dpi) and owns every handle, so the previous
            // set is not deleted here - doing so would free a handle other windows still use.
            ApplySettingsFonts(st, hwnd);
            // The strip's natural size is DPI-dependent, so the cached measure is stale now.
            if (st->hSponsor) {
                const SIZE sz = SponsorMeasure(st->dpi);
                st->sponsorW = sz.cx > 0 ? static_cast<int>(sz.cx) : 0;
                st->sponsorH = sz.cy > 0 ? static_cast<int>(sz.cy) : 0;
            }
            const RECT* nr = reinterpret_cast<const RECT*>(lp);
            SetWindowPos(hwnd, nullptr, nr->left, nr->top, nr->right - nr->left,
                         nr->bottom - nr->top, SWP_NOZORDER | SWP_NOACTIVATE);
            SettingsLayout(st, hwnd);
            RedrawSettings(hwnd);
            return 0;
        }
        case WM_COMMAND: {
            if (!st) break;
            const int id = LOWORD(wp);
            const int code = HIWORD(wp);

            // Page switch. The tab bar reports TABN_SELCHANGED; the stand-in buttons report a
            // plain click. Both land on the same page id, so both are handled here.
            if (code == static_cast<int>(theme::TABN_SELCHANGED)) {
                int sel = id;
                if (st->hNav) {
                    const int s2 = theme::TabBarGetSelected(st->hNav);
                    if (s2 >= IDC_NAV_PROFILES && s2 <= IDC_NAV_GENERAL) sel = s2;
                }
                if (sel >= IDC_NAV_PROFILES && sel <= IDC_NAV_GENERAL)
                    SwitchPage(st, hwnd, sel - IDC_NAV_PROFILES);
                return 0;
            }
            if (id >= IDC_NAV_PROFILES && id <= IDC_NAV_GENERAL) {
                SwitchPage(st, hwnd, id - IDC_NAV_PROFILES);
                return 0;
            }

            if (id == IDC_MAP && code == static_cast<int>(CMN_SELECTION_CHANGED)) {
                if (!st->loading && st->hMap) {
                    std::wstring name = ComboText(st->hMapMask);
                    Mask* m = st->work.FindMask(name);
                    if (m) {
                        m->ids = CoreMapGetSelection(st->hMap);
                        m->derived = false;
                        // The mask the user just hand-edited may be the game or heavy mask,
                        // so its parked count may have just changed.
                        RelayoutIfWarningsChanged(st);
                        // ...and the "In this mask" gauge is drawn from exactly these ids.
                        RepaintChrome(hwnd);
                    }
                }
                return 0;
            }
            switch (id) {
                case IDC_SEARCH:
                    // Live filter over the profile NAME and the game exe. The selection is
                    // deliberately not moved by typing - see RefreshProfileList.
                    if (code == EN_CHANGE && !st->loading) {
                        RefreshProfileList(st);
                        OverdrawSearchChrome(st, hwnd);
                    } else if (code == EN_SETFOCUS || code == EN_KILLFOCUS) {
                        // The focus ring and the placeholder are ours, so they have to be
                        // repainted when the control gains or loses focus.
                        OverdrawSearchChrome(st, hwnd);
                    }
                    return 0;
                case IDC_ADDGAME:
                    OnAddGame(st, hwnd);
                    return 0;
                case IDC_PROFLIST:
                    // A LIST ROW IS NOT A PROFILE INDEX. The list is ordered by
                    // ProfilesForDisplay and filtered by the search box, so the row has to be
                    // mapped back through st->rows.
                    if (code == LBN_SELCHANGE && !st->loading) {
                        const int row = static_cast<int>(
                            SendMessageW(st->hProfList, LB_GETCURSEL, 0, 0));
                        const int pi = ProfileForRow(st, row);
                        if (pi >= 0) {
                            StoreUiToProfile(st);
                            st->selProfile = pi;
                            LoadProfileToUi(st);
                        }
                    } else if (code == LBN_DBLCLK && !st->loading) {
                        if (st->selProfile >= 0 &&
                            st->selProfile < static_cast<int>(st->work.profiles.size())) {
                            Profile& p = st->work.profiles[static_cast<size_t>(st->selProfile)];
                            p.enabled = !p.enabled;
                            SetChecked(st->hEnabled, p.enabled);
                            RefreshProfileList(st);
                        }
                    }
                    return 0;
                case IDC_ENABLED:
                    if (!st->loading && st->selProfile >= 0 &&
                        st->selProfile < static_cast<int>(st->work.profiles.size())) {
                        st->work.profiles[static_cast<size_t>(st->selProfile)].enabled =
                            IsChecked(st->hEnabled);
                        RefreshProfileList(st);
                        // A disabled profile's rule can never run, and the status line says
                        // exactly that - so it has to move on the click.
                        if (RefreshAutoPinStatus(st)) {
                            if (st->hHeavy) InvalidateRect(st->hHeavy, nullptr, TRUE);
                            RepaintChrome(hwnd);
                        }
                    }
                    return 0;
                case IDC_PCT:
                    // The threshold appears in the Active sentence and as the meter's tick, so
                    // both track what is being typed rather than what was last applied.
                    if (code == EN_CHANGE && !st->loading) {
                        RefreshAutoPinStatus(st);
                        if (st->hHeavy) InvalidateRect(st->hHeavy, nullptr, TRUE);
                        RepaintChrome(hwnd);
                    }
                    return 0;
                case IDC_ADD:
                case IDC_DUP:
                case IDC_REMOVE:
                case IDC_RENAME:
                    OnProfileButton(st, hwnd, id);
                    return 0;
                case IDC_GAMEPICK: {
                    std::wstring pick = PickRunningProcess(hwnd);
                    if (!pick.empty()) SetWindowTextW(st->hGame, pick.c_str());
                    return 0;
                }
                case IDC_GAMEBROWSE: {
                    std::wstring path = BrowseForExe(hwnd);
                    if (!path.empty()) SetWindowTextW(st->hGame, path.c_str());
                    return 0;
                }
                case IDC_HEAVYPICK: {
                    std::wstring pick = PickRunningProcess(hwnd);
                    if (!pick.empty()) HeavyAppend(st, pick);
                    return 0;
                }
                case IDC_HEAVYADD: {
                    // "Browse..." - pick the executable off disk. A heavy entry is matched on
                    // the BASENAME (ProcessSnapshot::FindBySpec treats a spec containing a
                    // backslash as a full-path match), and a full path would stop matching the
                    // moment the app was reinstalled elsewhere, so only the basename is
                    // stored. This still adds apps that are NOT running right now, which is
                    // what the typed prompt this replaced was for.
                    std::wstring path = BrowseForExe(hwnd);
                    if (path.empty()) return 0;
                    HeavyAppend(st, BaseName(Trim(path)));
                    return 0;
                }
                case IDC_HEAVYREM: {
                    const int row = static_cast<int>(
                        SendMessageW(st->hHeavy, LB_GETCURSEL, 0, 0));
                    if (row < 0) return 0;
                    // An auto-pin row is a readback, not a setting: there is nothing in the
                    // profile to remove, and deleting it would put back a row the next tick
                    // while the user believed they had changed something. Refused silently -
                    // SyncAutoPinRows never leaves such a row selected, so reaching this
                    // needs a keyboard selection between two ticks.
                    if (SendMessageW(st->hHeavy, LB_GETITEMDATA,
                                     static_cast<WPARAM>(row), 0) != kHeavyRowManual) {
                        return 0;
                    }
                    SendMessageW(st->hHeavy, LB_DELETESTRING,
                                 static_cast<WPARAM>(row), 0);
                    // The selection follows the delete, but only within the user's OWN rows.
                    // Deleting the last manual entry used to leave the cursor on whatever
                    // occupied that index, which is now the first auto-pin row - a readback
                    // sitting under a Remove button that would refuse to act on it.
                    const int left = ManualRowCount(st);
                    if (left > 0) {
                        const int next = row < left ? row : left - 1;
                        SendMessageW(st->hHeavy, LB_SETCURSEL,
                                     static_cast<WPARAM>(next), 0);
                    } else {
                        SendMessageW(st->hHeavy, LB_SETCURSEL,
                                     static_cast<WPARAM>(-1), 0);
                    }
                    return 0;
                }
                case IDC_HEAVY:
                    return 0;   // selection only; the buttons above act on it
                case IDC_GAMEMASK:
                case IDC_HEAVYMASK:
                    // Warn at the moment of choosing, not after the user wonders why the
                    // mask did nothing. The choice is never blocked - a parked CCD can
                    // un-park, and the user may be assigning it deliberately.
                    if (code == CBN_SELCHANGE && !st->loading) {
                        RefreshLiveTopology(st);
                        // The heavy mask is the mask the Active sentence names.
                        const bool autoChanged = RefreshAutoPinStatus(st);
                        if (UpdateMaskWarnings(st)) {
                            SettingsLayout(st, hwnd);
                            RedrawSettings(hwnd);
                        } else if (autoChanged) {
                            RepaintChrome(hwnd);
                        }
                    }
                    return 0;
                case IDC_INSPECT:
                    ShowInspectReport(st, hwnd);
                    return 0;
                case IDC_AUTOPIN:
                    SyncAutoPinEnable(st);
                    return 0;
                case IDC_MAPMASK:
                    if (code == CBN_SELCHANGE) {
                        SelectMapMask(st);
                        RepaintChrome(hwnd);   // the stat row names and measures this mask
                    }
                    return 0;
                case IDC_MAPRESET:
                    OnResetMask(st, hwnd);
                    return 0;
                case IDC_STARTUP: {
                    bool on = IsChecked(st->hStartup);
                    st->work.startWithWindows = on;
                    if (!SetStartWithWindows(on)) {
                        MessageBoxW(hwnd,
                                    L"The Run registry value could not be written, so "
                                    L"\"Start with Windows\" was not changed.",
                                    L"Game Optimizer", MB_OK | MB_ICONWARNING);
                        SetChecked(st->hStartup, GetStartWithWindows());
                        st->work.startWithWindows = GetStartWithWindows();
                    }
                    return 0;
                }
                case IDC_APPLY:
                    ApplyChanges(st, hwnd);
                    return 0;
                case IDOK:
                    ApplyChanges(st, hwnd);
                    DestroyWindow(hwnd);
                    return 0;
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
            KillTimer(hwnd, kStatusTimer);
            KillTimer(hwnd, kSponsorFallbackTimer);
            // The browser goes when the window goes. This is rule 1 of webview_host.h: while
            // the app sits in the tray - which is nearly all of its life - there is no
            // WebView2, no user-data folder open and no extra process. Torn down HERE rather
            // than in WM_NCDESTROY because the child windows are still alive at this point.
            if (st != nullptr && st->web != nullptr) {
                WebSponsorDestroy(st->web);
                st->web = nullptr;
                st->webShowing = false;
            }
            return 0;
        case WM_NCDESTROY: {
            if (st) {
                // The two brushes are the only GDI objects this window owns; the fonts come
                // from theme::GetFont's cache and are freed by theme::Shutdown.
                if (st->cardBrush) DeleteObject(st->cardBrush);
                if (st->inputBrush) DeleteObject(st->inputBrush);
                delete st;
            }
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
            if (g_hSettings == hwnd) {
                g_hSettings = nullptr;
                if (g_msgHook) { UnhookWindowsHookEx(g_msgHook); g_msgHook = nullptr; }
            }
            break;
        }
        default:
            break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void RegisterSettingsClass() {
    static bool done = false;
    if (done) return;
    HINSTANCE hInst = GetModuleHandleW(nullptr);
    // Registering the tab bar again is harmless - RegisterClassExW simply fails with
    // ERROR_CLASS_ALREADY_EXISTS - and it means this window does not depend on the host
    // having got there first.
    theme::TabBarRegister(hInst);
    // Same reasoning for the sponsor strip: registering twice is harmless - RegisterClassExW
    // simply fails with ERROR_CLASS_ALREADY_EXISTS - and it means this window does not depend
    // on the host having got there first.
    SponsorRegister(hInst);
    WNDCLASSEXW wc;
    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = SettingsProc;
    wc.hInstance = hInst;
    wc.hIcon = LoadIconW(hInst, MAKEINTRESOURCEW(IDI_APPICON));
    wc.hIconSm = LoadIconW(hInst, MAKEINTRESOURCEW(IDI_APPICON));
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;   // WM_ERASEBKGND paints appBg; no light flash
    wc.lpszClassName = kSettingsClass;
    RegisterClassExW(&wc);
    done = true;
}

// ---------------------------------------------------------------------------
// A tiny modal name prompt, because there is no stock "InputBox" in Win32 and
// pulling in a .rc template for one EDIT is not worth it.
// ---------------------------------------------------------------------------

struct PromptState {
    std::wstring text;
    std::wstring caption;
    HWND hLabel = nullptr, hEdit = nullptr, hOk = nullptr, hCancel = nullptr;
    HFONT font = nullptr;
    int dpi = 96;
    bool ok = false;
    bool done = false;
};

const wchar_t kPromptClass[] = L"GameOptimizerPrompt";

LRESULT CALLBACK PromptProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    PromptState* st = reinterpret_cast<PromptState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg) {
        case WM_NCCREATE: {
            CREATESTRUCTW* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                              reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
            break;
        }
        case WM_CREATE: {
            st = reinterpret_cast<PromptState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
            st->dpi = DpiOf(hwnd);
            st->font = MakeUiFont(st->dpi, false);
            st->hLabel = Mk(hwnd, L"STATIC", st->caption.c_str(), SS_LEFT, -1);
            st->hEdit = Mk(hwnd, L"EDIT", st->text.c_str(), ES_AUTOHSCROLL | WS_TABSTOP,
                           100, WS_EX_CLIENTEDGE);
            st->hOk = Mk(hwnd, L"BUTTON", L"OK", BS_DEFPUSHBUTTON | WS_TABSTOP, IDOK);
            st->hCancel = Mk(hwnd, L"BUTTON", L"Cancel", BS_PUSHBUTTON | WS_TABSTOP, IDCANCEL);
            FontApply fa; fa.f = st->font;
            EnumChildWindows(hwnd, ApplyFontProc, reinterpret_cast<LPARAM>(&fa));

            const int d = st->dpi;
            const int M = MulDiv(12, d, 96), RH = MulDiv(23, d, 96), BW = MulDiv(90, d, 96);
            RECT rc; GetClientRect(hwnd, &rc);
            int cw = rc.right - rc.left;
            MoveWindow(st->hLabel, M, M, cw - 2 * M, MulDiv(18, d, 96), TRUE);
            MoveWindow(st->hEdit, M, M + MulDiv(20, d, 96), cw - 2 * M, RH, TRUE);
            int by = M + MulDiv(20, d, 96) + RH + MulDiv(10, d, 96);
            MoveWindow(st->hCancel, cw - M - BW, by, BW, RH, TRUE);
            MoveWindow(st->hOk, cw - M - 2 * BW - MulDiv(6, d, 96), by, BW, RH, TRUE);
            SendMessageW(st->hEdit, EM_SETSEL, 0, -1);
            SetFocus(st->hEdit);
            return 0;
        }
        case WM_COMMAND: {
            if (!st) break;
            if (LOWORD(wp) == IDOK) {
                st->text = Trim(GetText(st->hEdit));
                st->ok = true;
                st->done = true;
                return 0;
            }
            if (LOWORD(wp) == IDCANCEL) { st->ok = false; st->done = true; return 0; }
            break;
        }
        case WM_CTLCOLORSTATIC: {
            SetBkMode(reinterpret_cast<HDC>(wp), TRANSPARENT);
            return reinterpret_cast<LRESULT>(GetSysColorBrush(COLOR_BTNFACE));
        }
        case WM_CLOSE:
            if (st) { st->ok = false; st->done = true; }
            return 0;
        case WM_NCDESTROY:
            if (st && st->font) { DeleteObject(st->font); st->font = nullptr; }
            break;
        default:
            break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// Runs a nested modal loop for `hwnd` until `*done` becomes true.
void RunModalLoop(HWND hwnd, HWND owner, const bool* done) {
    if (owner) EnableWindow(owner, FALSE);
    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);
    MSG msg;
    while (!*done && IsWindow(hwnd)) {
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
}

bool PromptName(HWND owner, const wchar_t* prompt, std::wstring& io) {
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc;
        ZeroMemory(&wc, sizeof(wc));
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = PromptProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
        wc.lpszClassName = kPromptClass;
        RegisterClassExW(&wc);
        registered = true;
    }
    PromptState st;
    st.text = io;
    st.caption = prompt;

    int dpi = DpiOf(owner ? owner : GetDesktopWindow());
    RECT want = { 0, 0, MulDiv(360, dpi, 96), MulDiv(112, dpi, 96) };
    AdjustWindowRectEx(&want, WS_CAPTION | WS_SYSMENU, FALSE, WS_EX_DLGMODALFRAME);
    int w = want.right - want.left, h = want.bottom - want.top;
    RECT orc = { 0, 0, 0, 0 };
    if (owner) GetWindowRect(owner, &orc); else SystemParametersInfoW(SPI_GETWORKAREA, 0, &orc, 0);
    int x = orc.left + ((orc.right - orc.left) - w) / 2;
    int y = orc.top + ((orc.bottom - orc.top) - h) / 2;

    HWND hwnd = CreateWindowExW(WS_EX_DLGMODALFRAME, kPromptClass, L"Game Optimizer",
                                WS_POPUP | WS_CAPTION | WS_SYSMENU, x, y, w, h,
                                owner, nullptr, GetModuleHandleW(nullptr), &st);
    if (!hwnd) return false;
    RunModalLoop(hwnd, owner, &st.done);
    if (st.ok) io = st.text;
    return st.ok;
}

// ---------------------------------------------------------------------------
// Inspect
//
// WHAT THIS WINDOW MAY AND MAY NOT SAY. There are three outcomes for a CPU Set assignment:
// applied, failed, and accepted-then-ignored. MEASURED on this machine
// (docs\spec\04-measurements.md 2.3 / 2.4) the setter returned TRUE, the getter echoed all
// 16 assigned ids, and the threads ran on the other CCD at full speed. So a matching
// read-back is NOT evidence that anything took effect, and no string below may say
// "working", "active" or "verified".
//
// Observing placement would mean sampling GetCurrentProcessorNumberEx from inside the
// target process. This app never injects anything and uses no undocumented calls, so it
// cannot do that and does not pretend to. What it reports instead is the assignment, plus
// the two OBSERVABLE hazards that are the documented routes into silent-ignore: assigned
// processors that are parked, and a restrictive affinity mask, which Microsoft documents as
// respected above a conflicting CPU Set assignment.
// ---------------------------------------------------------------------------

const wchar_t kInspectClass[] = L"GameOptimizerInspect";

struct InspectState {
    std::wstring body;
    HWND hIntro = nullptr, hText = nullptr, hClose = nullptr;
    HFONT font = nullptr;
    int dpi = 96;
    bool done = false;
};

std::wstring HexMask(ULONG_PTR v) {
    wchar_t buf[32];
    swprintf_s(buf, 32, L"0x%llX", static_cast<unsigned long long>(v));
    return std::wstring(buf);
}

// Builds the whole report. Pure string work over an engine status snapshot plus one
// InspectProcess call per governed process.
std::wstring BuildInspectReport(SettingsState* st) {
    std::wstring out;

    if (!st->engine) return L"The engine is not running, so nothing is being governed.";

    EngineStatus s = st->engine->GetStatus();
    if (s.governed.empty()) {
        return L"No processes are being governed right now, so there is nothing to inspect. "
               L"A profile has to be enabled and its game running before Game Optimizer "
               L"assigns anything.";
    }

    // The EXPECTED ids come from the config the engine was last given (*out), not from the
    // working copy in the editor - otherwise an unsaved edit would be reported as a
    // mismatch against an assignment that was never made from it.
    const Config& applied = st->out ? *st->out : st->work;

    for (size_t i = 0; i < s.governed.size(); ++i) {
        const GovernedProcess& g = s.governed[i];

        std::wstring nm = g.name.empty() ? std::wstring(L"(unnamed)") : g.name;
        out += nm + L"  (pid " + std::to_wstring(static_cast<unsigned long>(g.pid)) + L")\r\n";

        const Mask* m = applied.FindMask(g.maskName);
        if (g.maskName.empty()) {
            out += L"    Assigned mask: none - this process is being cleared.\r\n\r\n";
            continue;
        }
        if (m == nullptr) {
            out += L"    Assigned mask: \"" + g.maskName +
                   L"\", which is not in the saved configuration. Nothing was applied.\r\n\r\n";
            continue;
        }

        InspectionResult r = InspectProcess(g.pid, m->ids, st->live);

        out += L"    Assigned mask: \"" + g.maskName + L"\" (" +
               std::to_wstring(m->ids.size()) + L" processors)\r\n";

        if (g.blocked) {
            out += L"    Game Optimizer could not apply this mask - the process is elevated or "
                   L"protected. Nothing was assigned to it.\r\n";
        }

        if (!r.opened) {
            out += L"    Read-back: this process could not be opened for querying (access "
                   L"denied, or it has exited).\r\n";
        } else if (r.assignmentMatches) {
            out += L"    Read-back: Windows reports the same " +
                   std::to_wstring(r.actualIds.size()) +
                   L" ids Game Optimizer assigned. That confirms the assignment was STORED. "
                   L"It is not evidence that the scheduler is honouring it.\r\n";
        } else if (r.actualIds.empty()) {
            out += L"    Read-back: Windows reports no default CPU sets for this process - "
                   L"nothing is assigned to it right now.\r\n";
        } else {
            out += L"    Read-back: Windows reports " + std::to_wstring(r.actualIds.size()) +
                   L" ids, which are not the ones Game Optimizer assigned. Something else has "
                   L"changed this process's CPU sets.\r\n";
        }

        if (r.totalInMask > 0 && r.parkedInMask == r.totalInMask) {
            out += L"    Parked: all " + std::to_wstring(r.totalInMask) +
                   L" processors in this mask are currently parked - Windows may ignore "
                   L"this assignment.\r\n";
        } else if (r.parkedInMask > 0) {
            out += L"    Parked: " + std::to_wstring(r.parkedInMask) + L" of " +
                   std::to_wstring(r.totalInMask) +
                   L" processors in this mask are currently parked - Windows may ignore "
                   L"this assignment.\r\n";
        } else {
            out += L"    Parked: none of the " + std::to_wstring(r.totalInMask) +
                   L" processors in this mask are parked right now.\r\n";
        }

        if (!r.opened) {
            out += L"    Affinity mask: not readable for this process.\r\n";
        } else if (r.hasRestrictiveAffinity) {
            out += L"    Affinity mask: something else has restricted this process to " +
                   HexMask(r.processAffinity) + L" of the machine's " +
                   HexMask(r.systemAffinity) +
                   L". Windows respects an affinity mask above any conflicting CPU Set "
                   L"assignment, so this can defeat the mask entirely.\r\n";
        } else {
            out += L"    Affinity mask: none set - the process may use the whole machine.\r\n";
        }

        out += L"\r\n";
    }

    return out;
}

void InspectLayout(InspectState* st, HWND hwnd) {
    const int d = st->dpi;
    const int M = MulDiv(12, d, 96), RH = MulDiv(23, d, 96), BW = MulDiv(90, d, 96);
    const int introH = MulDiv(92, d, 96);
    RECT rc; GetClientRect(hwnd, &rc);
    const int cw = rc.right - rc.left, ch = rc.bottom - rc.top;
    MoveWindow(st->hIntro, M, M, cw - 2 * M, introH, TRUE);
    int textTop = M + introH + MulDiv(6, d, 96);
    int textH = ch - textTop - M - RH - MulDiv(10, d, 96);
    if (textH < MulDiv(60, d, 96)) textH = MulDiv(60, d, 96);
    MoveWindow(st->hText, M, textTop, cw - 2 * M, textH, TRUE);
    MoveWindow(st->hClose, cw - M - BW, ch - M - RH, BW, RH, TRUE);
}

LRESULT CALLBACK InspectProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    InspectState* st = reinterpret_cast<InspectState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg) {
        case WM_NCCREATE: {
            CREATESTRUCTW* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                              reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
            break;
        }
        case WM_CREATE: {
            st = reinterpret_cast<InspectState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
            st->dpi = DpiOf(hwnd);
            st->font = MakeUiFont(st->dpi, false);
            st->hIntro = Mk(hwnd, L"STATIC",
                L"This shows what Game Optimizer assigned and what Windows reports back. It "
                L"cannot show where a process is actually running: reading that would mean "
                L"running code inside the process, and this app never injects anything. A "
                L"matching read-back is not proof the assignment took effect - measured on "
                L"this machine, Windows accepted a mask of 16 parked processors, echoed all "
                L"16 back, and ran the threads on the other CCD anyway. The parked and "
                L"affinity lines are the two things that can prevent a mask taking effect.",
                SS_LEFT, -1);
            st->hText = Mk(hwnd, L"EDIT", st->body.c_str(),
                           ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | WS_VSCROLL |
                               WS_TABSTOP,
                           200, WS_EX_CLIENTEDGE);
            st->hClose = Mk(hwnd, L"BUTTON", L"Close", BS_DEFPUSHBUTTON | WS_TABSTOP, IDOK);
            FontApply fa; fa.f = st->font;
            EnumChildWindows(hwnd, ApplyFontProc, reinterpret_cast<LPARAM>(&fa));
            InspectLayout(st, hwnd);
            SetFocus(st->hClose);
            return 0;
        }
        case WM_SIZE:
            if (st) InspectLayout(st, hwnd);
            return 0;
        case WM_COMMAND:
            if (st && (LOWORD(wp) == IDOK || LOWORD(wp) == IDCANCEL)) {
                st->done = true;
                return 0;
            }
            break;
        case WM_CTLCOLORSTATIC: {
            SetBkMode(reinterpret_cast<HDC>(wp), TRANSPARENT);
            return reinterpret_cast<LRESULT>(GetSysColorBrush(COLOR_BTNFACE));
        }
        case WM_CLOSE:
            if (st) st->done = true;
            return 0;
        case WM_NCDESTROY:
            if (st && st->font) { DeleteObject(st->font); st->font = nullptr; }
            break;
        default:
            break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void ShowInspectReport(SettingsState* st, HWND owner) {
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc;
        ZeroMemory(&wc, sizeof(wc));
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = InspectProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
        wc.lpszClassName = kInspectClass;
        RegisterClassExW(&wc);
        registered = true;
    }

    // Parked state is read at the moment the window opens; the report is a snapshot and does
    // not update behind the user's back.
    RefreshLiveTopology(st);

    InspectState is;
    is.body = BuildInspectReport(st);

    int dpi = DpiOf(owner ? owner : GetDesktopWindow());
    RECT want = { 0, 0, MulDiv(620, dpi, 96), MulDiv(480, dpi, 96) };
    AdjustWindowRectEx(&want, WS_CAPTION | WS_SYSMENU | WS_SIZEBOX, FALSE,
                       WS_EX_DLGMODALFRAME);
    int w = want.right - want.left, h = want.bottom - want.top;
    RECT orc = { 0, 0, 0, 0 };
    if (owner) GetWindowRect(owner, &orc);
    else SystemParametersInfoW(SPI_GETWORKAREA, 0, &orc, 0);
    int x = orc.left + ((orc.right - orc.left) - w) / 2;
    int y = orc.top + ((orc.bottom - orc.top) - h) / 2;

    HWND hwnd = CreateWindowExW(WS_EX_DLGMODALFRAME, kInspectClass,
                                L"What Game Optimizer assigned",
                                WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_SIZEBOX,
                                x, y, w, h, owner, nullptr,
                                GetModuleHandleW(nullptr), &is);
    if (!hwnd) return;
    RunModalLoop(hwnd, owner, &is.done);
}

}  // namespace

// ---------------------------------------------------------------------------
// Public entry points
// ---------------------------------------------------------------------------

std::wstring PickRunningProcess(HWND owner) {
    EnsureCommonControls();
    RegisterPickerClass();

    PickerState st;
    int dpi = DpiOf(owner ? owner : GetDesktopWindow());
    RECT want = { 0, 0, MulDiv(620, dpi, 96), MulDiv(460, dpi, 96) };
    AdjustWindowRectEx(&want, WS_CAPTION | WS_SYSMENU | WS_SIZEBOX, FALSE,
                       WS_EX_DLGMODALFRAME);
    int w = want.right - want.left, h = want.bottom - want.top;
    RECT orc = { 0, 0, 0, 0 };
    if (owner) GetWindowRect(owner, &orc);
    else SystemParametersInfoW(SPI_GETWORKAREA, 0, &orc, 0);
    int x = orc.left + ((orc.right - orc.left) - w) / 2;
    int y = orc.top + ((orc.bottom - orc.top) - h) / 2;

    HWND hwnd = CreateWindowExW(WS_EX_DLGMODALFRAME, kPickerClass,
                                L"Pick a running process",
                                WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_SIZEBOX,
                                x, y, w, h, owner, nullptr,
                                GetModuleHandleW(nullptr), &st);
    if (!hwnd) return std::wstring();
    RunModalLoop(hwnd, owner, &st.done);
    return st.result;
}

std::wstring BrowseForExe(HWND owner) {
    wchar_t buf[1024];
    buf[0] = L'\0';
    OPENFILENAMEW ofn;
    ZeroMemory(&ofn, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = owner;
    ofn.lpstrFilter = L"Executables (*.exe)\0*.exe\0All files (*.*)\0*.*\0";
    ofn.nFilterIndex = 1;
    ofn.lpstrFile = buf;
    ofn.nMaxFile = static_cast<DWORD>(sizeof(buf) / sizeof(buf[0]));
    ofn.lpstrTitle = L"Select a game executable";
    ofn.lpstrDefExt = L"exe";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | OFN_EXPLORER;
    if (!GetOpenFileNameW(&ofn)) return std::wstring();
    return std::wstring(buf);
}

void ShowSettings(HWND owner, Config& cfg, const Topology& topo, Engine& engine) {
    if (g_hSettings && IsWindow(g_hSettings)) {
        if (IsIconic(g_hSettings)) ShowWindow(g_hSettings, SW_RESTORE);
        SetForegroundWindow(g_hSettings);
        return;
    }
    EnsureCommonControls();
    RegisterSettingsClass();

    SettingsState* st = new SettingsState();
    st->out = &cfg;
    st->topo = &topo;
    st->engine = &engine;
    st->work = cfg;
    st->baseline = cfg;
    st->env = ProbeEnvironment();
    st->selProfile = st->work.profiles.empty() ? -1 : 0;

    int dpi = DpiOf(owner ? owner : GetDesktopWindow());
    RECT work = { 0, 0, 0, 0 };
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    // The rail's 208dip of width came back when the menu moved to the top, and the Profiles
    // page spends it on a second column. The default stays generous so neither column has to
    // start at its floor.
    int wantW = MulDiv(1000, dpi, 96);
    // The default should clear the minimum rather than open ON it - a window that starts at
    // its floor has every page at its tightest. 740dip is the page's own comfortable height
    // (the auto-pin card grew a status row this round).
    //
    // THE SPONSOR BAND IS ADDED FROM A MEASURE, NOT BAKED INTO THAT NUMBER, so this figure
    // stays right when either rendering changes shape.
    //
    // It asks for the WEB PANEL's height even though nothing has been created yet and the
    // runtime may well be missing. That is deliberate and it is the safe direction of the two:
    // the window is about to TRY WebView2, and a window that opens too short for the panel
    // shows it clipped on first paint. If the attempt fails, SponsorWebReady re-lays out with
    // the GDI row's much smaller band and the page content simply gains the space back - the
    // window is a little taller than it needed to be and nothing is hidden. The reverse
    // mistake cannot be repaired the same way.
    int wantH = MulDiv(740, dpi, 96);
    {
        const SIZE sp = WebSponsorMinSize(dpi);
        if (sp.cx > 0 && sp.cy > 0)
            wantH += static_cast<int>(sp.cy) + theme::Dp(theme::metric::kGap, dpi);
    }
    RECT want = { 0, 0, wantW, wantH };
    AdjustWindowRectEx(&want, WS_OVERLAPPEDWINDOW, FALSE, 0);
    int w = want.right - want.left;
    int h = want.bottom - want.top;
    int availH = work.bottom - work.top;
    if (availH > 0 && h > availH) h = availH;
    int x = work.left + ((work.right - work.left) - w) / 2;
    int y = work.top + ((work.bottom - work.top) - h) / 2;
    if (x < work.left) x = work.left;
    if (y < work.top) y = work.top;

    // No WS_VSCROLL: the sidebar replaced the scrolling column outright.
    // An owned window is otherwise excluded from the taskbar and Alt+Tab. Keep the owner
    // deliberately: it keeps Settings above the tray window and preserves correct closing.
    HWND hwnd = CreateWindowExW(WS_EX_APPWINDOW, kSettingsClass, L"Game Optimizer - Settings",
                                WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                                x, y, w, h, owner, nullptr,
                                GetModuleHandleW(nullptr), st);
    if (!hwnd) {
        delete st;
        return;
    }
    g_hSettings = hwnd;
    g_msgHook = SetWindowsHookExW(WH_GETMESSAGE, SettingsMsgHook, nullptr,
                                  GetCurrentThreadId());
    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);
    SetForegroundWindow(hwnd);
}

}  // namespace cd
