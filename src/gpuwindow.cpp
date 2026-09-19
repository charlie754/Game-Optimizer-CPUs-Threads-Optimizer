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
    bool controlsBroken = false;            // a control failed to create - decided once, in WM_CREATE
    bool broken = false;                    // a control this panel needs is missing or unusable: nothing is offered
    std::vector<UnfinishedRecord> unfinished;   // .pending records earlier changes left behind, oldest first
    std::vector<std::wstring> unfinishedShown;  // the .pending paths the notice last showed, lower-cased and sorted
    PanelLifetime life;                         // message boxes open for this panel, and whether it is destroyed (gpu_edit.h)

    HWND hPlan = nullptr, hTargetLbl = nullptr, hTarget = nullptr, hStatus = nullptr;
    HWND hList = nullptr, hPath = nullptr;
    HWND hBulk = nullptr, hClearSel = nullptr, hRemove = nullptr, hApply = nullptr, hClose = nullptr;
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

void SyncButtons(GpuState* st) {
    const bool decidable = !st->broken && !st->targetKey.empty();
    // The picker stays usable whenever there is anything to pick, even with no target: a failed lookup
    // clears the target, and the user must still be able to choose again (adversarial review, round 4).
    EnableWindow(st->hTarget, !st->broken && !st->candidates.empty() ? TRUE : FALSE);
    // Off while the main GPU is the target (founder decision, v0.5.6) or Windows' GPU preferences could not all be read
    // (Council round 2), whatever MovableCount says - AutoAssignAllowed, the rule DoBulk refuses on too.
    EnableWindow(st->hBulk, decidable && AutoAssignAllowed(st->prefsComplete, MainGpuTargeted(st)) && MovableCount(st) > 0
                                ? TRUE : FALSE);
    EnableWindow(st->hApply, decidable && AnySelected(st) ? TRUE : FALSE);
    EnableWindow(st->hRemove, RemovableCount(st) > 0 ? TRUE : FALSE);
    EnableWindow(st->hClearSel, AnySelected(st) ? TRUE : FALSE);
}

void Redraw(GpuState* st) {
    // 🔴 NEVER A NULL HANDLE HERE: InvalidateRect(NULL, ...) invalidates and redraws EVERY window on the desktop, and a
    // panel whose list box failed to create still comes through here to show why.
    if (st->hList) InvalidateRect(st->hList, nullptr, TRUE);
    RefreshLines(st);
    SyncButtons(st);
}

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
        if (!st->rows[i].listed) st->rows[i].r.selected = false;
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

// Read the policy evidence afresh without replacing listedChoice: a fresh policy check must
// never make the user's earlier consent apply to a different per-path GPU choice.
std::vector<GpuRow> ReadAutoChoices(GpuState* st) {
    const std::vector<GpuPreferenceEntry> entries = EnumerateGpuPreferenceEntries(&st->prefsComplete);
    st->prefPairs = GpuChoicePairs(entries);
    std::map<std::wstring, std::wstring> choices;
    for (const auto& entry : entries) choices[ToLower(entry.path)] = entry.choiceKey;
    std::vector<GpuRow> live;
    for (const auto& row : st->rows) {
        GpuRow r = row.r;
        r.assignedKey = choices[ToLower(r.exePath)];
        live.push_back(r);
    }
    return SelectForAutoAssign(live, st->targetKey, st->plan.gameKey, st->prefsComplete, st->prefPairs);
}

void DoBulk(GpuState* st, HWND hwnd) {
    const std::vector<GpuRow> picked = ReadAutoChoices(st);
    // The button is disabled while the main GPU is the target, and while Windows' GPU preferences could not all be read;
    // both are refused here as well (AutoAssignAllowed), so a click posted to a disabled button ticks nothing. Said
    // nowhere but the status line, which already gives the reason beside the button.
    if (st->targetKey.empty() || !AutoAssignAllowed(st->prefsComplete, MainGpuTargeted(st))) {
        for (auto& row : st->rows) if (row.autoSelected) row.r.selected = false;
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
    const std::vector<GpuRow> automatic = ReadAutoChoices(st);
    for (size_t i = 0; i < st->rows.size(); ++i) {
        Row& r = st->rows[i];
        if (!r.listed || !r.r.selected || r.r.exePath.empty()) continue;
        if (!removing && r.autoSelected && !automatic[i].selected) {
            r.r.selected = false;
            Refuse(refused, r, L"its automatic selection is no longer safe after reading Windows' GPU settings again; "
                              L"review the row and tick it by hand to change it");
            ++changedSinceListed;
            continue;
        }
        if (removing && r.r.assignedKey.empty()) continue;   // nothing of this feature's to remove
        EditPlan e;
        e.row = i;
        e.item.exePath = r.r.exePath;
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
            r.r.selected = false;
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
void DropTarget(GpuState* st, HWND hwnd) {
    st->targetKey.clear();
    for (size_t i = 0; i < st->rows.size(); ++i) st->rows[i].r.selected = false;
    Redraw(st);
    PanelMessageBox(st, hwnd,
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

// PASS TWO, and every row brought up to date with what the registry now holds. See RunGpuEdits for the order
// that keeps the restore file honest at any point a run can stop.
EditTally RunEdits(GpuState* st, const std::vector<EditPlan>& ready, bool removing, const std::wstring& targetKey,
                   GpuRestoreJournal& journal) {
    std::vector<GpuEditItem> items;
    for (size_t k = 0; k < ready.size(); ++k) items.push_back(ready[k].item);
    GpuEditOps ops;
    ops.write = [](const std::wstring& path, bool expectPresent, const std::wstring& expectValue, bool deleteValue,
                   const std::wstring& value, unsigned long& error) {
        return GuardedWriteGpuPreference(path, expectPresent, expectValue, deleteValue, value, &error);
    };
    ops.read = [](const std::wstring& path, std::wstring& value, bool& unreadable) {
        return ReadGpuPreference(path, value, &unreadable);
    };
    ops.record = [&journal](const GpuPreferenceBefore& row) { return RecordGpuRestoreRow(journal, row); };
    ops.afterRow = TestWriteDelay;
    if (!items.empty()) TestWriteDelay();
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
            case GpuEditOutcome::Done:         ++t.done; r.r.selected = false; break;
            case GpuEditOutcome::AlreadyDone:  Refuse(t.already, r, res.reason); r.r.selected = false; break;
            case GpuEditOutcome::Refused:      Refuse(t.refused, r, res.reason); break;
            case GpuEditOutcome::Unconfirmed:  Refuse(t.uncertain, r, res.reason); break;
            case GpuEditOutcome::NotAttempted: Refuse(t.notTried, r, res.reason); break;
        }
        // A ROW LEFT UNFINISHED AT ANOTHER PROGRAM'S GPU CHOICE IS UNTICKED (UntickAfterRun, gpu_edit.h): kept ticked, the next
        // Apply would compare with that choice - the listed one now - and write over it.
        if (UntickAfterRun(res.outcome, listedBefore, res.choiceText, IntendedChoiceText(ready[k].item, removing, targetKey))) {
            r.r.selected = false;
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
        if (st->rows[i].autoSelected && !automatic[i].selected) st->rows[i].r.selected = false;
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
    // THE TARGET IS CHECKED AGAIN HERE, NOT TRUSTED FROM THE PICKER: one of the GPUs the picker offers - the main
    // GPU among them since v0.5.6 - and the one the picker really shows (adversarial review, v0.5.5). It is copied
    // once, and that copy is what every row is written with.
    const std::wstring targetKey = st->targetKey;
    if (targetKey.empty()) return;   // Apply is disabled without a target
    if (!TargetStillShown(st, targetKey)) {
        DropTarget(st, hwnd);
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
    AssignConfirm confirm;
    confirm.count = pending;
    confirm.targetName = target;
    confirm.replacing = replacing;
    confirm.mainGpu = IsMainGpuKey(st->plan, targetKey);
    confirm.integrated = KindForKey(st->adapters, targetKey) == GpuKind::Integrated;
    const std::wstring ask = FormatAssignConfirm(confirm);
    int answer = 0;
    // A false return is a panel destroyed while the question was open: `st` is freed or about to be, so nothing more.
    if (!PanelMessageBox(st, hwnd, ask.c_str(), MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2, &answer)) return;
    if (answer != IDYES) return;
    // Checked again after the question: nothing may have moved the picker while it was open.
    if (!TargetStillShown(st, targetKey)) {
        DropTarget(st, hwnd);
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
        if (!PanelMessageBox(st, hwnd, notStarted.c_str(), MB_OK | MB_ICONWARNING)) return;
        RereadAfterChangedRows(st, hwnd, changedSinceListed);
        return;
    }
    const EditTally t = RunEdits(st, ready, false, targetKey, journal);
    changedSinceListed += t.changedSinceListed;   // pass two's too: those rows are unticked, said and read again the same way
    // THE RECORD GOES ONLY WHEN EVERY CHANGE IS OVER AND EVERY CHANGED ROW IS IN THE RESTORE FILE.
    const bool finished = ready.empty() || (t.unrecorded.empty() && FinishGpuRestore(journal));
    Redraw(st);

    // 🔴 IT REPORTS WHAT LANDED AND WHAT DID NOT, ALWAYS - by name, with the reason. A bulk registry
    // write that half failed must not look like one that succeeded.
    notWritten.insert(notWritten.end(), t.refused.begin(), t.refused.end());
    std::wstring msg = std::to_wstring(t.done) + L" of " + std::to_wstring(pending)
                     + L" assigned to " + target + L".";
    if (!t.already.empty()) msg += L"\r\n\r\nAlready set, nothing written:" + ListedLines(t.already);
    if (!notWritten.empty()) msg += L"\r\n\r\nNot written:" + ListedLines(notWritten);
    msg += ChangedSinceListedLine(changedSinceListed);
    if (!t.uncertain.empty()) msg += L"\r\n\r\nWritten, but not confirmed:" + ListedLines(t.uncertain);
    if (!t.notTried.empty()) msg += L"\r\n\r\nNot tried:" + ListedLines(t.notTried);
    msg += RestoreLines(journal, t, finished);
    if (t.done > 0) {
        // AN INSTRUCTION, NEVER A CLAIM THAT IT HAS TAKEN EFFECT. Windows reads this preference when
        // an application creates its graphics device, at start-up, so a running application keeps
        // the GPU it already has.
        msg += L"\r\n\r\nRestart those applications for the change to take effect. A running "
               L"application keeps the GPU it started on.";
    }
    const bool clean = notWritten.empty() && t.uncertain.empty() && t.notTried.empty() && finished;
    if (!PanelMessageBox(st, hwnd, msg.c_str(), MB_OK | (clean ? MB_ICONINFORMATION : MB_ICONWARNING))) return;
    RereadAfterChangedRows(st, hwnd, changedSinceListed);
}

void DoRemove(GpuState* st, HWND hwnd) {
    // 🔴 CONFIRM FIRST, DEFAULTING TO NO - EXACTLY AS APPLY DOES. Found in the v0.5.5 window's own
    // screenshot: after the bulk action (now "Auto assign GPU for Gaming") this red button sits lit
    // beside Apply, and it used to delete registry values the moment it was clicked.
    const size_t removable = RemovableCount(st);
    if (removable == 0) return;
    const std::wstring nl2 = std::wstring(1, wchar_t(13)) + wchar_t(10) + wchar_t(13) + wchar_t(10);
    const std::wstring ask = L"Remove the GPU assignment from " + std::to_wstring(removable) +
                             L" application" + (removable == 1 ? L"" : L"s") + L"?" + nl2 +
                             L"Each returns to Windows' default GPU choice the next time it starts, and the "
                             L"previous value of each one it changes is kept in a restore file. " +
                             L"Ticked applications with no assignment are left alone.";
    int answer = 0;
    // As in DoApply: a false return is a panel destroyed while the question was open, so nothing more.
    if (!PanelMessageBox(st, hwnd, ask.c_str(), MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2, &answer)) return;
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
        if (!PanelMessageBox(st, hwnd, notStarted.c_str(), MB_OK | MB_ICONWARNING)) return;
        RereadAfterChangedRows(st, hwnd, changedSinceListed);
        return;
    }
    // STRIP ONLY THIS FEATURE'S FIELDS; the whole value is deleted only when nothing else was in it.
    const EditTally t = RunEdits(st, ready, true, std::wstring(), journal);
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
    if (t.done > 0) {
        msg += L"\r\n\r\nThose applications return to Windows' default GPU choice the next time "
               L"they start.";
    }
    const bool clean = notRemoved.empty() && t.uncertain.empty() && t.notTried.empty() && finished;
    if (!PanelMessageBox(st, hwnd, msg.c_str(), MB_OK | (clean ? MB_ICONINFORMATION : MB_ICONWARNING))) return;
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
    int bx = x;
    MoveWindow(st->hBulk, bx, btnY, bw, ROW, TRUE);      bx += bw + GT;
    MoveWindow(st->hClearSel, bx, btnY, bn, ROW, TRUE);  bx += bn + GT;
    MoveWindow(st->hRemove, bx, btnY, bw, ROW, TRUE);

    const int rx = x + w;
    MoveWindow(st->hClose, rx - bn, btnY, bn, ROW, TRUE);
    MoveWindow(st->hApply, rx - bn - GT - bn, btnY, bn, ROW, TRUE);
    InvalidateRect(hwnd, nullptr, TRUE);
}

// Every control gets the body font at the panel's current dpi. WM_CREATE and WM_DPICHANGED_AFTERPARENT share this.
void ApplyPanelFont(GpuState* st) {
    HWND all[] = { st->hPlan, st->hTargetLbl, st->hTarget, st->hStatus, st->hList, st->hPath,
                   st->hBulk, st->hClearSel, st->hRemove, st->hApply, st->hClose };
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
    for (size_t i = 0; i < st->rows.size(); ++i) st->rows[i].r.selected = false;
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
            st->hClearSel = Mk(hwnd, L"BUTTON", L"Clear selection", BS_OWNERDRAW | WS_TABSTOP,
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
                                 !st->hBulk || !st->hClearSel || !st->hRemove || !st->hApply || !st->hClose;
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
                else if (di->hwndItem == st->hClearSel) k = theme::ButtonKind::Ghost;
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
                const int caret = static_cast<int>(HIWORD(wp));
                const LRESULT data = SendMessageW(st->hList, LB_GETITEMDATA, static_cast<WPARAM>(caret), 0);
                const size_t idx = (data != LB_ERR) ? static_cast<size_t>(data) : st->rows.size();
                if (!st->broken && idx < st->rows.size() && st->rows[idx].listed) {
                    st->rows[idx].r.selected = !st->rows[idx].r.selected;
                    st->rows[idx].autoSelected = false;
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
                const int sel = static_cast<int>(SendMessageW(st->hList, LB_GETCURSEL, 0, 0));
                if (sel >= 0) {
                    const size_t idx = static_cast<size_t>(
                        SendMessageW(st->hList, LB_GETITEMDATA, static_cast<WPARAM>(sel), 0));
                    if (!st->broken && idx < st->rows.size() && st->rows[idx].listed) {
                        st->rows[idx].r.selected = !st->rows[idx].r.selected;
                        st->rows[idx].autoSelected = false;
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
            switch (id) {
                case IDC_GPU_BULK:     DoBulk(st, hwnd); return 0;
                case IDC_GPU_CLEARSEL:
                    for (size_t i = 0; i < st->rows.size(); ++i) st->rows[i].r.selected = false;
                    Redraw(st);
                    return 0;
                case IDC_GPU_REMOVE:   DoRemove(st, hwnd); return 0;
                case IDC_GPU_APPLY:    DoApply(st, hwnd); return 0;
                case IDC_GPU_CLOSE:
                    // CANCEL UNTICKS AND HANDS BACK; IT NEVER DESTROYS. The separate window closed itself here; a tab
                    // that did the same would stay empty until Settings was opened again. A tick writes nothing, so
                    // there is nothing else to undo. Where to go next is Settings' call - the tab the user came from -
                    // so the panel only tells its parent, and touches nothing once that call returns.
                    for (size_t i = 0; i < st->rows.size(); ++i) st->rows[i].r.selected = false;
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
        for (size_t i = 0; i < st->rows.size(); ++i) st->rows[i].r.selected = false;
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
        if (f && (f == st->hBulk || f == st->hClearSel || f == st->hRemove || f == st->hApply || f == st->hClose))
            press = f;
    }
    if (press && IsWindowEnabled(press))
        PostMessageW(panel, WM_COMMAND, MAKEWPARAM(GetDlgCtrlID(press), BN_CLICKED), reinterpret_cast<LPARAM>(press));
    return true;
}

}  // namespace cd
