// Game Optimizer - the startup environment warning window.
//
// One MODELESS window, shown once at startup, reporting the machine-wide scheduling
// influences this app does not control: Windows Game Mode and the AMD 3D V-Cache
// Performance Optimizer. Like the first-run wizard it ADVISES and never ENFORCES - the only
// action it offers is opening the Windows Game Mode page so the user can decide.
//
// TWO SEAMS, AND NEITHER IS OPTIONAL:
//   * WHETHER to show, and in which tone, comes from DecideStartupWarning in
//     startup_warning.h - a pure function with its own unit tests.
//   * WHAT IT SAYS comes from PopupGameModeText in envwarning_text.h and Page2VCacheText
//     in firstrun_text.h. Nothing in this window composes product wording beyond the window
//     title, the two section headings, the button captions and the CPU name line.
//     THE GAME MODE WORDING IS THIS WINDOW'S OWN AND THE WIZARD'S IS NOT: the wizard's page-2
//     body ends "Nothing on this page needs your attention", which is false in a window
//     that has no page and opens itself at every login. The V-Cache half is still shared,
//     so that fact cannot drift. See envwarning_text.h, and tests group P, which pins both
//     wordings and asserts the wizard still says its own sentence.
#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "config.h"
#include "topology.h"
#include "util.h"

namespace cd {

// Shows nothing at all when DecideStartupWarning() reports nothing to say: no window, no
// window class, no controls, no message hook, and an immediate return.
//
// Otherwise creates a MODAL window owned by `owner` and DOES NOT RETURN until that window has
// been destroyed - it runs its own message loop, and `owner` is disabled for the duration.
// The tray and the engine keep running throughout, and closing the window does not exit the
// app. (It was modeless once. The operator reported it "hiding behind the main window", and
// the fix was to make the window a gate rather than a notification; see envwarning.cpp.)
//
// `cfg` IS THE LIVE CONFIG, AND IT IS WRITTEN. The V-Cache section offers to disable the AMD
// 3D V-Cache optimizer, which runs an elevated child that records the driver's original Start
// value into config.ini ON DISK. Every in-memory Config in this process is stale from that
// moment, so this window refreshes `cfg` from disk after a successful change. Pass the same
// Config the rest of the app is using - a copy would silently defeat that, and an open
// Settings window would then write its own older snapshot over the record on the next OK.
// Borrowing it is sound precisely because this call is modal: `cfg` must outlive the call,
// and the call ends when the window does.
void ShowEnvironmentWarning(HWND owner, Config& cfg, const EnvironmentInfo& env,
                            const Topology& topo);

}  // namespace cd
