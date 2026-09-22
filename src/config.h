// Game Optimizer - configuration model, INI load/save.
//
// INI, not JSON, on purpose: a hand-written JSON parser is a correctness liability for no
// gain here, and this grammar is three rules.
//   * a line is blank, a "; " or "# " comment, a "[section]", or "key=value"
//   * everything after the FIRST '=' is the value, verbatim (so paths with '=' survive)
//   * lists use '|', which is illegal in Windows filenames and so cannot occur in a value
// Unknown sections and unknown keys are preserved verbatim on save, so an older binary
// cannot destroy a newer config.
//
// No Win32 UI here: this TU links into the unit-test harness.
#pragma once
#include <map>
#include <string>
#include <vector>
#include "topology.h"

namespace cd {

struct Profile {
    std::wstring name;                    // section suffix, e.g. [profile:Overwatch]
    bool enabled = true;
    std::wstring game;                    // basename ("Overwatch.exe") or a full path
    std::wstring gameMask;                // mask name; must exist in Config::masks
    std::vector<std::wstring> heavy;      // basenames
    std::wstring heavyMask;

    // The foreground CPU% rule. ON by default - operator decision: a user who adds a profile
    // wants the machine managed, and leaving the useful half switched off is a worse default
    // than the occasional unwanted pin, which one Cancel undoes.
    bool autoPin = true;
    int  autoPinPercent = 3;              // 1..100, machine-wide %

    // NOT USER-EDITABLE, and deliberately still here.
    // The operator asked for "no more second limit", and the seconds control is gone from the
    // UI. This field stays as a fixed internal debounce because auto-pin is STICKY: a process
    // it pins stays pinned until the game exits. Firing on a single 250 ms sample would let a
    // momentary spike - a compile starting, a browser tab loading - strand that process on the
    // background mask for the whole session. Two ticks is the smallest value that still
    // requires the load to be real rather than instantaneous.
    // Config never writes this and ValidateAndRepair forces it back; see kAutoPinDebounceTicks.
    int  autoPinSeconds = 1;

    // EXTREME GAME MODE. OFF BY DEFAULT, and that default is a hard product rule rather than
    // a preference: this is the only rule in the program that touches a process the user has
    // never named and that is not busy. When it is on, every live process that is not the
    // game and not excluded is moved to `heavyMask` while the profile is governing - see
    // engine.h rule 4b.
    //
    // TWO THINGS IT DELIBERATELY DOES NOT DO, both of which a later edit will be tempted to
    // "fix":
    //   * it never overrides an exclusion. Rule 3 honours an explicit heavy-list entry even
    //     when the name is excluded, because that entry is the user naming one process. A
    //     blanket sweep is not a user naming anything, so it does not inherit that override.
    //   * it does nothing at all when no profile is governing, and nothing when `heavyMask`
    //     is empty - a blanket sweep to "clear" is not a thing anybody asked for.
    bool extremeMode = false;

    // RETIRED, AND THIS FIELD IS NOT THE FEATURE - IT IS THE MIGRATION SHIM. "All Games" was
    // a profile type that matched ANY process that looked like a game rather than one named
    // executable. v0.5.4 deleted it: the engine rule, the accessor, the pill and the
    // generated candidate list are all gone. This bool is the only thing left that knows the
    // word, and it exists so that an older config can be migrated rather than misread.
    //
    // WRITTEN ONLY BY ParseConfig, from the `all_games` key an earlier version wrote, and
    // NEVER SERIALIZED - so the key leaves the user's file on the next save and cannot come
    // back. ValidateAndRepair consumes it, clears it, clears the profile's stale `game`, and
    // reports what it did. Rule 6b in config.cpp says why the clear is not optional.
    //
    // ponytail: a permanent shim for a transient problem; it can go once no config in the
    // wild carries `all_games=`, which is not a date anyone can name. The ceiling is one bool.
    bool legacyAllGames = false;

    // Most-recently-used stamp (FILETIME as ULONGLONG, 0 = never). Drives the ordering of the
    // profile list: used profiles first, newest at the top, a separator, then the rest.
    ULONGLONG lastUsed = 0;

    // GPU isolation - per-profile overrides. Empty/absent means inherit from global [gpus] section.
    std::wstring gameGpu;      // adapter key if this profile should override the global game GPU
    std::wstring heavyGpu;     // adapter key if this profile should override the global background GPU
    bool gpuApplied = false;   // whether the preference has been written to the registry for this profile's game
};

// The fixed debounce described above, in poll ticks. At the default 250 ms poll this is 500 ms.
constexpr int kAutoPinDebounceTicks = 2;

struct Config {
    int  version = 1;
    bool startWithWindows = false;
    int  pollMs = 250;                    // clamped to 100..2000 on load
    bool notifications = false;
    bool paused = false;
    // The driver's configured Start value before this app disabled it. -1 means the app has
    // changed nothing. Restoring a guessed Manual value would silently rewrite a user's setup.
    int  vcacheOriginalStart = -1;
    bool firstRunDone = false;
    // Opt-out belongs to the V-Cache startup section only; Game Mode still reports itself.
    // True here also keeps the warning for older configs that have no preference key yet.
    bool showVCacheWarning = true;

    std::wstring topologySignature;       // guards stale CPU Set Ids - see topology.h
    std::vector<Mask> masks;
    std::vector<Profile> profiles;
    // Basenames applied to gameSet and autoPin. A final '*' is an exclusion-only prefix
    // wildcard; a bare '*' is ignored so it cannot disable the feature globally.
    std::vector<std::wstring> exclusions;

    // GPU isolation settings. Defaults: ON (true) for machines with 2+ GPUs, empty adapter keys.
    // Per-profile overrides in Profile::gameGpu and Profile::heavyGpu.
    bool autoIsolateGpu = true;      // whether GPU isolation is enabled
    std::wstring gameGpu;            // global default adapter key for game GPU (empty = not set)
    std::wstring backgroundGpu;      // global default adapter key for background GPU (empty = not set)

    // GPU Assignment also tells NVIDIA which GPU CUDA may use for that application (v0.5.8). ON by
    // default: an application moved to another GPU whose CUDA work stays on the game's card has not
    // really been moved, which is the whole point of the feature. It changes no registry value of its
    // own - it only decides whether APPLY also touches NVIDIA's driver profile, and with it off nothing
    // CUDA-related happens on an Apply and nothing about it is said.
    // 🔴 IT DOES NOT GATE REMOVE (Council round 1, F7). Remove assignment always puts back what this
    // product wrote, because a switch that can turn a change on but not off is a trap - clearing this
    // box would otherwise make every CUDA change already made permanent.
    bool setCudaGpu = true;

    // Verbatim lines from sections/keys this version did not recognise. Key is the
    // section name; value is the raw "key=value" lines. Re-emitted on save.
    std::map<std::wstring, std::vector<std::wstring>> unknown;

    const Mask* FindMask(const std::wstring& name) const;
    Mask*       FindMask(const std::wstring& name);
    const Profile* FindProfile(const std::wstring& name) const;
    bool IsExcluded(const std::wstring& exeBaseName) const;

    // Indices into `profiles`, ordered for display: profiles with lastUsed != 0 first, newest
    // first; then everything else in vector order. `separatorAfter` receives the count of
    // recently-used entries, so the UI can draw its divider - or -1 when there are none.
    std::vector<size_t> ProfilesForDisplay(int* separatorAfter) const;

    // Stamp a profile as used now. Called by the engine when a profile's game starts.
    void MarkProfileUsed(const std::wstring& name, ULONGLONG nowFileTime);
};

// A fresh config for this machine: masks derived from `t`, the default exclusion list, and
// one ENABLED profile named "Overwatch" so a new user has a worked example to adopt or delete
// rather than a blank screen. firstRunDone is false.
Config DefaultConfig(const Topology& t);

// Ships pre-populated: anti-cheat services, launchers, GPU vendor containers, audiodg and
// core Windows processes. PIDs 0 and 4 are refused in the engine regardless of this list.
std::vector<std::wstring> DefaultExclusions();

// Disk. LoadConfig returns false and sets *error when the file exists but cannot be parsed;
// a MISSING file is not an error - it returns false with *error empty so the caller can
// tell "no config yet" from "broken config".
bool LoadConfig(const std::wstring& path, Config& out, std::wstring* error);
bool SaveConfig(const std::wstring& path, const Config& c, std::wstring* error);

// In-memory, for tests. ParseConfig tolerates CRLF and LF, a UTF-8 BOM, and a malformed
// trailing line; it never throws.
bool ParseConfig(const std::wstring& text, Config& out, std::wstring* error);
std::wstring SerializeConfig(const Config& c);

// Applied after load and before use. Clamps pollMs, drops profiles naming a mask that does
// not exist, drops empty mask id lists, and de-duplicates. Returns a human-readable list of
// what it changed, empty when the config was already valid.
std::vector<std::wstring> ValidateAndRepair(Config& c, const Topology& t);

}  // namespace cd
