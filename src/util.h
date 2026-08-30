// Game Optimizer - small shared helpers. No product logic lives here.
#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <string>
#include <vector>

namespace cd {

// ---- Paths -----------------------------------------------------------------
// All under %LOCALAPPDATA%\GameOptimizer\, created on first use.
std::wstring GetConfigDir();
std::wstring GetConfigPath();    // config.ini
std::wstring GetJournalPath();   // applied.journal
std::wstring GetLogPath();       // GameOptimizer.log
std::wstring GetExePath();       // full path of the running exe

// ---- Strings ---------------------------------------------------------------
std::string  Narrow(const std::wstring& s);          // UTF-8
std::wstring Widen(const std::string& s);            // from UTF-8
std::wstring ToLower(std::wstring s);
std::wstring Trim(const std::wstring& s);            // strips space and tab, both ends
std::vector<std::wstring> Split(const std::wstring& s, wchar_t sep);  // empties dropped
std::wstring Join(const std::vector<std::wstring>& v, wchar_t sep);
std::wstring BaseName(const std::wstring& path);     // "C:\a\b.exe" -> "b.exe"
bool IEquals(const std::wstring& a, const std::wstring& b);
bool ParseIntW(const std::wstring& s, int& out);
bool ParseUlongW(const std::wstring& s, unsigned long& out);

// ---- Files -----------------------------------------------------------------
// UTF-8 on disk, wide in memory. ReadFileUtf8 returns false when the file is absent.
bool ReadFileUtf8(const std::wstring& path, std::wstring& out);
// Writes via a .tmp + MoveFileEx replace, so an interrupted write cannot truncate config.
bool WriteFileUtf8Atomic(const std::wstring& path, const std::wstring& text);

// ---- AMD 3D V-Cache status ------------------------------------------------
// Returns the Start value from HKLM\SYSTEM\CurrentControlSet\Services\<name>, or -1.
// This is query-only and does not require elevation.
int ReadServiceStartValue(const wchar_t* serviceName);

// ---- Autostart (HKCU\Software\Microsoft\Windows\CurrentVersion\Run) ---------
// The value is named "GameOptimizer".
std::wstring AutostartCommand(const std::wstring& exePath);
bool AutostartNeedsMigration(const std::wstring& existingValue);
bool GetStartWithWindows();
bool SetStartWithWindows(bool on);
// Older builds wrote a flagless command, which would make login startup open Settings.
// Repairs only an existing value; an absent value means the user left autostart disabled.
void MigrateAutostartCommand();

// ---- Migration from the previous product name ("CoreDirector") --------------
// Idempotent, safe to call on every start, and both log what they did.  Call BOTH before
// anything reads the config or the journal.
//
// MigrateLegacyConfigDir: when %LOCALAPPDATA%\GameOptimizer\config.ini is absent but
//   %LOCALAPPDATA%\CoreDirector\config.ini is present, COPIES config.ini and, with it,
//   applied.journal.  The old folder is never deleted - a copy is reversible, a move is not,
//   and this is the user's settings.
void MigrateLegacyConfigDir();
//
// MigrateLegacyAutostart: the old HKCU\...\Run value was named "CoreDirector" and pointed at
//   an exe that no longer exists, so Windows fails it silently at every login and leaves the
//   value behind forever.  Deletes it, and re-creates the run entry under the new name at the
//   CURRENT GetExePath() - but ONLY when the old value was actually present, so a user who
//   turned autostart off is never switched back on.
void MigrateLegacyAutostart();

// ---- Environment probes, for the first-run wizard and the risk warnings -----
enum class GameModeState { NotDeterminable, Off, On };
enum class AmdVCacheServiceState {
    NotDeterminable,
    NotInstalled,
    InstalledButStopped,
    Running
};

struct EnvironmentInfo {
    std::wstring cpuBrand;                  // from the registry, e.g. "AMD Ryzen 9 9950X3D..."
    bool  isAmd = false;
    bool  isIntel = false;
    bool  gameModeKeyPresent = false;       // HKCU\Software\Microsoft\GameBar exists
    DWORD autoGameModeEnabled = 0;          // ...\AutoGameModeEnabled (0 == Game Mode off)
    GameModeState gameModeState = GameModeState::NotDeterminable;
    bool  amdVCacheServicePresent = false;  // service "amd3dvcacheSvc"
    bool  amdVCacheServiceRunning = false;
    AmdVCacheServiceState amdVCacheServiceState =
        AmdVCacheServiceState::NotDeterminable;
    bool  amdVCacheDriverPresent = false;   // kernel driver "amd3dvcache"
    bool  amdVCacheDriverRunning = false;
    AmdVCacheServiceState amdVCacheDriverState =
        AmdVCacheServiceState::NotDeterminable;
    bool  isElevated = false;
};
EnvironmentInfo ProbeEnvironment();
// Refreshes only the live scheduling influences: one HKCU value read and two named status
// queries. It never enumerates services and never writes any source.
void RefreshEnvironmentStatus(EnvironmentInfo& info);

void OpenGameModeSettings();                    // ms-settings:gaming-gamemode
void OpenFolder(const std::wstring& path);

// ---- Logging ---------------------------------------------------------------
// Appends to GetLogPath(); truncates when it passes 1 MB. Safe from any thread.
void LogLine(const wchar_t* fmt, ...);

}  // namespace cd
