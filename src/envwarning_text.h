// Game Optimizer - the startup warning window's OWN Game Mode wording.
//
// WHY THIS EXISTS AND IS NOT firstrun_text.h. The popup used to print Page2GameModeText,
// the wizard's page-2 body. That text is correct where it lives and wrong here, for two
// reasons that are about the SURFACE and not about the facts:
//   * It ends "Nothing on this page needs your attention." There is no page - this is a
//     standalone window - and a window that opens itself at every login to announce that
//     nothing needs your attention argues against its own existence.
//   * It carries the registry path and the full explanation, because the wizard is
//     something the user chose to open and read. The popup is not; it gets the short form.
// So the two surfaces have deliberately DIVERGED. The wizard's wording is untouched, and
// tests/test_main.cpp group P asserts both halves of that: the popup text below, and that
// the wizard still says the sentence the popup must never say.
//
// PURE. No Windows types, no registry, no measurement - just the two strings, so a test can
// match them character for character. The CPU brand line the wizard prepends is composed by
// envwarning.cpp, deliberately outside this function, for exactly that reason.
#pragma once

#include <string>

namespace cd {

// The popup's own Game Mode wording. Deliberately NOT Page2GameModeText: that text was
// written for a wizard page the user chose to open, and says "on this page".
//
// `actionable` is Game Mode ON on a multi-CCD AMD part - the only case where Game Mode has
// a CCD preference to express and can therefore fight this tool. It comes from
// DecideStartupWarning's gameModeTone and from nowhere else.
inline std::wstring PopupGameModeText(bool actionable) {
    if (actionable) {
        return L"Windows Game Mode is on. On a multi-CCD AMD part it applies a "
               L"machine-wide CCD preference to whatever it decides is the game. That is "
               L"not per-game, and it competes with the masks Game Optimizer applies. If "
               L"your game ends up on the wrong CCD, check this first.";
    }
    // States the fact and stops. No "nothing needs your attention" - that sentence is what
    // made the original read as an alarm, and in an unbidden window it invites the question
    // "then why did you open?".
    return L"Windows Game Mode is on. This CPU has a single cache domain, so Game Mode has "
           L"no CCD preference to apply here and is not competing with Game Optimizer.";
}

}  // namespace cd
