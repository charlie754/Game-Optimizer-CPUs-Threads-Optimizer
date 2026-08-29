// Game Optimizer - implementation of games.h: finding the games on this machine.
//
// DESIGN NOTES, and the reasons behind the shapes that look odd:
//
//  * NOTHING HERE TOUCHES THE NETWORK.  Every source is the local filesystem or the local
//    registry.  There is no HTTP client, no socket, no WinINet/WinHTTP include.
//
//  * NOTHING HERE THROWS.  Every entry point catches its own failures and returns whatever it
//    managed to collect.  A missing launcher, an unreadable manifest, an ACL-locked directory
//    and a malformed file are all normal, not exceptional: a user with no Epic install must
//    still get a Steam list.
//
//  * MINIMUM ACCESS RIGHTS.  The only process handle taken anywhere in this file is
//    PROCESS_QUERY_LIMITED_INFORMATION.  We never ask for PROCESS_VM_READ,
//    PROCESS_VM_WRITE, PROCESS_VM_OPERATION or PROCESS_ALL_ACCESS, and we never call
//    SetProcessAffinityMask.  See HasGraphicsRuntime() below for the one place that costs us
//    a capability, and why we pay it rather than widen the mask.
//
//  * THE PARSERS ARE DELIBERATELY NOT GENERAL.  Steam's .vdf/.acf and Epic's .item are read by
//    a tolerant line scanner that pulls out the four or five keys we actually want.  A general
//    VDF or JSON implementation would be more code, more surface, and no more correct for this
//    job - and when these formats change, a scanner that ignores what it does not understand
//    degrades to "found fewer games" instead of "crashed".
//
//  * WORK IS CAPPED.  A Steam library can hold hundreds of folders and a full recursive walk of
//    one is slow enough to be a bug.  Directory recursion stops at 2 levels below a game folder,
//    and every enumeration has a hard budget.  Hitting a budget means a shorter list, never a
//    hang.

#include "games.h"

#include "util.h"

#include <psapi.h>
#include <tlhelp32.h>

#include <algorithm>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace cd {

namespace {

// ---- work budgets ----------------------------------------------------------
// Chosen so a pathological machine (many libraries, huge games) still finishes in well under a
// second.  Every one of these is a cap on effort, never a cap on correctness for normal input.
const size_t kMaxManifestBytes   = 128u * 1024u;  // an .acf / .item this big is not one of ours
const int    kMaxSteamLibraries  = 32;
const int    kMaxManifestsPerLib = 400;
const int    kMaxEpicManifests   = 400;
const int    kMaxGogGames        = 400;
const int    kMaxXboxPackages    = 300;
const int    kMaxExeScanEntries  = 1500;          // files+dirs looked at inside one game folder
const int    kMaxExeCandidates   = 200;
const int    kMaxExeDepth        = 2;             // game folder + 2 levels below it

// ---- small helpers ---------------------------------------------------------

std::wstring StripQuotesAndSpace(const std::wstring& s) {
    std::wstring t = Trim(s);
    if (t.size() >= 2 && t.front() == L'"' && t.back() == L'"')
        t = t.substr(1, t.size() - 2);
    return Trim(t);
}

// Lowercase, letters and digits only.  "Counter-Strike Global Offensive" -> "counterstrike...".
// Used for fuzzy comparison of a game name against an executable stem, where separators,
// punctuation and capitalisation all differ freely between the two.
std::wstring Normalize(const std::wstring& s) {
    std::wstring o;
    o.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        wchar_t c = s[i];
        if (c >= L'A' && c <= L'Z')       o.push_back(static_cast<wchar_t>(c - L'A' + L'a'));
        else if (c >= L'a' && c <= L'z')  o.push_back(c);
        else if (c >= L'0' && c <= L'9')  o.push_back(c);
    }
    return o;
}

// "Overwatch.exe" -> "overwatch"
std::wstring StemLower(const std::wstring& fileName) {
    std::wstring base = BaseName(fileName);
    size_t dot = base.find_last_of(L'.');
    if (dot != std::wstring::npos) base = base.substr(0, dot);
    return ToLower(base);
}

bool ContainsCI(const std::wstring& haystackLower, const std::wstring& needleLower) {
    if (needleLower.empty()) return true;
    return haystackLower.find(needleLower) != std::wstring::npos;
}

// Steam writes forward slashes in some values, backslashes in others.  Normalise once so
// everything downstream can assume a Windows path.
std::wstring NormalizeSlashes(std::wstring p) {
    for (size_t i = 0; i < p.size(); ++i)
        if (p[i] == L'/') p[i] = L'\\';
    while (!p.empty() && (p.back() == L'\\' || p.back() == L' ')) p.pop_back();
    return p;
}

std::wstring PathJoin(const std::wstring& a, const std::wstring& b) {
    if (a.empty()) return b;
    if (b.empty()) return a;
    std::wstring r = a;
    if (r.back() != L'\\' && r.back() != L'/') r.push_back(L'\\');
    size_t start = 0;
    while (start < b.size() && (b[start] == L'\\' || b[start] == L'/')) ++start;
    r.append(b, start, std::wstring::npos);
    return r;
}

bool DirectoryExists(const std::wstring& path) {
    if (path.empty()) return false;
    DWORD a = ::GetFileAttributesW(path.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

bool FileExists(const std::wstring& path) {
    if (path.empty()) return false;
    DWORD a = ::GetFileAttributesW(path.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

std::wstring EnvVar(const wchar_t* name) {
    wchar_t buf[MAX_PATH * 2];
    DWORD n = ::GetEnvironmentVariableW(name, buf, static_cast<DWORD>(ARRAYSIZE(buf)));
    if (n == 0 || n >= ARRAYSIZE(buf)) return std::wstring();
    return std::wstring(buf, n);
}

// Read at most `maxBytes` of a UTF-8 text file.  util::ReadFileUtf8 would do, but these files
// are attacker-shaped input from another vendor's directory: capping the read is cheaper than
// trusting the size, and we only ever need the first few kilobytes of keys.
bool ReadTextCapped(const std::wstring& path, size_t maxBytes, std::wstring& out) {
    out.clear();
    if (path.empty()) return false;

    HANDLE h = ::CreateFileW(path.c_str(), GENERIC_READ,
                             FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                             nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;

    std::string raw;
    raw.resize(maxBytes);
    DWORD read = 0;
    BOOL ok = ::ReadFile(h, &raw[0], static_cast<DWORD>(maxBytes), &read, nullptr);
    ::CloseHandle(h);
    if (!ok) return false;
    raw.resize(read);
    if (raw.empty()) return false;

    // Tolerate a UTF-8 BOM; anything else is passed through to MultiByteToWideChar.
    size_t off = 0;
    if (raw.size() >= 3 && static_cast<unsigned char>(raw[0]) == 0xEF &&
        static_cast<unsigned char>(raw[1]) == 0xBB &&
        static_cast<unsigned char>(raw[2]) == 0xBF) {
        off = 3;
    }
    out = Widen(raw.substr(off));
    return !out.empty();
}

// ---- deny lists ------------------------------------------------------------
// A game folder is full of things that are not the game.  Getting this wrong is not cosmetic:
// pinning "EasyAntiCheat.exe" to the game CCD would be actively harmful.

bool StartsWith(const std::wstring& s, const wchar_t* prefix) {
    std::wstring p(prefix);
    return s.size() >= p.size() && s.compare(0, p.size(), p) == 0;
}

// Never a game, whatever else is in the folder.
bool IsHardDeniedExe(const std::wstring& stemLower) {
    static const wchar_t* const kSubstrings[] = {
        L"setup", L"crashhandler", L"crashreport", L"crash_report", L"crashpad",
        L"redist", L"anticheat", L"easyanticheat", L"beservice", L"battleye",
        L"uninstall", L"unins", L"installer", L"updater", L"update_", L"patcher",
        L"dxsetup", L"vcredist", L"dotnet", L"directx", L"oalinst", L"cleanup",
        L"benchmark", L"activation", L"touchup", L"errorreport", L"quickboot",
        L"helper", L"webhelper", L"reporter", L"diagnostic", L"unitycrash"
    };
    for (size_t i = 0; i < ARRAYSIZE(kSubstrings); ++i)
        if (stemLower.find(kSubstrings[i]) != std::wstring::npos) return true;
    if (StartsWith(stemLower, L"unins")) return true;
    return false;
}

// Probably not the game - but if it is the only executable in the folder, it is what the user
// double-clicks, so it beats returning nothing.
bool IsSoftDeniedExe(const std::wstring& stemLower) {
    return stemLower.find(L"launch") != std::wstring::npos ||
           stemLower.find(L"bootstrap") != std::wstring::npos ||
           stemLower.find(L"config") != std::wstring::npos ||
           stemLower.find(L"settings") != std::wstring::npos ||
           stemLower.find(L"server") != std::wstring::npos ||
           stemLower.find(L"editor") != std::wstring::npos ||
           stemLower.find(L"tool") != std::wstring::npos;
}

// Directories that only ever hold redistributables, engine plumbing or anti-cheat.  Skipping
// them is most of the reason the scan stays fast.
bool IsDeniedDir(const std::wstring& nameLower) {
    static const wchar_t* const kDirs[] = {
        L"_commonredist", L"commonredist", L"redist", L"redistributable", L"directx",
        L"dotnet", L"vcredist", L"easyanticheat", L"easyanticheat_eos", L"battleye",
        L"crashreportclient", L"support", L"installers", L"prerequisites",
        L"__installer", L"engine", L"tools", L"dependencies", L"drivers"
    };
    for (size_t i = 0; i < ARRAYSIZE(kDirs); ++i)
        if (nameLower == kDirs[i]) return true;
    return false;
}

// ---- executable selection --------------------------------------------------

struct ExeCandidate {
    std::wstring path;
    std::wstring stemLower;
    ULONGLONG    size = 0;
    int          depth = 0;
    bool         soft = false;   // matched the soft deny list
};

void CollectExes(const std::wstring& dir, int depth, std::vector<ExeCandidate>& out,
                 int& budget) {
    if (depth > kMaxExeDepth) return;
    if (budget <= 0) return;
    if (out.size() >= static_cast<size_t>(kMaxExeCandidates)) return;

    WIN32_FIND_DATAW fd;
    ZeroMemory(&fd, sizeof(fd));
    HANDLE h = ::FindFirstFileExW(PathJoin(dir, L"*").c_str(), FindExInfoBasic, &fd,
                                  FindExSearchNameMatch, nullptr, 0);
    if (h == INVALID_HANDLE_VALUE) return;   // access denied / gone: not an error here

    std::vector<std::wstring> subdirs;
    do {
        if (--budget <= 0) break;
        std::wstring name = fd.cFileName;
        if (name == L"." || name == L"..") continue;
        if ((fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) continue;  // no junctions

        if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
            if (depth < kMaxExeDepth && !IsDeniedDir(ToLower(name)))
                subdirs.push_back(name);
            continue;
        }

        std::wstring lower = ToLower(name);
        if (lower.size() < 5 || lower.compare(lower.size() - 4, 4, L".exe") != 0) continue;

        std::wstring stem = StemLower(name);
        if (IsHardDeniedExe(stem)) continue;

        ExeCandidate c;
        c.path      = PathJoin(dir, name);
        c.stemLower = stem;
        c.size      = (static_cast<ULONGLONG>(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow;
        c.depth     = depth;
        c.soft      = IsSoftDeniedExe(stem);
        out.push_back(c);
        if (out.size() >= static_cast<size_t>(kMaxExeCandidates)) break;
    } while (::FindNextFileW(h, &fd));
    ::FindClose(h);

    for (size_t i = 0; i < subdirs.size(); ++i) {
        if (budget <= 0) break;
        CollectExes(PathJoin(dir, subdirs[i]), depth + 1, out, budget);
    }
}

// How well does an executable stem match one of the names we expect (the game's display name
// and its install directory)?  Higher is better; 0 means "no relationship at all".
int NameScore(const std::wstring& stemLower, const std::vector<std::wstring>& prefs) {
    std::wstring stem = Normalize(stemLower);
    if (stem.empty()) return 0;
    int best = 0;
    for (size_t i = 0; i < prefs.size(); ++i) {
        std::wstring p = Normalize(prefs[i]);
        if (p.empty()) continue;
        int s = 0;
        if (stem == p)                                                          s = 1000;
        else if (p.size() >= 4 && stem.size() >= 4 && p.find(stem) == 0)        s = 700;
        else if (p.size() >= 4 && stem.size() >= 4 && stem.find(p) == 0)        s = 650;
        else if (p.size() >= 5 && stem.find(p) != std::wstring::npos)           s = 600;
        else if (stem.size() >= 5 && p.find(stem) != std::wstring::npos)        s = 500;
        if (s > best) best = s;
    }
    return best;
}

// Pick the executable most likely to BE the game.  Order of preference, per the brief:
// a stem that matches the game or installdir name, then the largest executable; and anything
// on the deny list is skipped unless nothing else is left.
std::wstring ChooseExe(const std::wstring& gameDir, const std::vector<std::wstring>& prefs) {
    if (!DirectoryExists(gameDir)) return std::wstring();

    std::vector<ExeCandidate> cands;
    int budget = kMaxExeScanEntries;
    CollectExes(gameDir, 0, cands, budget);
    if (cands.empty()) return std::wstring();

    const ExeCandidate* best = nullptr;
    int bestScore = -1;
    ULONGLONG bestSize = 0;
    int bestDepth = 0;

    // Two passes: real candidates first, soft-denied ones only if the first pass found nothing.
    for (int pass = 0; pass < 2 && best == nullptr; ++pass) {
        for (size_t i = 0; i < cands.size(); ++i) {
            const ExeCandidate& c = cands[i];
            if ((pass == 0) == c.soft) continue;

            int score = NameScore(c.stemLower, prefs);
            // Shallower is better: the game usually sits at the root or in Binaries\Win64.
            score -= c.depth * 20;

            bool better = false;
            if (best == nullptr)               better = true;
            else if (score > bestScore)        better = true;
            else if (score == bestScore) {
                if (c.depth < bestDepth)       better = true;
                else if (c.depth == bestDepth && c.size > bestSize) better = true;
            }
            if (better) {
                best = &c; bestScore = score; bestSize = c.size; bestDepth = c.depth;
            }
        }
    }
    return best ? best->path : std::wstring();
}

// ---- registry --------------------------------------------------------------

bool RegReadString(HKEY root, const wchar_t* subKey, const wchar_t* valueName,
                   std::wstring& out) {
    out.clear();
    HKEY key = nullptr;
    if (::RegOpenKeyExW(root, subKey, 0, KEY_QUERY_VALUE | KEY_WOW64_64KEY, &key)
            != ERROR_SUCCESS)
        return false;

    DWORD type = 0, bytes = 0;
    LONG rc = ::RegQueryValueExW(key, valueName, nullptr, &type, nullptr, &bytes);
    if (rc != ERROR_SUCCESS || bytes == 0 ||
        (type != REG_SZ && type != REG_EXPAND_SZ) || bytes > 64u * 1024u) {
        ::RegCloseKey(key);
        return false;
    }
    std::vector<wchar_t> buf(bytes / sizeof(wchar_t) + 2, 0);
    rc = ::RegQueryValueExW(key, valueName, nullptr, &type,
                            reinterpret_cast<LPBYTE>(&buf[0]), &bytes);
    ::RegCloseKey(key);
    if (rc != ERROR_SUCCESS) return false;
    buf[buf.size() - 1] = 0;
    out = &buf[0];
    out = Trim(out);
    return !out.empty();
}

// ---- the tolerant text scanners --------------------------------------------
//
// Valve's KeyValues text (both .vdf and .acf) is, for our purposes:
//     "key"    "value"        <- a pair, both quoted, on one line
//     "key"                   <- a subkey, followed by { ... }
// We ignore the nesting entirely and collect every key/value pair we see.  That is wrong for a
// general parser (two different subtrees can hold the same key) and exactly right here, where
// one .acf describes one app and the keys we want appear once.

std::wstring UnescapeVdf(const std::wstring& s) {
    std::wstring o;
    o.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == L'\\' && i + 1 < s.size()) {
            wchar_t n = s[i + 1];
            if (n == L'\\' || n == L'"' || n == L'/') { o.push_back(n); ++i; continue; }
            if (n == L'n') { o.push_back(L'\n'); ++i; continue; }
            if (n == L't') { o.push_back(L'\t'); ++i; continue; }
        }
        o.push_back(s[i]);
    }
    return o;
}

struct KeyValue { std::wstring key, value; };

// Every line holding two quoted strings becomes one pair.  Anything else is skipped silently.
std::vector<KeyValue> ScanKeyValues(const std::wstring& text, size_t maxPairs) {
    std::vector<KeyValue> out;
    size_t pos = 0;
    while (pos < text.size() && out.size() < maxPairs) {
        size_t eol = text.find(L'\n', pos);
        std::wstring line = (eol == std::wstring::npos) ? text.substr(pos)
                                                        : text.substr(pos, eol - pos);
        pos = (eol == std::wstring::npos) ? text.size() : eol + 1;

        // Pull the first two quoted runs out of the line.
        std::wstring tok[2];
        int found = 0;
        size_t i = 0;
        while (found < 2 && i < line.size()) {
            if (line[i] != L'"') { ++i; continue; }
            ++i;                                          // step over the opening quote
            std::wstring acc;
            while (i < line.size()) {
                if (line[i] == L'\\' && i + 1 < line.size()) { acc.push_back(line[i]); acc.push_back(line[i + 1]); i += 2; continue; }
                if (line[i] == L'"') break;
                acc.push_back(line[i]);
                ++i;
            }
            if (i >= line.size()) { found = 0; break; }   // unterminated quote: drop the line
            ++i;                                          // step over the closing quote
            tok[found++] = acc;
        }
        if (found == 2) {
            KeyValue kv;
            kv.key   = ToLower(UnescapeVdf(tok[0]));
            kv.value = UnescapeVdf(tok[1]);
            out.push_back(kv);
        }
    }
    return out;
}

// Epic's .item files are JSON, but we only want three string values from the top level, so a
// scanner that finds "Key" then the next quoted string after the colon is enough - and it
// cannot be broken by a nesting level we did not anticipate.
bool JsonStringValue(const std::wstring& text, const wchar_t* key, std::wstring& out) {
    out.clear();
    std::wstring needle = L"\"";
    needle += key;
    needle += L"\"";
    std::wstring lowerText = ToLower(text);
    std::wstring lowerNeedle = ToLower(needle);

    size_t at = lowerText.find(lowerNeedle);
    if (at == std::wstring::npos) return false;
    size_t i = at + needle.size();
    while (i < text.size() && (text[i] == L' ' || text[i] == L'\t')) ++i;
    if (i >= text.size() || text[i] != L':') return false;
    ++i;
    while (i < text.size() && (text[i] == L' ' || text[i] == L'\t' ||
                               text[i] == L'\r' || text[i] == L'\n')) ++i;
    if (i >= text.size() || text[i] != L'"') return false;
    ++i;
    std::wstring acc;
    while (i < text.size() && text[i] != L'"') {
        if (text[i] == L'\\' && i + 1 < text.size()) { acc.push_back(text[i]); acc.push_back(text[i + 1]); i += 2; continue; }
        acc.push_back(text[i]);
        ++i;
    }
    if (i >= text.size()) return false;
    out = Trim(UnescapeVdf(acc));
    return !out.empty();
}

// ---- entry accumulation ----------------------------------------------------

struct Accumulator {
    std::vector<GameEntry>          list;
    std::map<std::wstring, size_t>  byExe;   // lowercased exe basename -> index into list

    static int Rank(const GameEntry& e) {
        return (e.installed ? 2 : 0) + (e.fullPath.empty() ? 0 : 1);
    }

    void Add(const GameEntry& in) {
        GameEntry e = in;
        e.exe = BaseName(e.exe);
        if (e.exe.empty()) return;
        if (e.name.empty()) e.name = StemLower(e.exe);

        std::wstring key = ToLower(e.exe);
        std::map<std::wstring, size_t>::iterator it = byExe.find(key);
        if (it == byExe.end()) {
            byExe[key] = list.size();
            list.push_back(e);
            return;
        }
        // De-dup per the header: prefer an installed entry with a real path over a bundled one,
        // but never lose information we already had.
        GameEntry& cur = list[it->second];
        if (Rank(e) > Rank(cur)) {
            std::wstring keepName = cur.name;
            cur = e;
            if (cur.name.empty()) cur.name = keepName;
        } else {
            if (cur.fullPath.empty()   && !e.fullPath.empty())   { cur.fullPath = e.fullPath; cur.installed = true; }
            if (cur.installDir.empty() && !e.installDir.empty()) cur.installDir = e.installDir;
        }
    }
};

// ---- Steam -----------------------------------------------------------------

std::wstring SteamRoot() {
    std::wstring p;
    if (RegReadString(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamPath", p)) {
        p = NormalizeSlashes(p);
        if (DirectoryExists(p)) return p;
    }
    if (RegReadString(HKEY_LOCAL_MACHINE, L"SOFTWARE\\WOW6432Node\\Valve\\Steam",
                      L"InstallPath", p)) {
        p = NormalizeSlashes(p);
        if (DirectoryExists(p)) return p;
    }
    if (RegReadString(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Valve\\Steam", L"InstallPath", p)) {
        p = NormalizeSlashes(p);
        if (DirectoryExists(p)) return p;
    }
    return std::wstring();
}

// libraryfolders.vdf has had two shapes over the years:
//     "1"   "D:\\SteamLibrary"                (old, value is the path)
//     "path" "D:\\SteamLibrary"               (current, nested one level deeper)
// Both fall out of a flat key/value scan, so we accept either and ignore everything else.
std::vector<std::wstring> SteamLibraries(const std::wstring& steamRoot) {
    std::vector<std::wstring> libs;
    if (steamRoot.empty()) return libs;
    libs.push_back(steamRoot);

    std::wstring text;
    std::wstring vdf = PathJoin(steamRoot, L"steamapps\\libraryfolders.vdf");
    if (!ReadTextCapped(vdf, kMaxManifestBytes, text)) {
        vdf = PathJoin(steamRoot, L"config\\libraryfolders.vdf");
        if (!ReadTextCapped(vdf, kMaxManifestBytes, text)) return libs;
    }

    std::vector<KeyValue> kvs = ScanKeyValues(text, 4000);
    for (size_t i = 0; i < kvs.size() && libs.size() < static_cast<size_t>(kMaxSteamLibraries); ++i) {
        const KeyValue& kv = kvs[i];
        bool isPathKey = (kv.key == L"path");
        bool isNumericKey = !kv.key.empty();
        for (size_t c = 0; c < kv.key.size() && isNumericKey; ++c)
            if (kv.key[c] < L'0' || kv.key[c] > L'9') isNumericKey = false;
        if (!isPathKey && !isNumericKey) continue;

        std::wstring p = NormalizeSlashes(kv.value);
        // A numeric key with a non-path value is some other counter; require a drive or a UNC.
        if (!isPathKey && p.find(L':') == std::wstring::npos && p.find(L"\\\\") != 0) continue;
        if (p.empty() || !DirectoryExists(p)) continue;

        bool dup = false;
        for (size_t j = 0; j < libs.size(); ++j)
            if (IEquals(libs[j], p)) { dup = true; break; }
        if (!dup) libs.push_back(p);
    }
    return libs;
}

void ScanSteam(Accumulator& acc) {
    std::wstring root = SteamRoot();
    if (root.empty()) return;

    std::vector<std::wstring> libs = SteamLibraries(root);
    for (size_t li = 0; li < libs.size(); ++li) {
        std::wstring steamapps = PathJoin(libs[li], L"steamapps");
        if (!DirectoryExists(steamapps)) continue;

        WIN32_FIND_DATAW fd;
        ZeroMemory(&fd, sizeof(fd));
        HANDLE h = ::FindFirstFileExW(PathJoin(steamapps, L"appmanifest_*.acf").c_str(),
                                      FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, 0);
        if (h == INVALID_HANDLE_VALUE) continue;

        int seen = 0;
        do {
            if (++seen > kMaxManifestsPerLib) break;
            if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) continue;

            std::wstring text;
            if (!ReadTextCapped(PathJoin(steamapps, fd.cFileName), kMaxManifestBytes, text))
                continue;

            std::wstring name, installdir;
            std::vector<KeyValue> kvs = ScanKeyValues(text, 400);
            for (size_t i = 0; i < kvs.size(); ++i) {
                if (name.empty()       && kvs[i].key == L"name")       name = Trim(kvs[i].value);
                if (installdir.empty() && kvs[i].key == L"installdir") installdir = Trim(kvs[i].value);
            }
            if (installdir.empty()) continue;

            std::wstring gameDir = PathJoin(steamapps, PathJoin(L"common", installdir));
            if (!DirectoryExists(gameDir)) continue;

            std::vector<std::wstring> prefs;
            if (!name.empty()) prefs.push_back(name);
            prefs.push_back(installdir);

            std::wstring exePath = ChooseExe(gameDir, prefs);
            if (exePath.empty()) continue;

            GameEntry e;
            e.name       = name.empty() ? installdir : name;
            e.exe        = BaseName(exePath);
            e.fullPath   = exePath;
            e.installDir = gameDir;
            e.source     = GameSource::Steam;
            e.installed  = true;
            acc.Add(e);
        } while (::FindNextFileW(h, &fd));
        ::FindClose(h);
    }
}

// ---- Epic ------------------------------------------------------------------

void ScanEpic(Accumulator& acc) {
    std::wstring programData = EnvVar(L"ProgramData");
    if (programData.empty()) programData = L"C:\\ProgramData";
    std::wstring dir = PathJoin(programData,
                                L"Epic\\EpicGamesLauncher\\Data\\Manifests");
    if (!DirectoryExists(dir)) return;

    WIN32_FIND_DATAW fd;
    ZeroMemory(&fd, sizeof(fd));
    HANDLE h = ::FindFirstFileExW(PathJoin(dir, L"*.item").c_str(), FindExInfoBasic, &fd,
                                  FindExSearchNameMatch, nullptr, 0);
    if (h == INVALID_HANDLE_VALUE) return;

    int seen = 0;
    do {
        if (++seen > kMaxEpicManifests) break;
        if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) continue;

        std::wstring text;
        if (!ReadTextCapped(PathJoin(dir, fd.cFileName), kMaxManifestBytes, text)) continue;

        std::wstring display, install, launch;
        JsonStringValue(text, L"DisplayName", display);
        JsonStringValue(text, L"InstallLocation", install);
        JsonStringValue(text, L"LaunchExecutable", launch);
        install = NormalizeSlashes(install);
        launch  = NormalizeSlashes(launch);
        if (install.empty() || !DirectoryExists(install)) continue;

        std::wstring exePath;
        if (!launch.empty()) {
            std::wstring candidate = PathJoin(install, launch);
            if (FileExists(candidate) && !IsHardDeniedExe(StemLower(candidate)))
                exePath = candidate;
        }
        if (exePath.empty()) {
            std::vector<std::wstring> prefs;
            if (!display.empty()) prefs.push_back(display);
            prefs.push_back(BaseName(install));
            exePath = ChooseExe(install, prefs);
        }
        if (exePath.empty()) continue;

        GameEntry e;
        e.name       = display.empty() ? StemLower(exePath) : display;
        e.exe        = BaseName(exePath);
        e.fullPath   = exePath;
        e.installDir = install;
        e.source     = GameSource::Epic;
        e.installed  = true;
        acc.Add(e);
    } while (::FindNextFileW(h, &fd));
    ::FindClose(h);
}

// ---- GOG -------------------------------------------------------------------

void ScanGogUnder(Accumulator& acc, const wchar_t* gamesKeyPath, int& budget) {
    HKEY games = nullptr;
    if (::RegOpenKeyExW(HKEY_LOCAL_MACHINE, gamesKeyPath, 0,
                        KEY_READ | KEY_WOW64_64KEY, &games) != ERROR_SUCCESS)
        return;

    for (DWORD idx = 0; budget > 0; ++idx, --budget) {
        wchar_t sub[256];
        DWORD subLen = static_cast<DWORD>(ARRAYSIZE(sub));
        LONG rc = ::RegEnumKeyExW(games, idx, sub, &subLen, nullptr, nullptr, nullptr, nullptr);
        if (rc != ERROR_SUCCESS) break;

        std::wstring subPath = gamesKeyPath;
        subPath += L"\\";
        subPath += sub;

        std::wstring name, path, exeVal;
        RegReadString(HKEY_LOCAL_MACHINE, subPath.c_str(), L"gameName", name);
        RegReadString(HKEY_LOCAL_MACHINE, subPath.c_str(), L"path", path);
        RegReadString(HKEY_LOCAL_MACHINE, subPath.c_str(), L"exe", exeVal);
        path   = NormalizeSlashes(path);
        exeVal = NormalizeSlashes(exeVal);
        if (path.empty() || !DirectoryExists(path)) continue;

        std::wstring exePath;
        if (!exeVal.empty()) {
            std::wstring candidate = exeVal;
            if (candidate.find(L':') == std::wstring::npos) candidate = PathJoin(path, candidate);
            if (FileExists(candidate) && !IsHardDeniedExe(StemLower(candidate)))
                exePath = candidate;
        }
        if (exePath.empty()) {
            std::vector<std::wstring> prefs;
            if (!name.empty()) prefs.push_back(name);
            prefs.push_back(BaseName(path));
            exePath = ChooseExe(path, prefs);
        }
        if (exePath.empty()) continue;

        GameEntry e;
        e.name       = name.empty() ? StemLower(exePath) : name;
        e.exe        = BaseName(exePath);
        e.fullPath   = exePath;
        e.installDir = path;
        e.source     = GameSource::Gog;
        e.installed  = true;
        acc.Add(e);
    }
    ::RegCloseKey(games);
}

void ScanGog(Accumulator& acc) {
    int budget = kMaxGogGames;
    ScanGogUnder(acc, L"SOFTWARE\\WOW6432Node\\GOG.com\\Games", budget);
    ScanGogUnder(acc, L"SOFTWARE\\GOG.com\\Games", budget);
}

// ---- Xbox ------------------------------------------------------------------
//
// BEST EFFORT AND EXPECTED TO RETURN NOTHING.  Packaged games live under
// %ProgramFiles%\WindowsApps, whose ACL denies even read access to a normal user token; there
// is a supported route (the PackageManager WinRT API) but it is a WinRT dependency this project
// does not take.  So: try, and treat access denied as "no Xbox games", never as an error.
void ScanXbox(Accumulator& acc) {
    std::wstring pf = EnvVar(L"ProgramFiles");
    if (pf.empty()) pf = L"C:\\Program Files";
    std::wstring root = PathJoin(pf, L"WindowsApps");

    WIN32_FIND_DATAW fd;
    ZeroMemory(&fd, sizeof(fd));
    HANDLE h = ::FindFirstFileExW(PathJoin(root, L"*").c_str(), FindExInfoBasic, &fd,
                                  FindExSearchNameMatch, nullptr, 0);
    if (h == INVALID_HANDLE_VALUE) return;   // almost always ERROR_ACCESS_DENIED: fine

    int seen = 0;
    do {
        if (++seen > kMaxXboxPackages) break;
        if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0) continue;
        std::wstring name = fd.cFileName;
        if (name == L"." || name == L"..") continue;
        if (name.find(L'_') == std::wstring::npos) continue;   // not a package family folder

        // "Publisher.Title_1.2.3.0_x64__hash" -> "Title"
        std::wstring famous = name.substr(0, name.find(L'_'));
        std::wstring title = famous;
        size_t dot = famous.find_last_of(L'.');
        if (dot != std::wstring::npos && dot + 1 < famous.size())
            title = famous.substr(dot + 1);

        std::wstring pkgDir = PathJoin(root, name);
        std::vector<std::wstring> prefs;
        prefs.push_back(title);
        std::wstring exePath = ChooseExe(pkgDir, prefs);
        if (exePath.empty()) continue;

        GameEntry e;
        e.name       = title;
        e.exe        = BaseName(exePath);
        e.fullPath   = exePath;
        e.installDir = pkgDir;
        e.source     = GameSource::Xbox;
        e.installed  = true;
        acc.Add(e);
    } while (::FindNextFileW(h, &fd));
    ::FindClose(h);
}

// ---- running processes -----------------------------------------------------

// PROCESS_QUERY_LIMITED_INFORMATION only.  This is the whole reason the app is uninteresting to
// anti-cheat: it is the same right Task Manager needs to show a name, and it grants no ability
// to read or write another process's memory.
std::wstring ProcessImagePath(DWORD pid) {
    HANDLE h = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (h == nullptr) return std::wstring();
    std::wstring path;
    std::vector<wchar_t> buf(1024, 0);
    DWORD chars = static_cast<DWORD>(buf.size());
    if (::QueryFullProcessImageNameW(h, 0, &buf[0], &chars) && chars > 0)
        path.assign(&buf[0], chars);
    ::CloseHandle(h);
    return path;
}

void ScanRunning(Accumulator& acc) {
    const std::vector<GameEntry>& bundled = BundledGames();
    std::map<std::wstring, const GameEntry*> known;
    for (size_t i = 0; i < bundled.size(); ++i)
        known[ToLower(bundled[i].exe)] = &bundled[i];

    HANDLE snap = ::CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return;

    PROCESSENTRY32W pe;
    ZeroMemory(&pe, sizeof(pe));
    pe.dwSize = sizeof(pe);
    if (::Process32FirstW(snap, &pe)) {
        do {
            std::wstring exe = pe.szExeFile;
            if (exe.empty()) continue;
            std::map<std::wstring, const GameEntry*>::iterator it = known.find(ToLower(exe));
            if (it == known.end()) continue;

            GameEntry e;
            e.name      = it->second->name;
            e.exe       = exe;
            e.fullPath  = ProcessImagePath(pe.th32ProcessID);
            e.source    = GameSource::Running;
            e.installed = true;
            if (!e.fullPath.empty()) {
                size_t slash = e.fullPath.find_last_of(L'\\');
                if (slash != std::wstring::npos) e.installDir = e.fullPath.substr(0, slash);
            }
            acc.Add(e);
        } while (::Process32NextW(snap, &pe));
    }
    ::CloseHandle(snap);
}

// ---- the "is this a game" heuristic ---------------------------------------

// Things that own fullscreen windows and are definitely not games.  A false positive here
// costs the user a pointless prompt, which is exactly the failure the header says to avoid.
bool IsExcludedProcess(const std::wstring& exeLower) {
    static const wchar_t* const kExact[] = {
        L"explorer.exe", L"dwm.exe", L"svchost.exe", L"csrss.exe", L"winlogon.exe",
        L"lsass.exe", L"services.exe", L"smss.exe", L"wininit.exe", L"taskmgr.exe",
        L"sihost.exe", L"ctfmon.exe", L"fontdrvhost.exe", L"searchhost.exe",
        L"startmenuexperiencehost.exe", L"shellexperiencehost.exe", L"applicationframehost.exe",
        L"textinputhost.exe", L"lockapp.exe", L"systemsettings.exe", L"rundll32.exe",
        L"chrome.exe", L"msedge.exe", L"firefox.exe", L"opera.exe", L"brave.exe",
        L"vivaldi.exe", L"iexplore.exe",
        L"discord.exe", L"slack.exe", L"teams.exe", L"ms-teams.exe", L"zoom.exe",
        L"spotify.exe", L"vlc.exe", L"mpc-hc64.exe", L"mpv.exe", L"potplayermini64.exe",
        L"obs64.exe", L"obs32.exe", L"streamlabs obs.exe", L"nvidia share.exe",
        L"steam.exe", L"steamwebhelper.exe", L"epicgameslauncher.exe", L"galaxyclient.exe",
        L"battle.net.exe", L"upc.exe", L"ubisoftconnect.exe", L"eadesktop.exe",
        L"riotclientservices.exe", L"riotclientux.exe", L"origin.exe", L"gog galaxy.exe",
        L"code.exe", L"devenv.exe", L"idea64.exe", L"pycharm64.exe", L"sublime_text.exe",
        L"notepad.exe", L"notepad++.exe", L"cmd.exe", L"powershell.exe", L"pwsh.exe",
        L"windowsterminal.exe", L"conhost.exe", L"wt.exe", L"explorer++.exe",
        L"photoshop.exe", L"illustrator.exe", L"afterfx.exe", L"blender.exe",
        // This product, under both its current and its pre-rename exe name. The old name is
        // kept deliberately: a user who still has the previous build on disk must not have it
        // offered to them as a game.
        L"gameoptimizer.exe", L"coredirector.exe"
    };
    for (size_t i = 0; i < ARRAYSIZE(kExact); ++i)
        if (exeLower == kExact[i]) return true;

    // Generic runtimes we must never guess about: a JVM or an Electron shell may well BE a
    // game, but it may equally be a build tool, and we cannot tell from the name.
    if (exeLower == L"javaw.exe" || exeLower == L"java.exe" ||
        exeLower == L"electron.exe" || exeLower == L"node.exe" ||
        exeLower == L"python.exe" || exeLower == L"pythonw.exe")
        return true;

    return IsHardDeniedExe(StemLower(exeLower));
}

struct FullscreenSearch {
    DWORD pid = 0;
    bool  found = false;
};

BOOL CALLBACK FullscreenEnumProc(HWND hwnd, LPARAM lp) {
    FullscreenSearch* s = reinterpret_cast<FullscreenSearch*>(lp);
    if (s == nullptr) return FALSE;

    DWORD wpid = 0;
    ::GetWindowThreadProcessId(hwnd, &wpid);
    if (wpid != s->pid) return TRUE;
    if (!::IsWindowVisible(hwnd)) return TRUE;

    RECT wr;
    if (!::GetWindowRect(hwnd, &wr)) return TRUE;
    if (wr.right <= wr.left || wr.bottom <= wr.top) return TRUE;

    HMONITOR mon = ::MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    if (mon == nullptr) return TRUE;

    MONITORINFO mi;
    ZeroMemory(&mi, sizeof(mi));
    mi.cbSize = sizeof(mi);
    if (!::GetMonitorInfoW(mon, &mi)) return TRUE;

    // A few pixels of slack: borderless-fullscreen windows are sometimes off by a pixel or
    // two, and a DPI-scaled desktop can round.
    const LONG kSlack = 4;
    if (wr.left   <= mi.rcMonitor.left   + kSlack &&
        wr.top    <= mi.rcMonitor.top    + kSlack &&
        wr.right  >= mi.rcMonitor.right  - kSlack &&
        wr.bottom >= mi.rcMonitor.bottom - kSlack) {
        s->found = true;
        return FALSE;   // stop enumerating, we have our answer
    }
    return TRUE;
}

// Tri-state on purpose.  See the comment in GuessGame(): with the access rights this project
// allows itself, the honest answer is almost always Unknown, and Unknown must not be treated
// as No (that would silently make every heuristic hit score lower than intended) nor as Yes
// (that would inflate confidence we have not earned).
enum class Tri { No, Yes, Unknown };

Tri HasGraphicsRuntime(DWORD pid) {
    HANDLE h = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (h == nullptr) return Tri::Unknown;

    Tri result = Tri::Unknown;
    HMODULE mods[512];
    DWORD needed = 0;
    // EnumProcessModulesEx is documented to require PROCESS_QUERY_INFORMATION *and*
    // PROCESS_VM_READ.  We hold neither, on purpose - this project must stay unattractive to
    // anti-cheat, and widening the mask to read another process's module list is exactly the
    // shape of access that gets a tool flagged.  So the call is attempted with the rights we
    // do have and its failure is expected, not exceptional.
    if (::EnumProcessModulesEx(h, mods, sizeof(mods), &needed, LIST_MODULES_ALL)) {
        result = Tri::No;
        DWORD count = needed / sizeof(HMODULE);
        if (count > ARRAYSIZE(mods)) count = ARRAYSIZE(mods);
        for (DWORD i = 0; i < count && result != Tri::Yes; ++i) {
            wchar_t base[MAX_PATH];
            base[0] = 0;
            if (::GetModuleBaseNameW(h, mods[i], base, static_cast<DWORD>(ARRAYSIZE(base))) == 0)
                continue;
            std::wstring m = ToLower(base);
            if (m.find(L"d3d11") != std::wstring::npos ||
                m.find(L"d3d12") != std::wstring::npos ||
                m.find(L"d3d9")  != std::wstring::npos ||
                m.find(L"dxgi")  != std::wstring::npos ||
                m.find(L"vulkan-1") != std::wstring::npos ||
                m.find(L"opengl32") != std::wstring::npos)
                result = Tri::Yes;
        }
    }
    ::CloseHandle(h);
    return result;
}

}  // namespace

// ---------------------------------------------------------------------------
// Public surface
// ---------------------------------------------------------------------------

const wchar_t* GameSourceName(GameSource s) {
    switch (s) {
        case GameSource::Steam:   return L"Steam";
        case GameSource::Epic:    return L"Epic";
        case GameSource::Gog:     return L"GOG";
        case GameSource::Xbox:    return L"Xbox";
        case GameSource::Running: return L"Running";
        case GameSource::Manual:  return L"Manual";
        case GameSource::Bundled: return L"Bundled";
    }
    return L"Unknown";
}

std::vector<GameEntry> DiscoverGames() {
    Accumulator acc;

    // Order matters only for de-duplication: installed sources go in first so a bundled entry
    // can never displace a real install, and the bundled pass below fills in the rest.
    ScanSteam(acc);
    ScanEpic(acc);
    ScanGog(acc);
    ScanXbox(acc);
    ScanRunning(acc);

    const std::vector<GameEntry>& bundled = BundledGames();
    for (size_t i = 0; i < bundled.size(); ++i) {
        GameEntry e = bundled[i];
        e.installed = false;
        e.fullPath.clear();
        e.installDir.clear();
        acc.Add(e);
    }

    LogLine(L"DiscoverGames: %u entries", static_cast<unsigned>(acc.list.size()));
    return acc.list;
}

// ---------------------------------------------------------------------------
// THE PROVENANCE OF THIS LIST, stated plainly because it decides how much the rest of the
// program is allowed to trust it:
//
//   * EVERY EXECUTABLE NAME BELOW IS ASSUMED, NOT MEASURED.  They come from general knowledge
//     of these titles.  Not one of them was verified against an installation on this machine or
//     any other, and no vendor manifest was consulted.  Treat the whole table as a plausible
//     guess that happens to be checked into source control.
//
//   * THE LIST IS A CONVENIENCE FOR SEARCH AND A WEAK SIGNAL FOR DETECTION - NEVER
//     AUTHORITATIVE.  In the picker it saves a user a Browse click, and that is its main job.
//     In GuessGame() a match on one of these basenames is a hint about a process, not proof of
//     one; it must never be the sole reason anything irreversible happens to that process.
//
//   * DISCOVERED ENTRIES ALWAYS OUTRANK BUNDLED ONES.  Steam, Epic, GOG, Xbox and a running
//     process all carry a real path off this machine, so they win every de-duplication against
//     a bundled row (see Accumulator::Rank and the ordering in DiscoverGames()).  A bundled row
//     survives only where nothing real was found.
//
// SELECTION RULE, and it is the opposite of the obvious one: an entry whose exe name is WRONG
// is worse than a missing entry.  A missing entry costs the user one Browse click; a wrong or
// colliding one silently mis-detects an unrelated process as this game, and can hand that
// process to an All Games profile.  So the bar for inclusion is COLLISION RISK, not fame:
//
//   * Engine template and engine project basenames are excluded even when they are genuinely
//     this game's binary, because several unrelated titles ship the identical file name.
//     "ShooterGame.exe" (the Unreal shooter template) is the clearest case; "hl2.exe" is the
//     same problem on Source, where the whole engine lineup and its mods share one launcher.
//   * A basename that is only the generic half of a title ("Cities" from "Cities: Skylines",
//     "FlightSimulator" from "Microsoft Flight Simulator") is excluded for the same reason.
//   * A short basename is fine when the token names exactly one game and means nothing else -
//     "bg3.exe", "cs2.exe", "re4.exe".  It is excluded when the token has other readings, so a
//     franchise abbreviation covering several games ("cod"), an ambiguous one ("GoW": God of
//     War, Gears of War) and a standard technical acronym ("NMS") are all out.
//   * Titles whose executable could not be named with confidence were left out rather than
//     guessed at.
//
// DO NOT GROW THIS LIST TO LOOK COMPLETE.  It is not a catalogue of PC games and cannot become
// one; every added row is another chance to pin the wrong process.
// ---------------------------------------------------------------------------
const std::vector<GameEntry>& BundledGames() {
    // Function-local static: built once, on first use, and never mutated afterwards.
    static const std::vector<GameEntry> kGames = [] {
        struct Row { const wchar_t* name; const wchar_t* exe; };
        static const Row rows[] = {
            // Valve / Source
            { L"Counter-Strike 2",              L"cs2.exe" },
            { L"Dota 2",                        L"dota2.exe" },
            { L"Portal 2",                      L"portal2.exe" },
            { L"Left 4 Dead 2",                 L"left4dead2.exe" },
            // Competitive shooters
            { L"VALORANT",                      L"VALORANT-Win64-Shipping.exe" },
            { L"Apex Legends",                  L"r5apex.exe" },
            { L"Titanfall 2",                   L"Titanfall2.exe" },
            { L"Overwatch 2",                   L"Overwatch.exe" },
            { L"Destiny 2",                     L"destiny2.exe" },
            { L"Rainbow Six Siege",             L"RainbowSix.exe" },
            { L"The Division 2",                L"TheDivision2.exe" },
            { L"Battlefield 2042",              L"BF2042.exe" },
            { L"Battlefield V",                 L"bfv.exe" },
            { L"PUBG: Battlegrounds",           L"TslGame.exe" },
            { L"Fortnite",                      L"FortniteClient-Win64-Shipping.exe" },
            { L"Escape from Tarkov",            L"EscapeFromTarkov.exe" },
            { L"Hunt: Showdown",                L"HuntGame.exe" },
            { L"Halo Infinite",                 L"HaloInfinite.exe" },
            { L"DOOM Eternal",                  L"DOOMEternalx64vk.exe" },
            { L"Warframe",                      L"Warframe.x64.exe" },
            { L"Helldivers 2",                  L"helldivers2.exe" },
            { L"Deep Rock Galactic",            L"FSD-Win64-Shipping.exe" },
            { L"Dead by Daylight",              L"DeadByDaylight-Win64-Shipping.exe" },
            // Blizzard
            { L"World of Warcraft",             L"Wow.exe" },
            { L"Diablo IV",                     L"Diablo IV.exe" },
            { L"Diablo III",                    L"Diablo III64.exe" },
            { L"StarCraft II",                  L"SC2_x64.exe" },
            { L"Heroes of the Storm",           L"HeroesOfTheStorm_x64.exe" },
            // MOBA / MMO
            { L"League of Legends",             L"League of Legends.exe" },
            { L"Final Fantasy XIV",             L"ffxiv_dx11.exe" },
            { L"Guild Wars 2",                  L"Gw2-64.exe" },
            { L"The Elder Scrolls Online",      L"eso64.exe" },
            { L"Lost Ark",                      L"LostArk.exe" },
            { L"New World",                     L"NewWorld.exe" },
            { L"Black Desert",                  L"BlackDesert64.exe" },
            { L"Path of Exile",                 L"PathOfExile_x64.exe" },
            // Survival / sandbox
            { L"Rust",                          L"RustClient.exe" },
            { L"ARK: Survival Ascended",        L"ArkAscended.exe" },
            { L"DayZ",                          L"DayZ_x64.exe" },
            { L"7 Days to Die",                 L"7DaysToDie.exe" },
            { L"Valheim",                       L"valheim.exe" },
            { L"Palworld",                      L"Palworld.exe" },
            { L"Subnautica",                    L"Subnautica.exe" },
            { L"Sons of the Forest",            L"SonsOfTheForest.exe" },
            { L"Project Zomboid",               L"ProjectZomboid64.exe" },
            { L"Factorio",                      L"factorio.exe" },
            { L"Enshrouded",                    L"enshrouded.exe" },
            { L"Terraria",                      L"Terraria.exe" },
            { L"Stardew Valley",                L"Stardew Valley.exe" },
            { L"Minecraft for Windows",         L"Minecraft.Windows.exe" },
            // Big single-player
            { L"Cyberpunk 2077",                L"Cyberpunk2077.exe" },
            { L"The Witcher 3: Wild Hunt",      L"witcher3.exe" },
            { L"Red Dead Redemption 2",         L"RDR2.exe" },
            { L"Grand Theft Auto V",            L"GTA5.exe" },
            { L"ELDEN RING",                    L"eldenring.exe" },
            { L"DARK SOULS III",                L"DarkSoulsIII.exe" },
            { L"Sekiro: Shadows Die Twice",     L"sekiro.exe" },
            { L"ARMORED CORE VI",               L"armoredcore6.exe" },
            { L"Baldur's Gate 3",               L"bg3.exe" },
            { L"Starfield",                     L"Starfield.exe" },
            { L"Skyrim Special Edition",        L"SkyrimSE.exe" },
            { L"Fallout 4",                     L"Fallout4.exe" },
            { L"Fallout 76",                    L"Fallout76.exe" },
            { L"Hogwarts Legacy",               L"HogwartsLegacy.exe" },
            { L"Horizon Zero Dawn",             L"HorizonZeroDawn.exe" },
            { L"Marvel's Spider-Man Remastered",L"Spider-Man.exe" },
            { L"Alan Wake 2",                   L"AlanWake2.exe" },
            { L"Metro Exodus",                  L"MetroExodus.exe" },
            { L"Dying Light 2",                 L"DyingLightGame_x64_rwdi.exe" },
            { L"Remnant II",                    L"Remnant2.exe" },
            { L"Assassin's Creed Valhalla",     L"ACValhalla.exe" },
            { L"Assassin's Creed Odyssey",      L"ACOdyssey.exe" },
            { L"Far Cry 6",                     L"FarCry6.exe" },
            { L"Monster Hunter: World",         L"MonsterHunterWorld.exe" },
            { L"Resident Evil 4",               L"re4.exe" },
            { L"Resident Evil Village",         L"re8.exe" },
            { L"Devil May Cry 5",               L"DevilMayCry5.exe" },
            { L"Street Fighter 6",              L"StreetFighter6.exe" },
            // Live service / free-to-play
            { L"Genshin Impact",                L"GenshinImpact.exe" },
            { L"Honkai: Star Rail",             L"StarRail.exe" },
            { L"Elite Dangerous",               L"EliteDangerous64.exe" },
            { L"Star Citizen",                  L"StarCitizen.exe" },
            { L"Roblox",                        L"RobloxPlayerBeta.exe" },
            { L"Sea of Thieves",                L"SoTGame.exe" },
            { L"Phasmophobia",                  L"Phasmophobia.exe" },
            { L"Among Us",                      L"Among Us.exe" },
            { L"Fall Guys",                     L"FallGuys_client_game.exe" },
            { L"Rocket League",                 L"RocketLeague.exe" },
            // Sim / strategy / indie
            { L"The Sims 4",                    L"TS4_x64.exe" },
            { L"Forza Horizon 5",               L"ForzaHorizon5.exe" },
            { L"Euro Truck Simulator 2",        L"eurotrucks2.exe" },
            { L"American Truck Simulator",      L"amtrucks.exe" },
            { L"BeamNG.drive",                  L"BeamNG.drive.x64.exe" },
            { L"Sid Meier's Civilization VI",   L"CivilizationVI.exe" },
            { L"Total War: WARHAMMER III",      L"Warhammer3.exe" },
            { L"RimWorld",                      L"RimWorldWin64.exe" },
            { L"Hades",                         L"Hades.exe" },
            { L"Hollow Knight",                 L"hollow_knight.exe" },
            { L"Celeste",                       L"Celeste.exe" },
            { L"Cuphead",                       L"Cuphead.exe" },
            { L"Vampire Survivors",             L"VampireSurvivors.exe" },
        };

        std::vector<GameEntry> v;
        v.reserve(ARRAYSIZE(rows));
        for (size_t i = 0; i < ARRAYSIZE(rows); ++i) {
            GameEntry e;
            e.name      = rows[i].name;
            e.exe       = rows[i].exe;
            e.source    = GameSource::Bundled;
            e.installed = false;
            v.push_back(e);
        }
        return v;
    }();
    return kGames;
}

std::vector<GameEntry> FilterGames(const std::vector<GameEntry>& all,
                                   const std::wstring& query) {
    std::wstring q = ToLower(Trim(query));
    if (q.empty()) return all;

    std::vector<GameEntry> out;
    out.reserve(all.size());
    for (size_t i = 0; i < all.size(); ++i) {
        const GameEntry& e = all[i];
        if (ContainsCI(ToLower(e.name), q) || ContainsCI(ToLower(e.exe), q))
            out.push_back(e);   // input order preserved: ranking is the caller's job
    }
    return out;
}

bool HasFullscreenWindow(DWORD pid) {
    if (pid == 0) return false;
    FullscreenSearch s;
    s.pid = pid;
    s.found = false;
    ::EnumWindows(&FullscreenEnumProc, reinterpret_cast<LPARAM>(&s));
    return s.found;
}

bool GuessGame(DWORD pid, const std::wstring& exeBaseName,
               const std::vector<GameEntry>& known, GameGuess& out) {
    out = GameGuess();
    if (pid == 0) return false;

    std::wstring exe = BaseName(exeBaseName);
    if (exe.empty()) return false;
    std::wstring exeLower = ToLower(exe);

    if (IsExcludedProcess(exeLower)) return false;

    out.pid = pid;
    out.exe = exe;
    out.displayName = exe;

    // 1. Known list: the discovered list first, then the bundled one, per the header's
    //    "discovered or bundled".
    const GameEntry* hit = nullptr;
    for (size_t i = 0; i < known.size() && hit == nullptr; ++i)
        if (IEquals(BaseName(known[i].exe), exe)) hit = &known[i];
    if (hit == nullptr) {
        const std::vector<GameEntry>& bundled = BundledGames();
        for (size_t i = 0; i < bundled.size() && hit == nullptr; ++i)
            if (IEquals(bundled[i].exe, exe)) hit = &bundled[i];
    }
    if (hit != nullptr) {
        out.fromKnownList = true;
        if (!hit->name.empty()) out.displayName = hit->name;
    }

    // 2. The heuristic half.  Evaluated even for a known-list hit, because the caller shows
    //    "fullscreen" in the prompt and a known game that is not on screen yet is a different
    //    situation from one that is.
    out.fullscreen = HasFullscreenWindow(pid);

    if (out.fromKnownList) {
        out.confidence = 0.9;
        return true;
    }
    if (!out.fullscreen) return false;   // no name match and no fullscreen window: not a game

    // Tri::Unknown is the normal answer here - see HasGraphicsRuntime().  Only a positive
    // answer raises confidence; an unknown one leaves us at the fullscreen-only score.
    Tri gfx = HasGraphicsRuntime(pid);
    out.confidence = (gfx == Tri::Yes) ? 0.6 : 0.35;
    return true;
}

}  // namespace cd
