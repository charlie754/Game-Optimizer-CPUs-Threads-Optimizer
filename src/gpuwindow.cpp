// Game Optimizer - the GPU Assignment panel. See gpuwindow.h for what it is for.
//
// A CHILD OF THE SETTINGS WINDOW SINCE v0.5.6, NOT A WINDOW OF ITS OWN. It used to be a top-level
// window with a modal loop that disabled Settings; it now sits hidden on the "GPU Assignment" tab
// and Settings shows it. So it has no loop, no title bar and no minimum size: the keyboard comes
// from Settings' own IsDialogMessageW hook, which steps into these controls only because the panel
// carries WS_EX_CONTROLPARENT, and the size comes from the card Settings puts it on. Mk and DpiOf
// stay file-local here exactly as they already are in settings.cpp, envwarning.cpp, firstrun.cpp,
// gameprompt.cpp and irqwindow.cpp.
//
// WHAT THIS PANEL REFUSES TO DO, each because the alternative is a silent wrong write:
//
//   * It does not act on a plan it cannot decide. PlanGpuIsolation returns empty keys and a
//     REASON for one GPU, for no display, and for two cards the registry cannot tell apart. In
//     every one of those the action buttons are dead and the reason is on screen.
//   * It does not pin an application whose full path it could not read. The preference is keyed
//     by full path; a basename is not a usable value name. Those are counted in one line rather
//     than listed as rows nobody can act on.
//   * It does not sweep Windows. Binaries under the Windows directory and anything on the
//     exclusion list are listed and can be ticked by hand, but the bulk action never ticks them.
//   * It does not guess from a partial read. While Windows' GPU preferences could not all be read,
//     the bulk action is off and refused, and a refresh keeps no tick: a main-GPU pin the read left
//     out cannot be told from no pin (AutoAssignAllowed, gpu_rows.h). Ticks by hand still work.
//   * It does not write without asking, and it does not report "done". Apply confirms first, then
//     counts what landed and what did not.
//   * It does not edit what it cannot put back. A value it cannot read, or one that is not plain
//     "name=value;" fields, is left exactly as it is and named in the result. Every other change is
//     added to a restore file the moment it is made, and read back.
//
// EVERY ONE OF THE LAST THREE WAS FOUND BY RUNNING THE FEATURE'S DATA PATH AGAINST THE OPERATOR'S
// REAL MACHINE BEFORE THIS WINDOW EXISTED - see gpu_policy.h.
#define WIN32_LEAN_AND_MEAN
#include "gpuwindow.h"

#include <algorithm>
#include <functional>
#include <map>
#include <new>
#include <set>
#include <string>
#include <vector>

#include "config.h"
#include "gpu_cuda.h"
#include "gpu_edit.h"
#include "gpu_policy.h"
#include "gpu_pref.h"
#include "gpu_rows.h"
#include "procwatch.h"
#include "theme.h"
#include "util.h"

namespace cd {
namespace {

const wchar_t kGpuClass[] = L"GameOptimizerGpu";

enum : int {
    IDC_GPU_LIST = 2100,
    IDC_GPU_TARGET,
    IDC_GPU_BULK,
    IDC_GPU_SELALL,
    IDC_GPU_CLEARSEL,
    IDC_GPU_REMOVE,
    IDC_GPU_APPLY,
    IDC_GPU_CLOSE,
    IDC_GPU_PATH,
};

// Posted by ActivateGpuPanel when an earlier change left its record behind and the panel has not yet said so.
const UINT WM_GPU_UNFINISHED = WM_APP + 1;

// Every message box this panel shows carries this title. Until v0.5.6 it named the separate window this panel replaced.
const wchar_t kGpuTitle[] = L"Game Optimizer - GPU Assignment";

// The bulk button's caption, in one place because LayoutGpu measures it. Renamed in v0.5.6 (operator request).
const wchar_t kBulkCaption[] = L"Auto assign GPU for Gaming";

int DpiOf(HWND h) {
    UINT d = h ? GetDpiForWindow(h) : 0;
    if (d == 0) d = GetDpiForSystem();
    if (d == 0) d = 96;
    return static_cast<int>(d);
}

HWND Mk(HWND parent, const wchar_t* cls, const wchar_t* text, DWORD style, int id) {
    return CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 10, 10, parent,
                           reinterpret_cast<HMENU>(static_cast<UINT_PTR>(id)),
                           reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(parent, GWLP_HINSTANCE)),
                           nullptr);
}

// One list entry. GpuRow carries what the pure logic needs; the rest is what only this window
// knows, kept beside it rather than pushed into gpu_rows.h because they are presentation facts.
struct Row {
    GpuRow r;
    bool autoSelected = false; // a bulk tick must still satisfy the policy when Apply prepares it
    bool system = false;       // a Windows image or on the exclusion list: never bulk-selected
    bool orphaned = false;     // lost its assignment when the app updated into a new folder
    std::wstring note;         // the reason, when there is one
    std::wstring where;        // parent folders, shown only when another row shares the name
    bool listed = false;       // really in the list box: only a row the user can see is ticked or written
    // Its GPU choice as this list read it (GpuChoiceText): what Apply and Remove compare with a fresh read before writing
    // (PrepareVerdict, gpu_edit.h). Set wherever assignedKey is set from a read, so the two describe one read.
    std::wstring listedChoice;
};

// EVERY UNTICK GOES THROUGH HERE, AND THE TICK'S ORIGIN GOES WITH IT (v0.5.7). `autoSelected` says Auto assign made a tick,
// and Apply re-checks such a tick against Auto assign's rule (PrepareEdits, and inside the guarded write). Thirteen sites
// cleared only `selected`, so an unticked row could keep saying Auto assign ticked it; one rule for all of them means no
// site has to remember the second field.
void Untick(Row& row) {
    row.r.selected = false;
    row.autoSelected = false;
}

// AND EVERY TICK BY HAND GOES THROUGH HERE, the mirror of Untick (v0.5.9). `autoSelected` must be CLEARED by a hand
// tick, not merely left alone: that is what tells Apply's two v0.5.7 re-checks this tick is the user's own override and
// not Auto assign's. The pair was written out verbatim at the Space and double-click sites, and Select all would have
// been a third copy - one rule for all three means no site has to remember the second field. Auto assign does NOT use
// this: DoBulk sets `autoSelected` true on purpose.
void Tick(Row& row) {
    row.r.selected = true;
    row.autoSelected = false;   // a tick by hand, never Auto assign's
}

struct GpuState {
    int dpi = 96;
    HFONT font = nullptr;
    HBRUSH cardBrush = nullptr;             // the Settings card this panel sits on; ours, deleted in WM_NCDESTROY

    std::vector<GpuAdapter> adapters;
    GpuPlan plan;
    std::vector<std::wstring> candidates;   // every GPU key the picker offers: the main GPU first, then the background GPUs
    std::wstring targetKey;                 // the one the user has picked, or the default
    std::vector<Row> rows;
    size_t unreadable = 0;                  // processes whose path could not be read
    bool prefsComplete = true;              // false: the registry walk could not see every preference
    // GpuChoicePairs of that walk: the other versions Auto assign checks a row against (AnotherVersionMayHoldMainGpuPin)
    std::vector<std::pair<std::wstring, std::wstring> > prefPairs;
    // "Also set which GPU CUDA uses" on the Setting tab, copied from the config on every activation
    // (v0.5.8). A bool, not the config itself: the panel borrows `cfg` for the length of one call and
    // stores nothing that could go stale (gpuwindow.h).
    bool cudaEnabled = true;
    bool controlsBroken = false;            // a control failed to create - decided once, in WM_CREATE
    bool broken = false;                    // a control this panel needs is missing or unusable: nothing is offered
    std::vector<UnfinishedRecord> unfinished;   // .pending records earlier changes left behind, oldest first
    std::vector<std::wstring> unfinishedShown;  // the .pending paths the notice last showed, lower-cased and sorted
    PanelLifetime life;                         // message boxes open for this panel, and whether it is destroyed (gpu_edit.h)

    HWND hPlan = nullptr, hTargetLbl = nullptr, hTarget = nullptr, hStatus = nullptr;
    HWND hList = nullptr, hPath = nullptr;
    HWND hBulk = nullptr, hSelAll = nullptr, hClearSel = nullptr, hRemove = nullptr, hApply = nullptr, hClose = nullptr;
};

GpuState* StateOf(HWND h) {
    return reinterpret_cast<GpuState*>(GetWindowLongPtrW(h, GWLP_USERDATA));
}

// THE ONE DELETE of a panel's state: from WM_NCDESTROY when no message box is open, otherwise from the return of the last
// one (PanelMessageBox). The font is theme-cached, never ours; the card brush is ours.
void FreeState(GpuState* st) {
    if (st->cardBrush) DeleteObject(st->cardBrush);
    delete st;
}

// EVERY MESSAGE BOX THIS PANEL SHOWS GOES THROUGH HERE, AND FOCUS COMES BACK TO WHERE IT WAS AFTER IT (stage review, v0.5.6).
// When a box closes, Windows activates Settings again and focus lands on the Settings frame itself: the next Enter then
// reached Settings' OK - save config.ini and close - and Space and the arrows reached nothing. So the control that had
// focus before the box gets it back while it is still a visible, enabled control of Settings (Apply is disabled once its
// ticks are written), and otherwise the list, while it is visible. ANY CONTROL OF SETTINGS, NOT ONLY THE PANEL'S (fix
// round, v0.5.6): the unfinished-change notice is posted by ActivateGpuPanel while the user arrows along the tab bar,
// which SwitchPage deliberately leaves focused, and sending focus to the list then turned the next Left/Right into a list
// move. The frame is not its own child, so a box raised with focus on the frame still hands focus to the list. The box
// is owned by Settings, the panel's top-level window - the window a box disables in any case.
//
// 🔴 FALSE MEANS THE PANEL WAS DESTROYED WHILE THE BOX WAS OPEN, AND THE CALLER RETURNS AT ONCE (Council review, v0.5.6).
// Tray Exit closes Settings while a box is open. `st` is the pointer the caller took before the box, and PanelLifetime
// (gpu_edit.h) keeps it valid across the box: WM_NCDESTROY only detaches the state while a box is open, and the return
// that finds it detached frees it - or leaves that to an outer box still open. So nothing here, and nothing after a false,
// reads the panel's handle, its focus or its data once it is detached. `*answer` (optional) receives MessageBoxW's answer,
// 0 when the box could not be shown.
bool PanelMessageBox(GpuState* st, HWND panel, const wchar_t* text, UINT type, int* answer = nullptr) {
    const HWND before = GetFocus();
    const HWND root = GetAncestor(panel, GA_ROOT);
    EnterModal(st->life);
    const int a = MessageBoxW(root ? root : panel, text, kGpuTitle, type);
    if (answer) *answer = a;
    if (LeaveModal(st->life)) {
        FreeState(st);
        return false;
    }
    if (st->life.detached) return false;   // an outer box is still open, and its return frees the state
    if (before && root && IsChild(root, before) && IsWindowVisible(before) && IsWindowEnabled(before)) SetFocus(before);
    else if (st->hList && IsWindowVisible(st->hList)) SetFocus(st->hList);
    return true;
}

std::wstring NameForKey(const std::vector<GpuAdapter>& adapters, const std::wstring& key) {
    if (key == WindowsPowerSavingKey()) return L"Windows: power saving";
    if (key == WindowsHighPerformanceKey()) return L"Windows: high performance";
    if (key == UnreadableChoiceKey()) return L"could not be read";
    for (size_t i = 0; i < adapters.size(); ++i) {
        if (adapters[i].adapterKey == key) return adapters[i].name;
    }
    return std::wstring();
}

// ---------------------------------------------------------------------------
// Building the list
// ---------------------------------------------------------------------------

// ONE ROW PER DISTINCT FULL PATH, NOT PER PID. Twenty renderer processes of one browser share a
// path and are one preference; a per-pid list would write the same value twenty times and make
// the pending-change count meaningless. Two installs of one app at different paths are correctly
// two rows, because the preference is per path.
std::vector<Row> BuildRows(const Config& cfg, const ProcessSnapshot& snap,
                           const std::vector<GpuPreferenceEntry>& entries,
                           size_t& unreadableOut) {
    // Each value by its path. Its choiceKey is empty whenever GpuChoicePairs leaves it out, so a row's assignedKey agrees
    // with every reader of those pairs; its choiceText is the listed choice Apply and Remove compare with later.
    std::map<std::wstring, const GpuPreferenceEntry*> values;
    for (size_t i = 0; i < entries.size(); ++i) values[ToLower(entries[i].path)] = &entries[i];

    // THE OPERATOR'S ONE EXCLUSION FROM THE BULK ACTION: an application that is already a game in
    // one of their profiles. Profile::game may be a full path or a basename, so both reduce to a
    // basename - the same reduction main.cpp makes when it decides whether to offer a prompt.
    std::set<std::wstring> profileGames;
    for (size_t i = 0; i < cfg.profiles.size(); ++i) {
        const std::wstring g = Trim(cfg.profiles[i].game);
        if (!g.empty()) profileGames.insert(ToLower(BaseName(g)));
    }

    const DWORD self = GetCurrentProcessId();
    std::map<std::wstring, Row> byPath;
    std::set<std::wstring> unreadableNames;

    const std::map<DWORD, ProcInfo>& all = snap.All();
    for (std::map<DWORD, ProcInfo>::const_iterator it = all.begin(); it != all.end(); ++it) {
        const ProcInfo& pi = it->second;
        if (pi.pid == self || pi.pid == 0 || pi.pid == 4 || pi.name.empty()) continue;

        if (pi.fullPath.empty()) {
            // COUNTED, NOT LISTED. [M] 85 of 254 processes on the operator's machine have a path
            // this unelevated app cannot read. A row per one of them is a wall of greyed entries
            // nobody can act on; one honest sentence says the same thing and hides nothing.
            unreadableNames.insert(ToLower(pi.name));
            continue;
        }

        const std::wstring key = ToLower(pi.fullPath);
        if (byPath.find(key) != byPath.end()) continue;   // another pid of the same binary

        Row row;
        row.r.exeName = pi.name;
        row.r.exePath = pi.fullPath;
        row.r.isProfileGame = profileGames.count(ToLower(pi.name)) != 0;
        // THREE SOURCES, AND THE THIRD IS THE ONE THE SCREENSHOT PROVED NECESSARY. [M] On the
        // operator's machine the bulk action ticked amdow.exe and AMDRSSrcExt.exe - AMD driver
        // components - because their config.ini carries the exclusion list as it was when that
        // config was written, and those two were added to the shipped defaults afterwards.
        row.system = IsWindowsImagePath(pi.fullPath) || cfg.IsExcluded(pi.name) ||
                     IsDefaultExcluded(pi.name);
        const std::map<std::wstring, const GpuPreferenceEntry*>::const_iterator a = values.find(key);
        if (a != values.end()) {
            row.r.assignedKey = a->second->choiceKey;
            row.listedChoice = a->second->choiceText;
        }
        byPath[key] = row;
    }
    unreadableOut = unreadableNames.size();

    std::vector<Row> out;
    for (std::map<std::wstring, Row>::const_iterator it = byPath.begin(); it != byPath.end(); ++it)
        out.push_back(it->second);

    // TWO ROWS, ONE FILE NAME: SHOW WHERE EACH LIVES. The preference is per path, so two installs
    // are correctly two rows - but claude.exe (Claude Desktop) and claude.exe (Claude Code) drawn
    // as identical lines ask the user to assign a GPU to a program they cannot identify. Only
    // colliding names get folders - see DisambiguatingFolders - and the full path of the row under
    // the cursor is always shown below the list.
    std::vector<std::pair<std::wstring, std::wstring> > namePath;
    for (size_t i = 0; i < out.size(); ++i) namePath.push_back(std::make_pair(out[i].r.exeName, out[i].r.exePath));
    const std::vector<std::wstring> where = DisambiguatingFolders(namePath);
    for (size_t i = 0; i < out.size(); ++i) out[i].where = where[i];

    // Applications first, then Windows and excluded binaries, each group in name order - so the
    // rows a user is likely to want are at the top and the list does not jump between openings.
    std::sort(out.begin(), out.end(), [](const Row& a, const Row& b) {
        if (a.system != b.system) return !a.system;
        const std::wstring la = ToLower(a.r.exeName), lb = ToLower(b.r.exeName);
        if (la != lb) return la < lb;
        return ToLower(a.r.exePath) < ToLower(b.r.exePath);
    });
    return out;
}

void MarkOrphans(GpuState* st, const std::vector<std::pair<std::wstring, std::wstring> >& reg) {
    std::vector<std::wstring> runningPaths;
    for (size_t i = 0; i < st->rows.size(); ++i)
        if (!st->rows[i].r.exePath.empty()) runningPaths.push_back(st->rows[i].r.exePath);

    const std::vector<OrphanedAssignment> orphans = FindOrphanedAssignments(
        reg, runningPaths,
        [](const std::wstring& p) {
            // "Missing" means Windows SAID missing. Access denied, an offline share or any other
            // failure is no proof the old version is gone, so it is never reported as an orphan.
            if (GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES) return true;
            const DWORD e = GetLastError();
            return e != ERROR_FILE_NOT_FOUND && e != ERROR_PATH_NOT_FOUND;
        });

    for (size_t o = 0; o < orphans.size(); ++o) {
        for (size_t i = 0; i < st->rows.size(); ++i) {
            Row& row = st->rows[i];
            if (row.orphaned || !WcsIcmp(row.r.exePath, orphans[o].livePath)) continue;
            row.orphaned = true;
            // Carried on the pure row, so SelectForBackground can leave a lost MAIN-GPU pin where the user put it.
            row.r.lostKey = orphans[o].lostKey;
            const std::wstring name = NameForKey(st->adapters, orphans[o].lostKey);
            row.note = L"lost its GPU when it updated"
                     + (name.empty() ? std::wstring() : L" - was on " + name);
            break;
        }
    }
}

size_t OrphanCount(const GpuState* st) {
    size_t n = 0;
    for (size_t i = 0; i < st->rows.size(); ++i) if (st->rows[i].orphaned) ++n;
    return n;
}

// True while the picker's target is the main GPU - the card games run on. "Auto assign GPU for Gaming" is off then
// (founder decision, v0.5.6): it moves background apps OFF that card, never onto it.
bool MainGpuTargeted(const GpuState* st) {
    return IsMainGpuKey(st->plan, st->targetKey);
}

// What "Auto assign GPU for Gaming" would tick: a listed row that is not Windows or excluded, and that
// SelectForAutoAssign itself would select - so not a profile game, not already on the target, not pinned to the
// main GPU (decision 10), not an application another version of which may hold such a pin
// (AnotherVersionMayHoldMainGpuPin), and nothing at all while the main GPU is the target or Windows' GPU preferences
// could not all be read (AutoAssignAllowed, Council round 2).
//
// 🔴 IT ASKS THE PURE SELECTOR, ROW BY ROW, RATHER THAN KEEPING A COPY OF ITS RULES. v0.5.5 kept a copy here that
// compared only against the target; once the picker offered the main GPU and the selector learned to skip main-GPU
// pins, that copy would have lit the button and counted rows DoBulk then refused to tick.
bool IsMovable(const GpuState* st, const Row& r) {
    if (st->targetKey.empty() || !r.listed || r.system) return false;
    return SelectForAutoAssign(std::vector<GpuRow>(1, r.r), st->targetKey, st->plan.gameKey, st->prefsComplete,
                               st->prefPairs)[0].selected;
}

size_t MovableCount(const GpuState* st) {
    size_t n = 0;
    for (size_t i = 0; i < st->rows.size(); ++i) if (IsMovable(st, st->rows[i])) ++n;
    return n;
}

// What "Select all" ticks: gpu_rows.h's SelectAllTicks, asked row by row rather than kept as a copy here - exactly as
// IsMovable asks SelectForAutoAssign. Nothing about Auto assign's policy applies; see the header for why the three
// exceptions have to hold at selection time. The button's own conditions - a target, a complete walk - are SyncButtons'
// and DoSelectAll's, not a row's.
bool IsSelectable(const GpuState* st, const Row& r) {
    return SelectAllTicks(r.r, r.listed, r.system, st->plan.gameKey, st->prefPairs);
}

// Rows Select all would NEWLY tick - what its enabled state reads (the count itself is the header's
// SelectAllPendingCount, so the rule and the count cannot drift). The two flags go across as they stand on each Row.
size_t SelectablePendingCount(const GpuState* st) {
    std::vector<GpuRow> rows;
    std::vector<bool> listed, system;
    for (size_t i = 0; i < st->rows.size(); ++i) {
        rows.push_back(st->rows[i].r);
        listed.push_back(st->rows[i].listed);
        system.push_back(st->rows[i].system);
    }
    return SelectAllPendingCount(rows, listed, system, st->plan.gameKey, st->prefPairs);
}

// Listed rows Auto assign leaves alone for a confirmed main-GPU pin, or (possibleOnly) an unreadable sibling preference -
// the rows the "nothing to move" sentence must not call already on the chosen GPU. Windows and excluded rows, and profile
// games, are skipped for their own reasons and are not counted. The pin rules themselves are IsMainGpuPin, the one
// SelectForBackground reads, and AnotherVersionMayHoldMainGpuPin, the one SelectForAutoAssign adds.
size_t MainGpuPinCount(const GpuState* st, bool possibleOnly = false) {
    size_t n = 0;
    for (size_t i = 0; i < st->rows.size(); ++i) {
        const Row& r = st->rows[i];
        if (!r.listed || r.system || r.r.isProfileGame) continue;
        const bool confirmed = IsMainGpuPin(r.r, st->plan.gameKey) ||
                               AnotherVersionMayHoldMainGpuPin(r.r, st->prefPairs, st->plan.gameKey, false);
        if (possibleOnly ? (!confirmed && AnotherVersionMayHoldMainGpuPin(r.r, st->prefPairs, st->plan.gameKey)) : confirmed)
            ++n;
    }
    return n;
}

bool AnySelected(const GpuState* st) {
    for (size_t i = 0; i < st->rows.size(); ++i) if (st->rows[i].r.selected) return true;
    return false;
}

size_t SelectedCount(const GpuState* st) {
    size_t n = 0;
    for (size_t i = 0; i < st->rows.size(); ++i) if (st->rows[i].listed && st->rows[i].r.selected) ++n;
    return n;
}

// Ticked rows that actually HAVE an assignment to remove. Remove assignment is enabled on this, not
// on "anything ticked": after Auto assign ticks forty unassigned apps, a lit red button that would
// do nothing to thirty-eight of them and delete the preference of the other two is a trap.
size_t RemovableCount(const GpuState* st) {
    size_t n = 0;
    for (size_t i = 0; i < st->rows.size(); ++i) {
        const Row& r = st->rows[i];
        if (r.listed && r.r.selected && !r.r.exePath.empty() && !r.r.assignedKey.empty()) ++n;
    }
    return n;
}

void LayoutGpu(HWND hwnd, GpuState* st);

// THE HEIGHT A WRAPPING LINE NEEDS, MEASURED - never a fixed two lines (adversarial review, v0.5.6). With the main
// GPU chosen and an app that lost its assignment, the status text is the reason, the lost-assignment sentence and the
// double-click hint - [M] by that review, with TextRenderer in Segoe UI Variable Text 9 pt: 48 px at the 824 px panel
// width Settings' minimum size gives, in a 40 px box, so the end of the notice and the hint were cut off. The text is measured with the control's own font at its real width, the way
// irqwindow.cpp's MeasureWrapped does; file-local like Mk and DpiOf, since no shared helper exists. Never below `minH`.
int WrappedHeight(HWND ctl, HDC dc, int w, int minH) {
    if (!ctl || !dc || w <= 0) return minH;
    const int len = GetWindowTextLengthW(ctl);
    if (len <= 0) return minH;
    std::wstring text(static_cast<size_t>(len) + 1, L'\0');
    text.resize(static_cast<size_t>(GetWindowTextW(ctl, &text[0], len + 1)));
    HFONT font = reinterpret_cast<HFONT>(SendMessageW(ctl, WM_GETFONT, 0, 0));
    if (!font) font = theme::GetFont(theme::Font::UiBody, DpiOf(ctl));
    RECT measured = { 0, 0, w, 0 };
    const HGDIOBJ old = SelectObject(dc, font);
    // A STATIC wraps with DT_WORDBREAK | DT_EXPANDTABS, so those are the flags that predict its lines.
    const int h = ::DrawTextW(dc, text.c_str(), static_cast<int>(text.size()), &measured,
                              DT_CALCRECT | DT_WORDBREAK | DT_EXPANDTABS);
    SelectObject(dc, old);
    return h > minH ? h : minH;
}

void RefreshLines(GpuState* st) {
    if (st->broken) {
        if (st->hPlan) SetWindowTextW(st->hPlan, L"This tab could not be built completely, so nothing can be changed here.");
        if (st->hStatus) SetWindowTextW(st->hStatus, L"");
        return;
    }
    std::wstring plan;
    if (st->plan.backgroundKey.empty()) {
        // plan.why VERBATIM when nothing can be decided - it is already a sentence for every
        // outcome, and rewording it here would be a second place for the wording to drift.
        plan = st->plan.why.empty() ? std::wstring(L"No GPU information is available.")
                                    : L"Nothing can be moved: " + st->plan.why + L".";
    } else {
        plan = L"Games stay on " + NameForKey(st->adapters, st->plan.gameKey) + L".";
    }
    if (!st->prefsComplete) {
        plan += L"  Windows' GPU preferences could not all be read, so some assignments may not be shown.";
    }
    if (st->unreadable > 0) {
        plan += L"  " + std::to_wstring(st->unreadable) + L" running program"
              + (st->unreadable == 1 ? L"" : L"s")
              + L" could not be read and cannot be assigned, so "
              + (st->unreadable == 1 ? L"it is" : L"they are") + L" not listed.";
    }
    SetWindowTextW(st->hPlan, plan.c_str());

    // 🔴 GATED ON THE PLAN, NOT ON THE ADAPTER COUNT. FormatGpuIsolateStatusLine takes a count
    // and would promise movable apps on a two-GPU machine whose plan is undecidable.
    const size_t orphans = OrphanCount(st);
    std::wstring status;
    if (st->targetKey.empty()) {
        // NO GPU TO ASSIGN TO, SO NO "TICK AND APPLY" (Council review, v0.5.6): Apply is disabled without a target. Whether a
        // GPU can still be chosen is SyncButtons' own rule for enabling the picker, so the sentence and the picker agree.
        status = FormatLostWithoutTargetLine(orphans, !st->broken && !st->candidates.empty());
    } else if (MainGpuTargeted(st)) {
        // WHY THE BULK BUTTON IS GREY, SAID BESIDE IT (founder decision, v0.5.6): a disabled button with no reason reads
        // as a broken one. The count sentence would be false here - it speaks of background apps moving to a second
        // GPU. A lost assignment is still said, after the reason.
        status = FormatMainGpuStatusLine();
        if (orphans > 0) status += L"  " + FormatGpuIsolateStatusLine(0, 2, orphans);
    } else if (!st->prefsComplete) {
        // THE OTHER REASON THE BULK BUTTON IS GREY, SAID BESIDE IT (Council round 2, v0.5.6 - AutoAssignAllowed). The count
        // sentence would be false here: MovableCount is 0 by that rule, and "nothing to move" would call every application
        // already assigned. A lost assignment that WAS found is still said: ticking it by hand is still how it is fixed.
        status = FormatIncompleteScanStatusLine();
        if (orphans > 0) status += L"  " + FormatGpuIsolateStatusLine(0, st->adapters.size(), orphans);
    } else {
        status = FormatGpuIsolateStatusLine(MovableCount(st), st->adapters.size(), orphans);
        if (status.empty()) status = FormatNothingToMoveLine(MainGpuPinCount(st), MainGpuPinCount(st, true));
    }
    status += (status.empty() ? L"" : L"  ") + std::wstring(L"Double-click a row, or press Space, to tick or untick it.");
    SetWindowTextW(st->hStatus, status.c_str());

    // A LINE THAT NOW NEEDS MORE OR FEWER ROWS MOVES THE LIST. The text changes on every tick and target change, after
    // LayoutGpu last sized these two lines; the panel is laid out again only when a measured height really changed,
    // so a tick does not repaint the whole tab.
    const HWND panel = st->hPlan && st->hStatus ? GetParent(st->hStatus) : nullptr;
    if (!panel) return;
    RECT clientRc, planRc, statusRc;
    GetClientRect(panel, &clientRc);
    GetWindowRect(st->hPlan, &planRc);
    GetWindowRect(st->hStatus, &statusRc);
    const int w = clientRc.right - clientRc.left, minH = theme::Dp(40, st->dpi);
    const HDC dc = GetDC(panel);
    const bool moved = dc && (WrappedHeight(st->hPlan, dc, w, minH) != planRc.bottom - planRc.top ||
                              WrappedHeight(st->hStatus, dc, w, minH) != statusRc.bottom - statusRc.top);
    if (dc) ReleaseDC(panel, dc);
    if (moved) LayoutGpu(panel, st);
}

// 🔴 A RUN IS GOING, AND NOTHING ELSE MAY START ONE (v0.5.9). Greater than zero for the whole of DoApply and DoRemove,
// including the message boxes they open.
//
// IT IS FILE-SCOPE AND NOT A FIELD OF GpuState, ON PURPOSE. RunGuard's destructor runs after the last message box of
// the function it guards, and PanelMessageBox may already have freed the state by then - Tray Exit closes Settings
// while a box is open (PanelLifetime, gpu_edit.h). A counter reached through `st` would be a read of freed memory at
// exactly that moment. One panel exists at a time, so one counter is enough.
int g_running = 0;

// THERE IS A GPU TO WRITE: the panel was built, and a target is picked and on screen (FillTargets clears `targetKey`
// otherwise, and an undecidable plan offers no candidate at all). Apply, Auto assign and Select all are lit only on it,
// and DoSelectAll refuses on it too, so a click already queued to a grey button ticks nothing.
bool Decidable(const GpuState* st) { return !st->broken && !st->targetKey.empty(); }

void SyncButtons(GpuState* st) {
    // 🔴 THE INPUT FREEZE, DECIDED IN ONE PLACE (v0.5.9). Everything that could start or change a run goes dead while
    // one is running, and it is decided HERE rather than beside each button so that a button added later is covered
    // without its author remembering. It also disarms the keyboard for nothing: GpuPanelKey posts a BN_CLICKED only
    // for a control that IsWindowEnabled.
    //
    // THE LIST BOX STAYS ENABLED so the run stays readable and scrollable. A double-click there only toggles a tick,
    // and the run's plan was frozen when PrepareEdits copied it - the WM_COMMAND handler refuses the toggle anyway.
    if (g_running > 0) {
        EnableWindow(st->hTarget, FALSE);
        EnableWindow(st->hBulk, FALSE);
        EnableWindow(st->hSelAll, FALSE);
        EnableWindow(st->hClearSel, FALSE);
        EnableWindow(st->hRemove, FALSE);
        EnableWindow(st->hApply, FALSE);
        EnableWindow(st->hClose, FALSE);
        return;
    }
    const bool decidable = Decidable(st);
    // The picker stays usable whenever there is anything to pick, even with no target: a failed lookup
    // clears the target, and the user must still be able to choose again (adversarial review, round 4).
    EnableWindow(st->hTarget, !st->broken && !st->candidates.empty() ? TRUE : FALSE);
    // Off while the main GPU is the target (founder decision, v0.5.6) or Windows' GPU preferences could not all be read
    // (Council round 2), whatever MovableCount says - AutoAssignAllowed, the rule DoBulk refuses on too.
    EnableWindow(st->hBulk, decidable && AutoAssignAllowed(st->prefsComplete, MainGpuTargeted(st)) && MovableCount(st) > 0
                                ? TRUE : FALSE);
    EnableWindow(st->hApply, decidable && AnySelected(st) ? TRUE : FALSE);
    EnableWindow(st->hRemove, RemovableCount(st) > 0 ? TRUE : FALSE);
    // THE SELECTION PAIR. Select all is lit only while there is a row left for it to tick: a list whose every eligible
    // row is already ticked, or which has no eligible row at all, leaves it grey rather than offering a click that
    // changes nothing. Deselect all keeps its own rule - anything ticked, eligible or not. Neither is offered on a
    // panel that could not be built (`broken`), where no row is listed and nothing may be written.
    //
    // 🔴 AND IT IS OFF WHILE WINDOWS' GPU PREFERENCES COULD NOT ALL BE READ, exactly as Auto assign is (chair
    // decision, v0.5.9 - see DoSelectAll for the reasoning). The condition is the completeness half of
    // AutoAssignAllowed and nothing else: the main GPU being the picker's choice still changes nothing here.
    //
    // 🔴 AND IT IS OFF WITHOUT A TARGET, exactly as Apply is (`decidable`; pre-publish review of v0.5.9). With no
    // target - one GPU, or two cards the plan cannot tell apart - there is no main GPU either, so IsMainGpuPin is
    // false for every row and a preference the user set in Windows' Graphics settings reads as an ordinary row.
    // A lit Select all would tick those, and Remove assignment would then delete them in bulk on one Yes.
    EnableWindow(st->hSelAll, decidable && st->prefsComplete && SelectablePendingCount(st) > 0 ? TRUE : FALSE);
    EnableWindow(st->hClearSel, !st->broken && AnySelected(st) ? TRUE : FALSE);
    // 🔴 CANCEL IS SET EXPLICITLY, THOUGH IT HAS NO CONDITION OF ITS OWN (v0.5.9). Until the freeze above existed
    // this control was simply never touched here and stayed enabled from WM_CREATE; the freeze turns it off, so
    // something has to turn it back on. A button disabled once and re-enabled nowhere is a dead Cancel for the rest
    // of the Settings session.
    EnableWindow(st->hClose, TRUE);
}

void Redraw(GpuState* st) {
    // 🔴 NEVER A NULL HANDLE HERE: InvalidateRect(NULL, ...) invalidates and redraws EVERY window on the desktop, and a
    // panel whose list box failed to create still comes through here to show why.
    if (st->hList) InvalidateRect(st->hList, nullptr, TRUE);
    RefreshLines(st);
    SyncButtons(st);
}

// 🔴 ONE OF THESE AT THE TOP OF DoApply AND DoRemove, AND IT IS WHAT MAKES THE FREEZE TRUE (v0.5.9). It raises
// g_running for the whole run - the confirmation and the result box included - and puts it back however the function
// returns, including the early returns a message box that destroyed the panel forces.
//
// 🔴 `st` IS NULLED BY THE CALLER ON EVERY PATH WHERE THE PANEL WAS DESTROYED, because the destructor's Redraw would
// otherwise read freed memory. Every `if (!PanelMessageBox(...))` return in the guarded functions does it, and so does
// a DropTarget that answers false - which is why DropTarget has a return value at all.
//
// The wait cursor is decoration and nothing more (v0.5.9): it is honest only because mouse messages are never pumped,
// and Windows judges a window hung from its message queue, never from its cursor. The pump below is the fix.
struct RunGuard {
    GpuState* st;
    explicit RunGuard(GpuState* state) : st(state) {
        ++g_running;
        if (g_running == 1) SetCursor(LoadCursorW(nullptr, IDC_WAIT));
        if (st) SyncButtons(st);   // the freeze starts NOW, not at the first repaint after it
    }
    ~RunGuard() {
        --g_running;
        if (g_running == 0) SetCursor(LoadCursorW(nullptr, IDC_ARROW));
        if (st) Redraw(st);
    }
};

// THE FULL PATH OF THE ROW UNDER THE CURSOR, below the list. Two rows can share a file name, and even their
// folders once a line is cut to fit; this is where a row's identity can always be read in full (adversarial
// review, round 5). It is a read-only edit box, so the path can be selected and copied too.
void UpdatePath(GpuState* st) {
    if (!st->hPath) return;
    std::wstring text = L"Select a row to see the full path of its program.";
    if (!st->broken && st->hList) {
        const LRESULT sel = SendMessageW(st->hList, LB_GETCURSEL, 0, 0);
        const LRESULT tie = sel == LB_ERR ? LB_ERR
                                          : SendMessageW(st->hList, LB_GETITEMDATA, static_cast<WPARAM>(sel), 0);
        if (tie >= 0 && static_cast<size_t>(tie) < st->rows.size() && st->rows[static_cast<size_t>(tie)].listed)
            text = st->rows[static_cast<size_t>(tie)].r.exePath;
    }
    // A PATH THAT CANNOT BE SHOWN DOES NOT LEAVE ANOTHER ROW'S ON SCREEN (adversarial review, round 6).
    if (!SetWindowTextW(st->hPath, text.c_str())) {
        LogLine(L"[gpu] the full-path line could not be updated, gle=%lu", GetLastError());
        SetWindowTextW(st->hPath, L"");
    }
}

void FillList(GpuState* st) {
    if (!st->broken && st->hList) {
        const int top = static_cast<int>(SendMessageW(st->hList, LB_GETTOPINDEX, 0, 0));
        SendMessageW(st->hList, WM_SETREDRAW, FALSE, 0);
        SendMessageW(st->hList, LB_RESETCONTENT, 0, 0);
        // 🔴 A ROW THAT IS NOT ON SCREEN CANNOT BE TICKED OR WRITTEN. Found by adversarial review of v0.5.5: a
        // failed insert left the row in `rows`, where the bulk action could tick it and Apply write it, with no
        // line the user could see or untick. Each item's text is the row's FULL PATH, so anything that reads
        // the list - a screen reader, a test - gets its real identity, not a file name two rows can share.
        const PopulateResult shown = PopulateControl(
            st->rows.size(),
            [st](size_t i) {
                return static_cast<long long>(SendMessageW(st->hList, LB_ADDSTRING, 0,
                                                           reinterpret_cast<LPARAM>(st->rows[i].r.exePath.c_str())));
            },
            [st](long long at, size_t i) {
                return SendMessageW(st->hList, LB_SETITEMDATA, static_cast<WPARAM>(at), static_cast<LPARAM>(i)) !=
                       LB_ERR;
            },
            [st](long long at) {
                return SendMessageW(st->hList, LB_DELETESTRING, static_cast<WPARAM>(at), 0) != LB_ERR;
            });
        for (size_t i = 0; i < st->rows.size(); ++i) st->rows[i].listed = shown.shown[i];
        if (shown.broken) {
            st->broken = true;
            LogLine(L"[gpu] a row of the GPU Assignment list could not be tied to its application or taken back");
        }
        if (top > 0) SendMessageW(st->hList, LB_SETTOPINDEX, static_cast<WPARAM>(top), 0);
        SendMessageW(st->hList, WM_SETREDRAW, TRUE, 0);
    }
    for (size_t i = 0; i < st->rows.size(); ++i) {
        if (st->broken || !st->hList) st->rows[i].listed = false;
        if (!st->rows[i].listed) Untick(st->rows[i]);
    }
    UpdatePath(st);
    Redraw(st);
}

void FillTargets(GpuState* st) {
    if (st->broken || !st->hTarget) {
        st->targetKey.clear();
        return;
    }
    SendMessageW(st->hTarget, CB_RESETCONTENT, 0, 0);
    std::vector<std::wstring> labels(st->candidates.size());
    for (size_t i = 0; i < st->candidates.size(); ++i) {
        labels[i] = NameForKey(st->adapters, st->candidates[i]);
        if (labels[i].empty()) labels[i] = st->candidates[i];
        // THE MAIN GPU IS NAMED AS SUCH, AND THE MARKER LEADS. Its DXGI name alone reads like any other card, and a
        // trailing marker is what DrawComboBox's ellipsis would cut first. Every other label is the card's name only:
        // no "integrated" marker (founder decision, v0.5.6 - that warning lives in Apply's question alone).
        if (IsMainGpuKey(st->plan, st->candidates[i])) labels[i] = L"Main GPU: " + labels[i];
    }
    // THE ITEM CARRIES ITS CANDIDATE INDEX. A failed insert would otherwise shift every later item by one, and
    // the GPU on screen would stop being the GPU Apply writes.
    int selIndex = -1;
    const PopulateResult shown = PopulateControl(
        st->candidates.size(),
        [st, &labels](size_t i) {
            return static_cast<long long>(SendMessageW(st->hTarget, CB_ADDSTRING, 0,
                                                       reinterpret_cast<LPARAM>(labels[i].c_str())));
        },
        [st, &selIndex](long long at, size_t i) {
            if (SendMessageW(st->hTarget, CB_SETITEMDATA, static_cast<WPARAM>(at), static_cast<LPARAM>(i)) == CB_ERR)
                return false;
            if (st->candidates[i] == st->targetKey) selIndex = static_cast<int>(at);
            return true;
        },
        [st](long long at) {
            return SendMessageW(st->hTarget, CB_DELETESTRING, static_cast<WPARAM>(at), 0) != CB_ERR;
        });
    if (shown.broken) {
        st->broken = true;
        st->targetKey.clear();
        LogLine(L"[gpu] a GPU in the GPU Assignment picker could not be tied to its card or taken back");
        return;
    }
    // NO VISIBLE TARGET, NO TARGET. If the default GPU never made it into the picker, keeping targetKey
    // would let Apply write to a card nobody can see selected (adversarial review, v0.5.5).
    if (selIndex < 0) st->targetKey.clear();
    // CB_SETCURSEL answers CB_ERR for -1 by design; for a real index it means the picker does not show the
    // default, so the default is not the target either (adversarial review, round 4).
    if (SendMessageW(st->hTarget, CB_SETCURSEL, static_cast<WPARAM>(selIndex), 0) == CB_ERR && selIndex >= 0)
        st->targetKey.clear();
}

// ---------------------------------------------------------------------------
// Actions
// ---------------------------------------------------------------------------

// Each walked value's choice key, by lower-cased path: how a fresh read gives a row its assignedKey. ONE LOOKUP, READ BY
// TWO: ReadAutoChoices, and the check inside an automatic row's guarded write (StillAutoEligible).
std::map<std::wstring, std::wstring> ChoicesByPath(const std::vector<GpuPreferenceEntry>& entries) {
    std::map<std::wstring, std::wstring> choices;
    for (const auto& entry : entries) choices[ToLower(entry.path)] = entry.choiceKey;
    return choices;
}

// Read the policy evidence afresh without replacing listedChoice: a fresh policy check must
// never make the user's earlier consent apply to a different per-path GPU choice.
//
// 🔴 EACH ROW'S assignedKey TAKES THE FRESH READ TOO (v0.5.7), so the GPU column, the status line and the buttons speak for
// the read Auto assign just acted on. An application pinned to the main GPU after the list was read made DoBulk tick
// nothing and say so, while the line above it still counted that application as one that can be moved. listedChoice is
// never touched: it is what Apply compares with (PrepareVerdict), and a fresh read must not become consent. Two limits,
// both on purpose: a path that a read which could not see everything did not return keeps its assignedKey - a partial read
// is no evidence of "no value" - and a row that now holds a setting drops its "lost its GPU" note, as PrepareEdits and
// RunEdits already drop it. EVERY CALLER REDRAWS AFTER THIS: DoBulk itself, and DoApply and DoRemove after PrepareEdits
// and RunEdits.
//
// ONE FRESH READ, READ BY TWO (v0.5.9): ReadAutoChoices below, and DoSelectAll. Both bulk ticks judge the same facts,
// read at the moment of the click, rather than the ones the list happened to be built from.
std::vector<GpuRow> RefreshRowEvidence(GpuState* st) {
    const std::vector<GpuPreferenceEntry> entries = EnumerateGpuPreferenceEntries(&st->prefsComplete);
    st->prefPairs = GpuChoicePairs(entries);
    const std::map<std::wstring, std::wstring> choices = ChoicesByPath(entries);
    std::vector<GpuRow> live;
    for (auto& row : st->rows) {
        const std::map<std::wstring, std::wstring>::const_iterator found = choices.find(ToLower(row.r.exePath));
        if (found != choices.end() || st->prefsComplete) {
            row.r.assignedKey = found != choices.end() ? found->second : std::wstring();
            if (!row.r.assignedKey.empty()) {
                row.orphaned = false;
                row.note.clear();
                row.r.lostKey.clear();
            }
        }
        live.push_back(row.r);
    }
    return live;
}

std::vector<GpuRow> ReadAutoChoices(GpuState* st) {
    const std::vector<GpuRow> live = RefreshRowEvidence(st);
    std::vector<GpuRow> picked =
        SelectForAutoAssign(live, st->targetKey, st->plan.gameKey, st->prefsComplete, st->prefPairs);
    // EVERY CALLER INDEXES THE ANSWER BY ROW. An answer of another size would tick or refuse the wrong rows, so it
    // selects nothing and is logged.
    if (picked.size() != st->rows.size()) {
        LogLine(L"[gpu] Auto assign's rule answered %zu rows for %zu; nothing is selected", picked.size(),
                st->rows.size());
        picked = live;
        for (auto& p : picked) p.selected = false;
    }
    return picked;
}

void DoBulk(GpuState* st, HWND hwnd) {
    const std::vector<GpuRow> picked = ReadAutoChoices(st);
    // The button is disabled while the main GPU is the target, and while Windows' GPU preferences could not all be read;
    // both are refused here as well (AutoAssignAllowed), so a click posted to a disabled button ticks nothing. Said
    // nowhere but the status line, which already gives the reason beside the button.
    if (st->targetKey.empty() || !AutoAssignAllowed(st->prefsComplete, MainGpuTargeted(st))) {
        for (auto& row : st->rows) if (row.autoSelected) Untick(row);
        Redraw(st);
        return;
    }

    // The pure selector has fresh per-path and sibling-version choices. Apply its result only
    // to listed, non-system rows; a manual selection remains possible for other rows.
    size_t n = 0;
    for (size_t i = 0; i < st->rows.size(); ++i) {
        Row& r = st->rows[i];
        r.r.selected = r.listed && !r.system && picked[i].selected;
        r.autoSelected = r.r.selected;
        if (r.r.selected) ++n;
    }
    Redraw(st);
    if (n == 0) PanelMessageBox(st, hwnd, FormatNothingToMoveLine(MainGpuPinCount(st), MainGpuPinCount(st, true)).c_str(),
                               MB_OK | MB_ICONINFORMATION);
}

// "SELECT ALL" - THE OTHER BULK TICK, AND IT IS NOT AUTO ASSIGN (v0.5.9, operator request: the tab offered a way to
// untick everything and no way to tick anything). It is not off while the main GPU is the picker's choice, and it never
// asks whether a row would actually move: it ticks what is safe to tick (SelectAllTicks, gpu_rows.h) and leaves Auto
// assign to decide what is worth moving.
//
// 🔴 BUT IT NEEDS A TARGET, EXACTLY AS APPLY DOES (Decidable; pre-publish review of v0.5.9). "Safe to tick" leans on
// IsMainGpuPin, and with no target there is no main GPU: one GPU, or two cards the plan cannot tell apart, leaves
// `plan.gameKey` empty, IsMainGpuPin false for every row, and a preference the user set in Windows' Graphics settings
// indistinguishable from an ordinary row. Ticking those in bulk would put them one Yes of Remove assignment away from
// being deleted.
//
// EVERY TICK IT MAKES IS A HAND TICK, through Tick() - because that is what it is. Ticking the same rows one at a time
// with Space would have left exactly this state, and Apply must treat them the same way.
//
// 🔴 SO THE EVIDENCE IS READ AGAIN FIRST, exactly as Auto assign reads it. A hand tick carries autoSelected = false and
// Apply's two v0.5.7 re-checks skip it by design, so this click has no net behind it: the three exceptions are checked
// against a fresh walk of Windows' preferences, or they are checked against a read an external pin made minutes ago has
// already outrun.
//
// IT ADDS TO THE SELECTION AND NEVER CLEARS ONE. A row ticked by hand that this rule would not tick - an excluded
// binary the user chose deliberately - is left exactly as the user left it; Deselect all is the button that clears.
//
// 🔴 AN INCOMPLETE PREFERENCE WALK STOPS IT, EXACTLY AS IT STOPS AUTO ASSIGN (chair decision, v0.5.9 - this replaces
// the corner-cut that first shipped with the button, which left it on and named the trade). Auto assign refuses
// everything while `prefsComplete` is false because a walk that missed a value cannot prove an application is
// unpinned (AutoAssignAllowed, AJ45), and the exception this button most depends on -
// AnotherVersionMayHoldMainGpuPin, applied at selection time because nothing downstream re-checks a hand tick - reads
// the very map that walk builds. The standing decision that a tick BY HAND stays available in that state still
// holds, and is what the list box is for: one row, judged by the user looking at it. Forty rows ticked by one click
// are not that, and cannot be judged row by row the way a single hand tick can.
//
// The condition is the completeness half of AutoAssignAllowed plus Decidable, and nothing else: the main GPU being the
// picker's choice does not stop Select all. SyncButtons carries the same two conditions, so the button is grey before
// it is refused - and a click already queued to it when it went grey is refused here.
void DoSelectAll(GpuState* st) {
    if (!Decidable(st)) return;
    // The fresh walk first, so the refusal below judges the read of this click and not the one the list was built
    // from - exactly the order DoBulk uses.
    RefreshRowEvidence(st);
    if (!st->prefsComplete) {
        // Said nowhere but the status line, which already gives the reason beside Auto assign
        // (FormatIncompleteScanStatusLine), and no tick made by hand is cleared: this button only ever adds.
        Redraw(st);
        return;
    }
    for (size_t i = 0; i < st->rows.size(); ++i)
        if (IsSelectable(st, st->rows[i])) Tick(st->rows[i]);
    Redraw(st);
}

// ---- The two passes every registry write goes through ----------------------------------------
//
// PASS ONE READS AND DECIDES; NOTHING IS WRITTEN. A row whose current value cannot be read, or is not
// plain "name=value;" fields, is refused and named - editing it could not be undone exactly. Every
// other row goes to pass two, RunGpuEdits (gpu_edit.h), with the value pass one read.
// How a row is named in a result: its file name, and its folders when another row shares the name.
std::wstring Describe(const Row& r) {
    return r.where.empty() ? r.r.exeName : r.r.exeName + L" (" + r.where + L")";
}

// Every row that is not cleanly done is named on screen AND written to the log with its full path,
// because the dialog shows at most twelve (adversarial review, round 3).
void Refuse(std::vector<std::wstring>& list, const Row& r, const std::wstring& reason) {
    list.push_back(Describe(r) + L" - " + reason);
    LogLine(L"[gpu] %s: %s", r.r.exePath.c_str(), reason.c_str());
}

struct EditPlan {
    size_t row = 0;
    GpuEditItem item;
};

// `changedSinceListed` counts the rows refused because their GPU choice is no longer the one the list showed
// (PrepareVerdict, gpu_edit.h). Those rows are UNTICKED: a tick for one is then made again, by hand or by Auto assign,
// from a list that shows what it holds now - which the caller has the tab read again for (GPUN_REFRESH).
void PrepareEdits(GpuState* st, bool removing, std::vector<EditPlan>& ready, std::vector<std::wstring>& refused,
                  size_t& changedSinceListed) {
    changedSinceListed = 0;
    // REMOVE SKIPS EXACTLY THE ROWS ITS QUESTION DID NOT COUNT (RemovableCount), as before v0.5.7: ReadAutoChoices below now
    // refreshes every row's assignedKey, and a row whose value vanished since stays PrepareVerdict's to refuse and name.
    std::vector<bool> hadAssignment;
    for (size_t i = 0; i < st->rows.size(); ++i) hadAssignment.push_back(!st->rows[i].r.assignedKey.empty());
    const std::vector<GpuRow> automatic = ReadAutoChoices(st);
    for (size_t i = 0; i < st->rows.size(); ++i) {
        Row& r = st->rows[i];
        if (!r.listed || !r.r.selected || r.r.exePath.empty()) continue;
        if (!removing && r.autoSelected && !automatic[i].selected) {
            Untick(r);
            Refuse(refused, r, L"its automatic selection is no longer safe after reading Windows' GPU settings again; "
                              L"review the row and tick it by hand to change it");
            ++changedSinceListed;
            continue;
        }
        if (removing && !hadAssignment[i]) continue;   // nothing of this feature's to remove
        EditPlan e;
        e.row = i;
        e.item.exePath = r.r.exePath;
        // Auto assign's tick is checked once more inside the guarded write (StillAutoEligible); a hand tick never is.
        e.item.automatic = !removing && r.autoSelected;
        bool unreadable = false;
        e.item.present = ReadGpuPreference(r.r.exePath, e.item.existing, &unreadable);
        const GpuPrepareVerdict verdict = PrepareVerdict(r.listedChoice, e.item.present, e.item.existing, unreadable);
        if (verdict == GpuPrepareVerdict::Ready) {
            ready.push_back(e);
            continue;
        }
        // A REFUSED ROW SHOWS WHAT WAS READ, not what the window thought was there (adversarial review, round 5).
        // ...and a row that now shows a setting drops a "lost its GPU" note that described the old state.
        // The lost key goes with the note: both describe the old state.
        r.r.assignedKey = unreadable ? UnreadableChoiceKey()
                                     : (e.item.present ? PreferenceChoiceKey(e.item.existing) : std::wstring());
        r.listedChoice = GpuChoiceText(e.item.present, e.item.existing, unreadable);
        if (!r.r.assignedKey.empty()) {
            r.orphaned = false;
            r.note.clear();
            r.r.lostKey.clear();
        }
        if (verdict == GpuPrepareVerdict::ChangedSinceListed) {
            Untick(r);
            ++changedSinceListed;
        }
        Refuse(refused, r, PrepareRefusalReason(verdict));
    }
}

std::vector<GpuPreferenceBefore> PlannedOf(const std::vector<EditPlan>& ready) {
    std::vector<GpuPreferenceBefore> planned;
    for (size_t k = 0; k < ready.size(); ++k) {
        GpuPreferenceBefore b;
        b.exePath = ready[k].item.exePath;
        b.present = ready[k].item.present;
        b.value = ready[k].item.existing;
        planned.push_back(b);
    }
    return planned;
}

// The GPU the picker is really showing, read back from the control - the only target Apply may write.
std::wstring VisibleTargetKey(const GpuState* st) {
    if (st->broken || !st->hTarget) return std::wstring();
    const LRESULT sel = SendMessageW(st->hTarget, CB_GETCURSEL, 0, 0);
    const LRESULT tie = sel == CB_ERR ? CB_ERR : SendMessageW(st->hTarget, CB_GETITEMDATA, static_cast<WPARAM>(sel), 0);
    size_t index = 0;
    return VisibleCandidate(sel, tie, st->candidates.size(), index) ? st->candidates[index] : std::wstring();
}

// `targetKey` is still the target, still what the picker shows, and one of the offered GPUs.
//
// THE MAIN GPU IS NO LONGER REFUSED HERE (v0.5.6). Operator request: removing an assignment does not put an application
// on the main GPU, so pinning one there has to be possible, and the picker now offers it. Membership in `candidates`
// is still what stops a key the picker never offered.
bool TargetStillShown(const GpuState* st, const std::wstring& targetKey) {
    return !targetKey.empty() && targetKey == st->targetKey &&
           VisibleTargetKey(st) == targetKey &&
           std::find(st->candidates.begin(), st->candidates.end(), targetKey) != st->candidates.end();
}

// A TARGET THAT CANNOT BE CONFIRMED IS DROPPED, AND SAID (adversarial review, round 5: Apply used to return
// without a word, leaving a lit button that did nothing). Ticks made for it go too, as when the picker changes.
//
// 🔴 FALSE MEANS THE PANEL WAS DESTROYED WHILE THE BOX WAS OPEN - PanelMessageBox' own contract, and until v0.5.9
// this function THREW THAT ANSWER AWAY. It was the one latent instance of exactly the defect RunGuard exists to
// guard against: with the answer discarded, the caller could not null `run.st`, and the guard's destructor would
// have redrawn freed memory. Every caller must now write `if (!DropTarget(st, hwnd)) run.st = nullptr;`.
bool DropTarget(GpuState* st, HWND hwnd) {
    st->targetKey.clear();
    for (size_t i = 0; i < st->rows.size(); ++i) Untick(st->rows[i]);
    Redraw(st);
    return PanelMessageBox(st, hwnd,
                L"Nothing was changed.\r\n\r\nThe GPU shown beside \"Assign ticked apps to\" could not be "
                L"confirmed. Choose the GPU again, then tick the applications again.",
                MB_OK | MB_ICONWARNING);
}

// TEST SEAM, INERT UNLESS SET. GAME_OPTIMIZER_TEST_WRITE_DELAY_MS pauses before the first write and after every
// row, so the end-to-end test can change a value where another program would, and stop the app part-way.
// Nothing in the product sets it; each pause is capped at thirty seconds. If it IS set, every Apply and Remove
// freezes this window for that long per row, and the windows it sleeps across are wider.
void TestWriteDelay() {
    wchar_t buf[16];
    const DWORD n = GetEnvironmentVariableW(L"GAME_OPTIMIZER_TEST_WRITE_DELAY_MS", buf, 16);
    if (n == 0 || n >= 16) return;
    const int ms = _wtoi(buf);
    if (ms > 0 && ms <= 30000) Sleep(static_cast<DWORD>(ms));
}

std::wstring ListedLines(const std::vector<std::wstring>& lines) {
    std::wstring s;
    const size_t shown = lines.size() < 12 ? lines.size() : 12;
    for (size_t i = 0; i < shown; ++i) s += L"\r\n    " + lines[i];
    if (lines.size() > shown)
        s += L"\r\n    and " + std::to_wstring(lines.size() - shown) + L" more - every one is in GameOptimizer.log";
    return s;
}

// What a run did, sorted the way the result dialog says it.
struct EditTally {
    size_t done = 0;
    std::vector<std::wstring> already, refused, uncertain, notTried, unrecorded;
    size_t changedSinceListed = 0;   // rows left unfinished at another program's GPU choice, and unticked (UntickAfterRun)
};

// 🔴 A ROW AUTO ASSIGN TICKED IS CHECKED AGAIN INSIDE ITS GUARDED WRITE (v0.5.7 - the sibling-pin race, Council review of
// v0.5.6). PrepareEdits asks Auto assign's rule again before the write, but the guarded write compared only the row's OWN
// value: another version of the application pinned to the main GPU after PrepareEdits and before the commit was not
// seen, and the row was moved to the background GPU. So the write asks once more, after its own comparison: Windows' GPU
// preferences are read again, the row gets the GPU choice that read found for it, and SelectForAutoAssign - the one rule,
// never a copy - must still tick it. A row no longer in the list, or a read that could not see everything, fails closed.
// A write to this key landing after this check and before the commit makes the commit fail (6704, [M] txprobe3), as said
// at GuardedWriteGpuPreference.
struct AutoWriteCheck {
    const GpuState* st;
    const std::wstring* exePath;
};

bool StillAutoEligible(void* context) {
    const AutoWriteCheck* c = static_cast<const AutoWriteCheck*>(context);
    bool complete = true;
    const std::vector<GpuPreferenceEntry> entries = EnumerateGpuPreferenceEntries(&complete);
    const std::map<std::wstring, std::wstring> choices = ChoicesByPath(entries);
    for (size_t i = 0; i < c->st->rows.size(); ++i) {
        if (!WcsIcmp(c->st->rows[i].r.exePath, *c->exePath)) continue;
        GpuRow row = c->st->rows[i].r;
        const std::map<std::wstring, std::wstring>::const_iterator found = choices.find(ToLower(row.exePath));
        row.assignedKey = found != choices.end() ? found->second : std::wstring();
        return SelectForAutoAssign(std::vector<GpuRow>(1, row), c->st->targetKey, c->st->plan.gameKey, complete,
                                   GpuChoicePairs(entries))[0].selected;
    }
    return false;
}

// ---------------------------------------------------------------------------
// The CUDA half of one run (v0.5.8)
// ---------------------------------------------------------------------------
//
// 🔴 ON APPLY IT FOLLOWS THE GPU WRITE AND CAN NEVER FAIL IT; ON REMOVE IT GOES FIRST AND CAN STOP IT.
// Windows' per-application GPU preference decides which adapter an application DRAWS on; NVIDIA keeps
// which GPU its CUDA work may use somewhere else entirely (gpu_cuda.h). So a row whose preference was
// written is ALSO given a settings entry of ours that says which GPU CUDA may use - and every way that
// can go wrong (no driver, a settings session refused, an entry NVIDIA already manages, three NVIDIA
// cards, a save the driver would not do) costs one sentence in the result and nothing else.
// Remove is the other way round (E4): the entry has to go BEFORE the Windows assignment, because the
// button that could try again is enabled only while the row still has an assignment.
struct CudaRun {
    bool on = false;                                     // a CUDA change is possible for this run
    bool removing = false;
    CudaOps ops;
    CudaTarget target;                                   // Apply only
    // ONE RECORD FOR THE WHOLE RUN, read before the question and written one line at a time (gpu_cuda.h,
    // E2). Apply adds and updates lines in it; Remove uses them and takes them out again.
    CudaRecord record;
    CudaRefusal whole = CudaRefusal::None;               // said ONCE for the run, not once per row
    // 🔴 THE CUDA HALF IS OVER FOR THIS RUN, WITHOUT ANYTHING HAVING REACHED DISK. The open driver session
    // may hold a change of ours that was neither saved nor taken back out, and NvAPI_DRS_SaveSettings
    // commits the WHOLE session - so no later row may save. This is NOT E5's whole-run stop: nothing was
    // lost, the notes are intact and the action can simply be run again. E5's stop is the GpuRowGate.
    bool stopped = false;
    // 🔴 R6-3: THE SESSION WAS OPENED ONLY TO LOOK. No ticked row has a line of ours, so there is nothing
    // to put back - but NVIDIA may still hold an entry this product made for one of them, and until round
    // 6 the commonest way to reach one (delete gpu-cuda-record.txt, then press Remove) said NOTHING at
    // all: the session was never opened, the Windows pin went, and the CUDA entry stayed forever. In this
    // mode every row is looked up and named if it is ours, NOTHING is ever refused or counted as
    // "nothing to put back", and the run-wide refusal is never set - blocking Remove is the trap E4 closes.
    bool lookOnly = false;
    size_t changed = 0, already = 0, noRecord = 0;
    // R5-1: of `changed`, the rows whose entry held other settings too, so only our own CUDA setting was
    // taken out of it and the entry itself was left standing.
    size_t keptEntry = 0;
    // R6-7: rows whose line already said the entry holds nothing of ours. Nothing to do, nothing failed.
    size_t nothingOfOurs = 0;
    std::vector<std::wstring> refused, unresolved, unrecorded;
    // R5-5: Remove rows with no line of ours that NVIDIA still holds a Game-Optimizer-named entry for.
    // Nothing is done about them and nothing is blocked; they are named so the user can act.
    std::vector<std::wstring> orphans;
    bool recordTouched = false;                          // a line was written or taken out of the record
};

// True when the record has a line for any ticked row this Remove would act on. 🔴 IT IS ASKED BEFORE THE
// DRIVER IS OPENED, AND THAT IS THE POINT (Council round 2): with nothing of ours recorded for anything
// ticked there is nothing to put back, so a machine with no NVIDIA driver was told "which GPU CUDA uses
// was not put back" about a change nobody had ever made.
bool AnyRecordedRow(GpuState* st, const CudaRecord& record) {
    for (size_t i = 0; i < st->rows.size(); ++i) {
        const Row& r = st->rows[i];
        // EXACTLY THE ROWS RemovableCount COUNTED, so the question's two numbers cannot disagree.
        if (!r.listed || !r.r.selected || r.r.exePath.empty() || r.r.assignedKey.empty()) continue;
        const CudaRecordRow* line = CudaLineFor(record, r.r.exePath);
        // 🔴 R6-7: A LINE SAYING "THE ENTRY IS OURS AND HOLDS NOTHING OF OURS" IS NOT SOMETHING TO PUT
        // BACK. Counting it would make a machine with no NVIDIA driver answer "nothing is removed at all"
        // over a change that has already been undone - E4's block spent on nothing.
        if (line != nullptr && !CudaRowHoldsNothingOfOurs(*line)) return true;
    }
    return false;
}

// Opened once per Apply or Remove and closed at its end - the driver session has to stay open across the
// whole run, because a value set on it is only real once that same session saves.
CudaRun OpenCudaRun(GpuState* st, const std::wstring& targetKey, bool removing) {
    CudaRun run;
    run.removing = removing;
    if (removing) {
        // 🔴 THE CHECK BOX GATES APPLY'S WRITES, AND NOT REMOVE'S RESTORE (Council round 1, F7). Gating
        // Remove on it too left the only way back behind the very switch a user flips to say "stop
        // touching CUDA": assign with it ticked, clear it, Remove - the GPU preference goes, the CUDA
        // exclusion stays, and with nothing assigned any more Remove is greyed out. A setting that can
        // turn a change on but not off is a trap, so what this product wrote it can always take back.
        run.record = ReadCudaRecord(GetConfigDir());
        if (run.record.state == CudaRecordState::Unreadable) {
            // 🔴 A NOTE NOBODY COULD READ MAY NAME THE VERY APPLICATION BEING REMOVED (E6), so it is
            // never the same answer as "there is nothing of ours", and the check box does not silence it.
            run.whole = CudaRefusal::RecordUnreadable;
            return run;
        }
        // 🔴 R6-3: NOTHING OF OURS RECORDED FOR ANYTHING TICKED IS NOT THE SAME AS NOTHING OF OURS BEING
        // THERE, AND THE DIFFERENCE IS THE COMMONEST WAY TO A LEFTOVER. The user deletes
        // gpu-cuda-record.txt - it is a text file in their configuration folder that says it is only a
        // note - and presses Remove. Until round 6 the session was never opened at all: the Windows pin
        // went, the NVIDIA settings entry this product made stayed behind forever, and not one word was
        // said about it. R5-5's sentence existed for exactly this and could only be reached when some
        // OTHER ticked row still had a line.
        //
        // So the session is opened anyway, to LOOK. Three outcomes, and only one of them is new:
        //   * it will not open -> exactly as before: silent, run.whole untouched, and Remove proceeds;
        //   * it opens and the ticked rows have no entry of ours -> still silent;
        //   * it opens and one of them does -> that entry is NAMED in the result and left alone.
        // 🔴 IT MUST NEVER SET run.whole AND MUST NEVER REFUSE A ROW. Blocking every Remove because a
        // driver session would not open is the trap E4 exists to close, and there is nothing here to put
        // back in the first place.
        if (!AnyRecordedRow(st, run.record)) {
            run.ops = MakeCudaOps();
            if (!run.ops.available) return run;   // silent, exactly as it was
            run.on = true;
            run.lookOnly = true;
            return run;
        }
        run.ops = MakeCudaOps();
        if (!run.ops.available) {
            // There IS something of ours recorded and no driver to put it back with. Said once.
            run.whole = run.ops.openRefusal;
            return run;
        }
        run.on = true;
        return run;
    }
    // OFF MEANS SILENT ON APPLY. Nothing CUDA-related happens, and nothing about it is said (founder-facing
    // setting, Setting tab).
    if (!st->cudaEnabled) return run;
    // 🔴 A NON-NVIDIA TARGET NEVER OPENS NVIDIA'S SETTINGS AT ALL (founder decision 18, verbatim: "ignore
    // all non-nvidia GPU by our CUDA config"). Asked here rather than after MakeCudaOps, so an application
    // pinned to the AMD integrated GPU costs no driver session and produces no sentence either way.
    if (!IsNvidiaAdapterKey(targetKey)) return run;
    run.record = ReadCudaRecord(GetConfigDir());
    if (run.record.state == CudaRecordState::Unreadable) {
        // Apply cannot keep the original safely without first reading what is already recorded for these
        // applications, and a change it cannot record is one Remove assignment cannot put back.
        run.whole = CudaRefusal::RecordUnreadable;
        return run;
    }
    run.ops = MakeCudaOps();
    if (!run.ops.available) {
        run.whole = run.ops.openRefusal;
        return run;
    }
    run.target = CudaTargetFor(targetKey, run.ops.listGpus(), run.ops.listIds());
    if (!run.target.act) {
        run.whole = run.target.refusal;
        return run;
    }
    run.on = true;
    return run;
}

// Closes the driver session however the action returns - including the early returns a message box that
// destroyed the panel forces. NvAPI_Unload is never called; see gpu_cuda.cpp.
struct CudaRunCloser {
    CudaRun* run;
    ~CudaRunCloser() {
        if (run && run->ops.close) run->ops.close();
    }
};

// How many ticked applications this product has a note for, so Remove's question can say there is really
// something to take away (Council round 1, F8). 🔴 IT ASKS THE DRIVER NOTHING. Until round 3 it also looked
// each row up to name the other programs a shared NVIDIA profile carries along; E1 means this product never
// writes on a shared profile at all, so there are no neighbours left to disclose.
size_t CudaRestoreCount(GpuState* st, const CudaRun& run) {
    size_t count = 0;
    if (!run.on || !run.removing) return count;
    for (size_t i = 0; i < st->rows.size(); ++i) {
        const Row& r = st->rows[i];
        // EXACTLY THE ROWS RemovableCount COUNTED, so the two numbers in one question cannot disagree.
        if (!r.listed || !r.r.selected || r.r.exePath.empty() || r.r.assignedKey.empty()) continue;
        const CudaRecordRow* line = CudaLineFor(run.record, r.r.exePath);
        // R6-7: as AnyRecordedRow does - a line holding nothing of ours has nothing to take away, so
        // promising the user it will be taken away would be false.
        if (line != nullptr && !CudaRowHoldsNothingOfOurs(*line)) ++count;
    }
    return count;
}

// One row's CUDA half, and the answer decides what happens to that row's Windows GPU preference.
// On APPLY this is called after a Done row - a refused, unconfirmed or untried row did not move GPU, so
// its CUDA setting must not move either - and the only answer that is not Go is E5's whole-run stop.
// On REMOVE it is called BEFORE the row is touched, and a refusal keeps the assignment (E4).
GpuRowGate CudaAfterRow(CudaRun& run, const Row& r, std::wstring& reason) {
    if (run.removing) {
        // 🔴 E4 AT THE LEVEL OF THE WHOLE RUN. If the CUDA half could not start at all - an unreadable
        // note, no NVIDIA driver to put anything back with - then NOTHING is removed. Stripping the
        // assignments would grey out Remove assignment while entries this product made are still there,
        // which is precisely the trap E4 exists to close. The reason is said ONCE, as run.whole.
        if (run.whole != CudaRefusal::None) {
            // R5-6: the unreadable-note reason names the folder and the legacy file pattern, so a user who
            // is told nothing can be removed is told where to look.
            reason = L"its GPU assignment was left in place: " + CudaWholeRunReason(run.whole, GetConfigDir());
            return GpuRowGate::SkipRow;
        }
        if (!run.on) return GpuRowGate::Go;
        if (run.stopped) {
            reason = L"not tried: NVIDIA's settings could not be finished for an application before it";
            Refuse(run.refused, r, L"not tried: NVIDIA's settings could not be finished for an application "
                                   L"before it");
            return GpuRowGate::SkipRow;
        }
        const CudaRecordRow* line = CudaLineFor(run.record, r.r.exePath);
        if (line == nullptr) {
            // The note was read whole - an unreadable one stopped the run before any row - so this really
            // means this product never changed that application's CUDA setting.
            //
            // 🔴 R5-5: EXCEPT THAT NVIDIA MAY STILL HOLD AN ENTRY NAMED FOR THIS PRODUCT FOR IT - a note
            // that was deleted, or a line that was spent while the driver kept the entry. It is SAID and
            // nothing else: the removal is NOT blocked (blocking every Remove is the trap E4 exists to
            // close) and the entry is NOT deleted (an entry no record claims is not ours to delete, E1).
            const CudaResolved orphan = ResolveCudaProfile(r.r.exePath, run.ops);
            if (orphan.state == CudaLookup::Found && IsCudaProfileWeMade(orphan.profileName)) {
                Refuse(run.orphans, r, FormatCudaOrphanEntryLine(orphan.profileName));
                return GpuRowGate::Go;
            }
            // 🔴 R6-3: IN LOOK-ONLY MODE NOTHING IS COUNTED. The session was opened for the orphan check
            // above and for nothing else, so a machine that has never used this feature reads exactly as
            // it did before - not one sentence about CUDA in the question or the result.
            if (run.lookOnly) return GpuRowGate::Go;
            ++run.noRecord;
            return GpuRowGate::Go;
        }
        const CudaRecordRow rec = *line;   // a COPY: the record is rewritten below, and `line` with it
        const CudaRowResult res = RestoreCudaForRow(r.r.exePath, rec, run.ops);
        // The open session may hold a delete that was neither saved nor put back, and the next row's save
        // would commit it with the line still naming it. No later row of this run asks the driver anything.
        if (res.sessionDirty) run.stopped = true;
        // 🔴 R6-7: THE LINE ALREADY SAID THE ENTRY HOLDS NOTHING OF OURS. An earlier Remove took our own
        // CUDA setting out of an entry it had to leave standing. Nothing to take away, nothing failed, and
        // THE LINE STAYS - it is what lets a later Apply own that entry again.
        if (res.outcome == CudaOutcome::NothingOfOurs) {
            ++run.nothingOfOurs;
            return GpuRowGate::Go;
        }
        if (res.outcome == CudaOutcome::Written || res.outcome == CudaOutcome::Absent) {
            if (res.outcome == CudaOutcome::Written) {
                ++run.changed;
                // R5-1: a restore that left the entry standing because it held settings we did not write.
                if (res.settingCleared) ++run.keptEntry;
            }
            else ++run.already;   // E3: NVIDIA has no entry for it any more, so there is nothing to take away
            run.recordTouched = true;
            // 🔴 R6-7: THE CLEAR-ONLY RESTORE KEEPS ITS LINE, WITH THE VALUE EMPTIED. Spending it stranded
            // the entry: it still exists, still carries this product's name, and with no line claiming it
            // every future Apply answered "NVIDIA manages this one" - forever, with NVIDIA Control Panel
            // the only way out. The line now says "we made this entry and there is nothing of ours in it",
            // which a later Apply can own again and a later Remove finds nothing to do about.
            if (res.settingCleared) {
                CudaRecordRow kept = rec;
                kept.lastWrote = CudaNothingWritten();
                kept.when.clear();   // stamped afresh: this is when the setting came out
                if (!SaveCudaRecordRow(run.record, kept))
                    Refuse(run.unrecorded, r,
                           L"its CUDA setting was taken out, but the record of it could not be updated");
                return GpuRowGate::Go;
            }
            // 🔴 THE LINE IS SPENT, AND ONLY NOW (E3). Leaving it is what lets a later Remove act on an
            // entry that is already gone.
            if (!ForgetCudaRecordRow(run.record, rec.appEntry))
                Refuse(run.unrecorded, r,
                       L"its CUDA setting was put back, but the record of it could not be updated");
            return GpuRowGate::Go;
        }
        Refuse(run.refused, r, CudaRowRefusalText(res));
        reason = L"its GPU assignment was left in place because which GPU CUDA uses could not be put back "
                 L"for it";
        return GpuRowGate::SkipRow;
    }
    if (!run.on) return GpuRowGate::Go;
    if (run.stopped) {
        Refuse(run.refused, r, L"not tried: NVIDIA's settings could not be finished for an application "
                               L"before it");
        return GpuRowGate::Go;
    }
    // 🔴 THE LINE GOES DOWN BEFORE THE SAVE. WriteCudaForRow calls this after the driver takes the value
    // and before NvAPI_DRS_SaveSettings, so a change that reaches disk is always one the note already
    // names - and a row whose line could not be written is taken back out of the driver instead.
    // The snapshot is what the note has to go back to if that undo succeeds: leaving a line pointing at a
    // value the driver does not hold would make the next Remove report a conflict that never happened.
    const std::vector<CudaRecordRow> before = run.record.rows;
    CudaApplyInputs in;
    in.record = [&run](const CudaRecordRow& line) { return SaveCudaRecordRow(run.record, line); };
    const CudaRowResult res = WriteCudaForRow(r.r.exePath, run.target, run.ops, run.record, in);
    if (res.recorded) run.recordTouched = true;
    // 🔴 R5-4: A CLEAN-UP THAT FAILED LEFT SOMETHING OF THIS ROW'S IN THE OPEN SESSION, and
    // NvAPI_DRS_SaveSettings commits the WHOLE session - so no later row of this run may save. The row
    // itself is also unresolved below, which stops the run outright; this is the belt under that brace, so
    // a future path that sets sessionDirty without unresolved cannot commit a leftover behind us.
    if (res.sessionDirty) run.stopped = true;
    if (res.recorded && res.undone && !SetCudaRecordRows(run.record, before))
        Refuse(run.unrecorded, r,
               L"its CUDA setting was not changed, but the record still names it; Remove assignment will "
               L"report that as a change somebody else made");
    switch (res.outcome) {
        case CudaOutcome::Written:
            ++run.changed;
            break;
        case CudaOutcome::AlreadySet:
            ++run.already;
            break;
        case CudaOutcome::Refused:
            if (res.unresolved) {
                // 🔴 E5. A change reached NVIDIA's database and could not be taken back out, so no later
                // application is attempted at all - for CUDA or for its Windows GPU preference.
                Refuse(run.unresolved, r, CudaRowRefusalText(res));
                run.stopped = true;
                return GpuRowGate::StopRun;
            }
            Refuse(run.refused, r, CudaRowRefusalText(res));
            // A note that cannot take a line will not take the next one either, and every row after this
            // would be written and taken back out again for nothing.
            if (res.refusal == CudaRefusal::NotRecorded) run.stopped = true;
            break;
        default:
            break;
    }
    return GpuRowGate::Go;
}

// What the result dialog says about CUDA. The twelve-line cap is ListedLines', the same one every other
// list in this window uses, so a run with forty refusals reads like every other long result.
CudaResultText CudaTextOf(const CudaRun& run, const std::wstring& targetName) {
    CudaResultText text;
    text.changed = run.changed;
    text.already = run.already;
    text.noRecord = run.noRecord;
    text.keptEntry = run.keptEntry;
    text.nothingOfOurs = run.nothingOfOurs;   // R6-7
    text.anyOrphan = !run.orphans.empty();
    text.orphanLines = ListedLines(run.orphans);
    text.targetName = targetName;
    text.wholeReason = CudaWholeRunReason(run.whole, GetConfigDir());
    text.anyRefused = !run.refused.empty();
    text.refusedLines = ListedLines(run.refused);
    text.recordStarted = run.recordTouched;
    text.recordPath = run.record.path;
    text.anyUnresolved = !run.unresolved.empty();
    text.unresolvedLines = ListedLines(run.unresolved);
    text.anyUnrecorded = !run.unrecorded.empty();
    text.unrecordedLines = ListedLines(run.unrecorded);
    return text;
}

// True when the CUDA half of a run has nothing to complain about, so a result the GPU half is happy with
// still shows the information icon rather than the warning one.
bool CudaClean(const CudaRun& run) {
    // R5-5: an entry nothing of ours claims is something the user has to go and deal with in NVIDIA Control
    // Panel, so the result wears the warning icon even though nothing failed and nothing was blocked.
    return run.whole == CudaRefusal::None && run.refused.empty() && run.unresolved.empty() &&
           run.unrecorded.empty() && run.orphans.empty();
}

// 🔴 THE ONE THING THAT KEEPS THIS TAB PAINTING WHILE A RUN WALKS ITS ROWS (v0.5.9) - the status line is set, that
// one control is repainted, and the queue's PAINTS ARE LET THROUGH. Nothing else.
//
// 🔴 WM_PAINT AND NOTHING ELSE, AS EXACT SINGLE-MESSAGE PEEKS, NEVER A RANGE. WM_PAINT is 0x000F and WM_CLOSE is
// 0x0010: a range one message wider would destroy this panel BETWEEN two registry writes, with the lifetime counter
// at zero, freeing the state the edit loop is still holding. Keys, mouse, WM_COMMAND, WM_TIMER, WM_CLOSE and
// WM_SYSCOMMAND all stay QUEUED and are never retrieved - a click posted during a run is still there when the run
// returns, which is what B3's refusals then decline. WM_TIMER in particular re-reads the environment and the
// topology and can lay the whole window out again, once a second, between two registry writes.
//
// EACH LOOP IS BOUNDED because a window that invalidates itself from its own WM_PAINT would otherwise spin here
// forever, and a run that never returns is worse than one that does not paint.
//
// RefreshLines and LayoutGpu are NOT called from here: this runs between two registry writes, and the row data it
// would read is mid-run.
void ShowRunProgress(GpuState* st, const std::wstring& line) {
    if (!st->hStatus) return;
    SetWindowTextW(st->hStatus, line.c_str());
    InvalidateRect(st->hStatus, nullptr, TRUE);
    UpdateWindow(st->hStatus);
    MSG msg;
    for (int i = 0; i < 32 && PeekMessageW(&msg, nullptr, WM_PAINT, WM_PAINT, PM_REMOVE); ++i)
        DispatchMessageW(&msg);
    for (int i = 0; i < 4 && PeekMessageW(&msg, nullptr, WM_SYNCPAINT, WM_SYNCPAINT, PM_REMOVE); ++i)
        DispatchMessageW(&msg);
}

// PASS TWO, and every row brought up to date with what the registry now holds. See RunGpuEdits for the order
// that keeps the restore file honest at any point a run can stop.
EditTally RunEdits(GpuState* st, const std::vector<EditPlan>& ready, bool removing, const std::wstring& targetKey,
                   GpuRestoreJournal& journal, CudaRun* cuda = nullptr) {
    const ULONGLONG started = GetTickCount64();
    std::vector<GpuEditItem> items;
    for (size_t k = 0; k < ready.size(); ++k) items.push_back(ready[k].item);
    // 🔴 THE PROGRESS COUNTER, AND THE CUDA SENTENCE IT MAY CARRY (v0.5.9). `on` alone is not "this run has a CUDA
    // half": Remove opens the driver session in look-only mode too (R6-3), where nothing is ever written and nothing
    // is ever saved, so a run that will not save must not promise the user a save per application.
    const bool cudaHalf = cuda != nullptr && cuda->on && !cuda->lookOnly;
    const size_t total = items.size();
    size_t rowsDone = 0;
    // One-based, and never past `total`: a run that stops part-way stops counting here, because RunGpuEdits' after-
    // a-stop fast path calls nothing at all. Making that number tidy would mean calling afterRow on that path, which
    // also fires TestWriteDelay and would change the timing of a documented end-to-end test seam.
    const auto publish = [&]() {
        ShowRunProgress(st, FormatRunProgressLine(removing, rowsDone < total ? rowsDone + 1 : total, total, cudaHalf));
    };
    GpuEditOps ops;
    ops.write = [st](const std::wstring& path, bool expectPresent, const std::wstring& expectValue, bool deleteValue,
                     const std::wstring& value, unsigned long& error, bool automatic) {
        // A hand tick is the user's override (founder decision, v0.5.6) and passes no check.
        if (!automatic) return GuardedWriteGpuPreference(path, expectPresent, expectValue, deleteValue, value, &error);
        AutoWriteCheck check = { st, &path };
        return GuardedWriteGpuPreference(path, expectPresent, expectValue, deleteValue, value, &error, nullptr, nullptr,
                                         StillAutoEligible, &check);
    };
    ops.read = [](const std::wstring& path, std::wstring& value, bool& unreadable) {
        return ReadGpuPreference(path, value, &unreadable);
    };
    ops.record = [&journal](const GpuPreferenceBefore& row) { return RecordGpuRestoreRow(journal, row); };
    // 🔴 THE PROGRESS LINE RIDES THE SEAM THAT ALREADY EXISTS, rather than a second callback beside it. `afterRow` is
    // "called after every row; may be empty" (gpu_edit.h) and was already wired to TestWriteDelay; that call and its
    // place are kept exactly - it is a documented end-to-end test seam - and the counter is published beside it.
    ops.afterRow = [&]() {
        TestWriteDelay();
        ++rowsDone;
        publish();
    };
    // 🔴 THE CUDA HALF RUNS INSIDE THE ROW LOOP, NOT IN A SECOND PASS OVER ITS RESULTS (E4, E5). It used to
    // run here, after every registry write had already happened - so a Remove could strip an assignment
    // before finding out that the NVIDIA entry had to stay, and an Apply whose rollback failed carried on
    // writing later applications while the result claimed nothing after it was tried.
    if (cuda) {
        ops.cudaRow = [st, &ready, cuda](size_t index, std::wstring& reason) {
            return CudaAfterRow(*cuda, st->rows[ready[index].row], reason);
        };
    }
    // AND ONCE BEFORE THE FIRST WRITE, where this seam already paused: the first row carries the first whole-database
    // save and is the slowest single unit of the run, so a counter that only appeared after it would leave the tab
    // blank for exactly the longest wait it exists to explain.
    if (!items.empty()) {
        TestWriteDelay();
        publish();
    }
    const std::vector<GpuEditResult> results = RunGpuEdits(items, removing, targetKey, ops);

    EditTally t;
    for (size_t k = 0; k < results.size(); ++k) {
        Row& r = st->rows[ready[k].row];
        const GpuEditResult& res = results[k];
        const std::wstring listedBefore = r.listedChoice;   // what the user said yes to: pass one found it unchanged
        r.r.assignedKey = res.choiceKey;   // read from the registry for every outcome, not-tried rows included
        r.listedChoice = res.choiceText;   // the same read: the list now shows it, so the next Apply compares with it
        const bool changed = res.outcome == GpuEditOutcome::Done || res.outcome == GpuEditOutcome::Unconfirmed;
        switch (res.outcome) {
            case GpuEditOutcome::Done:         ++t.done; Untick(r); break;
            case GpuEditOutcome::AlreadyDone:  Refuse(t.already, r, res.reason); Untick(r); break;
            case GpuEditOutcome::Refused:      Refuse(t.refused, r, res.reason); break;
            case GpuEditOutcome::Unconfirmed:  Refuse(t.uncertain, r, res.reason); break;
            case GpuEditOutcome::NotAttempted: Refuse(t.notTried, r, res.reason); break;
        }
        // A ROW LEFT UNFINISHED AT ANOTHER PROGRAM'S GPU CHOICE IS UNTICKED (UntickAfterRun, gpu_edit.h): kept ticked, the next
        // Apply would compare with that choice - the listed one now - and write over it.
        if (UntickAfterRun(res.outcome, listedBefore, res.choiceText, IntendedChoiceText(ready[k].item, removing, targetKey))) {
            Untick(r);
            ++t.changedSinceListed;
        }
        // "LOST ITS GPU" DESCRIBES THE OLD STATE. A row this run changed, or one that now holds a setting, no
        // longer carries that note (adversarial review, round 5).
        if (changed || !r.r.assignedKey.empty()) {
            r.orphaned = false;
            r.note.clear();
            r.r.lostKey.clear();
        }
        if (changed && !res.recorded)
            Refuse(t.unrecorded, r, L"changed, but its previous value could not be added to the restore file");
    }
    // Successful, refused, and unconfirmed writes all changed what we know. Refresh the
    // sibling-version evidence and invalidate earlier automatic ticks before the next action.
    const std::vector<GpuRow> automatic = ReadAutoChoices(st);
    for (size_t i = 0; i < st->rows.size(); ++i)
        if (st->rows[i].autoSelected && !automatic[i].selected) Untick(st->rows[i]);
    // 🔴 HOW LONG A ROW ACTUALLY TAKES, WHICH NOBODY HAS EVER MEASURED (v0.5.9). Every judgement about this run -
    // whether the bounded paint pump above is enough, whether a hundred-row Apply is minutes or seconds - has been an
    // assumption. It is one line at the end of a run, in ActivateGpuPanel's own form ("[gpu] panel refreshed in %llu
    // ms (%zu rows)"), and the CUDA flag is part of it because a whole-database save per row is the whole question.
    LogLine(L"[gpu] %s ran %zu rows in %llu ms (CUDA half %s)", removing ? L"Remove" : L"Apply", items.size(),
            GetTickCount64() - started, cudaHalf ? L"on" : L"off");
    return t;
}

// What the result dialog says about the restore file, and about the record kept while the change ran.
std::wstring RestoreLines(const GpuRestoreJournal& journal, const EditTally& t, bool finished) {
    return FormatRestoreLines(journal.regStarted, journal.regPath, journal.pendingPath, !t.unrecorded.empty(),
                              ListedLines(t.unrecorded), finished);
}

// WHEN THE RESTORE FILE CANNOT BE STARTED, THE ROWS ARE STILL NAMED (adversarial review, round 6): the ones pass one
// refused, and the ones never tried.
std::wstring NotStartedMessage(GpuState* st, const std::vector<EditPlan>& ready, const std::vector<std::wstring>& refused,
                               const wchar_t* refusedHeading) {
    std::vector<std::wstring> notTried;
    for (size_t k = 0; k < ready.size(); ++k)
        Refuse(notTried, st->rows[ready[k].row], L"not tried: the restore file could not be started");
    std::wstring msg = L"Nothing was changed.\r\n\r\nThe restore file could not be started in " + GetConfigDir() +
                       L", and nothing is written without one.\r\n\r\nNot tried:" + ListedLines(notTried);
    if (!refused.empty()) msg += L"\r\n\r\n" + std::wstring(refusedHeading) + ListedLines(refused);
    return msg;
}

// What a result says, after its "Not written:" or "Not removed:" list, when a row was left alone - refused by pass one, or
// left unfinished by pass two (UntickAfterRun) - because its GPU choice changed after the list was shown.
std::wstring ChangedSinceListedLine(size_t changedSinceListed) {
    if (changedSinceListed == 0) return std::wstring();
    return changedSinceListed == 1
               ? std::wstring(L"\r\n\r\nThe application whose GPU setting or automatic-selection eligibility changed is unticked, "
                              L"and the list is read again when this message closes. Tick it again to change it.")
               : std::wstring(L"\r\n\r\nThe applications whose GPU setting or automatic-selection eligibility changed are unticked, "
                              L"and the list is read again when this message closes. Tick one again to change it.");
}

// 🔴 A ROW LEFT ALONE FOR A CHANGED GPU CHOICE, IN EITHER PASS, HAS THE TAB READ AGAIN, ONCE ITS RESULT HAS BEEN READ (Council
// review, v0.5.6).
// The refused rows already show what was read, but another application may have changed in the same moment, so the
// whole list is read again - by Settings, which holds the config and the process snapshot the panel never stores
// (GPUN_REFRESH). Called only after the result box returned with the panel still attached. The refresh can disable the
// control that had focus - Apply, with nothing left ticked - so focus left on a disabled control goes to the list.
void RereadAfterChangedRows(GpuState* st, HWND hwnd, size_t changedSinceListed) {
    if (changedSinceListed == 0) return;
    SendMessageW(GetParent(hwnd), WM_COMMAND, MAKEWPARAM(GetDlgCtrlID(hwnd), GPUN_REFRESH), reinterpret_cast<LPARAM>(hwnd));
    const HWND f = GetFocus();
    if (f && IsChild(hwnd, f) && !IsWindowEnabled(f) && st->hList && IsWindowVisible(st->hList)) SetFocus(st->hList);
}

void DoApply(GpuState* st, HWND hwnd) {
    // 🔴 FIRST STATEMENT, BEFORE ANYTHING CAN RETURN (v0.5.9): the run is declared, the buttons go dead and the
    // WM_COMMAND handler starts refusing a second click - including one ALREADY IN THE QUEUE, which the confirmation
    // box's own modal loop would otherwise dispatch straight back into here. That re-entry opened a SECOND NVAPI DRS
    // session over the first, each carrying its own in-memory copy of gpu-cuda-record.txt, and whichever saved last
    // lost the other's lines. It took a double-press, not a rare race.
    RunGuard run(st);
    // THE TARGET IS CHECKED AGAIN HERE, NOT TRUSTED FROM THE PICKER: one of the GPUs the picker offers - the main
    // GPU among them since v0.5.6 - and the one the picker really shows (adversarial review, v0.5.5). It is copied
    // once, and that copy is what every row is written with.
    const std::wstring targetKey = st->targetKey;
    if (targetKey.empty()) return;   // Apply is disabled without a target
    if (!TargetStillShown(st, targetKey)) {
        if (!DropTarget(st, hwnd)) run.st = nullptr;
        return;
    }
    const size_t pending = SelectedCount(st);
    if (pending == 0) return;

    const std::wstring target = NameForKey(st->adapters, targetKey);

    // HONEST ABOUT WHAT IS REPLACED. A ticked row already on another card, or on a Windows GPU
    // setting, loses that choice; the confirm says how many, and the restore file keeps it.
    size_t replacing = 0;
    for (size_t i = 0; i < st->rows.size(); ++i) {
        const Row& r = st->rows[i];
        if (r.listed && r.r.selected && !r.r.assignedKey.empty() && r.r.assignedKey != targetKey) ++replacing;
    }

    // CONFIRM FIRST, DEFAULTING TO NO. This writes a per-application Windows setting for every ticked
    // row, possibly dozens. House precedent: settings.cpp's profile Remove confirm.
    //
    // The words come from FormatAssignConfirm, which the unit suite pins (AJ37): v0.5.5's sentences unchanged, plus a
    // warning when the target is the main GPU, and one when DXCore said the target is an integrated GPU. A GPU whose
    // kind could not be read gets no integrated warning - nothing is said that was not measured.
    // THE DRIVER SESSION IS OPENED BEFORE THE QUESTION, because the question has to say what the answer
    // will do: which GPU CUDA will use, and which other applications NVIDIA carries along with each
    // ticked one. It is closed however this function returns.
    CudaRun cuda = OpenCudaRun(st, targetKey, false);
    CudaRunCloser closer = { &cuda };

    AssignConfirm confirm;
    confirm.count = pending;
    confirm.targetName = target;
    confirm.replacing = replacing;
    confirm.mainGpu = IsMainGpuKey(st->plan, targetKey);
    confirm.integrated = KindForKey(st->adapters, targetKey) == GpuKind::Integrated;
    confirm.cudaLine = FormatCudaConfirmLine(cuda.on, target);
    const std::wstring ask = FormatAssignConfirm(confirm);
    int answer = 0;
    // A false return is a panel destroyed while the question was open: `st` is freed or about to be, so nothing more -
    // and the guard is told, or its destructor would redraw that freed state.
    if (!PanelMessageBox(st, hwnd, ask.c_str(), MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2, &answer)) {
        run.st = nullptr;
        return;
    }
    if (answer != IDYES) return;
    // Checked again after the question: nothing may have moved the picker while it was open.
    if (!TargetStillShown(st, targetKey)) {
        if (!DropTarget(st, hwnd)) run.st = nullptr;
        return;
    }

    std::vector<EditPlan> ready;
    std::vector<std::wstring> notWritten;
    size_t changedSinceListed = 0;
    PrepareEdits(st, false, ready, notWritten, changedSinceListed);

    GpuRestoreJournal journal;
    if (!ready.empty() && !BeginGpuRestore(GetConfigDir(), PlannedOf(ready), journal)) {
        Redraw(st);
        const std::wstring notStarted =
            NotStartedMessage(st, ready, notWritten, L"Not written:") + ChangedSinceListedLine(changedSinceListed);
        if (!PanelMessageBox(st, hwnd, notStarted.c_str(), MB_OK | MB_ICONWARNING)) {
            run.st = nullptr;
            return;
        }
        RereadAfterChangedRows(st, hwnd, changedSinceListed);
        return;
    }
    // The CUDA record is NOT a per-run file and is not started here: it is one file in the config folder,
    // already read by OpenCudaRun, and each row's line goes into it before that row's change is saved
    // (gpu_cuda.h, E2).
    const EditTally t = RunEdits(st, ready, false, targetKey, journal, &cuda);
    changedSinceListed += t.changedSinceListed;   // pass two's too: those rows are unticked, said and read again the same way
    // THE RECORD GOES ONLY WHEN EVERY CHANGE IS OVER AND EVERY CHANGED ROW IS IN THE RESTORE FILE.
    const bool finished = ready.empty() || (t.unrecorded.empty() && FinishGpuRestore(journal));
    Redraw(st);

    // 🔴 IT REPORTS WHAT LANDED AND WHAT DID NOT, ALWAYS - by name, with the reason. A bulk registry
    // write that half failed must not look like one that succeeded.
    notWritten.insert(notWritten.end(), t.refused.begin(), t.refused.end());
    std::wstring msg = std::to_wstring(t.done) + L" of " + std::to_wstring(pending)
                     + L" assigned to " + target + L".";
    // 🔴 R6-4: IT SAYS WHICH SETTING IT MEANS. "Already set, nothing written" is about WINDOWS' own
    // per-application GPU preference and nothing else - and since R5-3 those very rows DO get their CUDA
    // entry written. On a machine upgrading from v0.5.6 or v0.5.7 every ticked application is already
    // pinned, so this heading covers the whole list while the CUDA half below it reports real changes; the
    // unqualified wording read as "nothing happened for any of these".
    if (!t.already.empty())
        msg += L"\r\n\r\nWindows' own GPU preference was already set, nothing written:" + ListedLines(t.already);
    if (!notWritten.empty()) msg += L"\r\n\r\nNot written:" + ListedLines(notWritten);
    msg += ChangedSinceListedLine(changedSinceListed);
    if (!t.uncertain.empty()) msg += L"\r\n\r\nWritten, but not confirmed:" + ListedLines(t.uncertain);
    if (!t.notTried.empty()) msg += L"\r\n\r\nNot tried:" + ListedLines(t.notTried);
    msg += RestoreLines(journal, t, finished);
    const CudaResultText cudaText = CudaTextOf(cuda, target);
    msg += FormatCudaApplyLines(cudaText);
    // 🔴 R6-4, AND IT IS WHAT AN UPGRADE FROM v0.5.7 ACTUALLY HITS. This paragraph used to be printed only
    // when a REGISTRY value had been written. Since R5-3 the CUDA half also runs for a row whose Windows
    // pin is already what this Apply intends - which is every application v0.5.6 and v0.5.7 pinned - so on
    // those machines Apply writes no registry value, t.done is 0, and the one sentence telling the user
    // the change arrives at the application's NEXT LAUNCH was skipped. They saw "nothing written", no
    // restart advice, and concluded the feature had not worked. A CUDA setting that changed is a change,
    // and it needs the same restart.
    if (t.done > 0 || cudaText.changed > 0) {
        // AN INSTRUCTION, NEVER A CLAIM THAT IT HAS TAKEN EFFECT. Windows reads this preference when
        // an application creates its graphics device, at start-up, so a running application keeps
        // the GPU it already has. Byte-identical to v0.5.7's, which end-to-end scripts match on.
        msg += L"\r\n\r\nRestart those applications for the change to take effect. A running "
               L"application keeps the GPU it started on.";
        // 🔴 AND THE SECOND HALF ONLY WHEN IT IS TRUE. A row on the AMD integrated GPU, or a run with the
        // check box off, must not see the word CUDA anywhere (founder decision 18).
        if (cudaText.changed > 0)
            msg += L" The same is true of which GPU CUDA uses: NVIDIA reads that when an application "
                   L"starts as well.";
    }
    const bool clean = notWritten.empty() && t.uncertain.empty() && t.notTried.empty() && finished && CudaClean(cuda);
    if (!PanelMessageBox(st, hwnd, msg.c_str(), MB_OK | (clean ? MB_ICONINFORMATION : MB_ICONWARNING))) {
        run.st = nullptr;
        return;
    }
    RereadAfterChangedRows(st, hwnd, changedSinceListed);
}

void DoRemove(GpuState* st, HWND hwnd) {
    RunGuard run(st);   // as in DoApply, and for the same re-entry: see RunGuard and the WM_COMMAND refusals
    // 🔴 CONFIRM FIRST, DEFAULTING TO NO - EXACTLY AS APPLY DOES. Found in the v0.5.5 window's own
    // screenshot: after the bulk action (now "Auto assign GPU for Gaming") this red button sits lit
    // beside Apply, and it used to delete registry values the moment it was clicked.
    const size_t removable = RemovableCount(st);
    if (removable == 0) return;
    // Opened before the question for the same reason Apply opens it there: the driver session has to be
    // the one that later saves, and the records have to be readable before anything is undone - and, since
    // v0.5.8, because the question itself has to say what putting the CUDA setting back will touch (F8).
    CudaRun cuda = OpenCudaRun(st, std::wstring(), true);
    CudaRunCloser closer = { &cuda };
    // The words come from FormatRemoveConfirm, which the unit suite pins (AJ37b): v0.5.7's two sentences
    // unchanged, plus the CUDA line when a note really has something to take away - or when the CUDA half
    // cannot run at all, in which case E4 removes NOTHING and the question has to say so BEFORE Yes.
    RemoveConfirm confirm;
    confirm.count = removable;
    // R5-6: the blocked-Remove sentence names the configuration folder and the legacy file pattern.
    confirm.cudaLine = FormatCudaRestoreConfirmLine(CudaRestoreCount(st, cuda), !st->cudaEnabled,
                                                    CudaWholeRunReason(cuda.whole, GetConfigDir()));
    const std::wstring ask = FormatRemoveConfirm(confirm);
    int answer = 0;
    // As in DoApply: a false return is a panel destroyed while the question was open, so nothing more, and the guard
    // is told so its destructor does not redraw freed state.
    if (!PanelMessageBox(st, hwnd, ask.c_str(), MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2, &answer)) {
        run.st = nullptr;
        return;
    }
    if (answer != IDYES) return;

    std::vector<EditPlan> ready;
    std::vector<std::wstring> notRemoved;
    size_t changedSinceListed = 0;
    PrepareEdits(st, true, ready, notRemoved, changedSinceListed);

    GpuRestoreJournal journal;
    if (!ready.empty() && !BeginGpuRestore(GetConfigDir(), PlannedOf(ready), journal)) {
        Redraw(st);
        const std::wstring notStarted =
            NotStartedMessage(st, ready, notRemoved, L"Not removed:") + ChangedSinceListedLine(changedSinceListed);
        if (!PanelMessageBox(st, hwnd, notStarted.c_str(), MB_OK | MB_ICONWARNING)) {
            run.st = nullptr;
            return;
        }
        RereadAfterChangedRows(st, hwnd, changedSinceListed);
        return;
    }
    // STRIP ONLY THIS FEATURE'S FIELDS; the whole value is deleted only when nothing else was in it.
    const EditTally t = RunEdits(st, ready, true, std::wstring(), journal, &cuda);
    changedSinceListed += t.changedSinceListed;   // as in DoApply
    const bool finished = ready.empty() || (t.unrecorded.empty() && FinishGpuRestore(journal));
    Redraw(st);

    notRemoved.insert(notRemoved.end(), t.refused.begin(), t.refused.end());
    std::wstring msg = std::to_wstring(t.done) + L" of " + std::to_wstring(removable) + L" assignment"
                     + (removable == 1 ? L"" : L"s") + L" removed.";
    if (!t.already.empty()) msg += L"\r\n\r\nNothing to remove, nothing written:" + ListedLines(t.already);
    if (!notRemoved.empty()) msg += L"\r\n\r\nNot removed:" + ListedLines(notRemoved);
    msg += ChangedSinceListedLine(changedSinceListed);
    if (!t.uncertain.empty()) msg += L"\r\n\r\nChanged, but not confirmed:" + ListedLines(t.uncertain);
    if (!t.notTried.empty()) msg += L"\r\n\r\nNot tried:" + ListedLines(t.notTried);
    msg += RestoreLines(journal, t, finished);
    msg += FormatCudaRemoveLines(CudaTextOf(cuda, std::wstring()));
    if (t.done > 0) {
        msg += L"\r\n\r\nThose applications return to Windows' default GPU choice the next time "
               L"they start.";
    }
    const bool clean = notRemoved.empty() && t.uncertain.empty() && t.notTried.empty() && finished && CudaClean(cuda);
    if (!PanelMessageBox(st, hwnd, msg.c_str(), MB_OK | (clean ? MB_ICONINFORMATION : MB_ICONWARNING))) {
        run.st = nullptr;
        return;
    }
    RereadAfterChangedRows(st, hwnd, changedSinceListed);
}

// ---------------------------------------------------------------------------
// Painting and layout
// ---------------------------------------------------------------------------

void DrawRow(GpuState* st, const DRAWITEMSTRUCT* di) {
    if (di->itemID == static_cast<UINT>(-1)) return;
    const size_t idx = static_cast<size_t>(di->itemData);
    if (idx >= st->rows.size()) return;
    const Row& r = st->rows[idx];
    const theme::Palette& pal = theme::P();
    const int dpi = st->dpi;

    RECT rc = di->rcItem;
    const bool hot = (di->itemState & ODS_SELECTED) != 0;
    HBRUSH bg = CreateSolidBrush(hot ? pal.cardBgAlt : pal.inputBg);
    FillRect(di->hDC, &rc, bg);
    DeleteObject(bg);

    RECT t = rc;
    t.left += theme::Dp(8, dpi);
    t.right -= theme::Dp(8, dpi);

    // The [x] / [ ] mark the profile list already uses, so one visual language covers both lists.
    const SIZE ms = theme::MeasureText(di->hDC, L"[x] ", theme::Font::MonoSmall, dpi);
    RECT mr = t;
    mr.right = mr.left + ms.cx;
    theme::DrawText(di->hDC, mr, r.r.selected ? L"[x] " : L"[ ] ", theme::Font::MonoSmall, dpi,
                    r.r.selected ? pal.accent : pal.textDim,
                    DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    t.left = mr.right;

    // The GPU column, right-aligned so the names line up down the list. A row on the MAIN GPU says so: once the picker
    // can pin an application there, a bare card name would not tell that pin from any other card. The suffix is added
    // here and never in NameForKey, whose other callers ("Games stay on ...", Apply's question) would read it doubled.
    const std::wstring gpu = FormatRowGpu(
        r.r, [st](const std::wstring& k) {
            const std::wstring n = NameForKey(st->adapters, k);
            if (n.empty()) return std::wstring(L"another GPU");
            return IsMainGpuKey(st->plan, k) ? n + L" (main GPU)" : n;
        });
    const SIZE gs = theme::MeasureText(di->hDC, gpu, theme::Font::UiSmall, dpi);
    RECT gr = t;
    gr.left = (t.right - gs.cx > t.left) ? t.right - gs.cx : t.left;
    COLORREF gpuCol = pal.textSecondary;
    if (r.orphaned) gpuCol = pal.warn;
    else if (!st->targetKey.empty() && r.r.assignedKey == st->targetKey) gpuCol = pal.good;
    else if (r.r.assignedKey.empty()) gpuCol = pal.textDim;
    theme::DrawText(di->hDC, gr, gpu, theme::Font::UiSmall, dpi, gpuCol,
                    DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    t.right = gr.left - theme::Dp(12, dpi);

    std::wstring label = r.r.exeName;
    if (!r.where.empty()) label += L"  (" + r.where + L")";
    if (r.r.isProfileGame)  label += L"   (a game in your profiles)";
    else if (!r.note.empty()) label += L"   " + r.note;
    else if (r.system)      label += L"   (Windows or excluded - not moved automatically)";
    COLORREF nameCol = pal.textPrimary;
    if (r.system || r.r.isProfileGame) nameCol = pal.textSecondary;
    if (t.right > t.left) {
        theme::DrawText(di->hDC, t, label, theme::Font::MonoSmall, dpi, nameCol,
                        DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
    }
}

void LayoutGpu(HWND hwnd, GpuState* st) {
    RECT rc;
    GetClientRect(hwnd, &rc);
    const int dpi = st->dpi;
    const int GT = theme::Dp(8, dpi);
    const int ROW = theme::Dp(30, dpi);
    const int LINE = theme::Dp(40, dpi);   // two wrapped lines of UiBody: the least the plan and status lines get

    // NO PADDING OF ITS OWN. The panel sits inside a Settings card that is already inset by the card padding, so the
    // 16 dp margin the separate window kept would double it and cost the button row its room.
    const int x = rc.left;
    int y = rc.top;
    int w = rc.right - rc.left;
    if (w < 0) w = 0;

    // One DC for every measurement below: the two wrapping lines, and the bulk caption.
    const HDC mdc = GetDC(hwnd);
    const int planH = WrappedHeight(st->hPlan, mdc, w, LINE);
    MoveWindow(st->hPlan, x, y, w, planH, TRUE);
    y += planH + GT;

    const int lblW = theme::Dp(210, dpi);
    MoveWindow(st->hTargetLbl, x, y + theme::Dp(6, dpi), lblW, theme::Dp(20, dpi), TRUE);
    // A drop-down combo's window height is its OPEN height; the closed field is the item height.
    MoveWindow(st->hTarget, x + lblW, y, theme::Dp(320, dpi), theme::Dp(220, dpi), TRUE);
    y += ROW + GT;

    const int statusH = WrappedHeight(st->hStatus, mdc, w, LINE);
    MoveWindow(st->hStatus, x, y, w, statusH, TRUE);
    y += statusH + GT;

    const int btnY = rc.bottom - ROW;
    // The full path of the row under the cursor sits between the list and the buttons.
    const int pathY = btnY - GT - LINE;
    int listH = pathY - GT - y;
    if (listH < theme::Dp(80, dpi)) listH = theme::Dp(80, dpi);
    MoveWindow(st->hList, x, y, w, listH, TRUE);
    MoveWindow(st->hPath, x, pathY, w, LINE, TRUE);

    // THE BULK CAPTION IS MEASURED, NOT ASSUMED TO FIT. [M] "Auto assign GPU for Gaming" is 150 px of UiBody at 96 dpi
    // and 303 px at 192 dpi, against the old Dp(176) button's 156 and 312 px of text room once DrawButton's Dp(10) a
    // side is taken off: 6-9 px spare at every dpi, one font substitution away from DT_END_ELLIPSIS. So the button is
    // the measured caption plus that padding and Dp(8), never narrower than before; Remove assignment shares the width.
    // With no DC to measure in, Dp(190) - 20-45 px of spare room at the measured dpis.
    int bw = theme::Dp(176, dpi);
    const LONG text = mdc ? theme::MeasureText(mdc, kBulkCaption, theme::Font::UiBody, dpi).cx : 0;
    if (mdc) ReleaseDC(hwnd, mdc);
    const int need = text > 0 ? static_cast<int>(text) + 2 * theme::Dp(10, dpi) + theme::Dp(8, dpi) : theme::Dp(190, dpi);
    if (need > bw) bw = need;
    const int bn = theme::Dp(116, dpi);

    // THE RIGHT-HAND GROUP IS PLACED FIRST, BECAUSE THE LEFT GROUP'S ROOM IS WHATEVER IT LEAVES.
    //
    // 🔴 THE ROW DID NOT FIT ONCE SELECT ALL JOINED IT, AND THE OVERLAP WAS INVISIBLE AT THE DEFAULT SIZE. [M] At the
    // panel's MINIMUM width - 824 px at 96 dpi, from Settings' Dp(880) client minimum less its kGap and kCardPad - the
    // three old buttons ended at 488 and Apply began at 584, so a fourth Dp(116) button plus its Dp(8) gap wanted 124
    // px of the 96 that were free: 28 px of overlap at 96 dpi, 62 at 192. At the DEFAULT width it fits with room to
    // spare, which is exactly why looking at the panel would have passed it. Raising Dp(880) is closed (settings.cpp:
    // 900 leaves zero room on a 1366-wide screen, and three files would have to move together), so the row absorbs it
    // in its own arithmetic instead, the way settings.cpp's Reset/Add/Remove row shares its remainder.
    const int rx = x + w;
    const int applyX = rx - bn - GT - bn;
    MoveWindow(st->hClose, rx - bn, btnY, bn, ROW, TRUE);
    MoveWindow(st->hApply, applyX, btnY, bn, ROW, TRUE);

    // Select all and Deselect all SHARE what the row has left after the two wide buttons and the three gaps between
    // the four - at most the width Deselect all had alone, and never narrower than a readable caption. At the default
    // width the min() is inert and every button keeps the width it had before Select all existed.
    const int leftEdge = applyX - GT;   // the left group's right edge may never pass this
    int bs = (std::min)(bn, (leftEdge - x - 2 * bw - 3 * GT) / 2);
    bs = (std::max)(bs, theme::Dp(76, dpi));

    // 🔴 AND A HARD CLAMP THE PRECEDENT DOES NOT HAVE. The readable floor above can win on a panel narrower than
    // anything Settings will hand us, and an arithmetic that "cannot" fail is exactly the kind that ships an overlap.
    // No button of this group may start or end past `leftEdge`, so a row that genuinely cannot fit ellipsizes its
    // captions - theme::DrawButton already does that with DT_END_ELLIPSIS - instead of sliding under Apply. Each
    // button also starts after the previous one ENDS, so two of them can never overlap each other either.
    const HWND leftRow[4] = { st->hBulk, st->hSelAll, st->hClearSel, st->hRemove };
    const int leftW[4] = { bw, bs, bs, bw };
    int bx = x;
    for (int i = 0; i < 4; ++i) {
        if (bx > leftEdge) bx = leftEdge;
        int cw = leftW[i];
        if (bx + cw > leftEdge) cw = leftEdge - bx;
        if (cw < 0) cw = 0;
        MoveWindow(leftRow[i], bx, btnY, cw, ROW, TRUE);
        bx += cw + GT;
    }
    InvalidateRect(hwnd, nullptr, TRUE);
}

// Every control gets the body font at the panel's current dpi. WM_CREATE and WM_DPICHANGED_AFTERPARENT share this.
void ApplyPanelFont(GpuState* st) {
    HWND all[] = { st->hPlan, st->hTargetLbl, st->hTarget, st->hStatus, st->hList, st->hPath,
                   st->hBulk, st->hSelAll, st->hClearSel, st->hRemove, st->hApply, st->hClose };
    for (size_t i = 0; i < sizeof(all) / sizeof(all[0]); ++i)
        if (all[i]) SendMessageW(all[i], WM_SETFONT, reinterpret_cast<WPARAM>(st->font), TRUE);
}

// The .pending records earlier changes left behind, oldest first, each with whether its .reg exists and reads as a complete
// restore file - read from the disk NOW. ONE DISCOVERY, READ BY TWO: LoadGpuData on every visit to the tab, and the
// unfinished-change notice when it arrives, so the notice names the files that exist when it is shown, not the ones that
// existed when the visit began (Council round 2, v0.5.6).
std::vector<UnfinishedRecord> DiscoverUnfinished() {
    std::vector<UnfinishedRecord> out;
    const std::vector<std::wstring> pending = UnfinishedGpuRestores(GetConfigDir());
    for (size_t i = 0; i < pending.size(); ++i) {
        UnfinishedRecord u;
        u.pendingPath = pending[i];
        const std::wstring reg = pending[i].substr(0, pending[i].size() - 8) + L".reg";   // the name ends ".pending"
        u.regExists = GetFileAttributesW(reg.c_str()) != INVALID_FILE_ATTRIBUTES;
        std::wstring regText;
        u.regValid = u.regExists && ReadRestoreFileText(reg, regText) && IsCompleteRegRestoreText(regText);
        out.push_back(u);
    }
    return out;
}

// The GPU the picker's selection names, through the candidate index its item carries (FillTargets): empty when nothing is
// selected or the lookup fails.
std::wstring PickedCandidate(const GpuState* st) {
    const int sel = static_cast<int>(SendMessageW(st->hTarget, CB_GETCURSEL, 0, 0));
    // CB_GETITEMDATA answers CB_ERR on failure, which as a size_t is never in range.
    const size_t idx = (sel >= 0)
        ? static_cast<size_t>(SendMessageW(st->hTarget, CB_GETITEMDATA, static_cast<WPARAM>(sel), 0))
        : st->candidates.size();
    return idx < st->candidates.size() ? st->candidates[idx] : std::wstring();
}

// THE PICKER'S SELECTION BECOMES THE TARGET, AND EVERY TICK GOES. A FAILED LOOKUP IS NOT "NO CHANGE": the picker then shows
// something that is not known to be the old target, so the old target is dropped (adversarial review, round 3). A tick made
// for a different GPU is not a tick for this one, either way. CBN_SELCHANGE's whole path, and the one a closed-up picker
// takes when it shows another GPU than the target.
void TakePickedTarget(GpuState* st) {
    st->targetKey = PickedCandidate(st);
    for (size_t i = 0; i < st->rows.size(); ++i) Untick(st->rows[i]);
    Redraw(st);
}

LRESULT CALLBACK GpuProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    GpuState* st = StateOf(hwnd);

    switch (msg) {
        case WM_NCCREATE: {
            // 🔴 ONE OWNER FOR THE STATE: THIS WINDOW. It is made here and freed in WM_NCDESTROY, and nothing else
            // deletes it. v0.5.5's creator deleted it when CreateWindowExW failed - but that call can fail AFTER this
            // message, WM_NCDESTROY is still delivered, and the state was freed twice (adversarial review, v0.5.5). A
            // creator that never deletes cannot repeat that.
            GpuState* made = new (std::nothrow) GpuState();
            if (!made) return FALSE;   // creation stops here, and CreateGpuPanel logs it
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(made));
            return DefWindowProcW(hwnd, msg, wp, lp);
        }
        case WM_CREATE: {
            st = StateOf(hwnd);
            if (!st) return -1;
            // Set before any control exists: the list and the picker ask WM_MEASUREITEM while they are being created.
            st->dpi = DpiOf(hwnd);
            st->font = theme::GetFont(theme::Font::UiBody, st->dpi);
            // The panel sits on a Settings card, so its statics and buttons erase to the card rather than to the app
            // background theme::OnCtlColor assumes.
            st->cardBrush = CreateSolidBrush(theme::P().cardBg);

            st->hPlan = Mk(hwnd, L"STATIC", L"", SS_LEFT, -1);
            st->hTargetLbl = Mk(hwnd, L"STATIC", L"Assign ticked apps to:", SS_LEFT, -1);
            st->hTarget = Mk(hwnd, L"COMBOBOX", L"",
                             CBS_DROPDOWNLIST | CBS_OWNERDRAWFIXED | CBS_HASSTRINGS |
                                 WS_VSCROLL | WS_TABSTOP,
                             IDC_GPU_TARGET);
            st->hStatus = Mk(hwnd, L"STATIC", L"", SS_LEFT, -1);
            st->hList = Mk(hwnd, L"LISTBOX", L"",
                           LBS_NOTIFY | LBS_WANTKEYBOARDINPUT | LBS_HASSTRINGS | LBS_OWNERDRAWFIXED | LBS_NOINTEGRALHEIGHT |
                               WS_VSCROLL | WS_TABSTOP,
                           IDC_GPU_LIST);
            st->hPath = Mk(hwnd, L"EDIT", L"", ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL, IDC_GPU_PATH);
            st->hBulk = Mk(hwnd, L"BUTTON", kBulkCaption, BS_OWNERDRAW | WS_TABSTOP, IDC_GPU_BULK);
            // 🔴 THE ORDER OF THESE Mk() CALLS IS THE TAB ORDER. Win32 tab order is Z-order, which is creation order
            // (the same rule settings.cpp records where it creates the pages' controls), so Select all is created
            // immediately before Deselect all and the pair is reached by Tab in the order the row reads.
            st->hSelAll = Mk(hwnd, L"BUTTON", L"Select all", BS_OWNERDRAW | WS_TABSTOP, IDC_GPU_SELALL);
            // Renamed from "Clear selection" in v0.5.9 so the pair reads together (operator request).
            st->hClearSel = Mk(hwnd, L"BUTTON", L"Deselect all", BS_OWNERDRAW | WS_TABSTOP,
                               IDC_GPU_CLEARSEL);
            st->hRemove = Mk(hwnd, L"BUTTON", L"Remove assignment", BS_OWNERDRAW | WS_TABSTOP,
                             IDC_GPU_REMOVE);
            st->hApply = Mk(hwnd, L"BUTTON", L"Apply", BS_OWNERDRAW | WS_TABSTOP, IDC_GPU_APPLY);
            st->hClose = Mk(hwnd, L"BUTTON", L"Cancel", BS_OWNERDRAW | WS_TABSTOP, IDC_GPU_CLOSE);

            // 🔴 A CONTROL THAT WAS NOT CREATED IS NOT AN EMPTY CONTROL. SendMessage to a null handle
            // answers 0, which reads as success - so a list box that never existed would have marked
            // every row as listed (adversarial review, round 3). Without every control it needs, the
            // panel shows its reason and offers nothing. Decided here, once, before the font pass can
            // overwrite GetLastError; ActivateGpuPanel starts every refresh from this.
            st->controlsBroken = !st->hPlan || !st->hTargetLbl || !st->hTarget || !st->hStatus || !st->hList || !st->hPath ||
                                 !st->hBulk || !st->hSelAll || !st->hClearSel || !st->hRemove || !st->hApply || !st->hClose;
            st->broken = st->controlsBroken;
            if (st->controlsBroken) {
                LogLine(L"[gpu] the GPU Assignment panel could not create its controls, gle=%lu", GetLastError());
                return -1; // WM_NCDESTROY owns cleanup; Settings displays its existing failure message.
            }

            ApplyPanelFont(st);

            // NOTHING IS READ HERE. The panel is created with Settings, on whichever tab Settings opens; the adapters,
            // the preferences, the rows and the unfinished-change notice wait for ActivateGpuPanel, when the tab is
            // shown. A child has no title bar, so a panel that could not be built says so in the log above and on its
            // plan line.
            LayoutGpu(hwnd, st);
            Redraw(st);   // every action starts disabled
            return 0;
        }
        case WM_SIZE:
            if (st) LayoutGpu(hwnd, st);
            return 0;
        case WM_CLOSE:
            // 🔴 A CHILD PANEL IS NEVER CLOSED. [M] A read-only multiline edit in dialog mode answers Esc by POSTING
            // WM_CLOSE to its parent, and DefWindowProc destroys the window it reaches: Esc in the full-path box
            // destroyed this panel and every control on the tab. GpuPanelKey now takes Esc before the edit sees it;
            // this stays for any other sender, which gets nothing.
            return 0;
        case WM_ERASEBKGND: {
            // The CARD's colour: Settings draws a card behind this panel, and an appBg fill would stamp a dark
            // rectangle into it.
            RECT rc;
            GetClientRect(hwnd, &rc);
            if (st && st->cardBrush) FillRect(reinterpret_cast<HDC>(wp), &rc, st->cardBrush);
            else theme::FillBackground(reinterpret_cast<HDC>(wp), rc);
            return 1;
        }
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(hwnd, &ps);
            if (st && st->hTarget && IsWindowVisible(st->hTarget)) {
                RECT r;
                GetWindowRect(st->hTarget, &r);
                MapWindowPoints(nullptr, hwnd, reinterpret_cast<POINT*>(&r), 2);
                r.bottom = r.top + static_cast<int>(SendMessageW(st->hTarget, CB_GETITEMHEIGHT,
                                                                 static_cast<WPARAM>(-1), 0)) +
                           theme::Dp(6, st->dpi);
                theme::OverdrawComboFrame(dc, r, st->dpi, GetFocus() == st->hTarget);
            }
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_MEASUREITEM: {
            MEASUREITEMSTRUCT* mi = reinterpret_cast<MEASUREITEMSTRUCT*>(lp);
            const int dpi = st ? st->dpi : DpiOf(hwnd);
            if (mi->CtlType == ODT_LISTBOX) { mi->itemHeight = static_cast<UINT>(theme::Dp(24, dpi)); return TRUE; }
            if (mi->CtlType == ODT_COMBOBOX) { mi->itemHeight = static_cast<UINT>(theme::Dp(22, dpi)); return TRUE; }
            break;
        }
        case WM_DRAWITEM: {
            const DRAWITEMSTRUCT* di = reinterpret_cast<const DRAWITEMSTRUCT*>(lp);
            if (!st) break;
            if (di->CtlType == ODT_LISTBOX) { DrawRow(st, di); return TRUE; }
            if (di->CtlType == ODT_COMBOBOX) return theme::DrawComboBox(di, st->dpi);
            if (di->CtlType == ODT_BUTTON) {
                theme::ButtonKind k = theme::ButtonKind::Secondary;
                if (di->hwndItem == st->hApply || di->hwndItem == st->hBulk) k = theme::ButtonKind::Primary;
                else if (di->hwndItem == st->hRemove) k = theme::ButtonKind::Danger;
                else if (di->hwndItem == st->hClearSel || di->hwndItem == st->hSelAll) k = theme::ButtonKind::Ghost;
                return theme::DrawButton(di, k, st->dpi);
            }
            break;
        }
        case WM_CTLCOLORSTATIC:
        case WM_CTLCOLORLISTBOX:
        case WM_CTLCOLOREDIT:
        case WM_CTLCOLORBTN: {
            const HDC dc = reinterpret_cast<HDC>(wp);
            const HWND ctl = reinterpret_cast<HWND>(lp);
            // 🔴 THE FULL-PATH BOX ASKS AS A STATIC. A read-only EDIT sends WM_CTLCOLORSTATIC, not WM_CTLCOLOREDIT,
            // so without naming it here it would take the card colour and stop reading as a field. It keeps the input
            // surface the list box has.
            const bool input = msg == WM_CTLCOLORLISTBOX || msg == WM_CTLCOLOREDIT || (st && ctl == st->hPath);
            HBRUSH b = theme::OnCtlColor(input ? static_cast<UINT>(WM_CTLCOLOREDIT) : msg, dc, ctl);
            // Every other static and button sits ON the card - the same rule as settings.cpp's WM_CTLCOLORSTATIC.
            if (!input && b && st && st->cardBrush) {
                SetBkColor(dc, theme::P().cardBg);
                b = st->cardBrush;
            }
            if (b) return reinterpret_cast<LRESULT>(b);
            break;
        }
        case WM_DPICHANGED_AFTERPARENT: {
            // A CHILD IS NEVER SENT WM_DPICHANGED. Settings re-lays itself out on its own and moves this panel, but
            // without this the font, the fixed owner-draw item heights, DrawRow's metrics and LayoutGpu would all keep
            // the dpi the panel was created at. Whichever of Settings' move and this message comes first, this one
            // lays the panel out again at the new dpi.
            if (!st) break;
            st->dpi = DpiOf(hwnd);
            st->font = theme::GetFont(theme::Font::UiBody, st->dpi);   // theme-cached: the old handle is not ours to free
            ApplyPanelFont(st);
            // After the font, which a combo may re-measure from; these are the same heights WM_MEASUREITEM gives.
            if (st->hList) SendMessageW(st->hList, LB_SETITEMHEIGHT, 0, MAKELPARAM(theme::Dp(24, st->dpi), 0));
            if (st->hTarget) {
                SendMessageW(st->hTarget, CB_SETITEMHEIGHT, static_cast<WPARAM>(-1), theme::Dp(22, st->dpi));
                SendMessageW(st->hTarget, CB_SETITEMHEIGHT, 0, theme::Dp(22, st->dpi));
            }
            LayoutGpu(hwnd, st);
            InvalidateRect(hwnd, nullptr, TRUE);
            return 0;
        }
        case WM_VKEYTOITEM: {
            // SPACE TICKS THE ROW UNDER THE CARET - the keyboard route to what double-click does.
            // Without it a row could be ticked only with a mouse. Every other key keeps the list
            // box's own handling (-1); a handled Space returns -2 so the list box does nothing more.
            if (st && reinterpret_cast<HWND>(lp) == st->hList && LOWORD(wp) == VK_SPACE) {
                // As at LBN_DBLCLK: the list stays enabled during a run, so this toggle refuses for itself (v0.5.9).
                if (g_running != 0) return -2;
                const int caret = static_cast<int>(HIWORD(wp));
                const LRESULT data = SendMessageW(st->hList, LB_GETITEMDATA, static_cast<WPARAM>(caret), 0);
                const size_t idx = (data != LB_ERR) ? static_cast<size_t>(data) : st->rows.size();
                if (!st->broken && idx < st->rows.size() && st->rows[idx].listed) {
                    if (st->rows[idx].r.selected) Untick(st->rows[idx]);
                    else Tick(st->rows[idx]);
                    Redraw(st);
                }
                return -2;
            }
            return -1;
        }
        case WM_COMMAND: {
            if (!st) break;
            const int id = LOWORD(wp);
            const int code = HIWORD(wp);

            // TOGGLE ON DOUBLE-CLICK, NOT ON SELECTION. LBN_SELCHANGE also fires for arrow-key
            // navigation, so toggling on it would flip a row every time the user walked the list.
            if (id == IDC_GPU_LIST && code == LBN_DBLCLK) {
                // The list box stays ENABLED during a run so it can be read and scrolled, so its toggle is the one
                // list path that has to refuse for itself (v0.5.9). A run's plan is frozen, but a tick changed
                // under it would leave the tab claiming a state the run never acted on.
                if (g_running != 0) return 0;
                const int sel = static_cast<int>(SendMessageW(st->hList, LB_GETCURSEL, 0, 0));
                if (sel >= 0) {
                    const size_t idx = static_cast<size_t>(
                        SendMessageW(st->hList, LB_GETITEMDATA, static_cast<WPARAM>(sel), 0));
                    if (!st->broken && idx < st->rows.size() && st->rows[idx].listed) {
                        if (st->rows[idx].r.selected) Untick(st->rows[idx]);
                        else Tick(st->rows[idx]);
                        Redraw(st);
                    }
                }
                return 0;
            }
            if (id == IDC_GPU_LIST && code == LBN_SELCHANGE) {
                UpdatePath(st);
                return 0;
            }
            if (id == IDC_GPU_TARGET && code == CBN_SELCHANGE) {
                TakePickedTarget(st);
                return 0;
            }
            // 🔴 A PICKER THAT CLOSES SHOWING ANOTHER GPU THAN THE TARGET TAKES THAT GPU (Council round 2, v0.5.6). The target
            // used to change only on CBN_SELCHANGE. Choose one GPU, open the picker, press Up to preview another, then Esc:
            // if Windows put the old selection back without a CBN_SELCHANGE, the target stayed on the previewed GPU while the
            // picker showed the old one - a mismatch Apply's TargetStillShown refused, but one the status line, the bulk
            // button and the counts did not show. So when the list closes, the selection is read again: a different GPU runs
            // CBN_SELCHANGE's own path (TakePickedTarget - the target, the ticks, the buttons, the lines), and the same GPU
            // changes nothing. A target DropTarget cleared counts as different, so opening and closing the picker takes the
            // GPU it shows; that is still a GPU of `candidates`, read from the control, and Apply checks it again before it
            // writes.
            // 🔴 CBN_CLOSEUP ONLY, NEVER CBN_SELENDCANCEL (Council round 2 fix check, v0.5.6). Windows sends CBN_SELENDCANCEL
            // when focus merely passes through a picker nobody opened, so reading the selection on it made tabbing past the
            // picker after DropTarget take the GPU it showed, and clear the ticks made since, with no GPU chosen by the user.
            // [S: that check's native probes, not re-run by this change] Focus into and out of a closed picker sent
            // CBN_SELENDCANCEL and CBN_KILLFOCUS and no CBN_CLOSEUP; every close of an open list - Esc, Enter, a mouse pick,
            // focus leaving it - sent CBN_CLOSEUP; Esc kept the previewed selection; CB_RESETCONTENT and a refill of a closed
            // picker sent nothing. The chair's live run tabs through the picker after a dropped target.
            if (id == IDC_GPU_TARGET && code == CBN_CLOSEUP) {
                if (PickedCandidate(st) != st->targetKey) TakePickedTarget(st);
                return 0;
            }
            if (id == IDC_GPU_TARGET && (code == CBN_SETFOCUS || code == CBN_KILLFOCUS)) {
                InvalidateRect(hwnd, nullptr, TRUE);   // the frame drawn around it shows focus
                return 0;
            }
            if (code != BN_CLICKED) break;
            // 🔴 EVERY ONE OF THESE REFUSES AT THE MESSAGE WHILE A RUN IS GOING, NOT ONLY AT THE BUTTON (v0.5.9).
            // DISABLING A BUTTON DOES NOT TAKE BACK A CLICK ALREADY IN THE QUEUE, and a disabled owner does not stop
            // a POSTED message either: MessageBoxW runs its own modal loop, which dispatches that click straight
            // into DoApply while Apply's own question is still open. GpuPanelKey's Enter and Esc are posted the same
            // way and arrive by the same route. SyncButtons' freeze is what stops the SECOND click; this is what
            // stops the one that was already on its way.
            switch (id) {
                case IDC_GPU_BULK:     if (g_running != 0) return 0; DoBulk(st, hwnd); return 0;
                case IDC_GPU_SELALL:   if (g_running != 0) return 0; DoSelectAll(st); return 0;
                case IDC_GPU_CLEARSEL:
                    if (g_running != 0) return 0;
                    for (size_t i = 0; i < st->rows.size(); ++i) Untick(st->rows[i]);
                    Redraw(st);
                    return 0;
                case IDC_GPU_REMOVE:   if (g_running != 0) return 0; DoRemove(st, hwnd); return 0;
                case IDC_GPU_APPLY:    if (g_running != 0) return 0; DoApply(st, hwnd); return 0;
                case IDC_GPU_CLOSE:
                    // Cancel unticks the rows, so a run walking them must not see one vanish mid-loop.
                    if (g_running != 0) return 0;
                    // CANCEL UNTICKS AND HANDS BACK; IT NEVER DESTROYS. The separate window closed itself here; a tab
                    // that did the same would stay empty until Settings was opened again. A tick writes nothing, so
                    // there is nothing else to undo. Where to go next is Settings' call - the tab the user came from -
                    // so the panel only tells its parent, and touches nothing once that call returns.
                    for (size_t i = 0; i < st->rows.size(); ++i) Untick(st->rows[i]);
                    Redraw(st);
                    SendMessageW(GetParent(hwnd), WM_COMMAND, MAKEWPARAM(GetDlgCtrlID(hwnd), GPUN_CANCEL),
                                 reinterpret_cast<LPARAM>(hwnd));
                    return 0;
                default: break;
            }
            break;
        }
        case WM_GPU_UNFINISHED: {
            if (!st) break;
            // 🔴 A CHANGE THAT STOPPED PART-WAY IS SAID WHEN THIS TAB IS NEXT SHOWN (adversarial review, round 5) - once
            // per set of leftover records, not on every visit. Its .reg lists only what really changed; the .pending
            // record lists everything planned. It names only the files that exist - see FormatUnfinishedNotice.
            //
            // 🔴 ONLY OVER THIS TAB, AND RECORDED AS SAID ONLY WHEN IT IS SAID (Council review, v0.5.6). ActivateGpuPanel
            // posts this, and it can arrive after the user has left the tab: it is then not shown over another page, and
            // since nothing was recorded, the next visit posts it again. It is recorded before the box opens - a copy
            // dispatched by the box's own message loop must not open a second box - and put back if no box appeared.
            //
            // 🔴 THE RECORDS ARE READ FROM THE DISK AGAIN HERE, NOT TAKEN FROM THE VISIT THAT POSTED THIS (Council round 2,
            // v0.5.6). The notice used to speak for the list ActivateGpuPanel read, so a record deleted before this arrived
            // was still named, and a .reg that had become unreadable was still recommended. Now the fresh set is what is
            // compared, formatted and recorded: records deleted meanwhile are not named, and when none is left - or the
            // fresh set is the one already shown, a second copy of this message included - nothing is shown.
            if (!IsWindowVisible(hwnd)) return 0;
            st->unfinished = DiscoverUnfinished();
            std::vector<std::wstring> pendingNow;
            for (size_t i = 0; i < st->unfinished.size(); ++i) pendingNow.push_back(st->unfinished[i].pendingPath);
            if (!ShouldPostUnfinished(st->unfinishedShown, pendingNow)) return 0;
            const std::wstring text = FormatUnfinishedNotice(st->unfinished);
            if (text.empty()) return 0;
            const std::vector<std::wstring> shownBefore = st->unfinishedShown;
            MarkUnfinishedShown(st->unfinishedShown, pendingNow);
            int answer = 0;
            if (!PanelMessageBox(st, hwnd, text.c_str(), MB_OK | MB_ICONWARNING, &answer)) return 0;
            if (answer == 0) st->unfinishedShown = shownBefore;   // MessageBoxW could not show it: said on the next visit
            return 0;
        }
        case WM_NCDESTROY: {
            // THE STATE IS DETACHED HERE, AND FREED ONCE - see WM_NCCREATE, FreeState and PanelLifetime (gpu_edit.h).
            // Detached first, so nothing that runs after this finds a freed pointer through the handle; freed now unless
            // a message box is still open for the panel, whose return then frees it and ends the action that opened it.
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
            if (st && DetachPanel(st->life)) FreeState(st);
            return DefWindowProcW(hwnd, msg, wp, lp);
        }
        default: break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void RegisterGpuClass() {
    static bool done = false;
    if (done) return;
    WNDCLASSEXW wc;
    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = GpuProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;   // WM_ERASEBKGND paints the card colour; no light flash on show
    wc.lpszClassName = kGpuClass;
    RegisterClassExW(&wc);
    done = true;
}

// THE DATA BLOCK the separate window read once per opening, now read on every activation of the tab. Every field it
// fills is rebuilt from nothing, so one activation never inherits another's adapters, rows or records.
void LoadGpuData(GpuState* st, const Config& cfg, const ProcessSnapshot& snap) {
    // A TICK SURVIVES A REFRESH ONLY FOR THE SAME PROGRAM, THE SAME GPU AND THE ROW AS IT WAS TICKED (gpuwindow.h).
    // Remembered by full path, because the rows are rebuilt and their order can change, with what the row said then - read
    // here, before the plan below is replaced, so a main-GPU pin is judged with the main GPU of that visit. The rules are
    // KeepsPickedTarget and KeptTicks in gpu_rows.h, where the unit suite can reach them (AJ39).
    const std::wstring oldTarget = st->targetKey;
    std::vector<TickedRowFacts> ticked;
    std::set<std::wstring> automaticPaths;
    for (const auto& row : st->rows)
        if (row.r.selected && row.autoSelected) automaticPaths.insert(ToLower(row.r.exePath));
    for (size_t i = 0; i < st->rows.size(); ++i)
        if (st->rows[i].listed && st->rows[i].r.selected)
            ticked.push_back(FactsOfRow(st->rows[i].r, st->rows[i].listedChoice, st->plan.gameKey));

    st->cudaEnabled = cfg.setCudaGpu;
    std::wstring err;
    const bool adaptersRead = EnumerateGpuAdapters(st->adapters, &err);
    if (!adaptersRead) st->adapters.clear();
    st->plan = PlanGpuIsolation(st->adapters);
    // THE REAL REASON, NOT "FEWER THAN 2 GPUS" (adversarial review, round 4).
    if (!adaptersRead) st->plan.why = err.empty() ? std::wstring(L"the graphics adapters could not be read") : err;

    bool prefsComplete = true;
    // Whether the read the ticks above were kept or made on was complete - taken before this read replaces it (KeptTicks).
    const bool previousPrefsComplete = st->prefsComplete;
    // THE VALUES AS READ, AND THE PAIRS EVERYTHING ELSE WORKS FROM - only values that hold a GPU choice (GpuChoicePairs).
    // A value with only Windows' own fields reads as no value, or it hides a pin lost to an update (Council review, v0.5.6).
    const std::vector<GpuPreferenceEntry> entries = EnumerateGpuPreferenceEntries(&prefsComplete);
    const std::vector<std::pair<std::wstring, std::wstring> > reg = GpuChoicePairs(entries);
    st->prefsComplete = prefsComplete;
    st->prefPairs = reg;
    st->unfinished = DiscoverUnfinished();
    st->candidates = PickerTargetKeys(st->adapters, st->plan);
    // THE DEFAULT IS STILL ChooseBackgroundKey's, WHICH IS NEVER THE MAIN GPU (AJ2, AJ3). A GPU picked on an earlier
    // visit stays picked while the picker still offers it, so moving between tabs does not quietly put the picker back
    // on the default and drop the ticks made for the GPU the user chose.
    st->targetKey = KeepsPickedTarget(oldTarget, st->candidates) ? oldTarget
                                                                 : ChooseBackgroundKey(st->adapters, st->plan, reg);
    st->unreadable = 0;
    st->rows = BuildRows(cfg, snap, entries, st->unreadable);
    MarkOrphans(st, reg);
    // After MarkOrphans, so a main-GPU pin newly found lost to an update is part of what the rebuilt row says.
    std::vector<TickedRowFacts> rebuilt;
    for (size_t i = 0; i < st->rows.size(); ++i)
        rebuilt.push_back(FactsOfRow(st->rows[i].r, st->rows[i].listedChoice, st->plan.gameKey));
    // 🔴 AN INCOMPLETE READ OF WINDOWS' GPU PREFERENCES AFTER A COMPLETE ONE KEEPS NO TICK (Council round 2, v0.5.6): a kept
    // tick may have come from Auto assign, and this read could have left out the main-GPU pin that would have dropped it.
    // After an incomplete read every tick was made by hand, and the usual rules keep it. KeptTicks decides.
    const std::vector<bool> kept = KeptTicks(oldTarget, st->candidates, ticked, rebuilt, previousPrefsComplete, prefsComplete);
    std::vector<GpuRow> plain;
    for (const auto& row : st->rows) plain.push_back(row.r);
    const std::vector<GpuRow> automatic = SelectForAutoAssign(plain, st->targetKey, st->plan.gameKey,
                                                            prefsComplete, reg);
    for (size_t i = 0; i < st->rows.size(); ++i) {
        Row& row = st->rows[i];
        row.autoSelected = automaticPaths.count(ToLower(row.r.exePath)) != 0;
        row.r.selected = kept[i] && (!row.autoSelected || (!row.system && automatic[i].selected));
        if (!row.r.selected) Untick(row);   // a tick that did not survive takes its origin with it
    }
}

}  // namespace

HWND CreateGpuPanel(HWND parent, int id) {
    RegisterGpuClass();
    // HIDDEN, AND NO WS_TABSTOP ON THE PANEL ITSELF. Settings shows it with its tab; Tab must land on the controls
    // inside it, which Settings' IsDialogMessageW reaches only through WS_EX_CONTROLPARENT - never on the bare panel.
    HWND hwnd = CreateWindowExW(WS_EX_CONTROLPARENT, kGpuClass, L"", WS_CHILD | WS_CLIPCHILDREN, 0, 0, 10, 10, parent,
                                reinterpret_cast<HMENU>(static_cast<UINT_PTR>(id)), GetModuleHandleW(nullptr), nullptr);
    // No delete here on failure: WM_NCDESTROY is the state's one owner, and it runs even when creation fails late.
    if (!hwnd) LogLine(L"[gpu] the GPU Assignment panel could not be created, gle=%lu", GetLastError());
    return hwnd;
}

void ActivateGpuPanel(HWND panel, const Config& cfg, const ProcessSnapshot& snap) {
    GpuState* st = panel ? StateOf(panel) : nullptr;
    if (!st) return;
    const ULONGLONG started = GetTickCount64();

    LoadGpuData(st, cfg, snap);
    // BROKEN STARTS FROM WHAT CREATION LEFT: a list or picker that could not be filled safely on an earlier visit is
    // tried again, instead of disabling the tab for the rest of the Settings session. FillTargets and FillList still
    // set it on a failure of their own.
    st->broken = st->controlsBroken;
    FillTargets(st);
    // NO TARGET ON SCREEN, NO TICKS KEPT FOR ONE: a tick is made for a GPU, exactly as when the picker changes.
    if (st->targetKey.empty())
        for (size_t i = 0; i < st->rows.size(); ++i) Untick(st->rows[i]);
    FillList(st);
    LayoutGpu(panel, st);
    SyncButtons(st);

    // The cost of showing this tab: everything above runs on the UI thread, inside Settings' page switch.
    LogLine(L"[gpu] panel refreshed in %llu ms (%zu rows)", GetTickCount64() - started, st->rows.size());

    // THE UNFINISHED-CHANGE NOTICE, ONCE PER SET OF RECORDS - the rule is ShouldPostUnfinished in gpu_rows.h (AJ39).
    // Posted, so it shows over the tab once Settings has finished switching to it. Nothing is recorded as said here - the
    // handler records it when its box appears - so a post that fails is logged and simply made again on the next visit.
    std::vector<std::wstring> pendingNow;
    for (size_t i = 0; i < st->unfinished.size(); ++i) pendingNow.push_back(st->unfinished[i].pendingPath);
    if (ShouldPostUnfinished(st->unfinishedShown, pendingNow) && !PostMessageW(panel, WM_GPU_UNFINISHED, 0, 0))
        LogLine(L"[gpu] the unfinished-change notice could not be queued, gle=%lu; the next visit to the tab posts it again",
                GetLastError());
}

void FocusGpuPanel(HWND panel) {
    GpuState* st = panel ? StateOf(panel) : nullptr;
    if (st && st->hList && IsWindowVisible(st->hList)) SetFocus(st->hList);
}

// ENTER AND ESC BELONG TO THE TAB, NOT TO SETTINGS (adversarial review, v0.5.6). Settings' hook asks here before its
// IsDialogMessageW, which would otherwise turn Enter into Settings' OK - save config.ini and close - and Esc into its
// Cancel, and would hand Esc in the multiline path box to the edit, which posts WM_CLOSE to this panel.
//   * Esc is the tab's Cancel, anywhere in the panel: untick, write nothing, back to the tab the user came from -
//     what Esc did to v0.5.5's own window, which closed.
//   * Enter presses the focused button of the panel, as Space already does; anywhere else it does nothing, as it did
//     in v0.5.5. Apply and Remove still ask before they write, defaulting to No.
//   * An open picker handles both keys itself. On close-up, the target follows the GPU the picker actually shows.
//     Enter and Esc are declined here
//     while CB_GETDROPPEDSTATE says the list is down, so IsDialogMessageW offers them to the combo box, which closes its
//     list. [A] That a dropped combo box asks for Enter and Esc through WM_GETDLGCODE is Windows' own behaviour: no file
//     of this product states it and no run of v0.5.6's Council round measured it.
//   * FOCUS ON SETTINGS' TAB BAR IS NOT IN THIS TAB, even while this tab shows: the bar is not the panel or inside it, so
//     the test below declines both keys and IsDialogMessageW gives them to Settings' OK and Cancel, as on every other
//     page. Checked by reading in v0.5.6's Council round, not run: theme.cpp's tab bar answers WM_GETDLGCODE with
//     DLGC_WANTARROWS alone, settings.cpp's IDOK saves config.ini and closes, and its IDCANCEL closes.
// Both are POSTED as the click a mouse would make, so a question Apply opens runs its modal loop from the ordinary
// message loop, never from inside the hook that is answering this key.
//
// 🔴 TAB IN THE FULL-PATH BOX IS TAKEN HERE TOO, OR IT GOES NOWHERE. [M] tabprobe, a window of this exact shape (a plain
// top-level pumped through IsDialogMessageW from a WH_GETMESSAGE hook, a WS_EX_CONTROLPARENT panel, a read-only
// multiline edit): the edit answers WM_GETDLGCODE with 0x008d - it wants every key - so IsDialogMessageW hands Tab to
// it, the edit posts WM_NEXTDLGCTL to its parent, and this panel is not a dialog and ignores it. Tab and Shift+Tab left
// focus in the box, with or without WS_TABSTOP, and a click is enough to put focus there. So Tab there moves to the
// next or previous tab stop of the whole Settings window, as the dialog manager would; GetNextDlgTabItem steps into
// this panel through WS_EX_CONTROLPARENT. [M] the same probe with this rule: Tab reached the next button, Shift+Tab
// the list. Tab anywhere else keeps IsDialogMessageW's own handling.
bool GpuPanelKey(HWND panel, const MSG& msg) {
    if (msg.message != WM_KEYDOWN ||
        (msg.wParam != VK_RETURN && msg.wParam != VK_ESCAPE && msg.wParam != VK_TAB)) return false;
    GpuState* st = panel ? StateOf(panel) : nullptr;
    if (!st || !IsWindowVisible(panel) || (msg.hwnd != panel && !IsChild(panel, msg.hwnd))) return false;
    if (msg.wParam == VK_TAB) {
        if (!st->hPath || GetFocus() != st->hPath) return false;
        const HWND root = GetAncestor(panel, GA_ROOT);
        const HWND next = root ? GetNextDlgTabItem(root, st->hPath, GetKeyState(VK_SHIFT) < 0) : nullptr;
        if (next) SetFocus(next);
        return true;
    }
    if (st->hTarget && SendMessageW(st->hTarget, CB_GETDROPPEDSTATE, 0, 0)) return false;
    HWND press = nullptr;
    if (msg.wParam == VK_ESCAPE) {
        press = st->hClose;
    } else {
        const HWND f = GetFocus();
        if (f && (f == st->hBulk || f == st->hSelAll || f == st->hClearSel || f == st->hRemove || f == st->hApply ||
                  f == st->hClose))
            press = f;
    }
    if (press && IsWindowEnabled(press))
        PostMessageW(panel, WM_COMMAND, MAKEWPARAM(GetDlgCtrlID(press), BN_CLICKED), reinterpret_cast<LPARAM>(press));
    return true;
}

}  // namespace cd
