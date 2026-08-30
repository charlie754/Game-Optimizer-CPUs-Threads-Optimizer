// Game Optimizer - configuration model, INI load/save.
//
// The grammar is the one stated in config.h and nothing more:
//   * a line is blank, a comment (first non-space char is ';' or '#'), a "[section]",
//     or "key=value"
//   * the split is on the FIRST '=' only, so both mask names and values may contain '='
//   * lists use '|' (illegal in a Windows filename, so it cannot occur inside a value)
//
// Two invariants this file exists to defend:
//   1. ParseConfig is TOTAL. Every malformed line is skipped, never fatal, never throws.
//      A user hand-editing config.ini must not be able to brick the tray.
//   2. ParseConfig(SerializeConfig(c)) == c, INCLUDING Config::unknown. Sections and keys
//      this build does not recognise are carried through verbatim, so running an older
//      binary once cannot silently delete a newer build's settings.
//
// No Win32 UI and no mutable globals: this TU links into the unit-test harness.
#include "config.h"

#include <algorithm>

#include "util.h"

namespace cd {
namespace {

// ---- Canonical section and key names ---------------------------------------
// Everything the serializer emits is lowercase; the parser matches case-insensitively so
// a hand-edited "[General]" or "Poll_Ms=" still lands on the right field.
const wchar_t* const kSecGeneral    = L"general";
const wchar_t* const kSecMasks      = L"masks";
const wchar_t* const kSecTopology   = L"topology";
const wchar_t* const kSecExclusions = L"exclusions";
const wchar_t* const kSecProfixLow  = L"profile:";   // followed by the profile name

// A mask the user has hand-edited (Mask::derived == false) is written with a leading
// "custom" token in its id list: "Cache=custom,256,258". A plain list is a derived mask.
// The token is not a number, so an older parser that only reads numbers ignores it.
const wchar_t* const kCustomToken = L"custom";

enum class SecKind { None, General, Masks, Topology, Exclusions, Profile, Unknown };

struct SectionRef {
    SecKind kind = SecKind::None;
    std::wstring canonical;   // key used in Config::unknown
    std::wstring profileName; // only for SecKind::Profile
};

bool StartsWithNoCase(const std::wstring& s, const wchar_t* prefix) {
    const size_t n = wcslen(prefix);
    if (s.size() < n) return false;
    return IEquals(s.substr(0, n), std::wstring(prefix));
}

SectionRef ClassifySection(const std::wstring& raw) {
    SectionRef r;
    const std::wstring name = Trim(raw);
    if (IEquals(name, std::wstring(kSecGeneral))) {
        r.kind = SecKind::General;   r.canonical = kSecGeneral;   return r;
    }
    if (IEquals(name, std::wstring(kSecMasks))) {
        r.kind = SecKind::Masks;     r.canonical = kSecMasks;     return r;
    }
    if (IEquals(name, std::wstring(kSecTopology))) {
        r.kind = SecKind::Topology;  r.canonical = kSecTopology;  return r;
    }
    if (IEquals(name, std::wstring(kSecExclusions))) {
        r.kind = SecKind::Exclusions; r.canonical = kSecExclusions; return r;
    }
    if (StartsWithNoCase(name, kSecProfixLow)) {
        r.kind = SecKind::Profile;
        r.profileName = Trim(name.substr(wcslen(kSecProfixLow)));
        r.canonical = std::wstring(kSecProfixLow) + r.profileName;
        return r;
    }
    r.kind = SecKind::Unknown;
    r.canonical = name;   // preserved exactly as written
    return r;
}

// ---- Line handling ---------------------------------------------------------

// Accepts CRLF, LF and a lone CR. The final line is returned even without a terminator.
std::vector<std::wstring> SplitLines(const std::wstring& text) {
    std::vector<std::wstring> lines;
    std::wstring cur;
    for (size_t i = 0; i < text.size(); ++i) {
        const wchar_t ch = text[i];
        if (ch == L'\n') {
            lines.push_back(cur);
            cur.clear();
        } else if (ch == L'\r') {
            lines.push_back(cur);
            cur.clear();
            if (i + 1 < text.size() && text[i + 1] == L'\n') ++i;
        } else {
            cur.push_back(ch);
        }
    }
    lines.push_back(cur);
    return lines;
}

bool IsCommentLine(const std::wstring& trimmed) {
    if (trimmed.empty()) return false;
    return trimmed[0] == L';' || trimmed[0] == L'#';
}

// 0/1, true/false, yes/no, on/off - case-insensitive. Returns false on anything else and
// leaves `out` untouched, so a garbage value keeps the field's existing default.
bool ParseBoolW(const std::wstring& v, bool& out) {
    const std::wstring s = ToLower(Trim(v));
    if (s == L"1" || s == L"true" || s == L"yes" || s == L"on")   { out = true;  return true; }
    if (s == L"0" || s == L"false" || s == L"no" || s == L"off")  { out = false; return true; }
    return false;
}

const wchar_t* BoolText(bool b) { return b ? L"1" : L"0"; }

// util.h only offers a 32-bit ParseUlongW, and a FILETIME stamp needs 64 bits. Local, total,
// and saturating: a value too large to represent is clamped rather than wrapped, because a
// wrapped stamp would silently reorder the recently-used list.
bool ParseUlonglongW(const std::wstring& s, ULONGLONG& out) {
    const std::wstring t = Trim(s);
    if (t.empty()) return false;
    ULONGLONG v = 0;
    bool any = false;
    for (size_t i = 0; i < t.size(); ++i) {
        const wchar_t ch = t[i];
        if (ch < L'0' || ch > L'9') return false;
        const ULONGLONG d = static_cast<ULONGLONG>(ch - L'0');
        if (v > (0xFFFFFFFFFFFFFFFFull - d) / 10ull) {
            v = 0xFFFFFFFFFFFFFFFFull;
            any = true;
            continue;   // keep validating the remaining characters
        }
        v = v * 10ull + d;
        any = true;
    }
    if (!any) return false;
    out = v;
    return true;
}

int ClampInt(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

// ---- Serialization helpers -------------------------------------------------

void AppendKv(std::wstring& out, const wchar_t* key, const std::wstring& value) {
    out += key;
    out += L'=';
    out += value;
    out += L"\r\n";
}

void AppendKv(std::wstring& out, const std::wstring& key, const std::wstring& value) {
    out += key;
    out += L'=';
    out += value;
    out += L"\r\n";
}

bool AlreadyConsumed(const std::vector<std::wstring>& consumed, const std::wstring& key) {
    for (size_t i = 0; i < consumed.size(); ++i) {
        if (consumed[i] == key) return true;
    }
    return false;
}

// Re-emits the preserved lines belonging to `section` and records which map keys were used.
void AppendUnknownFor(std::wstring& out, const Config& c, const std::wstring& section,
                      std::vector<std::wstring>& consumed) {
    for (std::map<std::wstring, std::vector<std::wstring>>::const_iterator it = c.unknown.begin();
         it != c.unknown.end(); ++it) {
        if (!IEquals(it->first, section)) continue;
        for (size_t i = 0; i < it->second.size(); ++i) {
            out += it->second[i];
            out += L"\r\n";
        }
        consumed.push_back(it->first);
    }
}

std::wstring MaskValue(const Mask& m) {
    std::wstring v;
    if (!m.derived) v = std::wstring(kCustomToken);
    for (size_t i = 0; i < m.ids.size(); ++i) {
        if (!v.empty()) v += L',';
        v += std::to_wstring(static_cast<unsigned long long>(m.ids[i]));
    }
    return v;
}

// Returns false when the value cannot be a mask id list at all, which is how a preserved
// unknown key is told apart from a mask: in [masks] every key is a user-chosen mask NAME,
// so there is no keyword to match on. The test is "empty, or carries at least one id or
// the custom marker" - that keeps a tolerant "1,two,3" as the mask {1,3} while letting a
// future build's "some_key=some_word" survive verbatim into Config::unknown.
bool ParseMaskValue(const std::wstring& value, Mask& m) {
    if (Trim(value).empty()) return true;   // a named mask with no processors yet
    bool sawSomething = false;
    const std::vector<std::wstring> tokens = Split(value, L',');
    for (size_t i = 0; i < tokens.size(); ++i) {
        const std::wstring tok = Trim(tokens[i]);
        if (tok.empty()) continue;
        if (IEquals(tok, std::wstring(kCustomToken))) {
            m.derived = false;
            sawSomething = true;
            continue;
        }
        unsigned long id = 0;
        if (ParseUlongW(tok, id)) {
            m.ids.push_back(static_cast<ULONG>(id));
            sawSomething = true;
        }
        // Anything else is skipped: one malformed token must not lose the whole mask.
    }
    return sawSomething;
}

}  // namespace

// ---- Config lookups --------------------------------------------------------

const Mask* Config::FindMask(const std::wstring& name) const {
    for (size_t i = 0; i < masks.size(); ++i) {
        if (IEquals(masks[i].name, name)) return &masks[i];
    }
    return nullptr;
}

Mask* Config::FindMask(const std::wstring& name) {
    for (size_t i = 0; i < masks.size(); ++i) {
        if (IEquals(masks[i].name, name)) return &masks[i];
    }
    return nullptr;
}

const Profile* Config::FindProfile(const std::wstring& name) const {
    for (size_t i = 0; i < profiles.size(); ++i) {
        if (IEquals(profiles[i].name, name)) return &profiles[i];
    }
    return nullptr;
}

// Display order: everything the user has actually used, newest first, then everything else in
// the order it appears in the file. std::stable_sort is load-bearing - two profiles stamped in
// the same 100 ns tick must not swap places between two paints of the same list.
std::vector<size_t> Config::ProfilesForDisplay(int* separatorAfter) const {
    std::vector<size_t> used;
    std::vector<size_t> rest;
    for (size_t i = 0; i < profiles.size(); ++i) {
        if (profiles[i].lastUsed != 0) {
            used.push_back(i);
        } else {
            rest.push_back(i);
        }
    }

    const std::vector<Profile>* pv = &profiles;
    std::stable_sort(used.begin(), used.end(),
                     [pv](size_t a, size_t b) { return (*pv)[a].lastUsed > (*pv)[b].lastUsed; });

    if (separatorAfter) *separatorAfter = used.empty() ? -1 : static_cast<int>(used.size());

    std::vector<size_t> order;
    order.reserve(profiles.size());
    order.insert(order.end(), used.begin(), used.end());
    order.insert(order.end(), rest.begin(), rest.end());
    return order;
}

void Config::MarkProfileUsed(const std::wstring& name, ULONGLONG nowFileTime) {
    for (size_t i = 0; i < profiles.size(); ++i) {
        if (IEquals(profiles[i].name, name)) {
            profiles[i].lastUsed = nowFileTime;
            return;
        }
    }
    // No-op when the profile is gone: the engine may stamp a profile the user deleted
    // between one poll tick and the next, and that must not resurrect it.
}

const Profile* Config::AllGamesProfile() const {
    for (size_t i = 0; i < profiles.size(); ++i) {
        if (profiles[i].isAllGames) return &profiles[i];
    }
    return nullptr;
}

bool Config::IsExcluded(const std::wstring& exeBaseName) const {
    const std::wstring want = BaseName(exeBaseName);
    if (want.empty()) return false;
    for (size_t i = 0; i < exclusions.size(); ++i) {
        const std::wstring spec = BaseName(exclusions[i]);
        if (spec == L"*") continue;  // Never let one malformed entry exclude every process.
        if (spec.size() > 1 && spec[spec.size() - 1] == L'*') {
            const std::wstring prefix = spec.substr(0, spec.size() - 1);
            if (want.size() >= prefix.size() &&
                IEquals(want.substr(0, prefix.size()), prefix)) {
                return true;
            }
            continue;
        }
        if (IEquals(spec, want)) return true;
    }
    return false;
}

// ---- Defaults --------------------------------------------------------------

std::vector<std::wstring> DefaultExclusions() {
    std::vector<std::wstring> v;
    v.push_back(L"EasyAntiCheat.exe");
    v.push_back(L"EasyAntiCheat_EOS.exe");
    v.push_back(L"BEService.exe");
    v.push_back(L"vgc.exe");
    v.push_back(L"vgtray.exe");
    v.push_back(L"audiodg.exe");
    // Real-time camera/mic processing, like the audio engine above; moving it to a
    // background or parked mask risks live-stream dropouts.
    v.push_back(L"NVIDIA Broadcast*");
    v.push_back(L"dwm.exe");
    v.push_back(L"csrss.exe");
    v.push_back(L"services.exe");
    v.push_back(L"svchost.exe");
    v.push_back(L"lsass.exe");
    v.push_back(L"wininit.exe");
    v.push_back(L"winlogon.exe");
    v.push_back(L"explorer.exe");
    v.push_back(L"MsMpEng.exe");
    v.push_back(L"NVDisplay.Container.exe");
    v.push_back(L"nvcontainer.exe");
    v.push_back(L"AMDRSServ.exe");
    v.push_back(L"RadeonSoftware.exe");
    // AMD display-driver client core; a timeout here can leave the user with a black screen.
    v.push_back(L"atieclxx.exe");
    // AMD display-driver service core; starving it can cause a driver timeout or black screen.
    v.push_back(L"atiesrxx.exe");
    // AMD display-driver helper; moving vendor driver helpers risks destabilizing the driver path.
    v.push_back(L"amdow.exe");
    // AMD V-Cache optimizer owns CCD parking decisions; pinning that coordinator defeats its job.
    v.push_back(L"amd3dvcache*");
    // AMD Crash Defender is the driver-timeout recovery path; it must remain unconstrained.
    v.push_back(L"amdfendr*");
    // AMD driver app-compatibility service; moving it risks disrupting the protected driver path.
    v.push_back(L"AmdAppCompat*");
    // AMD driver provisioning-package service; vendor driver services are never auto-pin targets.
    v.push_back(L"AmdPpkg*");
    // AMD Radeon source extension is real-time driver media plumbing; do not move that path.
    v.push_back(L"AMDRSSrcExt.exe");
    v.push_back(L"steam.exe");
    v.push_back(L"EpicGamesLauncher.exe");
    v.push_back(L"Battle.net.exe");
    v.push_back(L"GameOptimizer.exe");
    return v;
}

Config DefaultConfig(const Topology& t) {
    Config c;
    c.version = 1;
    c.startWithWindows = false;
    c.pollMs = 250;
    c.notifications = false;
    c.paused = false;
    c.firstRunDone = false;
    c.topologySignature = t.signature;
    c.masks = DeriveMasks(t);
    c.exclusions = DefaultExclusions();

    // The default heavy list, shared by both shipped profiles.
    std::vector<std::wstring> heavy;
    heavy.push_back(L"claude.exe");
    heavy.push_back(L"node.exe");
    heavy.push_back(L"obs64.exe");
    // NVIDIA Broadcast is deliberately absent: its real-time media work is excluded by
    // default from background or parked masks; see DefaultExclusions().
    heavy.push_back(L"firefox.exe");

    // A worked example, ENABLED - operator decision, reversing the original "ships disabled"
    // rule. A profile the user has to go and switch on is a profile that never runs, and the
    // engine only ever acts while the named game is actually in front, which one Cancel undoes.
    Profile p;
    p.name = L"Overwatch";
    p.enabled = true;
    p.game = L"Overwatch.exe";
    p.gameMask = t.defaultGameMask;
    p.heavyMask = t.defaultHeavyMask;
    p.heavy = heavy;
    p.autoPin = true;
    p.autoPinPercent = 8;
    p.autoPinSeconds = kAutoPinDebounceTicks;
    c.profiles.push_back(p);

    // ALL GAMES, and it must be LAST: config.h states the All Games profile is considered
    // after every specific one, so a named game always wins over it. Its `game` field is
    // empty by definition - it matches on "looks like a game", not on a name.
    Profile all;
    all.name = L"All Games";
    all.isAllGames = true;
    all.enabled = true;
    all.game.clear();
    all.gameMask = t.defaultGameMask;
    all.heavyMask = t.defaultHeavyMask;
    all.heavy = heavy;
    all.autoPin = true;
    all.autoPinPercent = 8;
    all.autoPinSeconds = kAutoPinDebounceTicks;
    c.profiles.push_back(all);
    return c;
}

// ---- Parse -----------------------------------------------------------------

bool ParseConfig(const std::wstring& text, Config& out, std::wstring* error) {
    if (error) error->clear();

    out = Config();
    out.masks.clear();
    out.profiles.clear();
    out.exclusions.clear();
    out.unknown.clear();

    std::wstring body = text;
    if (!body.empty() && body[0] == 0xFEFF) body.erase(0, 1);   // UTF-8 BOM, widened

    const std::vector<std::wstring> lines = SplitLines(body);

    SectionRef section;                 // SecKind::None until the first header
    size_t profileIndex = static_cast<size_t>(-1);
    int sectionCount = 0;
    int orphanKeyLines = 0;             // key=value seen before any [section]

    for (size_t li = 0; li < lines.size(); ++li) {
        const std::wstring line = Trim(lines[li]);
        if (line.empty()) continue;
        if (IsCommentLine(line)) continue;

        if (line[0] == L'[') {
            const size_t close = line.rfind(L']');
            if (close == std::wstring::npos || close < 1) continue;   // malformed: skip
            section = ClassifySection(line.substr(1, close - 1));
            ++sectionCount;
            profileIndex = static_cast<size_t>(-1);
            if (section.kind == SecKind::Profile) {
                Profile p;
                p.name = section.profileName;
                out.profiles.push_back(p);
                profileIndex = out.profiles.size() - 1;
            }
            continue;
        }

        const size_t eq = line.find(L'=');
        if (eq == std::wstring::npos) continue;                       // malformed: skip
        const std::wstring key = Trim(line.substr(0, eq));
        const std::wstring value = Trim(line.substr(eq + 1));
        if (key.empty()) continue;

        if (section.kind == SecKind::None) { ++orphanKeyLines; continue; }

        bool known = false;
        switch (section.kind) {
            case SecKind::General: {
                known = true;
                if (IEquals(key, std::wstring(L"version"))) {
                    int v = out.version;
                    if (ParseIntW(value, v)) out.version = v;
                } else if (IEquals(key, std::wstring(L"start_with_windows"))) {
                    ParseBoolW(value, out.startWithWindows);
                } else if (IEquals(key, std::wstring(L"poll_ms"))) {
                    int v = out.pollMs;
                    if (ParseIntW(value, v)) out.pollMs = v;
                } else if (IEquals(key, std::wstring(L"notifications"))) {
                    ParseBoolW(value, out.notifications);
                } else if (IEquals(key, std::wstring(L"paused"))) {
                    ParseBoolW(value, out.paused);
                } else if (IEquals(key, std::wstring(L"first_run_done"))) {
                    ParseBoolW(value, out.firstRunDone);
                } else {
                    known = false;
                }
                break;
            }
            case SecKind::Topology: {
                known = true;
                if (IEquals(key, std::wstring(L"signature"))) {
                    out.topologySignature = value;
                } else {
                    known = false;
                }
                break;
            }
            case SecKind::Exclusions: {
                known = true;
                if (IEquals(key, std::wstring(L"names"))) {
                    const std::vector<std::wstring> parts = Split(value, L'|');
                    for (size_t i = 0; i < parts.size(); ++i) {
                        const std::wstring n = Trim(parts[i]);
                        if (!n.empty()) out.exclusions.push_back(n);
                    }
                } else {
                    known = false;
                }
                break;
            }
            case SecKind::Masks: {
                // Every key in [masks] is a mask name. Names may contain spaces, which is
                // exactly why the split is on the FIRST '=' and nothing else.
                Mask m;
                m.name = key;
                m.derived = true;
                known = ParseMaskValue(value, m);
                if (known) out.masks.push_back(m);
                break;
            }
            case SecKind::Profile: {
                known = true;
                if (profileIndex >= out.profiles.size()) { known = false; break; }
                Profile& p = out.profiles[profileIndex];
                if (IEquals(key, std::wstring(L"enabled"))) {
                    ParseBoolW(value, p.enabled);
                } else if (IEquals(key, std::wstring(L"game"))) {
                    p.game = value;
                } else if (IEquals(key, std::wstring(L"game_mask"))) {
                    p.gameMask = value;
                } else if (IEquals(key, std::wstring(L"heavy"))) {
                    const std::vector<std::wstring> parts = Split(value, L'|');
                    p.heavy.clear();
                    for (size_t i = 0; i < parts.size(); ++i) {
                        const std::wstring n = Trim(parts[i]);
                        if (!n.empty()) p.heavy.push_back(n);
                    }
                } else if (IEquals(key, std::wstring(L"heavy_mask"))) {
                    p.heavyMask = value;
                } else if (IEquals(key, std::wstring(L"auto_pin"))) {
                    ParseBoolW(value, p.autoPin);
                } else if (IEquals(key, std::wstring(L"auto_pin_percent"))) {
                    int v = p.autoPinPercent;
                    if (ParseIntW(value, v)) p.autoPinPercent = v;
                } else if (IEquals(key, std::wstring(L"auto_pin_seconds"))) {
                    // Still READ, never written: a config from a build that persisted this
                    // key must be consumed here rather than fall through to Config::unknown,
                    // which would re-emit a setting that is no longer the user's to set.
                    // ValidateAndRepair overwrites whatever lands here.
                    int v = p.autoPinSeconds;
                    if (ParseIntW(value, v)) p.autoPinSeconds = v;
                } else if (IEquals(key, std::wstring(L"all_games"))) {
                    ParseBoolW(value, p.isAllGames);
                } else if (IEquals(key, std::wstring(L"last_used"))) {
                    ULONGLONG v = p.lastUsed;
                    if (ParseUlonglongW(value, v)) p.lastUsed = v;
                } else {
                    known = false;
                }
                break;
            }
            case SecKind::Unknown:
            case SecKind::None:
            default:
                known = false;
                break;
        }

        if (!known) {
            // Verbatim, so an older binary re-emits a newer build's settings untouched.
            out.unknown[section.canonical].push_back(line);
        }
    }

    // A file with content but not one [section] header is not this format at all - that is
    // the BROKEN case LoadConfig has to be able to report. An empty file is not an error.
    if (sectionCount == 0 && orphanKeyLines > 0) {
        if (error) *error = L"No [section] header found - this file is not a Game Optimizer config.";
        return false;
    }
    return true;
}

// ---- Serialize -------------------------------------------------------------

std::wstring SerializeConfig(const Config& c) {
    std::wstring out;
    std::vector<std::wstring> consumed;

    out += L"; Game Optimizer configuration. Written by Game Optimizer; safe to hand-edit.\r\n";
    out += L"; Lists use '|'. Everything after the first '=' is the value, verbatim.\r\n";
    out += L"; Sections and keys this build does not know are preserved on save.\r\n";
    out += L"\r\n";

    out += L"[general]\r\n";
    AppendKv(out, L"version", std::to_wstring(c.version));
    AppendKv(out, L"start_with_windows", BoolText(c.startWithWindows));
    AppendKv(out, L"poll_ms", std::to_wstring(c.pollMs));
    AppendKv(out, L"notifications", BoolText(c.notifications));
    AppendKv(out, L"paused", BoolText(c.paused));
    AppendKv(out, L"first_run_done", BoolText(c.firstRunDone));
    AppendUnknownFor(out, c, std::wstring(kSecGeneral), consumed);
    out += L"\r\n";

    out += L"[masks]\r\n";
    out += L"; MaskName=<cpu set ids>. A leading 'custom' marks a hand-edited mask.\r\n";
    for (size_t i = 0; i < c.masks.size(); ++i) {
        AppendKv(out, c.masks[i].name, MaskValue(c.masks[i]));
    }
    AppendUnknownFor(out, c, std::wstring(kSecMasks), consumed);
    out += L"\r\n";

    out += L"[topology]\r\n";
    AppendKv(out, L"signature", c.topologySignature);
    AppendUnknownFor(out, c, std::wstring(kSecTopology), consumed);
    out += L"\r\n";

    out += L"[exclusions]\r\n";
    AppendKv(out, L"names", Join(c.exclusions, L'|'));
    AppendUnknownFor(out, c, std::wstring(kSecExclusions), consumed);
    out += L"\r\n";

    for (size_t i = 0; i < c.profiles.size(); ++i) {
        const Profile& p = c.profiles[i];
        out += L"[";
        out += kSecProfixLow;
        out += p.name;
        out += L"]\r\n";
        AppendKv(out, L"enabled", BoolText(p.enabled));
        AppendKv(out, L"game", p.game);
        AppendKv(out, L"game_mask", p.gameMask);
        AppendKv(out, L"heavy", Join(p.heavy, L'|'));
        AppendKv(out, L"heavy_mask", p.heavyMask);
        AppendKv(out, L"auto_pin", BoolText(p.autoPin));
        AppendKv(out, L"auto_pin_percent", std::to_wstring(p.autoPinPercent));
        // auto_pin_seconds is deliberately NOT written: it is a fixed internal debounce, not
        // a setting, and writing it would invite a hand-edit ValidateAndRepair silently undoes.
        AppendKv(out, L"all_games", BoolText(p.isAllGames));
        AppendKv(out, L"last_used", std::to_wstring(static_cast<unsigned long long>(p.lastUsed)));
        AppendUnknownFor(out, c, std::wstring(kSecProfixLow) + p.name, consumed);
        out += L"\r\n";
    }

    // Whole sections this build never understood, in map (sorted) order so a diff between
    // two saves stays meaningful.
    for (std::map<std::wstring, std::vector<std::wstring>>::const_iterator it = c.unknown.begin();
         it != c.unknown.end(); ++it) {
        if (AlreadyConsumed(consumed, it->first)) continue;
        if (it->second.empty()) continue;
        if (it->first.empty()) continue;
        out += L"[";
        out += it->first;
        out += L"]\r\n";
        for (size_t i = 0; i < it->second.size(); ++i) {
            out += it->second[i];
            out += L"\r\n";
        }
        out += L"\r\n";
    }

    return out;
}

// ---- Disk ------------------------------------------------------------------

bool LoadConfig(const std::wstring& path, Config& out, std::wstring* error) {
    if (error) error->clear();

    std::wstring text;
    if (!ReadFileUtf8(path, text)) {
        const DWORD attr = ::GetFileAttributesW(path.c_str());
        if (attr == INVALID_FILE_ATTRIBUTES) {
            // MISSING, not broken: *error stays empty so the caller runs first-run setup.
            return false;
        }
        if (error) *error = L"Could not read " + path + L".";
        return false;
    }
    return ParseConfig(text, out, error);
}

bool SaveConfig(const std::wstring& path, const Config& c, std::wstring* error) {
    if (error) error->clear();
    if (!WriteFileUtf8Atomic(path, SerializeConfig(c))) {
        if (error) *error = L"Could not write " + path + L".";
        return false;
    }
    return true;
}

// ---- Validate --------------------------------------------------------------

std::vector<std::wstring> ValidateAndRepair(Config& c, const Topology& t) {
    (void)t;   // repairs are config-internal; topology is not consulted here
    std::vector<std::wstring> notes;

    // 1. Poll interval.
    if (c.pollMs < 100 || c.pollMs > 2000) {
        const int was = c.pollMs;
        c.pollMs = ClampInt(c.pollMs, 100, 2000);
        notes.push_back(L"Poll interval " + std::to_wstring(was) + L" ms is out of range; using "
                        + std::to_wstring(c.pollMs) + L" ms.");
    }

    // 2. Masks: an empty id list can never be applied, so it is dropped rather than kept
    //    as a trap for a profile to reference.
    {
        std::vector<Mask> kept;
        for (size_t i = 0; i < c.masks.size(); ++i) {
            if (c.masks[i].ids.empty()) {
                notes.push_back(L"Mask \"" + c.masks[i].name + L"\" listed no processors; removed.");
                continue;
            }
            kept.push_back(c.masks[i]);
        }
        c.masks.swap(kept);
    }

    // 3. Duplicate mask names, first wins.
    {
        std::vector<Mask> kept;
        for (size_t i = 0; i < c.masks.size(); ++i) {
            bool dup = false;
            for (size_t j = 0; j < kept.size(); ++j) {
                if (IEquals(kept[j].name, c.masks[i].name)) { dup = true; break; }
            }
            if (dup) {
                notes.push_back(L"Mask \"" + c.masks[i].name + L"\" was listed twice; kept the first.");
                continue;
            }
            kept.push_back(c.masks[i]);
        }
        c.masks.swap(kept);
    }

    // 4. Duplicate profile names, first wins.
    {
        std::vector<Profile> kept;
        for (size_t i = 0; i < c.profiles.size(); ++i) {
            bool dup = false;
            for (size_t j = 0; j < kept.size(); ++j) {
                if (IEquals(kept[j].name, c.profiles[i].name)) { dup = true; break; }
            }
            if (dup) {
                notes.push_back(L"Profile \"" + c.profiles[i].name
                                + L"\" was listed twice; kept the first.");
                continue;
            }
            kept.push_back(c.profiles[i]);
        }
        c.profiles.swap(kept);
    }

    // 5. Profiles naming a mask that does not exist are DROPPED, not silently repointed:
    //    guessing a replacement mask would move a game onto the wrong CCD without saying so.
    {
        std::vector<Profile> kept;
        for (size_t i = 0; i < c.profiles.size(); ++i) {
            const Profile& p = c.profiles[i];
            const bool gameOk = (c.FindMask(p.gameMask) != nullptr);
            const bool heavyOk = (c.FindMask(p.heavyMask) != nullptr);
            if (!gameOk || !heavyOk) {
                const std::wstring missing = !gameOk ? p.gameMask : p.heavyMask;
                notes.push_back(L"Profile \"" + p.name + L"\" refers to mask \"" + missing
                                + L"\", which does not exist; the profile was removed.");
                continue;
            }
            kept.push_back(p);
        }
        c.profiles.swap(kept);
    }

    // 6. Per-profile clamps. Deliberately AFTER the drops above: clamping a profile that
    //    is about to be discarded would report repairs to the user for a profile that no
    //    longer exists.
    for (size_t i = 0; i < c.profiles.size(); ++i) {
        Profile& p = c.profiles[i];
        if (p.autoPinPercent < 1 || p.autoPinPercent > 100) {
            const int was = p.autoPinPercent;
            p.autoPinPercent = ClampInt(p.autoPinPercent, 1, 100);
            notes.push_back(L"Profile \"" + p.name + L"\": auto-pin threshold " + std::to_wstring(was)
                            + L"% is out of range; using " + std::to_wstring(p.autoPinPercent) + L"%.");
        }
        // autoPinSeconds is NOT a user setting any more (config.h): it is a fixed internal
        // debounce. It is forced, unconditionally, and deliberately WITHOUT a note - telling
        // the user their auto-pin hold was "repaired" would be reporting a change to a value
        // they can no longer see or set.
        p.autoPinSeconds = kAutoPinDebounceTicks;
    }

    // 6b. Exactly one All Games profile may exist, and it matches on "looks like a game"
    //     rather than on a name, so a `game` field on one is dead weight that would read as
    //     a working filter. Both are reported: the user chose these and must be told.
    {
        bool seenAllGames = false;
        for (size_t i = 0; i < c.profiles.size(); ++i) {
            Profile& p = c.profiles[i];
            if (!p.isAllGames) continue;
            if (seenAllGames) {
                p.isAllGames = false;
                notes.push_back(L"Profile \"" + p.name
                                + L"\" was a second All Games profile; only one is allowed, so "
                                  L"it is now an ordinary profile.");
                continue;
            }
            seenAllGames = true;
            if (!p.game.empty()) {
                notes.push_back(L"Profile \"" + p.name
                                + L"\" is an All Games profile, so its game \"" + p.game
                                + L"\" was cleared; it matches any game.");
                p.game.clear();
            }
        }
    }

    // 7. An empty exclusion list would let anti-cheat services onto the game CCD.
    if (c.exclusions.empty()) {
        c.exclusions = DefaultExclusions();
        notes.push_back(L"The exclusion list was empty; the built-in defaults were restored.");
    }

    return notes;
}

}  // namespace cd
