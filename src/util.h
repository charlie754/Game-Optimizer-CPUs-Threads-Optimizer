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

// THREE OUTCOMES, NOT TWO, AND THE THIRD ONE IS A CORRECTNESS REQUIREMENT.
//
// "The file is not there" and "the file is there and I could not read it" are opposite
// facts about the same failed read, and a caller that rewrites the file cannot treat them
// alike: rewriting over a MISSING file creates it, and rewriting over an UNREADABLE one
// DESTROYS whatever it held. The restore journal is exactly that caller - it reads, edits
// and rewrites - and it conflated the two until v0.4.4, so a single unreadable read
// silently dropped every recovery record on disk.
//
// Missing    - CreateFileW answered ERROR_FILE_NOT_FOUND / ERROR_PATH_NOT_FOUND, i.e. there
//              is provably nothing there.
// Unreadable - anything else: a sharing violation, a denied ACL, a directory, a file too
//              large to be one of ours, a read that failed part way. Content MAY exist.
enum class FileReadResult { Ok, Missing, Unreadable };
FileReadResult ReadFileUtf8Checked(const std::wstring& path, std::wstring& out);

// UTF-8 on disk, wide in memory. ReadFileUtf8 returns false when the file is absent.
// It is ReadFileUtf8Checked with the two failures folded back together, kept for the callers
// that genuinely cannot act on the difference (a missing config is a default config).
bool ReadFileUtf8(const std::wstring& path, std::wstring& out);

// Writes via a .tmp + MoveFileEx replace, so an interrupted write cannot truncate config.
// FALSE MEANS NOTHING REACHED DISK: the temp file is deleted and the destination is left
// exactly as it was. A failed FlushFileBuffers counts as a failure - see util.cpp for why
// ignoring it made "the journal write succeeded" a claim about a buffer rather than a disk.
bool WriteFileUtf8Atomic(const std::wstring& path, const std::wstring& text);

// ---- AMD 3D V-Cache status ------------------------------------------------
// Returns the Start value from HKLM\SYSTEM\CurrentControlSet\Services\<name>, or -1.
// This is query-only and does not require elevation.
int ReadServiceStartValue(const wchar_t* serviceName);

// ---- Autostart (HKCU\Software\Microsoft\Windows\CurrentVersion\Run) ---------
// The value is named "GameOptimizer".
std::wstring AutostartCommand(const std::wstring& exePath);
std::wstring AutostartExeFromCommand(const std::wstring& command);
bool AutostartNeedsMigration(const std::wstring& existingValue,
                             const std::wstring& currentExe);
bool GetStartWithWindows();
bool SetStartWithWindows(bool on);
// Repairs an existing value that lacks --tray, or that names a different exe copy. The flag
// test is a substring search, so a stored command whose PATH happens to contain "--tray" reads
// as already migrated; that is a pre-existing limitation of the flag half, not of the path half.
// An absent value means the user left autostart disabled and is left alone.
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

// ---- AMD V-Cache policy agent image name ----------------------------------
// Exported so the engine can match the same name without duplication.
extern const wchar_t* const kVCacheAgentImage;

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
    // AMD's actual policy engine: it watches the foreground window and issues the IOCTL.
    // The service only launches it and the kernel driver is a relay, so neither of those
    // means anything is steering - THIS is the flag that means something really is.
    bool  amdVCacheAgentRunning = false;    // "amd3dvcacheUser.exe" in this logon session
    bool  isElevated = false;
};
EnvironmentInfo ProbeEnvironment();
// True when `pid` belongs to the caller's own logon session. False if either session
// cannot be read - an unreadable session is not evidence of a match.
bool IsProcessInOurSession(DWORD pid);

// True when AMD's V-Cache policy agent is running in the CALLER'S logon session.
// Cheap enough to call on the engine's tick path - it is one process snapshot and no
// registry or service-control access, unlike ProbeEnvironment().
bool IsAmdVCacheAgentRunning();
// Refreshes only the live scheduling influences: one HKCU value read, two named status
// queries and one process snapshot. It never enumerates services and never writes any source.
void RefreshEnvironmentStatus(EnvironmentInfo& info);

void OpenGameModeSettings();                    // ms-settings:gaming-gamemode
void OpenFolder(const std::wstring& path);

// ---- Logging ---------------------------------------------------------------
// Appends to GetLogPath(); truncates when it passes 1 MB. Safe from any thread.
void LogLine(const wchar_t* fmt, ...);

}  // namespace cd
