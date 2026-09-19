// Game Optimizer - the GPU Assignment panel.
//
// A child window on the Settings window's "GPU Assignment" tab, which replaced the separate "Isolate
// GPU" window in v0.5.6. It keeps its own Apply and Cancel beside Settings' OK / Cancel / Apply
// (founder decision): Settings' buttons commit config.ini, while this panel's Apply writes the
// registry the moment it is confirmed, and its Cancel unticks every row and writes nothing.
//
// WHAT IT IS FOR. Windows keeps a per-application GPU preference in
// HKCU\Software\Microsoft\DirectX\UserGpuPreferences, keyed by the FULL EXE PATH. On a machine
// with two GPUs that lets the game keep the fast card while background applications are pushed
// onto the other one. This panel shows what every running application is currently assigned to,
// and can move a selection of them in one action.
//
// ITS INPUTS ARE BORROWED AND READ ONLY. `snap` supplies the application list - the Settings window
// already keeps a refreshed snapshot, and taking a second one here would both cost a full process
// walk and let the two disagree about what is running. `cfg` supplies the profiles whose games must
// be excluded from the bulk action and the exclusion list. The panel changes the REGISTRY, never
// config.ini, so there is nothing to save and no dirty state for the caller to reconcile.
//
// IT WRITES THE REGISTRY, WHICH NOTHING ELSE IN THIS PRODUCT DOES TO A THIRD PARTY'S SETTINGS.
// Two consequences are designed in rather than discovered: every row that is not written is named
// with its reason, and every Apply and Remove adds each value it changes to a .reg restore file in the
// app's data folder the moment that change is made - opening that file puts them back. An earlier
// version of this comment promised the window itself could undo every change; it could not
// (adversarial review, v0.5.5).
#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace cd {

struct Config;
class ProcessSnapshot;

// Creates the GPU Assignment panel as a HIDDEN child of `parent`, with control id `id`. It holds controls only;
// nothing is read until ActivateGpuPanel. Returns nullptr on failure (the failure is logged).
HWND CreateGpuPanel(HWND parent, int id);

// Re-reads the adapters, Windows' GPU preferences, the unfinished-change records and the running applications
// from `cfg` and `snap`, and refills the panel. A row that was ticked stays ticked if it is still listed, the
// picker's target is unchanged, the row still says what it said when it was ticked - its GPU choice, a lost
// assignment, whether it is a profile game or pinned to the main GPU - and this read of Windows' GPU preferences is
// complete, or the read before it was not complete either (KeptTicks, gpu_rows.h). Posts the unfinished-change notice when the set of leftover records differs
// from the set this panel last showed; the notice reads the records again when it arrives and speaks only for those,
// a notice that arrives while the panel is hidden is not shown, and the next call posts it again. `cfg` and `snap`
// are read during the call and never stored.
void ActivateGpuPanel(HWND panel, const Config& cfg, const ProcessSnapshot& snap);

// Puts keyboard focus on the panel's application list.
void FocusGpuPanel(HWND panel);

// Enter and Esc for the panel, asked by the host's message hook BEFORE IsDialogMessageW. True when `msg` is a key
// press addressed to the panel or one of its controls and the panel has claimed it; the host then drops the message.
// Esc does what the panel's Cancel does; Enter presses the focused button of the panel and otherwise does nothing.
// An open picker keeps both keys, so it can close itself. Tab and Shift+Tab are claimed only while focus is in the
// read-only full-path box, which would otherwise keep them, and move to the next or previous tab stop of the host.
bool GpuPanelKey(HWND panel, const MSG& msg);

// The WM_COMMAND notification codes the panel sends its parent, with the panel's own control id:
//   GPUN_CANCEL  - its Cancel button has unticked every row.
//   GPUN_REFRESH - Apply or Remove refused a row whose GPU choice changed after the list was shown, and its result has
//                  been read: the parent calls ActivateGpuPanel again, since only it holds the config and the snapshot.
enum : int { GPUN_CANCEL = 1, GPUN_REFRESH = 2 };

}  // namespace cd
