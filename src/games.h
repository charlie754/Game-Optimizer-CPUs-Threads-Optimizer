// Game Optimizer - discovering the games installed on this machine.
//
// Why this exists: asking a user to type "Overwatch.exe" is a bad first experience. The app
// should be able to show them a list of games it already knows about and let them search it.
//
// WHERE THE LIST COMES FROM, and what was deliberately NOT used:
//   * Steam    - steamapps\libraryfolders.vdf gives every library path, then each
//                appmanifest_<id>.acf in those libraries gives a name and an install dir.
//                Plain text, documented by convention, no API and no elevation.
//   * Epic     - %ProgramData%\Epic\EpicGamesLauncher\Data\Manifests\*.item, JSON-ish.
//   * GOG      - HKLM\SOFTWARE\WOW6432Node\GOG.com\Games\<id>, values gameName / path.
//   * Xbox     - the Windows app model; games are packaged and their executables live under
//                WindowsApps, which is ACL-locked. Enumerated best-effort, may come back empty.
//   * Manual   - anything the user browses to, remembered.
//
// The operator asked whether the list could come from NVIDIA Profile Inspector. It could not,
// as far as I could establish: that tool reads the NVIDIA driver's own profile database
// through NVAPI's undocumented DRS (Driver Settings) entry points, which means taking a
// dependency on nvapi64.dll, an NVIDIA GPU, and an interface NVIDIA does not document for
// third parties. It would also give profile names for driver tuning rather than install paths.
// This is a statement about what I looked into, not a claim that it is impossible - if someone
// wants that route it should be re-examined rather than assumed closed.
//
// NOTHING HERE TOUCHES THE NETWORK. Discovery is entirely local filesystem and registry.
#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <string>
#include <vector>

namespace cd {

enum class GameSource { Steam, Epic, Gog, Xbox, Running, Manual, Bundled };

struct GameEntry {
    std::wstring name;        // display name, e.g. "Overwatch 2"
    std::wstring exe;         // basename we match on, e.g. "Overwatch.exe"
    std::wstring fullPath;    // full path when known; may be empty
    std::wstring installDir;  // may be empty
    GameSource   source = GameSource::Bundled;
    bool         installed = true;   // false for a bundled entry we did not find on disk
};

const wchar_t* GameSourceName(GameSource s);

// Scan every source above. Slow enough (tens of ms to a few hundred) that it must not run on
// the UI thread during a paint; call it once on a worker and cache. Never throws.
// Entries are de-duplicated by lowercased exe basename, preferring an installed entry with a
// real path over a bundled one.
std::vector<GameEntry> DiscoverGames();

// A small built-in list of well-known game executables, used to (a) recognise a running
// process as a game even when no launcher claims it, and (b) give a new user something to
// search before any scan completes. Names only - no paths, no versions, nothing that goes
// stale badly.
const std::vector<GameEntry>& BundledGames();

// Case-insensitive substring match over name and exe. Empty query returns everything.
// Results keep the input order, so callers control ranking.
std::vector<GameEntry> FilterGames(const std::vector<GameEntry>& all,
                                   const std::wstring& query);

// ---- Runtime game detection ------------------------------------------------
// Used by the "we noticed you started X" prompt and by the All Games profile.
//
// A process is treated as a game when it is NOT excluded and at least one holds:
//   * its basename is in the discovered or bundled game list, OR
//   * it owns a visible top-level window that covers a whole monitor (fullscreen or
//     borderless), AND it has a graphics runtime loaded.
//
// The second test is a HEURISTIC and will be wrong sometimes - a fullscreen video player or a
// browser in presentation mode can satisfy it. That is why the prompt asks rather than acts,
// and why a wrong guess costs one Cancel rather than a mis-pinned machine.
struct GameGuess {
    DWORD pid = 0;
    std::wstring exe;
    std::wstring displayName;   // from the game list when known, else the exe basename
    bool  fromKnownList = false;
    bool  fullscreen = false;
    double confidence = 0.0;    // 0..1; known-list hits score high, heuristic-only lower
};

// Inspect one process. Returns false when it does not look like a game at all.
bool GuessGame(DWORD pid, const std::wstring& exeBaseName,
               const std::vector<GameEntry>& known, GameGuess& out);

// Does this pid own a visible window covering an entire monitor?
bool HasFullscreenWindow(DWORD pid);

}  // namespace cd
