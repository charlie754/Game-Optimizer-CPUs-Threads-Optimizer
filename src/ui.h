// Game Optimizer - all Win32 UI. Tray, core map control, settings, first-run wizard.
//
// No .rc dialog templates and no image assets: every icon is drawn with GDI at runtime and
// every window is built from CreateWindowEx, so the repository carries no binary blobs and
// the exe stays DPI-scalable.
#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <string>
#include <vector>
#include "config.h"
#include "engine.h"
#include "topology.h"

namespace cd {

// ---- App-wide messages -----------------------------------------------------
constexpr UINT WM_APP_TRAY   = WM_APP + 1;   // Shell_NotifyIcon callback
constexpr UINT WM_APP_STATUS = WM_APP + 2;   // engine -> UI, "status changed, redraw"

// ---- Tray menu command ids -------------------------------------------------
// Explorer uses the lowest resource id for the app icon, so keep this below the 40000+ menu ids.
constexpr UINT IDI_APPICON = 101;

enum : UINT {
    IDM_STATUS = 40000,      // disabled, shows the status line
    IDM_SETTINGS,
    IDM_PAUSE,
    IDM_STARTUP,
    IDM_OPENFOLDER,
    IDM_EXIT
};

// ---- Tray ------------------------------------------------------------------
bool TrayInit(HWND hwnd, UINT callbackMsg);
void TrayShutdown();
void TrayUpdate(const EngineStatus& st);          // icon state + tooltip
void TrayShowMenu(HWND hwnd, const EngineStatus& st, bool startWithWindows);
void TrayNotify(const std::wstring& title, const std::wstring& text);

// ---- Core map control ------------------------------------------------------
// A registered child window class. One cell per logical processor, grouped into a labelled
// panel per LLC domain (per processor group above 64 LPs), SMT siblings adjacent. Each cell
// shows its LogicalProcessorIndex; the panel header shows the domain's L3 size.
//
// Live `Parked` state is drawn, because a user staring at a fully parked CCD needs to see
// that rather than wonder why the mask "did nothing" - see docs\spec\03-risks.md §3.
extern const wchar_t* kCoreMapClass;
constexpr WORD CMN_SELECTION_CHANGED = 0x8001;    // sent to the parent as WM_COMMAND

void CoreMapRegister(HINSTANCE hInst);
void CoreMapSetTopology(HWND hMap, const Topology* t);          // borrowed, must outlive
void CoreMapSetSelection(HWND hMap, const std::vector<ULONG>& ids);
std::vector<ULONG> CoreMapGetSelection(HWND hMap);
void CoreMapSetEditable(HWND hMap, bool editable);
void CoreMapRefreshParked(HWND hMap);              // re-reads Parked flags and repaints

// ---- Windows ---------------------------------------------------------------
// Modeless, single-instance; brings an existing window forward. Writes `cfg` back and calls
// Engine::SetConfig on OK/Apply.
void ShowSettings(HWND owner, Config& cfg, const Topology& topo, Engine& engine);

// Three pages: topology confirmation, the Game Mode advisory, first profile.
// Returns false only if the user closed it outright; `cfg.firstRunDone` is set either way
// so it does not reappear every launch.
bool RunFirstRunWizard(HWND owner, Config& cfg, const Topology& topo);

// Modal pickers. Both return an empty string on cancel.
std::wstring PickRunningProcess(HWND owner);   // returns the exe basename
std::wstring BrowseForExe(HWND owner);         // returns a full path

// A game picker backed by cd::DiscoverGames(), with a search box. Returns the chosen exe
// basename, or empty on cancel. `outDisplayName` receives the friendly name when known.
std::wstring PickGame(HWND owner, std::wstring* outDisplayName);

// ---- "We noticed you started a game" prompt --------------------------------
// Shown when the engine detects a running process that looks like a game and no enabled
// profile already covers it.
//
// RULES THIS PROMPT MUST FOLLOW, because an optimiser that interrupts a match is worse than
// one that does nothing:
//   * It NEVER steals focus from a fullscreen game. It is a tray balloon or a small
//     bottom-right toast window with WS_EX_NOACTIVATE, never a modal dialog.
//   * Cancel is remembered. Declining for a given executable must not ask again for that
//     executable unless the user clears it in Settings.
//   * It appears once per game, not once per launch.
//   * Nothing is applied until Apply is pressed. Detection alone never changes scheduling.
enum class GamePromptResult { Dismissed, Apply, Never };

// Modeless; posts WM_APP_GAMEPROMPT to `notify` with the result in wParam and a heap-allocated
// std::wstring* of the exe in lParam, which the receiver takes ownership of and deletes.
void ShowGamePrompt(HWND notify, const std::wstring& exeBaseName,
                    const std::wstring& displayName, const std::wstring& maskName);
constexpr UINT WM_APP_GAMEPROMPT = WM_APP + 3;

}  // namespace cd
