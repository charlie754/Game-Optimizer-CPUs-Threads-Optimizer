// Game Optimizer - GPU Assignment tab row decision logic.
//
// WHY THIS FILE EXISTS. The GPU Assignment tab needs to show the user which applications
// could be moved to another GPU, and which ones cannot. This module provides
// PURE, testable decision logic that separates the business rules from the Win32 API
// calls that enumerate running processes and read their GPU assignments.
//
// The core problem: when a user runs a profile that assigns an app to the background GPU,
// but that app is marked as a "profile game" (i.e. it has an existing profile in our config),
// moving it could break that profile. This module helps the UI show clearly which rows
// can be moved and why.
//
// PHASE 1 IS CORE + CONFIG + TESTS ONLY. NO UI. This file provides the PURE decision logic
// inlined here so it is unit-testable without Win32. All Win32 calls live elsewhere.
#pragma once
#include <algorithm>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "gpu_pref.h"
#include "gpu_policy.h"
#include "util.h"

namespace cd {

// One row in the GPU Assignment tab's application list.
struct GpuRow {
    std::wstring exeName;        // "claude.exe"
    std::wstring exePath;        // full path of the RUNNING process
    std::wstring assignedKey;    // adapter key the REGISTRY names, empty if none
    std::wstring runningKey;     // adapter key the process is ACTUALLY on, empty if unknown
    std::wstring lostKey;        // the key an earlier version's path was pinned to, when this app lost it by updating
    bool isProfileGame = false;  // this exe is the `game` of some profile in our config
    bool selected = false;       // user's checkbox state
};

// Human label for one row's GPU situation. Never invent a GPU name; caller supplies the
// lookup. Returns e.g. L"RTX 4090" or, when they disagree, L"RTX 4090 -> running on RTX 5090".
// `nameFor` maps an adapter key to a display name; an unknown key yields L"unknown GPU",
// and an EMPTY key yields L"not assigned".
//
// WHY: the dialog must show the TRUTH about which GPU the process is on, especially when
// it disagrees with what the registry says. The arrow form makes that disagreement loud.
inline std::wstring FormatRowGpu(const GpuRow& row,
                                  const std::function<std::wstring(const std::wstring&)>& nameFor) {
    // What the registry says (or absence thereof)
    std::wstring assigned = row.assignedKey.empty() ? L"not assigned" : nameFor(row.assignedKey);

    // What the process is actually on
    std::wstring running = row.runningKey.empty() ? L"unknown GPU" : nameFor(row.runningKey);

    // If they match, return just one name. Otherwise show the mismatch.
    //
    // 🔴 AN EMPTY runningKey MEANS "NOT MEASURED", NEVER "RUNNING ELSEWHERE". Nothing in this
    // product can fill runningKey - there is no per-process "which adapter is it on" query - so
    // before this guard EVERY assigned row rendered "RTX 4090 -> running on unknown GPU", a
    // disagreement nobody measured, shouted on every row of the list. Show the arrow only
    // when both sides are real facts.
    if (row.runningKey.empty() || row.assignedKey == row.runningKey) {
        return assigned;
    }

    return assigned + L" -> running on " + running;
}

// Is this row's preference actually in effect? Reuses gpu_pref.h's GpuPrefState semantics.
// Given what the registry stores (`assignedKey`) and where the process actually is
// (`runningKey`), classify the situation. Assumes we WANT the process on `wantKey`.
//
// WHY: the dialog needs to know if a process's stored preference is stale (setting correct
// but process not yet restarted) vs wrong (setting never written or manually edited).
inline GpuPrefState RowState(const GpuRow& row, const std::wstring& wantKey) {
    // Build the stored value as it would appear in the registry
    std::wstring haveValue;
    if (!row.assignedKey.empty()) {
        haveValue = FormatPreferenceValue(row.assignedKey);
    }

    return ClassifyGpuPref(wantKey, haveValue, row.runningKey);
}

// A ROW THE USER PINNED TO THE MAIN GPU (`gameKey`): the registry still names that GPU for it, or it has no value and
// the assignment its earlier version lost by updating into a new folder (`lostKey`) named that GPU. An empty `gameKey`
// is never a main GPU. ONE RULE, READ BY TWO: SelectForBackground's decision 10 below, and the GPU Assignment tab's
// "nothing to move" sentence (FormatNothingToMoveLine), so the sentence cannot speak of rows the selector does not skip.
inline bool IsMainGpuPin(const GpuRow& r, const std::wstring& gameKey) {
    return !gameKey.empty() && (r.assignedKey == gameKey || (r.assignedKey.empty() && r.lostKey == gameKey));
}

// THE "Auto assign GPU for Gaming" BULK ACTION.
// Select every row that SHOULD be moved to the background GPU, and return a new vector with
// `selected` set accordingly. RULES, in order:
//   - If backgroundKey is empty, select nothing (we do not know where to put things).
//   - If backgroundKey IS the main GPU (`gameKey`), select nothing: this action moves background
//     apps OFF the main GPU, never onto it. Founder decision, v0.5.6: the button is disabled while
//     the main GPU is the picker's target, and this refuses too, so the rule is testable here.
//   - NEVER select a row where isProfileGame is true. Those are games the user has profiles
//     for; moving one onto the background GPU could wreck it. This is the operator's explicit
//     "exclude all other profiles app already existed in our app".
//   - NEVER select a row already assigned to the main GPU, while `skipMainGpuPins` holds - nor a row
//     with no assignment whose earlier version's lost assignment (`lostKey`) named the main GPU.
//   - NEVER select a row whose runningKey is already backgroundKey AND whose assignedKey is
//     already backgroundKey - there is nothing to do.
//   - Otherwise select it.
//
// WHY: this function drives the checkboxes in the dialog. The operator ruling is clear:
// do not touch apps that are tied to user profiles. The last rule ensures we do not
// mark as "selected for change" a row that is already correct.
//
// An empty `gameKey` - every caller written before v0.5.6 - keeps exactly the earlier behaviour.
//
// 🔴 `skipMainGpuPins` IS v0.5.6's DECISION 10, AND ITS DEFAULT BELOW IS THE ONE PLACE TO FLIP IT. Founder
// decision, v0.5.6 ("Skip them"): once the picker can pin an application to the main GPU, one bulk click
// plus Apply would sweep that deliberate pin back to the background GPU, with the confirm calling it only
// "another GPU setting". So a row on the main GPU is never ticked. The window's IsMovable must agree with it.
//
// 🔴 A PIN LOST TO AN AUTO-UPDATE IS STILL THE USER'S PIN (adversarial review, v0.5.6). An app pinned to the main
// GPU that updates into a new version folder runs from a path with no value, so its assignedKey is empty - and
// without the lostKey test it was ticked for the background GPU, while its own note said "was on" the main GPU.
inline std::vector<GpuRow> SelectForBackground(const std::vector<GpuRow>& rows,
                                                const std::wstring& backgroundKey,
                                                const std::wstring& gameKey = std::wstring(),
                                                bool skipMainGpuPins = true) {
    std::vector<GpuRow> result = rows;

    // If we do not know the background GPU, or it is the main GPU, select nothing
    if (backgroundKey.empty() || (!gameKey.empty() && backgroundKey == gameKey)) {
        for (auto& r : result) {
            r.selected = false;
        }
        return result;
    }

    for (auto& r : result) {
        // NEVER select a row that is tied to an existing profile
        if (r.isProfileGame) {
            r.selected = false;
            continue;
        }

        // NEVER sweep a deliberate main-GPU pin back to the background GPU (decision 10, above) - one the
        // registry still holds, or one lost when the app updated into a new folder
        if (skipMainGpuPins && IsMainGpuPin(r, gameKey)) {
            r.selected = false;
            continue;
        }

        // Do not select a row that is already correct.
        //
        // 🔴 WHEN runningKey IS UNKNOWN, THE REGISTRY IS THE ONLY FACT, SO JUDGE ON IT ALONE.
        // The old test required runningKey == backgroundKey as well, and runningKey has no
        // producer anywhere in this product - so that condition never held, and the bulk
        // action re-selected, and Apply rewrote, every app that was ALREADY on the background
        // GPU. When runningKey IS known, both must agree, exactly as before.
        const bool assignedOk = (r.assignedKey == backgroundKey);
        const bool runningOk = r.runningKey.empty() || (r.runningKey == backgroundKey);
        if (assignedOk && runningOk) {
            r.selected = false;
            continue;
        }

        // Otherwise select it
        r.selected = true;
    }

    return result;
}

// Rows whose stored preference is CORRECT but which are running elsewhere - the orphaned-path
// case. These are the ones needing a RESTART rather than a registry write, and the dialog
// must say so distinctly.
//
// WHY: if a setting IS correct but the process has not yet applied it, the user needs to
// restart the app, not write a new registry value. This helper identifies those rows so the
// UI can group them and explain the restart requirement.
inline std::vector<GpuRow> RowsNeedingRestart(const std::vector<GpuRow>& rows,
                                               const std::wstring& wantKey) {
    std::vector<GpuRow> result;

    for (const auto& r : rows) {
        GpuPrefState state = RowState(r, wantKey);

        // Only StaleNotApplied rows need a restart: the preference is CORRECT in the
        // registry but the process has not yet applied it.
        if (state == GpuPrefState::StaleNotApplied) {
            result.push_back(r);
        }
    }

    return result;
}

// One app that lost its GPU assignment when it auto-updated into a new folder.
struct OrphanedAssignment {
    std::wstring exeName;    // "claude.exe"
    std::wstring stalePath;  // the path the registry still names
    std::wstring livePath;   // where the matching process actually runs now
    std::wstring lostKey;    // the adapter key the stale entry named
};

// BaseName comes from util.h - the project already has it, and a second copy here
// collided at link time with util.cpp's out-of-line definition.
// Case-insensitive wide string comparison (for Windows paths and exe names).
inline bool WcsIcmp(const std::wstring& a, const std::wstring& b) {
    if (a.size() != b.size()) {
        return false;
    }
    return ::_wcsicmp(a.c_str(), b.c_str()) == 0;
}

// THE ORPHANED-UPDATE DETECTOR.
// Reports one entry per RUNNING process whose basename matches a registry path that:
//   (a) names a non-empty adapter key, AND
//   (b) no longer exists on disk, AND
//   (c) sits under the SAME INSTALL ROOT as the running path (SameInstallRoot), AND
//   (d) while the process's OWN live path has NO registry entry at all.
// That triple is the signature of an auto-update: Windows keys the preference to the full
// path, so a new versioned folder silently drops the assignment with no error anywhere.
//
// `registry` is (full path -> adapter key); an empty key means a preference exists but does
// not name a specific adapter.
//
// 🔴 PASS GpuChoicePairs (gpu_pref.h), NEVER EVERY VALUE. "An entry" in (d) must be a GPU CHOICE. Council review, v0.5.6:
// Windows writes values such as "AppStatus=1;AutoHDREnable=2097;" for an executable it has only seen, and fed every value,
// this function took such a value at the new version's path as the live path's own assignment - so an application whose
// main-GPU pin was lost to an auto-update got no lostKey, and Auto assign ticked it for the background GPU. A value that
// names no GPU by key - "GpuPreference=0" among them (Council round 2, v0.5.6) - is left out of the pairs too. AJ42 drives
// it from raw values through this detector and SelectForBackground.
// `pathExists` is injected so this stays pure and testable.
// Basename comparison MUST be case-insensitive - Windows paths are.
// If several stale entries match one running process, report the one whose path sorts last
// (the most recent version folder), and say why in a comment.
inline std::vector<OrphanedAssignment> FindOrphanedAssignments(
    const std::vector<std::pair<std::wstring, std::wstring>>& registry,
    const std::vector<std::wstring>& runningPaths,
    const std::function<bool(const std::wstring&)>& pathExists) {

    std::vector<OrphanedAssignment> result;

    // For each running process path
    for (const auto& livePath : runningPaths) {
        std::wstring liveBaseName = BaseName(livePath);

        // Check if the live path itself already has a registry entry
        bool liveHasEntry = false;
        for (const auto& reg : registry) {
            if (WcsIcmp(reg.first, livePath)) {
                liveHasEntry = true;
                break;
            }
        }

        // If the live path already has an entry, skip it (not orphaned)
        if (liveHasEntry) {
            continue;
        }

        // Look for stale registry entries that match this process's basename
        std::vector<std::pair<std::wstring, std::wstring>> matchingStaleEntries;
        for (const auto& reg : registry) {
            const auto& stalePath = reg.first;
            const auto& lostKey = reg.second;

            // Must have a non-empty adapter key to be orphaned - and a value that could not be read is
            // no evidence of an assignment at all, so it is never reported as a lost one.
            if (lostKey.empty() || lostKey == UnreadableChoiceKey()) {
                continue;
            }

            // Basename must match (case-insensitive)...
            if (!WcsIcmp(BaseName(stalePath), liveBaseName)) {
                continue;
            }

            // 🔴 ...AND THE TWO MUST BE ONE INSTALL AT TWO VERSIONS. Basename alone is not
            // identity. Found on the operator's machine: Claude Code's own claude.exe
            // (Roaming\Claude\claude-code\<ver>\) was reported as having lost Claude Desktop's
            // assignment (Local\AnthropicClaude\app-<ver>\) - two different applications that
            // share a file name. "Restart it to reapply" would have pinned the wrong program. An
            // auto-updating app moves between SIBLING version folders under one install root.
            if (!SameInstallRoot(stalePath, livePath)) {
                continue;
            }

            // ...AND ONLY THEN ASK THE DISK. Last on purpose - adversarial review, v0.5.5: a file
            // probe can block for seconds on an offline share, and when it ran first it probed
            // every assigned path in the registry, on the UI thread, each time the window opened.
            // The checks above cost nothing and rule almost every entry out.
            if (pathExists(stalePath)) {
                continue;
            }

            matchingStaleEntries.push_back(reg);
        }

        // If we found stale entries, report the one that sorts last (most recent version)
        if (!matchingStaleEntries.empty()) {
            std::sort(matchingStaleEntries.begin(), matchingStaleEntries.end(),
                      [](const auto& a, const auto& b) { return NaturalPathLess(a.first, b.first); });

            OrphanedAssignment orphan;
            orphan.exeName = liveBaseName;
            orphan.stalePath = matchingStaleEntries.back().first;
            orphan.livePath = livePath;
            orphan.lostKey = matchingStaleEntries.back().second;
            result.push_back(orphan);
        }
    }

    return result;
}

// THE GPU ASSIGNMENT TAB'S STATUS LINE, while a background GPU is the picker's target.
// Returns L"" when there is nothing to say. Otherwise a single sentence naming how many apps
// could move or how many lost their assignment, e.g. L"7 background apps can be moved to the GPU
// chosen above." or L"1 app lost its GPU assignment after an update - tick it and Apply to put it
// on the GPU chosen above." Singular/plural must both read correctly. Returns empty when gpuCount < 2.
//
// A LOST ASSIGNMENT OUTRANKS AN OPPORTUNITY. When driftedApps > 0 the line reports that
// instead, because it is a defect the user cannot otherwise discover - they would never
// think to look.
//
// v0.5.6: it said "isolated to a second GPU" until the picker also offered the main GPU; it now
// names the picker, as the lost-assignment sentence already did. Its only caller is gpuwindow.cpp.
inline std::wstring FormatGpuIsolateStatusLine(size_t movableApps, size_t gpuCount,
                                               size_t driftedApps = 0) {
    // Feature does not apply unless there are 2+ GPUs
    if (gpuCount < 2) {
        return L"";
    }

    // A lost assignment outranks an opportunity
    if (driftedApps > 0) {
        std::wstring appWord = (driftedApps == 1) ? L"app" : L"apps";
        std::wstring actionWord = (driftedApps == 1) ? L"lost its" : L"lost their";

        // 🔴 THE OLD SENTENCE WAS A FALSE INSTRUCTION: "restart them to reapply". Restarting does
        // nothing. The preference is keyed by the FULL PATH, the app now runs from a NEW path, and
        // that path has no entry at all - so there is nothing for a restart to reapply. It was
        // caught in the Isolate GPU window's own screenshot, sitting above the one button that
        // actually fixes it. The fix is to assign the live path again, and restart after that.
        const wchar_t* pron = (driftedApps == 1) ? L"it" : L"them";
        wchar_t buf[256];
        int n = swprintf_s(buf, sizeof(buf) / sizeof(buf[0]),
                          L"%zu %s %s GPU assignment after an update - tick %s and Apply to "
                          L"put %s on the GPU chosen above.",
                          driftedApps, appWord.c_str(), actionWord.c_str(), pron, pron);
        if (n < 0) {
            return L"";
        }
        return std::wstring(buf);
    }

    // No apps to isolate? Nothing to say.
    if (movableApps == 0) {
        return L"";
    }

    // Format the count with correct singular/plural
    std::wstring appWord = (movableApps == 1) ? L"app" : L"apps";

    wchar_t buf[256];
    int n = swprintf_s(buf, sizeof(buf) / sizeof(buf[0]), L"%zu background %s can be moved to the GPU chosen above.",
                       movableApps, appWord.c_str());
    if (n < 0) {
        return L"";
    }

    return std::wstring(buf);
}

// The status line while the MAIN GPU is the picker's target, shown instead of the movable-count sentence.
//
// Founder decision, v0.5.6: with the main GPU chosen, "Auto assign GPU for Gaming" is disabled AND the status
// line says why - a greyed button with no reason reads as a broken one. The count sentence above would be false
// here, and AH5d and AI2a pin its "background" wording for a background target, so this is a separate string.
inline std::wstring FormatMainGpuStatusLine() {
    return L"Auto assign GPU for Gaming is off while the main GPU is chosen - it only moves background apps off "
           L"that GPU. Tick applications and Apply to put them on the main GPU.";
}

// AN APPLICATION WITH NO GPU VALUE OF ITS OWN, ANOTHER VERSION OF WHICH MAY HOLD A MAIN-GPU PIN: an entry of `reg`
// (GpuChoicePairs) at another path with the same file name, under the same install root (SameInstallRoot), that names the
// main GPU (`gameKey`) or could not be read. Whether that path still exists is not asked. An empty `gameKey` is never a
// main GPU. ONE RULE, READ BY TWO: SelectForAutoAssign, and the "nothing to move" sentence's pin count (MainGpuPinCount in
// gpuwindow.cpp), so that sentence never calls such an application already assigned to the chosen GPU (AJ40's defect).
//
// 🔴 A PIN IS FOUND LOST ONLY WHEN WINDOWS SAYS THE OLD PATH IS GONE (Council round 2 fix check, v0.5.6). FindOrphanedAssignments
// reports a lost assignment only for an old path confirmed missing, so an updater that keeps the previous version folder on
// disk, or an old path that cannot be probed (access denied, an offline share - MarkOrphans counts both as present), gave
// the new version no lostKey; and an old value that could not be read is skipped there as no assignment at all. The new
// path then read as an application nobody pinned, and Auto assign ticked it for the background GPU. This fails closed and
// asks no disk: it stops Auto assign only, and a tick by hand stays available under the same write guards. The row's note,
// the lost count and FindOrphanedAssignments are unchanged. AJ46 pins it.
// ponytail: any other version counts, so a main-GPU pin on a version the user long since left keeps Auto assign off that
// application for as long as its value stays in the key; judge only the newest other version (NaturalPathLess) if that
// is ever reported.
inline bool AnotherVersionMayHoldMainGpuPin(const GpuRow& r, const std::vector<std::pair<std::wstring, std::wstring> >& reg,
                                            const std::wstring& gameKey, bool includeUnreadable = true) {
    if (gameKey.empty() || !r.assignedKey.empty() || r.exePath.empty()) return false;
    const std::wstring name = BaseName(r.exePath);
    for (size_t i = 0; i < reg.size(); ++i) {
        if (reg[i].second != gameKey && (!includeUnreadable || reg[i].second != UnreadableChoiceKey())) continue;
        if (WcsIcmp(reg[i].first, r.exePath) || !WcsIcmp(BaseName(reg[i].first), name)) continue;
        if (SameInstallRoot(reg[i].first, r.exePath)) return true;
    }
    return false;
}

// ---- When "Auto assign GPU for Gaming" may tick at all ------------------------------------------------------------
//
// Not while the main GPU is the target (founder decision, v0.5.6 - SelectForBackground refuses that too), and not while
// Windows' GPU preferences could not all be read.
//
// 🔴 A WALK OF THE KEY THAT STOPPED PART-WAY IS NO EVIDENCE THAT NOTHING WAS PINNED (Council round 2, v0.5.6). A main-GPU
// pin lost to an update is recognised only from the OLD path's value (lostKey, FindOrphanedAssignments). An incomplete
// walk can leave that value out, and the new path - no value, or only Windows' own fields - then reads as an application
// nobody pinned: Auto assign ticked it, and Apply wrote the background GPU to it. PrepareVerdict cannot catch that: the
// live path's value really is unchanged, and what is missing is the history. So Auto assign ticks nothing then; a tick by
// hand, made looking at the row, stays available under the same write guards. AJ45 pins both reasons.
inline bool AutoAssignAllowed(bool prefsComplete, bool mainGpuTargeted) {
    return prefsComplete && !mainGpuTargeted;
}

// What Auto assign ticks: nothing while AutoAssignAllowed says no, otherwise SelectForBackground's own choice less every
// application another version of which may hold a main-GPU pin (AnotherVersionMayHoldMainGpuPin). ONE RULE, READ BY TWO:
// the tab's bulk action and its movable count (IsMovable in gpuwindow.cpp), so the button, the count and the ticks cannot
// disagree. The main GPU is the target exactly when IsMainGpuKey says so: a non-empty key equal to it. `reg` is the
// GpuChoicePairs of the read the rows came from - no default, because an empty one quietly switches that check off.
inline std::vector<GpuRow> SelectForAutoAssign(const std::vector<GpuRow>& rows, const std::wstring& targetKey,
                                               const std::wstring& gameKey, bool prefsComplete,
                                               const std::vector<std::pair<std::wstring, std::wstring> >& reg) {
    if (!AutoAssignAllowed(prefsComplete, !gameKey.empty() && targetKey == gameKey)) {
        std::vector<GpuRow> none = rows;
        for (size_t i = 0; i < none.size(); ++i) none[i].selected = false;
        return none;
    }
    std::vector<GpuRow> picked = SelectForBackground(rows, targetKey, gameKey);
    for (size_t i = 0; i < picked.size(); ++i)
        if (picked[i].selected && AnotherVersionMayHoldMainGpuPin(picked[i], reg, gameKey)) picked[i].selected = false;
    return picked;
}

// The status line while Windows' GPU preferences could not all be read and a background GPU is the target: why Auto
// assign is grey, said beside it, as FormatMainGpuStatusLine says it for the main GPU. Wording fixed by the Council
// round 2 fix spec, v0.5.6; AJ45 pins it.
inline std::wstring FormatIncompleteScanStatusLine() {
    return L"Auto assign GPU for Gaming is off because Windows' GPU settings could not all be read, so an application "
           L"pinned to the main GPU might not be recognised. Tick applications by hand to change them.";
}

// WHAT THE TAB SAYS WHEN AUTO ASSIGN HAS NOTHING TO TICK, while a background GPU is the picker's target: the status
// line, and DoBulk's message. `mainGpuPins` is how many listed applications Auto assign leaves alone only because they
// are pinned to the main GPU (IsMainGpuPin - decision 10, "Skip them") or another version of them may be
// (AnotherVersionMayHoldMainGpuPin).
//
// 🔴 IT NEVER SAID WHERE THE PINS WERE (stage review, v0.5.6). This was one fixed sentence, shown even when the rows left
// unticked were the user's main-GPU pins, so it described those pins as already moved. AJ40 pins both sentences.
//
// 🔴 "ASSIGNED TO", NEVER "ON" (Council review, v0.5.6). Nothing in this product can tell where a running application
// renders - GpuRow::runningKey has no producer - and an application assigned a moment ago still runs on the GPU it
// started on. The sentence speaks of the assignment, which is the one thing the tab reads.
inline std::wstring FormatNothingToMoveLine(size_t mainGpuPins, size_t possiblePins = 0) {
    if (possiblePins != 0)
        return L"Some applications were left unticked because another version's GPU setting could not be read. "
               L"Auto assign also leaves main-GPU pins alone. Review the applications and tick them by hand to change them.";
    if (mainGpuPins == 0) return L"Every application that can be moved is already assigned to that GPU.";
    return L"Every application that can be moved is already assigned to that GPU or pinned to the main GPU. Auto assign "
           L"GPU for Gaming leaves applications pinned to the main GPU where they are.";
}

// THE STATUS LINE WHEN APPLICATIONS LOST THEIR ASSIGNMENT AND NO GPU IS THE PICKER'S TARGET - adapters that could not be
// read, a plan that cannot be decided, a picker that could not show its GPU, or a target that was dropped. Empty for none.
// `aGpuCanBeChosen` is true while the picker is enabled and offers GPUs (SyncButtons' own rule): the user can then choose
// one, and the sentence says so.
//
// 🔴 NO INSTRUCTION WITH NOTHING TO FOLLOW IT (Council review, v0.5.6). This case used FormatGpuIsolateStatusLine with a
// made-up count of two GPUs, which told the user to "tick it and Apply to put it on the GPU chosen above" - with no GPU
// chosen and Apply disabled.
// 🔴 AND NO "NO GPU IS AVAILABLE" UNDER A PICKER THAT OFFERS THEM (Council round 2, v0.5.6). After a target is dropped -
// DropTarget's own box says "Choose the GPU again" - the picker stays enabled with every GPU in it, and the line beneath
// said none was available. AJ44 pins all four forms.
inline std::wstring FormatLostWithoutTargetLine(size_t lostApps, bool aGpuCanBeChosen) {
    if (lostApps == 0) return std::wstring();
    if (aGpuCanBeChosen) {
        if (lostApps == 1) return L"1 app lost its GPU assignment after an update. Choose a GPU above to assign it again.";
        return std::to_wstring(lostApps) +
               L" apps lost their GPU assignment after an update. Choose a GPU above to assign them again.";
    }
    if (lostApps == 1)
        return L"1 app lost its GPU assignment after an update. No GPU is available to assign it to right now.";
    return std::to_wstring(lostApps) +
           L" apps lost their GPU assignment after an update. No GPU is available to assign them to right now.";
}

// ---- What survives a refresh of the GPU Assignment tab ------------------------------------------
//
// The tab re-reads everything each time it is entered. These two rules decide what the user's earlier visit leaves
// behind, and they live here rather than in gpuwindow.cpp because that file is not in the unit build: deleting
// either rule there passed Gate A (adversarial review, v0.5.6). AJ39 pins both.

// A GPU picked on an earlier visit stays picked while the picker still offers it.
inline bool KeepsPickedTarget(const std::wstring& oldTarget, const std::vector<std::wstring>& candidates) {
    return !oldTarget.empty() && std::find(candidates.begin(), candidates.end(), oldTarget) != candidates.end();
}

// What one row of the tab said about its program: what a tick was made against, and what a rebuilt row says now.
struct TickedRowFacts {
    std::wstring exePath;
    std::wstring listedChoice;   // the GPU choice the row showed (GpuChoiceText) - what Apply compares with (PrepareVerdict)
    std::wstring lostKey;        // the assignment an earlier version lost, when the row said so
    bool isProfileGame = false;
    bool mainGpuPin = false;     // IsMainGpuPin, with the main GPU of the visit the row was read on
};

inline TickedRowFacts FactsOfRow(const GpuRow& r, const std::wstring& listedChoice, const std::wstring& gameKey) {
    TickedRowFacts f;
    f.exePath = r.exePath;
    f.listedChoice = listedChoice;
    f.lostKey = r.lostKey;
    f.isProfileGame = r.isProfileGame;
    f.mainGpuPin = IsMainGpuPin(r, gameKey);
    return f;
}

// A TICK SURVIVES A REFRESH ONLY FOR THE SAME PROGRAM, THE SAME GPU, AND THE ROW AS IT WAS TICKED. One flag per rebuilt
// row, in `rows` order: set when the picked GPU is kept, that row's full path was ticked before, and everything the row
// said about the program then it still says - the GPU choice byte for byte, the lost assignment, whether it is a profile
// game and whether it is pinned to the main GPU. Paths compare case-insensitively, because the rows are rebuilt and
// neither their order nor a path's casing is promised. A tick made for a GPU the picker no longer offers is a tick for
// nothing, so none survives then.
//
// 🔴 A KEPT TICK WAS RE-BASED ONTO A READ MADE AFTER IT (Council round 2, v0.5.6). This kept a tick by path alone, and the
// rebuilt row carries the NEW read's GPU choice - so an application the user pinned to the main GPU in Windows Settings
// between two visits to the tab kept the tick made for the background GPU, and Apply's PrepareVerdict compared the new pin
// with itself and wrote over it: founder decision "Skip them", broken by moving between tabs. The same held for a program
// that became a profile game, or whose main-GPU pin was newly found lost to an update. A tick is dropped then, and made
// again from a list that shows what the program holds now. A deliberate tick on a main-GPU row that did not change is
// kept. AJ39 pins every case.
//
// 🔴 AND NONE SURVIVES INTO A READ THAT COULD NOT SEE ALL OF WINDOWS' GPU PREFERENCES, FROM ONE THAT COULD
// (`previousScanComplete` true, `scanComplete` false - Council round 2, v0.5.6). A kept tick may have come from Auto assign
// on that complete visit, which an incomplete read switches off (AutoAssignAllowed), and the facts compared below come
// from the incomplete read: a lost main-GPU pin it left out compares as "unchanged". Every tick goes, and is made again by
// hand from a list that says the read was incomplete.
// 🔴 ...BUT AFTER AN INCOMPLETE READ THE USUAL RULES APPLY (Council round 2 fix check, v0.5.6). Auto assign ticks nothing
// during such a visit, so every tick carried out of it was made by hand. Dropping those too lost, with no word, the ticks
// of the rows an Apply had just refused, on the very refresh that Apply asks for (GPUN_REFRESH). No default: the one
// caller in the product must say which reads it has. AJ45 pins all four transitions.
// ponytail: which ticks Auto assign made is not recorded, so a complete-then-incomplete refresh drops hand ticks as well;
// a per-row "ticked by Auto assign" flag, carried beside `selected`, if that loss is ever reported.
inline std::vector<bool> KeptTicks(const std::wstring& oldTarget, const std::vector<std::wstring>& candidates,
                                   const std::vector<TickedRowFacts>& ticked, const std::vector<TickedRowFacts>& rows,
                                   bool previousScanComplete, bool scanComplete) {
    std::vector<bool> kept(rows.size(), false);
    if ((previousScanComplete && !scanComplete) || !KeepsPickedTarget(oldTarget, candidates)) return kept;
    std::map<std::wstring, const TickedRowFacts*> byPath;
    for (size_t i = 0; i < ticked.size(); ++i) byPath[ToLower(ticked[i].exePath)] = &ticked[i];
    for (size_t i = 0; i < rows.size(); ++i) {
        const std::map<std::wstring, const TickedRowFacts*>::const_iterator t = byPath.find(ToLower(rows[i].exePath));
        if (t == byPath.end()) continue;
        const TickedRowFacts& was = *t->second;
        kept[i] = was.listedChoice == rows[i].listedChoice && was.lostKey == rows[i].lostKey &&
                  was.isProfileGame == rows[i].isProfileGame && was.mainGpuPin == rows[i].mainGpuPin;
    }
    return kept;
}

// THE UNFINISHED-CHANGE NOTICE, ONCE PER SET OF RECORDS. `pendingPaths` is this visit's leftover records, in any order
// and casing; `shown` is the set the notice last spoke for, kept lower-cased and sorted. Arrowing along the tab bar enters
// the tab every time, and a warning on every pass would teach the user to dismiss it unread; a set that empties and
// later refills is said again.
inline std::vector<std::wstring> UnfinishedSet(const std::vector<std::wstring>& pendingPaths) {
    std::vector<std::wstring> now;
    for (size_t i = 0; i < pendingPaths.size(); ++i) now.push_back(ToLower(pendingPaths[i]));
    std::sort(now.begin(), now.end());
    return now;
}

// True when the notice must be shown: the set is not empty and is not the one the notice last showed. It records nothing
// as shown - MarkUnfinishedShown does, once the box is really on screen - except that an empty set forgets the last one.
//
// 🔴 A QUEUED NOTICE IS NOT A SHOWN NOTICE (Council review, v0.5.6). This used to record the set when the notice was only
// POSTED: a post that failed, or one that arrived after the user had left the tab, then kept the same records from ever
// being said. The window asks this again when the notice arrives, and says nothing while the tab is hidden.
inline bool ShouldPostUnfinished(std::vector<std::wstring>& shown, const std::vector<std::wstring>& pendingPaths) {
    const std::vector<std::wstring> now = UnfinishedSet(pendingPaths);
    if (now.empty()) {
        shown.clear();
        return false;
    }
    return now != shown;
}

// Records `pendingPaths` as the set the notice now shows.
inline void MarkUnfinishedShown(std::vector<std::wstring>& shown, const std::vector<std::wstring>& pendingPaths) {
    shown = UnfinishedSet(pendingPaths);
}

}  // namespace cd
