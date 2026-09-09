// Game Optimizer - WHICH profile governs when MORE THAN ONE game is running.
//
// THE DEFECT THIS FILE EXISTS TO FIX, in the operator's own words: "I usually have 2 games
// open at once. For example, I play Overwatch, it swap Overwatch as default. Then I open
// another game, the another game probably was pinned to heavy mask at the background during
// Overwatch."
//
// Rule 1 in engine.cpp used to stop at the FIRST enabled profile whose game was live, in
// CONFIG-FILE ORDER, and the foreground was never consulted for the choice at all. On the
// reference machine the enabled profiles are ordered Palworld, Overwatch 2, NTEGlobalGame,
// StarRail, ... - so with Palworld and Overwatch both alive Palworld took the V-Cache mask
// no matter which one was on screen, and under extreme game mode Overwatch was swept onto
// the Freq cores as an ordinary background process.
//
// WHY THIS IS ITS OWN HEADER AND NOT TEN LINES INSIDE ComputeDesired.
//
//   * ComputeDesired must stay PURE - no Win32, no globals, NO STATICS. A dwell counter is
//     state that survives across ticks, and the only honest place for it is the CALLER. So
//     the state is a plain struct the watcher owns (Engine::Impl::selection) and passes in
//     by reference, exactly as the auto-pin `sticky` list has always been passed in.
//   * The decision itself needs no snapshot, no process table and no window. Expressed over
//     an abstract candidate list it is unit-testable on its own, which is the whole point of
//     lifting it out: the dwell arithmetic can be driven tick by tick in a test without
//     building a synthetic process tree for each one.
//
// The one thing that is NOT here is "does the foreground window belong to this game?" - that
// needs the process tree, so it stays in engine.cpp next to ProcessSnapshot::Descendants and
// arrives here as the already-answered `ownsForeground` flag.
#pragma once

#include <string>
#include <vector>

#include "util.h"   // IEquals

namespace cd {

// HOW LONG A CHALLENGER MUST HOLD THE FOREGROUND BEFORE IT TAKES OVER, in milliseconds.
//
// Not zero, and the reason is measured rather than aesthetic: under extreme game mode a
// switch re-pins on the order of 150 processes, and every one of those is an OpenProcess
// plus a SetProcessDefaultCpuSets. Alt-tabbing between two games with no dwell would run
// that whole sweep twice a second and thrash the machine the feature exists to protect.
constexpr int kForegroundDwellMs = 3000;

// Why the selection is what it is, for THIS tick. An output field: the watcher reads it to
// write a log line that says WHY the profile changed, because "profile 'X' selected" alone
// cannot distinguish the first pin of a session from an alt-tab three hours in.
enum class SelectReason {
    Unchanged = 0,   // the same profile as last tick, or still nothing at all
    First,           // nothing was governing; a candidate takes effect IMMEDIATELY
    Foreground,      // a challenger held the foreground for the full dwell and took over
    Released,        // the governing profile's game exited; another candidate took over
    Cleared,         // there is nothing left to govern
};

// One live game that COULD govern this tick.
struct SelectionCandidate {
    // The profile's name, and the identity this module tracks across ticks. NOT the game pid:
    // a game that closes and reopens is the same profile and must not lose its selection to
    // its own restart, and a pid is reusable while a name is not.
    std::wstring name;
    // The foreground window belongs to this candidate's game process or to one of its
    // descendants. Answered by the caller - see the header comment.
    bool ownsForeground = false;
};

// Caller-owned selection state. The watcher owns the storage; ComputeDesired reads and
// updates it. Default-constructed means "nothing has ever been selected", which is exactly
// what a stateless caller wants and is why ComputeDesired can accept a null pointer for it.
struct ProfileSelection {
    std::wstring selected;      // the profile governing right now; empty means none
    std::wstring challenger;    // the profile that has been holding the foreground...
    int challengerTicks = 0;    // ...for this many CONSECUTIVE ticks
    SelectReason reason = SelectReason::Unchanged;   // OUTPUT, valid for the last call only
};

// Pure. How many consecutive ticks kForegroundDwellMs works out to at this poll interval.
//
// DERIVED, NEVER HARDCODED: the poll interval is a user setting clamped to 100..2000 ms, so
// a fixed tick count would mean a 30-second dwell at one end and a 1.5-second dwell at the
// other. At the default 250 ms this is 12. Never less than 1 - a dwell of zero ticks is not
// a dwell, and would reintroduce the thrash the constant exists to prevent.
inline int ForegroundSwitchDwellTicks(int pollMs) {
    if (pollMs <= 0) return 1;
    const int ticks = (kForegroundDwellMs + pollMs - 1) / pollMs;   // ceil
    return ticks < 1 ? 1 : ticks;
}

// Pure. WHICH candidate governs. Returns an index into `candidates`, or -1 when there are
// none. `sel` is read AND updated; `sel.reason` is written on every call.
//
// The rules, in the order they are tested, and each one is a line the operator asked for:
//
//   1. NO CANDIDATES -> nothing governs. The state is wiped, so the next game to appear is
//      a first selection and is pinned at once rather than after a dwell.
//   2. NO INCUMBENT -> the foreground candidate if there is one, otherwise the first, and
//      IMMEDIATELY. The dwell damps SWITCHING; making the first pin of a session wait three
//      seconds would be a regression dressed up as safety.
//   3. THE INCUMBENT'S GAME HAS EXITED -> the same immediate choice as rule 2, reported as
//      Released. A profile whose game is gone must not hold the mask for another dwell.
//   4. THE FOREGROUND BELONGS TO NO CANDIDATE -> KEEP THE INCUMBENT. This is the single most
//      important rule here. Alt-tabbing to Discord, to a browser or to this app's own
//      settings window must never un-pin the game that is still running.
//   5. THE FOREGROUND BELONGS TO THE INCUMBENT -> keep it, and reset the challenger counter.
//   6. THE FOREGROUND BELONGS TO A CHALLENGER -> count consecutive ticks; take over only on
//      the `dwellTicks`-th one. The counter resets to 1 the moment a DIFFERENT challenger
//      appears and is cleared entirely by rules 4 and 5, so the dwell is CONTINUOUS holding
//      and not an accumulated total.
inline int ChooseProfile(const std::vector<SelectionCandidate>& candidates,
                         int dwellTicks,
                         ProfileSelection& sel) {
    sel.reason = SelectReason::Unchanged;

    if (candidates.empty()) {
        const bool had = !sel.selected.empty();
        sel.selected.clear();
        sel.challenger.clear();
        sel.challengerTicks = 0;
        if (had) sel.reason = SelectReason::Cleared;
        return -1;
    }
    if (dwellTicks < 1) dwellTicks = 1;

    int fg = -1;
    for (size_t i = 0; i < candidates.size(); ++i) {
        if (candidates[i].ownsForeground) { fg = static_cast<int>(i); break; }
    }

    int incumbent = -1;
    if (!sel.selected.empty()) {
        for (size_t i = 0; i < candidates.size(); ++i) {
            if (IEquals(candidates[i].name, sel.selected)) {
                incumbent = static_cast<int>(i);
                break;
            }
        }
    }

    // Rules 2 and 3: no incumbent to defend, so whoever is in front takes it now.
    if (incumbent < 0) {
        const int chosen = (fg >= 0) ? fg : 0;
        sel.reason = sel.selected.empty() ? SelectReason::First : SelectReason::Released;
        sel.selected = candidates[chosen].name;
        sel.challenger.clear();
        sel.challengerTicks = 0;
        return chosen;
    }

    // Rules 4 and 5: nobody is challenging.
    if (fg < 0 || fg == incumbent) {
        sel.challenger.clear();
        sel.challengerTicks = 0;
        return incumbent;
    }

    // Rule 6: somebody is.
    if (!sel.challenger.empty() && IEquals(sel.challenger, candidates[fg].name)) {
        ++sel.challengerTicks;
    } else {
        sel.challenger = candidates[fg].name;
        sel.challengerTicks = 1;
    }
    if (sel.challengerTicks < dwellTicks) return incumbent;

    sel.selected = candidates[fg].name;
    sel.challenger.clear();
    sel.challengerTicks = 0;
    sel.reason = SelectReason::Foreground;
    return fg;
}

// Pure. The word the log line uses for a reason. Kept beside the enum so a new reason cannot
// be added without a name, which is how a log line ends up reading "(unknown)".
inline const wchar_t* SelectReasonText(SelectReason r) {
    switch (r) {
        case SelectReason::First:      return L"first selection";
        case SelectReason::Foreground: return L"foreground";
        case SelectReason::Released:   return L"previous game exited";
        case SelectReason::Cleared:    return L"nothing to govern";
        case SelectReason::Unchanged:  break;
    }
    return L"unchanged";
}

}  // namespace cd
