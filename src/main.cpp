// Game Optimizer - entry point, wiring and the message loop.
//
// This translation unit owns the ORDER of things, which is the part that actually matters:
//
//   startup   window -> DetectTopology -> RecoverFromJournal -> LoadConfig -> repair ->
//             topology-signature check -> tray -> foreground hook -> first run -> engine
//   shutdown  engine.Stop() -> StopForegroundTracking() -> SaveConfig -> TrayShutdown()
//
// RecoverFromJournal runs BEFORE anything else can touch a CPU set. If the previous run was
// killed with masks applied, some background app is still pinned to half the machine right
// now; clearing that is the first useful thing this process can do, and doing it after the
// engine has started would race the watcher thread for the same pids.
//
// Every failure path still ends with a tray icon that says what went wrong. A tray app that
// exits silently with no window and no message is indistinguishable from one that never ran.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#include <commctrl.h>
#include <objbase.h>     // WIN32_LEAN_AND_MEAN drops ole2.h; the shell dialogs need COM

#include <algorithm>
#include <exception>
#include <map>
#include <stdio.h>
#include <string>
#include <thread>
#include <vector>

#include "ui.h"
#include "engine.h"
#include "config.h"
#include "games.h"
#include "topology.h"
#include "procwatch.h"
#include "applier.h"
#include "util.h"
#include "theme.h"

namespace cd {
// Implemented in tray.cpp. It is not in ui.h - the header is frozen - but the Explorer
// restart broadcast can only be observed by a window procedure, and the only window
// procedure lives here.
bool TrayHandleTaskbarCreated(UINT msg);
}  // namespace cd

using namespace cd;

namespace {

const wchar_t* const kWndClass  = L"GameOptimizerMessageWindow";
const wchar_t* const kMutexName = L"Local\\GameOptimizer.SingleInstance";
const wchar_t* const kAppTitle  = L"Game Optimizer";

HINSTANCE    g_hInst           = nullptr;
HWND         g_hwnd            = nullptr;
Config       g_cfg;
Topology     g_topo;
// DELIBERATELY LEAKED, AND THAT IS THE FIX. A namespace-scope Engine registers ~Engine with
// the CRT onexit table, so it ran AFTER every lazily-constructed function-local static in
// this program had already been destroyed: applier.cpp:45 (the journal CRITICAL_SECTION),
// util.cpp:127 (the log CRITICAL_SECTION), util.cpp:169 (the config-dir wstring). That is
// the measured 0xC0000005 on every exit. A reference to a never-deleted object registers no
// destructor, so there is no exit-time ordering left to get wrong. All shutdown work is
// EngineShutdownGuard below, which runs while wWinMain is still on the stack and every
// static is alive. DO NOT "fix" this leak: doing so restores the crash.
Engine&      g_engine = *(new Engine());
bool         g_topoOk          = false;
std::wstring g_topoError;
UINT         g_msgShowSettings = 0;
bool         g_engineStarted   = false;

// ---- Status ----------------------------------------------------------------
// With no usable topology the engine is never started, so it has no opinion to report.
// The tray still has to say something honest.
EngineStatus CurrentStatus() {
    if (!g_topoOk) {
        EngineStatus st;
        st.paused  = g_cfg.paused;
        st.tooltip = L"Game Optimizer - CPU topology unavailable, nothing is governed";
        return st;
    }
    return g_engine.GetStatus();
}

void SaveConfigQuiet() {
    std::wstring err;
    if (!SaveConfig(GetConfigPath(), g_cfg, &err)) {
        LogLine(L"[main] SaveConfig failed: %s", err.empty() ? L"(no detail)" : err.c_str());
    }
}

void StopEngineOnce() {
    if (g_engineStarted) {
        g_engine.Stop();          // idempotent by contract; the flag just avoids the noise
        g_engineStarted = false;
    }
}

void OpenSettings(HWND owner) {
    // Modeless and single-instance inside ShowSettings; calling it again just brings the
    // existing window forward.
    ShowSettings(owner, g_cfg, g_topo, g_engine);
}

// ---------------------------------------------------------------------------
// "We noticed you started a game"
// ---------------------------------------------------------------------------
// The shape of this feature, and why it is split across a thread and a message:
//
//   * DETECTION runs on its own thread. It takes a process snapshot every few seconds and
//     asks games.h whether anything looks like a game. It must not run on the UI thread:
//     ProcessSnapshot::Take walks every process on the machine, and doing that inside the
//     message loop stalls the tray and any open window.
//   * The DECISION runs on the UI thread, because it reads g_cfg, which the UI thread owns.
//     The detector therefore only ever says "this exe is running and looks like a game"; the
//     filtering - declined, excluded, already covered, already asked - happens here.
//   * NOTHING IS APPLIED by detection. ui.h is explicit: the prompt asks, and only Apply
//     changes anything.
//
// The prompt appears AT MOST ONCE PER EXECUTABLE PER RUN. That is enforced twice on purpose:
// the detector will not post the same exe twice, and MaybePromptForGame will not prompt for
// an exe it has already prompted for. One guard covers a restarted detector, the other covers
// a candidate that arrived from anywhere else.
struct DetectedGame {
    std::wstring exe;
    std::wstring displayName;
};

// Detector -> UI. WM_APP+1..3 belong to ui.h (tray, status, game prompt); +16 leaves that
// block room to grow. lParam is a heap DetectedGame the handler deletes.
const UINT kMsgGameDetected = WM_APP + 16;

// THE DECLINE LIST'S HOME IN THE CONFIG FILE.
//
// "Never for this game" has to survive a restart, and config.h is frozen for this round - so
// there is no Config field to put it in. Config::unknown preserves any section this build
// does not recognise VERBATIM across a load/save cycle, which is exactly the property needed:
// the list is written as
//
//     [declined_games]
//     exe=Overwatch.exe
//     exe=Cyberpunk2077.exe
//
// and comes back through ParseConfig into Config::unknown[L"declined_games"] as those same
// lines. A future config.h can promote it to a real field by reading that section once.
const wchar_t* const kDeclinedSection = L"declined_games";
const wchar_t* const kDeclinedPrefix  = L"exe=";

const DWORD kDetectIntervalMs = 3000;

std::thread g_detectThread;
HANDLE      g_detectStop = nullptr;
bool        g_promptOpen = false;              // a toast is on screen right now
std::vector<std::wstring> g_promptedExes;      // lowercased; UI thread only

bool ContainsExe(const std::vector<std::wstring>& v, const std::wstring& lowerExe) {
    for (size_t i = 0; i < v.size(); ++i) {
        if (v[i] == lowerExe) return true;
    }
    return false;
}

// ---- The decline list ------------------------------------------------------
bool IsDeclined(const std::wstring& exe) {
    std::map<std::wstring, std::vector<std::wstring>>::const_iterator it =
        g_cfg.unknown.find(kDeclinedSection);
    if (it == g_cfg.unknown.end()) return false;
    const size_t plen = wcslen(kDeclinedPrefix);
    for (size_t i = 0; i < it->second.size(); ++i) {
        const std::wstring& line = it->second[i];
        if (line.size() <= plen) continue;
        if (!IEquals(line.substr(0, plen), std::wstring(kDeclinedPrefix))) continue;
        if (IEquals(Trim(line.substr(plen)), exe)) return true;
    }
    return false;
}

void AddDeclined(const std::wstring& exe) {
    if (exe.empty() || IsDeclined(exe)) return;
    g_cfg.unknown[kDeclinedSection].push_back(std::wstring(kDeclinedPrefix) + exe);
    SaveConfigQuiet();
    LogLine(L"[main] declined the game prompt for %s (persisted in [%s])",
            exe.c_str(), kDeclinedSection);
}

// ---- Filtering -------------------------------------------------------------
// An All Games profile covers everything, so a specific prompt would be noise.
bool CoveredByEnabledProfile(const std::wstring& exe) {
    for (size_t i = 0; i < g_cfg.profiles.size(); ++i) {
        const Profile& p = g_cfg.profiles[i];
        if (!p.enabled) continue;
        if (p.isAllGames) return true;
        if (p.game.empty()) continue;
        if (IEquals(BaseName(p.game), exe)) return true;
    }
    return false;
}

// A profile name doubles as an INI section suffix ([profile:<name>]), so the characters the
// grammar uses for something else are stripped rather than escaped.
std::wstring SanitizeProfileName(const std::wstring& s) {
    std::wstring out;
    for (size_t i = 0; i < s.size(); ++i) {
        const wchar_t c = s[i];
        if (c == L'[' || c == L']' || c == L'=' || c == L'|' || c < 32) continue;
        out += c;
    }
    return Trim(out);
}

std::wstring UniqueProfileName(const std::wstring& exe) {
    std::wstring stem = exe;
    const size_t dot = stem.find_last_of(L'.');
    if (dot != std::wstring::npos && dot > 0) stem = stem.substr(0, dot);
    stem = SanitizeProfileName(stem);
    if (stem.empty()) stem = L"Game";

    std::wstring candidate = stem;
    for (int n = 2; n < 1000; ++n) {
        bool clash = false;
        for (size_t i = 0; i < g_cfg.profiles.size(); ++i) {
            if (IEquals(g_cfg.profiles[i].name, candidate)) { clash = true; break; }
        }
        if (!clash) return candidate;
        candidate = stem + L" " + std::to_wstring(n);
    }
    return stem;
}

// Apply. A new profile is ENABLED - operator decision, recorded in config.h: a profile the
// user just said yes to is not something to leave switched off. autoPin/autoPinPercent are
// left at the Profile defaults rather than restated, so this cannot drift away from them.
void CreateProfileForGame(const std::wstring& exe) {
    if (exe.empty()) return;
    if (!g_topoOk) {
        LogLine(L"[main] cannot create a profile for %s: no usable topology", exe.c_str());
        return;
    }

    Profile p;
    p.name      = UniqueProfileName(exe);
    p.enabled   = true;
    p.game      = exe;
    p.gameMask  = g_topo.defaultGameMask;
    p.heavyMask = g_topo.defaultHeavyMask;
    if (g_cfg.FindMask(p.gameMask) == nullptr && !g_cfg.masks.empty()) {
        p.gameMask = g_cfg.masks[0].name;
    }
    if (g_cfg.FindMask(p.heavyMask) == nullptr) p.heavyMask.clear();
    g_cfg.profiles.push_back(p);

    const std::vector<std::wstring> repairs = ValidateAndRepair(g_cfg, g_topo);
    for (size_t i = 0; i < repairs.size(); ++i) {
        LogLine(L"[main] config repaired after adding %s: %s", p.name.c_str(),
                repairs[i].c_str());
    }

    SaveConfigQuiet();
    if (g_engineStarted) g_engine.SetConfig(g_cfg);
    TrayUpdate(CurrentStatus());
    LogLine(L"[main] created profile '%s' for %s on mask '%s'", p.name.c_str(), exe.c_str(),
            p.gameMask.c_str());
    if (g_cfg.notifications) {
        TrayNotify(kAppTitle, p.name + L" is now managed on " + p.gameMask + L".");
    }
}

// The mask the prompt names. It has to be a mask that actually exists in this config, because
// the sentence is a promise about what Apply will do.
std::wstring PromptMaskName() {
    if (g_cfg.FindMask(g_topo.defaultGameMask) != nullptr) return g_topo.defaultGameMask;
    if (!g_cfg.masks.empty()) return g_cfg.masks[0].name;
    return std::wstring();
}

// A rejection here is FINAL for this run, because the detector posts each exe once and will
// not offer it again. That is deliberate for the permanent reasons (declined, excluded,
// already covered) and a stated trade for the two transient ones: while paused the user has
// asked not to be managed, and while another prompt is on screen a second toast would stack
// nags in the same corner. ui.h's rule is "once per game, not once per launch" - erring
// towards asking less is the direction that rule points.
void MaybePromptForGame(const DetectedGame& d) {
    if (d.exe.empty()) return;
    if (!g_topoOk) return;                    // nothing could be applied, so do not ask
    if (g_cfg.paused) return;                 // the user has switched the app off
    if (g_promptOpen) return;                 // one toast at a time

    const std::wstring key = ToLower(d.exe);
    if (ContainsExe(g_promptedExes, key)) return;   // once per exe per run
    if (g_cfg.IsExcluded(d.exe)) return;
    if (IsDeclined(d.exe)) return;
    if (CoveredByEnabledProfile(d.exe)) return;

    g_promptedExes.push_back(key);
    g_promptOpen = true;
    LogLine(L"[main] prompting for detected game %s", d.exe.c_str());
    ShowGamePrompt(g_hwnd, d.exe, d.displayName, PromptMaskName());
}

// ---- The detector thread ---------------------------------------------------
void DetectThreadMain(HWND target) {
    // One scan, on this thread, never on the UI thread - games.h measures it at tens to
    // hundreds of milliseconds. Bundled names are merged in so a game no launcher claims is
    // still recognised by name.
    std::vector<GameEntry> known = DiscoverGames();
    {
        const std::vector<GameEntry>& bundled = BundledGames();
        for (size_t i = 0; i < bundled.size(); ++i) {
            bool dup = false;
            for (size_t j = 0; j < known.size(); ++j) {
                if (IEquals(known[j].exe, bundled[i].exe)) { dup = true; break; }
            }
            if (!dup) known.push_back(bundled[i]);
        }
    }

    std::vector<std::wstring> postedExes;     // lowercased; this thread only
    const int totalLps = GetTotalLogicalProcessors();

    for (;;) {
        if (WaitForSingleObject(g_detectStop, kDetectIntervalMs) == WAIT_OBJECT_0) return;

        ProcessSnapshot snap;
        // prev == nullptr: this detector wants names and pids, never CPU%, so it has no
        // reason to keep a previous snapshot alive between ticks.
        if (!snap.Take(nullptr, static_cast<int>(kDetectIntervalMs), totalLps, 0)) continue;

        const std::map<DWORD, ProcInfo>& all = snap.All();
        for (std::map<DWORD, ProcInfo>::const_iterator it = all.begin(); it != all.end();
             ++it) {
            const ProcInfo& pi = it->second;
            if (pi.name.empty()) continue;
            const std::wstring key = ToLower(pi.name);
            if (ContainsExe(postedExes, key)) continue;

            GameGuess guess;
            if (!GuessGame(pi.pid, pi.name, known, guess)) continue;
            // The window heuristic is allowed to be wrong - that is why the prompt asks -
            // but a bare guess with no supporting signal is not worth interrupting for.
            if (!guess.fromKnownList && guess.confidence < 0.5) continue;

            postedExes.push_back(key);
            DetectedGame* d = new DetectedGame();
            d->exe         = pi.name;
            d->displayName = guess.displayName.empty() ? pi.name : guess.displayName;
            if (!PostMessageW(target, kMsgGameDetected, 0,
                              reinterpret_cast<LPARAM>(d))) {
                delete d;          // the UI window is gone; nobody can take ownership
                return;
            }
        }
    }
}

void StartGameDetection() {
    if (g_detectStop != nullptr) return;
    g_detectStop = CreateEventW(nullptr, TRUE, FALSE, nullptr);   // manual reset
    if (g_detectStop == nullptr) {
        LogLine(L"[main] game detection not started: CreateEvent failed, err=%lu",
                GetLastError());
        return;
    }
    g_detectThread = std::thread(DetectThreadMain, g_hwnd);
}

void StopGameDetection() {
    if (g_detectStop != nullptr) SetEvent(g_detectStop);
    if (g_detectThread.joinable()) g_detectThread.join();
    if (g_detectStop != nullptr) {
        CloseHandle(g_detectStop);
        g_detectStop = nullptr;
    }
    // Candidates still sitting in the queue own heap strings nobody will read now.
    MSG q;
    while (PeekMessageW(&q, nullptr, kMsgGameDetected, kMsgGameDetected, PM_REMOVE)) {
        delete reinterpret_cast<DetectedGame*>(q.lParam);
    }
}

// The shutdown net that ~Engine used to be, relocated into wWinMain's lifetime where every
// function-local static is still alive. Both calls are self-gated no-ops (g_detectStop,
// g_engineStarted), so this is safe on the early-return paths where nothing was started.
// StopGameDetection is included because ~std::thread on the joinable g_detectThread
// (main.cpp:144) calls std::terminate.
struct EngineShutdownGuard {
    ~EngineShutdownGuard() { StopGameDetection(); StopEngineOnce(); }
};

// ---- Command line ----------------------------------------------------------
struct CmdOptions {
    bool bench = false;
    // The Run entry passes --tray so login stays silent; MigrateAutostartCommand repairs the
    // flagless entries written by older builds before this value controls launch visibility.
    bool tray  = false;
    bool vcacheSet = false;
    int vcacheSetValue = -1;
};

CmdOptions ParseCommandLine() {
    CmdOptions opt;
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) return opt;
    for (int i = 1; i < argc; ++i) {
        const std::wstring a = ToLower(argv[i] ? argv[i] : L"");
        if (a == L"--bench")      opt.bench = true;
        else if (a == L"--tray")  opt.tray  = true;
        else if (a == L"--vcache-set") {
            // Presence selects this mode even when the argument is malformed. Falling through to
            // normal startup would turn a bad elevated helper invocation into an elevated tray.
            opt.vcacheSet = true;
            if (i + 1 < argc) {
                const std::wstring value = argv[++i] ? argv[i] : L"";
                if (value == L"0") opt.vcacheSetValue = 0;
                if (value == L"1") opt.vcacheSetValue = 1;
            }
        }
        // Anything else is ignored on purpose: an unknown switch must not stop the app
        // from starting up in the tray.
    }
    LocalFree(argv);
    return opt;
}

const wchar_t* const kVCacheDriverKeyPath =
    L"SYSTEM\\CurrentControlSet\\Services\\amd3dvcache";
const wchar_t* const kVCacheServiceKeyPath =
    L"SYSTEM\\CurrentControlSet\\Services\\amd3dvcacheSvc";

LONG WriteFixedServiceStartValue(const wchar_t* fixedKeyPath, DWORD startValue) {
    HKEY key = nullptr;
    LONG rc = ::RegOpenKeyExW(HKEY_LOCAL_MACHINE, fixedKeyPath, 0, KEY_SET_VALUE, &key);
    if (rc != ERROR_SUCCESS) return rc;
    rc = ::RegSetValueExW(key, L"Start", 0, REG_DWORD,
                          reinterpret_cast<const BYTE*>(&startValue), sizeof(startValue));
    ::RegCloseKey(key);
    return rc;
}

void RestoreVCacheConfigValue(Config& cfg, int priorValue, const std::wstring& configPath) {
    cfg.vcacheOriginalStart = priorValue;
    std::wstring restoreError;
    if (!SaveConfig(configPath, cfg, &restoreError)) {
        LogLine(L"[vcache-set] config rollback failed: %s",
                restoreError.empty() ? L"(no detail)" : restoreError.c_str());
    }
}

int RunVCacheSet(int requestedValue) {
    if (requestedValue != 0 && requestedValue != 1) {
        LogLine(L"[vcache-set] invalid or missing value; exiting without changes");
        return 2;
    }

    // The measured SCM stop request failed with ERROR_INVALID_SERVICE_CONTROL (1052), and the
    // device is CR_NOT_DISABLEABLE. Changing these two fixed Start values followed by a reboot
    // is the measured working route; no service-control operation belongs in this mode.
    const int driverStart = ReadServiceStartValue(L"amd3dvcache");
    const int serviceStart = ReadServiceStartValue(L"amd3dvcacheSvc");
    if (driverStart < 0 || serviceStart < 0) {
        LogLine(L"[vcache-set] could not read both fixed Start values (driver=%d, service=%d); "
                L"exiting without changes", driverStart, serviceStart);
        return 3;
    }

    const std::wstring configPath = GetConfigPath();
    if (configPath.empty()) {
        LogLine(L"[vcache-set] config path is unavailable; exiting without changes");
        return 4;
    }

    Config cfg;
    std::wstring configError;
    if (!LoadConfig(configPath, cfg, &configError) && !configError.empty()) {
        LogLine(L"[vcache-set] config is unreadable: %s; exiting without changes",
                configError.c_str());
        return 4;
    }

    const int priorRecordedStart = cfg.vcacheOriginalStart;
    const DWORD targetStart = requestedValue == 1
        ? 4u
        : static_cast<DWORD>(cfg.vcacheOriginalStart >= 0 ? cfg.vcacheOriginalStart : 3);
    if (requestedValue == 1) {
        // Record the measured driver value before disabling anything. Manual (3) is common,
        // not guaranteed; a guessed restore value would silently change the user's setup.
        cfg.vcacheOriginalStart = driverStart;
        if (!SaveConfig(configPath, cfg, &configError)) {
            LogLine(L"[vcache-set] could not persist vcache_original_start: %s; "
                    L"exiting without registry changes",
                    configError.empty() ? L"(no detail)" : configError.c_str());
            return 5;
        }
    }

    LONG driverWrite = WriteFixedServiceStartValue(kVCacheDriverKeyPath, targetStart);
    if (driverWrite != ERROR_SUCCESS) {
        if (requestedValue == 1)
            RestoreVCacheConfigValue(cfg, priorRecordedStart, configPath);
        LogLine(L"[vcache-set] amd3dvcache Start write failed, err=%ld", driverWrite);
        return 6;
    }

    LONG serviceWrite = WriteFixedServiceStartValue(kVCacheServiceKeyPath, targetStart);
    if (serviceWrite != ERROR_SUCCESS) {
        const LONG driverRollback =
            WriteFixedServiceStartValue(kVCacheDriverKeyPath, static_cast<DWORD>(driverStart));
        const LONG serviceRollback =
            WriteFixedServiceStartValue(kVCacheServiceKeyPath, static_cast<DWORD>(serviceStart));
        if (requestedValue == 1)
            RestoreVCacheConfigValue(cfg, priorRecordedStart, configPath);
        LogLine(L"[vcache-set] amd3dvcacheSvc Start write failed, err=%ld; rollback "
                L"driver=%ld service=%ld", serviceWrite, driverRollback, serviceRollback);
        return 7;
    }

    if (requestedValue == 0) {
        // Clear only after both fixed Start values were restored. If the atomic config save
        // fails, put both values back so the visible checkbox can honestly remain unchanged.
        cfg.vcacheOriginalStart = -1;
        if (!SaveConfig(configPath, cfg, &configError)) {
            const LONG driverRollback = WriteFixedServiceStartValue(
                kVCacheDriverKeyPath, static_cast<DWORD>(driverStart));
            const LONG serviceRollback = WriteFixedServiceStartValue(
                kVCacheServiceKeyPath, static_cast<DWORD>(serviceStart));
            LogLine(L"[vcache-set] restored Start values but could not clear config: %s; "
                    L"rollback driver=%ld service=%ld",
                    configError.empty() ? L"(no detail)" : configError.c_str(),
                    driverRollback, serviceRollback);
            return 8;
        }
    }

    LogLine(L"[vcache-set] configured amd3dvcache and amd3dvcacheSvc Start=%lu; "
            L"vcache_original_start=%d; restart required",
            static_cast<unsigned long>(targetStart), cfg.vcacheOriginalStart);
    return 0;
}

int RunBench() {
    // MICROSECONDS, from QueryPerformanceCounter. The millisecond figure this used to print
    // was an artefact of the instrument, not a property of the code: GetTickCount64 advances
    // once per timer interrupt (~15.6 ms), so a tick could only ever be reported as 0 (it
    // fell between two interrupts) or 15-16 (it straddled one), however fast it really was.
    const int kRuns = 40;
    std::vector<double> costs;
    costs.reserve(kRuns);
    for (int i = 0; i < kRuns; ++i) {
        g_engine.TickOnce();                     // the int return is still milliseconds
        costs.push_back(g_engine.LastTickMicros());
    }

    std::vector<double> sorted = costs;
    std::sort(sorted.begin(), sorted.end());
    const double lo  = sorted.front();
    const double hi  = sorted.back();
    const double med = sorted[sorted.size() / 2];

    // Tick cost scales with the number of live processes, so the timings mean nothing
    // without it. Taken after the run, so it describes the machine that was measured.
    // autoPinPercent 0: this snapshot is only ever asked for its size.
    ProcessSnapshot finalSnap;
    int procCount = 0;
    if (finalSnap.Take(nullptr, g_cfg.pollMs, GetTotalLogicalProcessors(), 0)) {
        procCount = static_cast<int>(finalSnap.Count());
    }

    LogLine(L"[bench] %d ticks over %d processes: min=%.1f us median=%.1f us max=%.1f us",
            kRuns, procCount, lo, med, hi);

    wchar_t buf[512];
    swprintf_s(buf, ARRAYSIZE(buf),
               L"%d engine ticks on this machine, over %d live processes:\n\n"
               L"    min       %.1f us\n"
               L"    median    %.1f us\n"
               L"    max       %.1f us\n\n"
               L"Poll interval is %d ms, so a tick must stay well under that.",
               kRuns, procCount, lo, med, hi, g_cfg.pollMs);
    MessageBoxW(nullptr, buf, L"Game Optimizer - benchmark", MB_OK | MB_ICONINFORMATION);
    return 0;
}

// ---- Window procedure ------------------------------------------------------
LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    // Registered messages are runtime values, so they cannot be switch cases.
    if (g_msgShowSettings != 0 && msg == g_msgShowSettings) {
        OpenSettings(hwnd);
        return 0;
    }
    if (TrayHandleTaskbarCreated(msg)) {
        TrayUpdate(CurrentStatus());
        return 0;
    }

    switch (msg) {
    case WM_APP_TRAY: {
        // Legacy (version 0) notify-icon callback: lParam carries the mouse message.
        // LOWORD is correct for the versioned packing too, so this survives either.
        const UINT ev = static_cast<UINT>(LOWORD(lp));
        if (ev == WM_RBUTTONUP || ev == WM_CONTEXTMENU) {
            TrayShowMenu(hwnd, CurrentStatus(), GetStartWithWindows());
        } else if (ev == WM_LBUTTONDBLCLK) {
            OpenSettings(hwnd);
        }
        return 0;
    }

    case WM_APP_STATUS:
        TrayUpdate(CurrentStatus());
        return 0;

    // The detector found something that looks like a game. lParam is a heap DetectedGame
    // this handler owns; the decision itself is MaybePromptForGame's, on this thread,
    // because it reads g_cfg.
    case kMsgGameDetected: {
        DetectedGame* d = reinterpret_cast<DetectedGame*>(lp);
        if (d != nullptr) {
            const DetectedGame copy = *d;
            delete d;                     // freed FIRST, so no branch below can skip it
            MaybePromptForGame(copy);
        }
        return 0;
    }

    // The prompt answered. ui.h: lParam is a heap std::wstring the receiver owns and
    // deletes, ON EVERY PATH - including Dismissed, where nothing else happens at all.
    case WM_APP_GAMEPROMPT: {
        std::wstring* payload = reinterpret_cast<std::wstring*>(lp);
        const std::wstring exe = (payload != nullptr) ? *payload : std::wstring();
        delete payload;                   // before any early return can be written
        g_promptOpen = false;

        switch (static_cast<GamePromptResult>(wp)) {
        case GamePromptResult::Apply:
            CreateProfileForGame(exe);
            break;
        case GamePromptResult::Never:
            AddDeclined(exe);
            break;
        case GamePromptResult::Dismissed:
        default:
            // Nothing is applied and nothing is remembered beyond "do not ask again this
            // run", which g_promptedExes already recorded when the prompt was shown.
            break;
        }
        return 0;
    }

    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDM_STATUS:
            return 0;                       // disabled; never actually dispatched

        case IDM_SETTINGS:
            OpenSettings(hwnd);
            return 0;

        case IDM_PAUSE: {
            g_cfg.paused = !g_cfg.paused;
            g_engine.SetPaused(g_cfg.paused);
            SaveConfigQuiet();
            TrayUpdate(CurrentStatus());
            if (g_cfg.notifications) {
                TrayNotify(kAppTitle, g_cfg.paused
                                          ? L"Paused. All CPU Set assignments cleared."
                                          : L"Resumed.");
            }
            LogLine(L"[main] paused=%d", g_cfg.paused ? 1 : 0);
            return 0;
        }

        case IDM_STARTUP: {
            const bool want = !GetStartWithWindows();
            if (!SetStartWithWindows(want)) {
                LogLine(L"[main] SetStartWithWindows(%d) failed, err=%lu",
                        want ? 1 : 0, GetLastError());
                MessageBoxW(hwnd,
                            L"Could not update the Windows startup entry.\n"
                            L"HKCU\\Software\\Microsoft\\Windows\\CurrentVersion\\Run "
                            L"could not be written.",
                            kAppTitle, MB_OK | MB_ICONWARNING);
            }
            // Re-read rather than trusting the write: the registry is the source of truth
            // for this setting, not our copy of it.
            g_cfg.startWithWindows = GetStartWithWindows();
            SaveConfigQuiet();
            return 0;
        }

        case IDM_OPENFOLDER:
            OpenFolder(GetConfigDir());
            return 0;

        case IDM_EXIT:
            DestroyWindow(hwnd);
            return 0;

        default:
            break;
        }
        break;

    case WM_QUERYENDSESSION:
        // Clear now. Once WM_ENDSESSION arrives the process can be terminated at any
        // moment, and a stranded mask outlives the reboot.
        StopEngineOnce();
        return TRUE;

    case WM_ENDSESSION:
        if (wp) StopEngineOnce();
        return 0;

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;

    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

bool RegisterAndCreateWindow() {
    WNDCLASSEXW wc;
    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = g_hInst;
    wc.lpszClassName = kWndClass;
    if (!RegisterClassExW(&wc)) {
        if (GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return false;
    }

    // A real top-level window, never shown - NOT HWND_MESSAGE. A message-only window does
    // not receive broadcast messages, and "TaskbarCreated" is a broadcast.
    g_hwnd = CreateWindowExW(0, kWndClass, kAppTitle, WS_OVERLAPPED,
                             0, 0, 0, 0, nullptr, nullptr, g_hInst, nullptr);
    if (g_hwnd == nullptr) return false;

    HICON big = LoadIconW(g_hInst, MAKEINTRESOURCEW(IDI_APPICON));
    if (big != nullptr) {
        SendMessageW(g_hwnd, WM_SETICON, ICON_BIG,   reinterpret_cast<LPARAM>(big));
        SendMessageW(g_hwnd, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(big));
    } else {
        // The message window is deliberately allowed to survive a missing resource. The
        // measured consequence is cosmetic; refusing startup would also remove the tray UI.
        LogLine(L"[main] app icon resource %u could not be loaded, err=%lu; continuing",
                IDI_APPICON, GetLastError());
    }
    return true;
}

// ---- Startup steps ---------------------------------------------------------
// Everything that can fail on a strange machine. Returns with g_cfg usable in every case.
void LoadEverything(bool& outTopologyChanged) {
    outTopologyChanged = false;

    g_topoOk = DetectTopology(g_topo, &g_topoError);
    if (!g_topoOk) {
        LogLine(L"[main] DetectTopology failed: %s",
                g_topoError.empty() ? L"(no detail)" : g_topoError.c_str());
    } else {
        LogLine(L"[main] topology: %s, %d LPs, %u domain(s), sig=%s",
                KindName(g_topo.kind), g_topo.totalLogicalProcessors,
                static_cast<unsigned>(g_topo.domains.size()), g_topo.signature.c_str());
    }

    // Before ANY apply path can run.
    const int recovered = RecoverFromJournal();
    if (recovered > 0) {
        LogLine(L"[main] journal recovery cleared %d process(es) from a previous run",
                recovered);
    }

    const std::wstring cfgPath = GetConfigPath();
    std::wstring cfgErr;
    if (!LoadConfig(cfgPath, g_cfg, &cfgErr)) {
        if (cfgErr.empty()) {
            LogLine(L"[main] no config.ini yet - starting from defaults");
        } else {
            LogLine(L"[main] config.ini is unreadable (%s) - backing it up as config.ini.bad",
                    cfgErr.c_str());
            const std::wstring bad = cfgPath + L".bad";
            if (!MoveFileExW(cfgPath.c_str(), bad.c_str(), MOVEFILE_REPLACE_EXISTING)) {
                LogLine(L"[main] could not rename the broken config, err=%lu", GetLastError());
            }
        }
        g_cfg = DefaultConfig(g_topo);
    }

    const std::vector<std::wstring> repairs = ValidateAndRepair(g_cfg, g_topo);
    for (size_t i = 0; i < repairs.size(); ++i) {
        LogLine(L"[main] config repaired: %s", repairs[i].c_str());
    }

    if (!g_topoOk) return;

    if (!g_cfg.topologySignature.empty() && g_cfg.topologySignature != g_topo.signature) {
        // The stored CPU Set Ids may no longer mean what they meant. Re-derive the masks
        // and KEEP the profiles: a user's game/heavy lists are hand-written and are still
        // correct, only the hardware mapping under them moved.
        outTopologyChanged = true;
        LogLine(L"[main] topology signature changed: '%s' -> '%s', re-deriving masks",
                g_cfg.topologySignature.c_str(), g_topo.signature.c_str());

        g_cfg.masks = DeriveMasks(g_topo);
        for (size_t i = 0; i < g_cfg.profiles.size(); ++i) {
            Profile& p = g_cfg.profiles[i];
            if (!p.gameMask.empty() && g_cfg.FindMask(p.gameMask) == nullptr) {
                p.gameMask = g_topo.defaultGameMask;
            }
            if (!p.heavyMask.empty() && g_cfg.FindMask(p.heavyMask) == nullptr) {
                p.heavyMask = g_topo.defaultHeavyMask;
            }
        }
        g_cfg.topologySignature = g_topo.signature;

        const std::vector<std::wstring> more = ValidateAndRepair(g_cfg, g_topo);
        for (size_t i = 0; i < more.size(); ++i) {
            LogLine(L"[main] config repaired after re-derive: %s", more[i].c_str());
        }
    } else if (g_cfg.topologySignature.empty()) {
        g_cfg.topologySignature = g_topo.signature;
    }
}

}  // namespace

// ----------------------------------------------------------------------------
int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE hPrevInst, LPWSTR lpCmdLine, int nCmdShow) {
    (void)hPrevInst;
    (void)lpCmdLine;
    (void)nCmdShow;

    EngineShutdownGuard shutdownGuard;   // declared first => destroyed last, on EVERY return

    // ELEVATED SET DISPATCH MUST STAY FIRST. The helper has one job and returns before the
    // single-instance mutex, COM, window classes, config, tray, CPU Sets or engine exist.
    const CmdOptions opt = ParseCommandLine();
    if (opt.vcacheSet) return RunVCacheSet(opt.vcacheSetValue);

    g_hInst           = hInst;
    g_msgShowSettings = RegisterWindowMessageW(L"GameOptimizer.ShowSettings");

    // ---- Single instance ---------------------------------------------------
    // Local\ scope: one instance per session, so fast user switching still works.
    HANDLE hMutex = CreateMutexW(nullptr, TRUE, kMutexName);
    if (hMutex != nullptr && GetLastError() == ERROR_ALREADY_EXISTS) {
        if (g_msgShowSettings != 0) {
            PostMessageW(HWND_BROADCAST, g_msgShowSettings, 0, 0);
        }
        CloseHandle(hMutex);
        return 0;
    }

    // FIRST, before config/journal reads and before --tray controls launch visibility. The
    // rename migrations preserve existing state; the command migration prevents the old,
    // measured flagless Run value from opening Settings at every login.
    MigrateLegacyConfigDir();
    MigrateLegacyAutostart();
    MigrateAutostartCommand();

    if (opt.tray) LogLine(L"[main] started with --tray");

    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);

    INITCOMMONCONTROLSEX icc;
    icc.dwSize = sizeof(icc);
    icc.dwICC  = ICC_WIN95_CLASSES | ICC_STANDARD_CLASSES | ICC_LISTVIEW_CLASSES;
    InitCommonControlsEx(&icc);

    // The core map is a custom window class, not a common control: Settings and the first-run
    // wizard both CreateWindowEx it BY NAME, so the class has to exist before either of them
    // can build its children. Registering it here - after InitCommonControlsEx and before the
    // tray, the wizard or any ShowSettings - is what makes those calls return a window instead
    // of NULL. It is idempotent and logs its own failure.
    CoreMapRegister(hInst);

    // The sidebar rail is registered here for exactly the same reason and with the same
    // ordering rationale: Settings and the first-run wizard create it BY NAME, so the class
    // must exist before either of them builds its children. Idempotent, logs its own failure.
    cd::theme::NavRegister(hInst);

    if (!RegisterAndCreateWindow()) {
        MessageBoxW(nullptr,
                    L"Game Optimizer could not create its message window and cannot start.",
                    kAppTitle, MB_OK | MB_ICONERROR);
        CoUninitialize();
        if (hMutex) { ReleaseMutex(hMutex); CloseHandle(hMutex); }
        return 1;
    }

    // ---- Load, in the fixed order ------------------------------------------
    bool topologyChanged = false;
    std::wstring startupFailure;
    try {
        LoadEverything(topologyChanged);
    } catch (const std::exception& e) {
        g_topoOk       = false;
        startupFailure = Widen(e.what());
    } catch (...) {
        g_topoOk       = false;
        startupFailure = L"unknown exception during startup";
    }
    if (!startupFailure.empty()) {
        LogLine(L"[main] startup failed: %s", startupFailure.c_str());
        if (g_cfg.masks.empty() && g_cfg.profiles.empty()) g_cfg = DefaultConfig(g_topo);
    }

    // ---- --bench: measure and leave, no tray icon ---------------------------
    if (opt.bench) {
        int rc = 0;
        if (!g_topoOk) {
            MessageBoxW(nullptr,
                        L"--bench needs a detected CPU topology, and detection failed.",
                        kAppTitle, MB_OK | MB_ICONERROR);
            rc = 2;
        } else {
            g_engine.SetTopology(g_topo);
            g_engine.SetConfig(g_cfg);
            g_engineStarted = true;      // so StopEngineOnce clears whatever ticks applied
            rc = RunBench();
            StopEngineOnce();
        }
        DestroyWindow(g_hwnd);
        CoUninitialize();
        if (hMutex) { ReleaseMutex(hMutex); CloseHandle(hMutex); }
        return rc;
    }

    // ---- Tray, unconditionally ---------------------------------------------
    // Even a completely failed startup must leave something on screen that says so.
    if (!TrayInit(g_hwnd, WM_APP_TRAY)) {
        LogLine(L"[main] the tray icon could not be added");
    }
    TrayUpdate(CurrentStatus());

    if (!g_topoOk) {
        std::wstring msg =
            L"Game Optimizer could not read this machine's CPU topology, so it cannot "
            L"derive any CPU Set masks.\n\nNothing will be governed. The tray icon stays "
            L"available so you can open Settings, the log and the config folder.";
        if (!g_topoError.empty())    msg += L"\n\nDetail: " + g_topoError;
        if (!startupFailure.empty()) msg += L"\n\nDetail: " + startupFailure;
        MessageBoxW(g_hwnd, msg.c_str(), kAppTitle, MB_OK | MB_ICONERROR);
    }

    // ---- Foreground hook, first run, engine ---------------------------------
    bool wizardWasShown = false;
    try {
        StartForegroundTracking();

        if (topologyChanged) {
            MessageBoxW(g_hwnd,
                        L"This machine's CPU layout has changed since Game Optimizer last "
                        L"ran - a BIOS update, a CPU swap or a core-count change will do "
                        L"it.\n\nThe CPU Set masks have been re-derived from the live "
                        L"hardware. Your profiles were kept; check that each one still "
                        L"names the mask you want.",
                        kAppTitle, MB_OK | MB_ICONINFORMATION);
        }

        wizardWasShown = !g_cfg.firstRunDone;
        if (wizardWasShown) {
            RunFirstRunWizard(g_hwnd, g_cfg, g_topo);
            g_cfg.firstRunDone = true;   // contractually already true; never ask twice
            SaveConfigQuiet();
        } else {
            SaveConfigQuiet();           // persists repairs and any re-derived masks
        }

        if (g_topoOk) {
            g_engine.SetTopology(g_topo);
            g_engine.SetConfig(g_cfg);
            g_engine.SetPaused(g_cfg.paused);
            g_engine.Start(g_hwnd, WM_APP_STATUS);
            g_engineStarted = true;
            TrayUpdate(CurrentStatus());

            // Last, and only with a usable topology: with no masks to offer there is
            // nothing the prompt could truthfully promise to apply.
            StartGameDetection();
        }
    } catch (const std::exception& e) {
        LogLine(L"[main] engine start failed: %s", Widen(e.what()).c_str());
        MessageBoxW(g_hwnd,
                    L"Game Optimizer started but its engine could not be started. Nothing "
                    L"is being governed. See GameOptimizer.log in the config folder.",
                    kAppTitle, MB_OK | MB_ICONERROR);
    } catch (...) {
        LogLine(L"[main] engine start failed: unknown exception");
        MessageBoxW(g_hwnd,
                    L"Game Optimizer started but its engine could not be started. Nothing "
                    L"is being governed. See GameOptimizer.log in the config folder.",
                    kAppTitle, MB_OK | MB_ICONERROR);
    }

    // ---- Launch visibility --------------------------------------------------
    // The wizard above still finishes before a manual launch reaches Settings. --bench never
    // reaches here because it returned above, and --tray is reserved for the Run entry so a
    // login start stays quiet even for entries migrated from older builds.
    if (!opt.tray) {
        LogLine(wizardWasShown
                    ? L"[main] first run - opening Settings on the Profiles page"
                    : L"[main] ordinary launch - opening Settings on the Profiles page");
        OpenSettings(g_hwnd);
    }

    // ---- Message loop -------------------------------------------------------
    MSG m;
    ZeroMemory(&m, sizeof(m));   // GetMessage can fail before it ever writes to m
    BOOL got;
    while ((got = GetMessageW(&m, nullptr, 0, 0)) != 0) {
        if (got == -1) break;          // a bad hwnd would otherwise spin here forever
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }

    // ---- Shutdown, in the fixed order ---------------------------------------
    // The detector goes first: it is the only thread still posting to a window that has just
    // been destroyed, and stopping it here is what lets the queue be drained of candidates
    // nobody will read.
    StopGameDetection();
    StopEngineOnce();                  // clears every mask, joins the watcher, truncates
    StopForegroundTracking();          // UI thread only, which is where we are
    SaveConfigQuiet();
    TrayShutdown();
    // Last, and after the tray: the icons are built from theme colours, and nothing may
    // still be painting once the cached fonts and brushes are gone.
    cd::theme::Shutdown();

    CoUninitialize();
    if (hMutex) { ReleaseMutex(hMutex); CloseHandle(hMutex); }
    return static_cast<int>(m.wParam);
}
