// Game Optimizer - pure wording helpers used by the Settings UI and unit tests.
#pragma once

#include <map>
#include <string>
#include <vector>

#include "engine.h"
#include "settings_environment.h"

namespace cd {

inline std::wstring FormatFullyParkedMaskWarning(const std::wstring& maskName,
                                                 int processorCount,
                                                 bool amdVCacheAgentRunning,
                                                 bool amdVCacheServiceRunning,
                                                 bool amdVCacheDriverRunning,
                                                 bool amdVCachePresent) {
    const std::wstring prefix =
        L"Warning: all " + std::to_wstring(processorCount) + L" processors in \"" +
        maskName + L"\" are currently parked. ";
    // The agent (amd3dvcacheUser.exe) is the only component that actively steers the kernel driver.
    // The service (amd3dvcacheSvc) is a launcher only and cannot steer. Test the agent first.
    if (amdVCacheAgentRunning) {
        return prefix +
               L"The running AMD 3D V-Cache optimizer (amd3dvcacheUser.exe) is the likely cause. " +
               AmdVCacheRunningEffectText() +
               L" Stopping the AMD service also stops this agent, and takes effect immediately. "
               L"Windows can accept assignments to that CCD and then ignore them.";
    }
    if (amdVCacheServiceRunning || amdVCacheDriverRunning) {
        return prefix +
               L"AMD's 3D V-Cache optimizer service or driver is running, but the part that "
               L"actively steers (amd3dvcacheUser.exe) is not, so this is probably not coming "
               L"from the optimizer. A BIOS option can park a CCD below the operating system. "
               L"Look for a game-aware or adaptive CCD parking setting - not the CCD or SMT "
               L"controls that disable a CCD at boot, which are a different feature. Windows can "
               L"accept assignments to a parked CCD and then ignore them.";
    }
    if (amdVCachePresent) {
        return prefix +
               L"AMD's 3D V-Cache optimizer is installed, and neither its service nor its driver "
               L"is running, so nothing on the Windows side explains this. A BIOS option can "
               L"park a CCD below the operating system. Look for a game-aware or adaptive CCD "
               L"parking setting - not the CCD or SMT controls that disable a CCD at boot, "
               L"which are a different feature. Windows can accept assignments to a parked CCD "
               L"and then ignore them.";
    }
    return prefix +
           L"Windows can accept an assignment to a fully parked mask and then ignore "
           L"it - the process keeps running elsewhere.";
}

// THE BOX REFLECTS THE SERVICE START TYPE, NOT A RUNNING PROCESS, AND THAT DISTINCTION IS A
// FIXED BUG. It used to key on IsAmdVCacheAgentRunning() while a click writes the SERVICE
// start type through --vcache-run. Those are different objects: stop the service and its
// per-session agent can still be alive, which pinned the box to "unchecked" and turned every
// click into a STOP. The operator disabled the service three times in a row that way, and only
// the log showed it. Checked now means exactly what the click means: "configured Disabled".
//
// A start value this code could not read comes back as -1, which is NOT 4, so the box shows
// unchecked. That is the safe direction: it never claims a stop that was not configured.
//
// It reports CONFIGURATION and nothing else. It is handed a start type, never a service or
// process state, so "checked" means "configured Disabled" and not "the optimizer is stopped
// right now" - the per-session agent can outlive a disabled service, which is the whole
// reason the paragraph above exists.
inline bool VCacheStopBoxChecked(int serviceStartValue) { return serviceStartValue == 4; }

// The restore control exists only for users stranded by the OLD disable feature, i.e. only
// when a driver Start value was recorded.
inline bool ShowVCacheRestoreControl(int vcacheOriginalStart) { return vcacheOriginalStart >= 0; }

// AMD's INF installs this service as SERVICE_AUTO_START (2). Stopping it means Disabled (4),
// and clearing the box must restore AMD's own default rather than guess at Manual.
inline int VCacheServiceStartTypeFor(bool stopRequested) { return stopRequested ? 4 : 2; }

// The driver's restore value: what we recorded, or AMD's documented DEMAND_START default.
// The service always restores to AMD's own SERVICE_AUTO_START (2), independent of what the
// driver originally was. They differ in AMD's own INF (service is 2, driver is 3).
inline int VCacheRestoreDriverStart(int recordedOriginal) {
    return recordedOriginal >= 0 ? recordedOriginal : 3;
}

// ---- Extreme game mode, and the auto-pin rule it swallows --------------------------------

// Pure. IS THE AUTO-PIN GROUP LIVE AT ALL?
//
// Extreme game mode moves EVERY non-game, non-excluded process to the background mask.
// Auto-pin moves the subset of them that stay above a percentage threshold. One is a strict
// subset of the other, so while extreme mode is on the auto-pin rule cannot move anything
// extreme mode has not already moved, and its threshold field is a control with no outcome.
//
// IT GREYS THE GROUP; IT NEVER CLEARS Profile::autoPin. The stored setting has to survive a
// round trip through extreme mode being switched on and off again - a UI that silently
// rewrites a saved preference because another box was ticked is a defect, not a
// simplification. That is why this is a display predicate and nothing here writes.
//
// Note what it does NOT claim: the engine still runs rule 4 first and still tags what it
// picked AUTO (see Test_Z8). This is about the CONTROLS, not about the rule being switched
// off underneath the user.
inline bool AutoPinControlsEnabled(bool extremeModeChecked) {
    return !extremeModeChecked;
}

// Pure. What the auto-pin status line says while extreme mode has superseded it. It replaces
// the "Auto-pin is off for this profile" sentence in that state, because the setting is NOT
// off - it is stored, untouched, and simply cannot add anything - and a greyed control whose
// sentence still reports "Active - 3 apps moved" is two answers to one question.
inline std::wstring AutoPinSupersededByExtremeText() {
    return L"Extreme game mode already covers every background process, so this rule cannot "
           L"add anything while it is on.";
}

// ---- The two informative paragraphs, now behind an (i) icon ------------------------------
//
// Operator request, 2026-09-09: the two explanatory blocks became hover tooltips on a small
// circled "i" beside each check box. THEY ARE THE ONLY TWO THAT MOVED. The live status lines
// and the AMD V-Cache warning row stay on the page, because a warning behind an icon is a
// warning that does not work - which is the entire reason that row exists.
//
// They live here rather than as literals at the CreateWindowExW call so a test can pin the
// wording, exactly as AmdVCacheActiveWarningText is pinned: a tooltip is invisible to every
// other check in this project, so the string is the only thing that can be asserted on.
inline std::wstring AutoPinInfoTipText() {
    return L"While this game is in front, processes that stay above the threshold move to "
           L"the background mask until the game exits. The list above tags them AUTO.";
}

// The second sentence this paragraph used to carry - "It needs the background mask unparked:
// use \"Stop AMD's 3D V-Cache optimizer\" on the Setting page." - was DELETED on the
// operator's instruction, together with the live parked-status line under it. The V-Cache
// warning row on this same page already reports when the optimizer is running, which is the
// thing that parks the mask; the deleted text and the deleted line were both saying that a
// second and a third time, in another voice.
inline std::wstring ExtremeModeInfoTipText() {
    return L"Not only the busy ones and not only the ones you named - everything except the "
           L"game and the exclusion list.";
}

// ---- "the AMD optimizer is running right now" -------------------------------------------

// Pure. WHETHER THE PROFILES PAGE SHOWS THE V-CACHE WARNING ROW AT ALL.
//
// IT IS DRIVEN BY THE AGENT PROCESS, NEVER BY THE CHECKBOX. The "Stop AMD's 3D V-Cache
// optimizer" box on the Setting page reports a SERVICE START TYPE - what is configured -
// and this row has to report what is RUNNING. Those are different objects and they can
// disagree for a whole boot session: `pnputil /restart-device` brings the driver back with
// no reboot while the service still returns 1068 until the next one, and a per-session
// agent outlives a service that was stopped under it. A row keyed on the checkbox would
// therefore say "not active" while amd3dvcacheUser.exe was steering the firmware.
//
// TWO SOURCES, AND EITHER ONE IS ENOUGH, because neither is complete on its own:
//
//   agentRunningNow           EnvironmentInfo::amdVCacheAgentRunning - the unconditional
//                             per-session process probe the Settings window already
//                             refreshes once a second. True whenever the agent exists,
//                             game or no game.
//   engineSawAgentWhileGamePinned
//                             EngineStatus::amdVCacheAgentActive - the watcher's own
//                             reading, taken from the tick's existing snapshot and ONLY
//                             while a profile's game is actually governed. It is the
//                             stronger fact - it is the moment two schedulers really are
//                             steering the same threads - and it is false at idle by
//                             construction, so it cannot carry the row on its own: a user
//                             who opens Settings with no game running would see nothing.
//
// So the row is an OR, and that is not belt-and-braces: it is one source that is always
// live but knows nothing about the game, plus one that knows about the game and is silent
// the rest of the time.
inline bool ShowAmdVCacheActiveWarning(bool agentRunningNow,
                                       bool engineSawAgentWhileGamePinned) {
    return agentRunningNow || engineSawAgentWhileGamePinned;
}

// Pure. The row's sentence, FIXED - it interpolates nothing, so its wrapped height is a
// constant the layout can be measured against once. Operator wording, 2026-09-08, taken
// verbatim and deliberately not softened.
//
// IT IS NOW THE ONLY ROW ON THIS PAGE THAT MENTIONS THE OPTIMIZER, and that is why the
// extreme-mode parked line was deleted rather than merged. Until 2026-09-09 a sibling line
// (FormatExtremeModeParkedStatus) reported whether the processors of ONE named mask were
// parked at this instant, while this reports whether AMD's policy agent is RUNNING AT ALL.
// The agent is the cause and the parked mask is the effect, so the two rows were the same
// fact told twice - and this is the half that names the process and what to do about it.
//
// WHAT IT MUST NEVER SAY. This project has never measured a frame rate, so no string in it
// may promise one - see the tripwire in Test_Z9. "would not be fully optimized" is a
// statement about the assignment this app makes being ignorable, not a claim about frames,
// and it says would-not rather than will-be: it withholds a promise instead of making one.
inline std::wstring AmdVCacheActiveWarningText() {
    return L"Warning - AMD 3D V-Cache is active. Game would not be fully optimized. "
           L"Please turn it off in Setting";
}

// Pure. The Setting page's blocked-process sentence. (That page's TAB now reads
// "Setting"; its identifiers are still PAGE_GENERAL / hGen* - see the TabBarAddItem
// call in settings.cpp for why the two were deliberately not renamed together.)
//
// IT GROUPS BY EXECUTABLE AND IT CAPS, and both are forced by extreme game mode rather than
// chosen. The old version pasted every blocked process NAME into one string. At the handful
// of processes rules 2-4 govern that is a short, complete sentence. Rule 4b governs the whole
// desktop, most of which refuses an unelevated process - so the same code produces a string
// of a hundred names in a control whose height is a fixed Dp(52), and the promise made three
// lines above it, that anything the app cannot touch "will be named here rather than skipped
// silently", is broken by CLIPPING while still reading as if it were kept.
//
// So: distinct executables, case-insensitively, sorted, at most `maxNamed` of them, and an
// explicit "and N more" for the rest. The COUNT is of processes and the LIST is of apps, and
// the sentence says which is which - a user with 40 refused svchost-like processes under 6
// names is told both numbers rather than one number twice.
//
// `maxNamed == 0` means "no cap", which is what a test asks for when it wants the whole list.
inline std::wstring FormatBlockedProcessesLine(const std::vector<std::wstring>& names,
                                               size_t maxNamed) {
    if (names.empty()) {
        return L"No processes are currently blocked. Game Optimizer runs unelevated on "
               L"purpose; anything it cannot touch will be named here rather than skipped "
               L"silently.";
    }

    // Keyed on the lowercased basename, valued with the casing the engine reported, exactly
    // as AutoPinnedExeNames does - so the row reads "NVIDIA Broadcast.exe" and not a
    // flattened one, and the order is stable across ticks because the KEY is what sorts.
    std::map<std::wstring, std::wstring> byKey;
    size_t unnamed = 0;
    for (size_t i = 0; i < names.size(); ++i) {
        const std::wstring shown = Trim(BaseName(Trim(names[i])));
        if (shown.empty()) { ++unnamed; continue; }
        const std::wstring key = ToLower(shown);
        if (byKey.find(key) == byKey.end()) byKey[key] = shown;
    }
    // A process whose name could not be read still exists and still refused us. Counting it
    // as an app would be a lie; dropping it silently is the failure this whole function is
    // about, so it gets its own placeholder name.
    if (unnamed > 0) byKey[L"\x7f"] = L"(name unreadable)";

    std::wstring list;
    size_t shownCount = 0;
    for (std::map<std::wstring, std::wstring>::const_iterator it = byKey.begin();
         it != byKey.end(); ++it) {
        if (maxNamed != 0 && shownCount >= maxNamed) break;
        if (!list.empty()) list += L", ";
        list += it->second;
        ++shownCount;
    }
    if (byKey.size() > shownCount)
        list += L" and " + std::to_wstring(byKey.size() - shownCount) + L" more";

    return L"Blocked (access denied), " + std::to_wstring(names.size()) + L" processes in " +
           std::to_wstring(byKey.size()) + L" apps: " + list +
           L". These are elevated or protected processes; no mask was applied to them.";
}

// ---- What EXTREME GAME MODE actually moved -----------------------------------------------

// Pure. The Profiles page's extreme-mode sweep sentence.
//
// THE DEFECT IT EXISTS FOR, in the operator's words: "Extreme Mode should show what other
// application had been pinned into heavy mask. Now, it's 0 show." Rule 4b moves ~150
// processes and, until this line, not one of them appeared anywhere in the window - the
// heavy list showed only the three executables the user had named themselves. A feature that
// moves the whole desktop and reports nothing is indistinguishable from a feature that is
// switched off, which is exactly what was reported.
//
// IT GROUPS, IT COUNTS, AND IT CAPS TWICE - and every one of those is forced rather than
// chosen. `exes` arrives already grouped by executable and sorted count-descending by
// ExtremeSweptExes, so the first names shown are the ones that moved the most.
//
//   maxNamed      how many executables may be named. 0 means no cap, which is what a test
//                 asks for when it wants the whole list.
//   maxListChars  a ceiling on the LIST's own length in characters, 0 for no cap. This is
//                 the guard maxNamed cannot give: a cap on the NUMBER of names says nothing
//                 about their LENGTH, and this page's static has a fixed height. Six names
//                 of forty characters is the same clipped promise as sixty names of four.
//                 AT LEAST ONE NAME IS ALWAYS SHOWN, even when it alone exceeds the budget -
//                 a sentence that names nothing is worse than one that runs a line long, and
//                 the static's DT_END_ELLIPSIS is the backstop for that single case.
//
// The COUNT is of PROCESSES and the LIST is of APPS, and the sentence says which is which,
// exactly as FormatBlockedProcessesLine does - a user with 147 processes under 62 names is
// told both numbers rather than one number twice.
//
// `processCount` comes from ExtremeSweptProcessCount and is NOT the sum of `exes`, on
// purpose: a process whose name could not be read is dropped from the grouping and was still
// requested, so summing the groups would under-report what the app asked for.
//
// 🔴 IT REPORTS WHAT WAS REQUESTED AND WHAT DID NOT LAND. IT MUST NEVER SAY "MOVED".
// v0.4.3 shipped "Extreme game mode also moved N processes in M apps to <mask>", built from
// a count that includes every REFUSED assignment - and this app runs unelevated on purpose,
// so [M] ~41 processes on the operator's own desktop refuse it on every single tick. The
// sentence therefore over-stated the count AND claimed effective placement, which this
// product cannot observe at all: applier.h records a MEASURED case where the setter returned
// TRUE, the getter echoed all 16 ids, and not one of those processors ran a sample. The
// honest verb is what this app DID - it requested - and the refusals are named beside it.
//
// `notApplied` comes from ExtremeSweptNotAppliedCount. It is folded into the sentence rather
// than given a row of its own because the two numbers are only meaningful together: "147
// requested" alone over-claims and "41 refused" alone is a fragment.
//
// 🔴 AND IT IS THE LAST CLAUSE, NEVER THE ONE THAT INTRODUCES THE LIST. v0.4.4 got the
// words right and the PLACE wrong: it spliced the refusal clause in front of the colon, so
// the row on the operator's machine read "... requested Freq for 197 processes in 90 apps;
// 44 were not applied: conhost.exe x19, obs-browser-page.exe x16, chrome.exe x14,
// msedgewebview2.exe x12 and 86 more apps." A colon binds what follows it to the number in
// front of it, so that sentence offers those four apps as the breakdown of the 44. THEY ARE
// NOT. The list is the largest contributors to the 197 REQUESTS, and [M] three checks on
// that same screen each refute the plain reading:
//
//   19+16+14+12 = 61, which already exceeds the 44 the clause claims to be enumerating;
//   4 named + "86 more apps" = 90, the app count of the 197 - not the 38 apps the Setting
//     page reports for the refusals;
//   chrome.exe is ABSENT from that page's case-insensitive alphabetical blocked list, where
//     it would sort between choice.exe and ChtIME.exe - yet it is named here as "not
//     applied". All four also carry an amber SWEPT badge in the list ~300 px above, so ONE
//     screen said "swept" and "not applied" about the same four executables.
//
// So the ORDER is load-bearing rather than a matter of taste: the head, then the list its
// own colon introduces, then the refusal clause behind a semicolon that closes the list.
// The names are then bound to the apps, and the refusal count stands alone with nothing to
// attach to. The exes.empty() branch obeys the same rule for the same reason - see there.
//
// EMPTY MEANS DRAW NOTHING. The row takes height only while it has something to say, exactly
// as the AMD V-Cache row above it does.
inline std::wstring FormatExtremeSweptLine(const std::vector<SweptExe>& exes,
                                           size_t processCount,
                                           size_t notApplied,
                                           const std::wstring& maskName,
                                           size_t maxNamed,
                                           size_t maxListChars) {
    if (processCount == 0) return std::wstring();
    const std::wstring mask =
        maskName.empty() ? std::wstring(L"the background mask") : maskName;
    // Defensive: a caller that hands in more refusals than requests would otherwise print a
    // sentence that cannot be true. Clamped rather than asserted - this is a status row.
    if (notApplied > processCount) notApplied = processCount;
    const std::wstring tail =
        notApplied == 0 ? std::wstring()
                        : (L"; " + std::to_wstring(notApplied) + L" were not applied");

    std::wstring head = L"Extreme game mode also requested " + mask + L" for " +
                        std::to_wstring(processCount) +
                        (processCount == 1 ? L" process" : L" processes");
    // Every swept process was an executable the user had ALREADY named, so the grouping is
    // empty while the sweep is plainly working. Naming zero apps under a non-zero count would
    // read as a defect in this line rather than as the true statement it is.
    //
    // `tail` GOES LAST HERE TOO, and for the same reason the list branch below needs it to:
    // "12 processes; 4 were not applied, all of them apps you already listed above" attaches
    // "all of them" to the four REFUSALS, when what it qualifies is the twelve REQUESTS.
    if (exes.empty()) {
        return head + L", all of them apps you already listed above" + tail + L".";
    }
    // The colon introduces the APP LIST and nothing else. Nothing may be spliced between the
    // app count and that colon - see the header note above.
    head += L" in " + std::to_wstring(exes.size()) +
            (exes.size() == 1 ? L" app" : L" apps") + L": ";

    std::wstring list;
    size_t shown = 0;
    for (size_t i = 0; i < exes.size(); ++i) {
        if (maxNamed != 0 && shown >= maxNamed) break;
        const std::wstring piece = exes[i].name + L" x" + std::to_wstring(exes[i].count);
        // The length budget applies from the SECOND name onwards - see maxListChars above.
        if (shown > 0 && maxListChars != 0 &&
            list.size() + 2 + piece.size() > maxListChars) {
            break;
        }
        if (!list.empty()) list += L", ";
        list += piece;
        ++shown;
    }
    if (exes.size() > shown) {
        const size_t rest = exes.size() - shown;
        list += L" and " + std::to_wstring(rest) + (rest == 1 ? L" more app" : L" more apps");
    }
    // head : list ; refusals .  - the refusal clause closes the sentence and introduces
    // nothing. `tail` is empty when nothing was refused, so there is no dangling separator.
    return head + list + tail + L".";
}

// ---- WHICH profile is the engine governing? ----------------------------------------------

// Pure. Given, for every profile in the working config, whether it matches the engine's
// published status EXACTLY (by name) and whether it matches only through the game-executable
// FALLBACK, which one is the governing profile? -1 for "cannot tell".
//
// THE FALLBACK IS A RENAME HELPER AND IT WAS BEING USED AS AN IDENTITY. StatusDescribesProfile
// answers "is this profile the active one" and deliberately falls back to matching the game
// executable, so a profile the operator has renamed but not yet applied is still recognised.
// Scanning the list and taking the FIRST profile that answers yes then picks whichever comes
// first in the file - so a DISABLED profile A and an enabled profile B naming the same
// executable put the NOW pill, and the panel-follow, on A while the engine governs B.
//
// Two rules, and the second is the one that makes the first safe:
//   1. AN EXACT NAME MATCH ANYWHERE IN THE LIST BEATS EVERY FALLBACK. The name is what the
//      engine actually publishes; the executable is an inference about it.
//   2. A FALLBACK IS ONLY USED WHEN IT IS UNAMBIGUOUS. Two profiles sharing one executable
//      say nothing about which is governing, and guessing is how the wrong panel is shown
//      with full confidence. -1 leaves the selection alone, which is the honest answer.
inline int PickGoverningProfile(const std::vector<bool>& exactNameMatch,
                                const std::vector<bool>& fallbackMatch) {
    for (size_t i = 0; i < exactNameMatch.size(); ++i)
        if (exactNameMatch[i]) return static_cast<int>(i);

    int only = -1;
    for (size_t i = 0; i < fallbackMatch.size(); ++i) {
        if (!fallbackMatch[i]) continue;
        if (only >= 0) return -1;          // ambiguous - two profiles, one executable
        only = static_cast<int>(i);
    }
    return only;
}

// ---- Does the Profiles panel follow the profile the engine is actually governing? ---------

// Everything the decision needs, gathered by the window and passed in so the rule itself has
// no Win32 in it and can be driven by a test.
struct ProfileFollowInputs {
    // The engine is governing a profile and it is THIS config's profile number.
    bool haveGoverning = false;
    int  governingIndex = -1;
    // What the editor is showing right now.
    int  selectedIndex = -1;
    // An EDIT, LISTBOX or COMBOBOX on the Profiles page holds the keyboard focus, i.e. the
    // operator is typing into, or choosing from, this page RIGHT NOW.
    bool editingFocus = false;
    // A combo on this page has its list dropped open.
    bool dropdownOpen = false;
    // A modal (rename, new mask) is up over this window.
    bool modalUp = false;
    // The operator moved the selection themselves and the governing profile has not changed
    // since. See ShouldFollowGoverningProfile for why this latch exists.
    bool userChoseSelection = false;
};

// Pure. MAY THE PANEL SWITCH ITSELF TO THE GOVERNING PROFILE RIGHT NOW?
//
// Operator request: "if I'm playing Overwatch it should show Overwatch Profile Panel. Then I
// swap to Palworld, then it should auto swap to Palworld profile panel." The engine already
// switches; until this rule the window did not, so the operator could not see it working -
// and the screenshot that prompted the request showed the Overwatch panel (game mask Cache)
// while the engine was governing Palworld, whose sweep had Overwatch.exe on Freq.
//
// FOUR THINGS BLOCK IT, and every one of them is a case where taking the selection away
// would be worse than the bug it fixes:
//
//   editingFocus       the operator is typing in a field or picking a row. An auto-switch
//                      mid-keystroke rewrites the box under their hands.
//   dropdownOpen       a combo list is open. Re-loading the profile behind an open dropdown
//                      leaves the list showing one profile's masks and the page another's.
//   modalUp            a rename or new-mask prompt is up. The window is disabled and the
//                      value being typed belongs to the profile that was selected when the
//                      prompt opened.
//   userChoseSelection the operator deliberately clicked a DIFFERENT profile to look at it.
//                      The latch is cleared when the governing profile CHANGES - which is
//                      the operator swapping games, the very event they asked to be followed
//                      - so this suppresses the yank without disabling the feature.
//
// NOTHING IS DISCARDED WHEN IT DOES FIRE. The caller stores the editor back into its profile
// before loading the new one, exactly as a manual click on the list already does, so an
// unsaved edit survives the switch and is still there to Apply.
inline bool ShouldFollowGoverningProfile(const ProfileFollowInputs& in) {
    if (!in.haveGoverning) return false;
    if (in.governingIndex < 0) return false;
    if (in.governingIndex == in.selectedIndex) return false;   // already showing it
    if (in.editingFocus) return false;
    if (in.dropdownOpen) return false;
    if (in.modalUp) return false;
    if (in.userChoseSelection) return false;
    return true;
}

}  // namespace cd
