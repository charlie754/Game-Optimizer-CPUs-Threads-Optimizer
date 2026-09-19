// Game Optimizer - GPU assignment POLICY: which GPU, and which applications.
//
// PURE and header-only, like gpu_pref.h's decision half and gpu_rows.h, so the unit suite can
// drive every rule here without a GPU, a registry or a running process.
//
// WHY THIS FILE EXISTS SEPARATELY. gpu_pref.h decides what the machine HAS (adapters, a plan);
// gpu_rows.h decides how a row LOOKS and what the bulk action SELECTS. What is here decides
// POLICY that neither can: which of several possible background GPUs to default to, which
// running binaries must never be swept, and whether two paths are one install at two versions.
// Every rule in this file was found missing by running the feature against the operator's real
// machine, not by reading a spec - see each one.
#pragma once

#include <map>
#include <string>
#include <utility>
#include <vector>

#include "config.h"
#include "gpu_pref.h"
#include "util.h"

namespace cd {

// ---- Which binaries belong to Windows -------------------------------------------------------
//
// True for an image under the Windows directory of any drive: "C:\Windows\System32\dwm.exe",
// "d:\windows\explorer.exe". False for "C:\WindowsApps\..." - a different folder that merely
// starts with the same letters.
//
// 🔴 FOUND ON THE OPERATOR'S MACHINE: 67 distinct running paths, 24 of them under the Windows
// directory - dwm.exe among them, which is the desktop compositor. Pinning the compositor to a
// 2 GB integrated GPU is not "isolating a background app", and the bulk action swept it anyway.
// Such rows stay VISIBLE and stay SELECTABLE by hand - operator ruling, "all app should be an
// option for user" - but "Auto assign GPU for Gaming" never ticks them.
inline bool IsWindowsImagePath(const std::wstring& path) {
    const std::wstring low = ToLower(path);
    // "x:\windows\" - drive letter, colon, then exactly the Windows folder.
    if (low.size() < 11) return false;
    if (low[1] != L':') return false;
    return low.compare(2, 9, L"\\windows\\") == 0;
}

// ---- Which binaries the product itself says must not be moved -------------------------------
//
// True when the SHIPPED default exclusion list names this executable, whatever the user's own
// config.ini says.
//
// 🔴 FOUND ON THE OPERATOR'S MACHINE, IN THE ISOLATE GPU WINDOW'S OWN SCREENSHOT. "Game Optimize
// for GPU" ticked amdow.exe and AMDRSSrcExt.exe - AMD display-driver components that
// DefaultExclusions() protects with the comment "moving vendor driver helpers risks destabilizing
// the driver path". They were not protected here because an existing user's config.ini carries
// the exclusion list AS IT WAS WHEN THAT CONFIG WAS WRITTEN, and entries added to the defaults
// since never reach it. For CPU masks that is a known and separate question; for a feature that
// rewrites which GPU a driver component uses, the product's own safety list must apply on every
// machine, so it is consulted in addition to the user's list, never instead of it.
inline bool IsDefaultExcluded(const std::wstring& exeName) {
    static const Config defaults = [] {
        Config c;
        c.exclusions = DefaultExclusions();
        return c;
    }();
    return defaults.IsExcluded(exeName);
}

// ---- Telling two rows with one file name apart ----------------------------------------------
//
// The last `levels` directory names above the file, joined with a backslash, or empty when the
// path has fewer. ParentFoldersOf("C:\a\b\c\x.exe", 2) -> "b\c".
//
// 🔴 FOUND IN THE WINDOW'S SCREENSHOT: claude.exe appeared twice and bash.exe twice, with nothing
// to tell the rows apart. One claude.exe is Claude Desktop and the other is Claude Code - two
// different programs a user may well want on different GPUs, presented as identical lines. A
// preference is per path, so the path is the identity; this is the part of it a person can read.
inline std::wstring ParentFoldersOf(const std::wstring& path, int levels) {
    size_t end = path.find_last_of(L"\\/");
    if (end == std::wstring::npos) return std::wstring();
    size_t start = end;
    for (int i = 0; i < levels; ++i) {
        if (start == 0) return std::wstring();
        const size_t prev = path.find_last_of(L"\\/", start - 1);
        if (prev == std::wstring::npos) return std::wstring();
        start = prev;
    }
    return path.substr(start + 1, end - start - 1);
}

// For each (file name, full path): the folders to show beside a row whose file name another row shares, or
// empty. Two folder levels first; the WHOLE folder when that still reads the same as another row, or when the
// path is too shallow to have two levels.
//
// 🔴 Found by adversarial review, round 5: "C:\Alpha\x.exe" and "D:\Beta\x.exe" have no second folder level,
// so both got an empty suffix and were drawn as the same line.
inline std::vector<std::wstring> DisambiguatingFolders(
    const std::vector<std::pair<std::wstring, std::wstring> >& namePath) {
    struct Folder {
        static std::wstring Of(const std::wstring& p) {
            const size_t cut = p.find_last_of(L"\\/");
            return cut == std::wstring::npos ? std::wstring() : p.substr(0, cut);
        }
    };
    std::vector<std::wstring> where(namePath.size());
    std::map<std::wstring, size_t> nameCount;
    for (size_t i = 0; i < namePath.size(); ++i) ++nameCount[ToLower(namePath[i].first)];
    for (size_t i = 0; i < namePath.size(); ++i) {
        if (nameCount[ToLower(namePath[i].first)] < 2) continue;
        where[i] = ParentFoldersOf(namePath[i].second, 2);
        if (where[i].empty()) where[i] = Folder::Of(namePath[i].second);
    }
    std::map<std::wstring, size_t> labelCount;
    for (size_t i = 0; i < namePath.size(); ++i)
        if (!where[i].empty()) ++labelCount[ToLower(namePath[i].first + L"|" + where[i])];
    for (size_t i = 0; i < namePath.size(); ++i) {
        if (!where[i].empty() && labelCount[ToLower(namePath[i].first + L"|" + where[i])] > 1)
            where[i] = Folder::Of(namePath[i].second);
    }
    return where;
}

// ---- Whether two paths are one install at two versions --------------------------------------
//
// The directory ABOVE the versioned folder, lowercased, or empty when the path is too shallow to
// have one. "C:\A\app-1.2\x.exe" -> "c:\a".
inline std::wstring InstallRootOf(const std::wstring& path) {
    const size_t file = path.find_last_of(L"\\/");
    if (file == std::wstring::npos || file == 0) return std::wstring();
    const size_t version = path.find_last_of(L"\\/", file - 1);
    if (version == std::wstring::npos) return std::wstring();
    return ToLower(path.substr(0, version));
}

// True when a stale preference path and a live running path are the same application at two
// versions: same install root, both non-empty.
//
// 🔴 FOUND ON THE OPERATOR'S MACHINE, AND IT IS A FALSE POSITIVE THE DETECTOR SHIPPED WITH.
// FindOrphanedAssignments matched stale to live by BASENAME alone, so Claude Code's own
// claude.exe (Roaming\Claude\claude-code\2.1.266\) was reported as having "lost" Claude
// Desktop's GPU assignment (Local\AnthropicClaude\app-1.49585.0\) - two different applications
// that happen to share a file name. Acting on it would pin the wrong program. An auto-updating
// app moves between SIBLING version folders under one root; an unrelated app does not.
//
// 🔴 ...AND BOTH FOLDERS UNDER THAT ROOT MUST BE VERSIONS OF ONE NAME. Found by adversarial review
// of v0.5.5: a shared root alone matched "C:\Program Files\OldApp\helper.exe" to
// "C:\Program Files\OtherApp\helper.exe" - two installs side by side, not one install at two
// versions - and every Squirrel app ("Local\Discord\Update.exe", "Local\Slack\Update.exe") shares
// both a root and an exe name. A version folder has a digit in its name, and two versions of one
// app differ only in digits and dots.
// ponytail: digit-stripped name match, so sibling folders "App1" and "App2" still pass; a real
// version parser if two such apps with one exe name ever turn up.
inline bool VersionFolderStem(const std::wstring& folder, std::wstring& stem) {
    stem.clear();
    bool digit = false;
    for (size_t i = 0; i < folder.size(); ++i) {
        const wchar_t c = folder[i];
        if (c >= L'0' && c <= L'9') { digit = true; continue; }
        if (c == L'.') continue;
        stem += c;
    }
    stem = ToLower(stem);
    return digit;
}

inline bool SameInstallRoot(const std::wstring& stalePath, const std::wstring& livePath) {
    const std::wstring a = InstallRootOf(stalePath);
    const std::wstring b = InstallRootOf(livePath);
    if (a.empty() || a != b) return false;
    std::wstring sa, sb;
    return VersionFolderStem(ParentFoldersOf(stalePath, 1), sa) &&
           VersionFolderStem(ParentFoldersOf(livePath, 1), sb) && sa == sb;
}

// Case-insensitive path order in which a run of digits compares as a NUMBER: "app-9" before
// "app-10". Found by adversarial review of v0.5.5: the orphan detector picked the newest stale
// version by plain string order, in which "app-9" sorts AFTER "app-10".
inline bool NaturalPathLess(const std::wstring& a, const std::wstring& b) {
    const std::wstring x = ToLower(a);
    const std::wstring y = ToLower(b);
    size_t i = 0, j = 0;
    while (i < x.size() && j < y.size()) {
        const bool dx = x[i] >= L'0' && x[i] <= L'9';
        const bool dy = y[j] >= L'0' && y[j] <= L'9';
        if (dx && dy) {
            size_t ei = i, ej = j;
            while (ei < x.size() && x[ei] >= L'0' && x[ei] <= L'9') ++ei;
            while (ej < y.size() && y[ej] >= L'0' && y[ej] <= L'9') ++ej;
            size_t si = i, sj = j;                       // leading zeros carry no value
            while (si + 1 < ei && x[si] == L'0') ++si;
            while (sj + 1 < ej && y[sj] == L'0') ++sj;
            if (ei - si != ej - sj) return (ei - si) < (ej - sj);
            const int c = x.compare(si, ei - si, y, sj, ej - sj);
            if (c != 0) return c < 0;
            i = ei;
            j = ej;
            continue;
        }
        if (x[i] != y[j]) return x[i] < y[j];
        ++i;
        ++j;
    }
    return (x.size() - i) < (y.size() - j);
}

// ---- Which GPU the background applications move TO ------------------------------------------
//
// Every adapter key that can take background applications, in enumeration order: every key
// except the game GPU's, each once. Empty when the plan is undecidable.
//
// The game GPU's key is excluded by KEY, not by pointer, so a second DXGI entry for the same
// card never appears as a choice - the operator's RTX 5090 is listed twice by DXGI.
inline std::vector<std::wstring> CandidateBackgroundKeys(const std::vector<GpuAdapter>& adapters,
                                                         const GpuPlan& plan) {
    std::vector<std::wstring> out;
    if (plan.backgroundKey.empty()) return out;
    for (size_t i = 0; i < adapters.size(); ++i) {
        const std::wstring& k = adapters[i].adapterKey;
        if (k.empty() || k == plan.gameKey) continue;
        bool seen = false;
        for (size_t j = 0; j < out.size(); ++j) if (out[j] == k) { seen = true; break; }
        if (!seen) out.push_back(k);
    }
    return out;
}

// Every GPU the target picker offers: the main GPU FIRST, then every background candidate, each key once.
// Empty when the plan is undecidable.
//
// 🔴 THE MAIN GPU IS ADDED HERE, NEVER IN CandidateBackgroundKeys. Operator request, v0.5.6: removing an
// assignment does not put an application on the main GPU, so a user must be able to pin one there. But
// ChooseBackgroundKey counts registry pins only over CandidateBackgroundKeys, so adding the main GPU THERE
// would make every pin to it a vote - the DEFAULT target could become the game's own card, and each pin
// written through the picker would add another vote. AJ2 and AJ3 guard that default; AJ34 guards this list.
// ponytail: offered only on a decidable plan - a machine whose plan refuses (identical twin cards, a display
// query that failed) names no main GPU; offering one there needs a main-GPU choice PlanGpuIsolation does not make.
// Second ceiling: on a decidable plan whose main-GPU key is shared by a physically identical card (twin cards
// plus a third GPU), the key cannot say which twin, and which one Windows picks for it is unmeasured. It is
// still offered - either twin is the main GPU's own model, never the background GPU. AJ34 pins that case.
inline std::vector<std::wstring> PickerTargetKeys(const std::vector<GpuAdapter>& adapters, const GpuPlan& plan) {
    std::vector<std::wstring> out;
    if (plan.backgroundKey.empty()) return out;
    if (!plan.gameKey.empty()) out.push_back(plan.gameKey);
    // CandidateBackgroundKeys skips every entry with the game GPU's key and lists each other key once, so the
    // second DXGI entry of the main GPU never adds a row.
    const std::vector<std::wstring> background = CandidateBackgroundKeys(adapters, plan);
    out.insert(out.end(), background.begin(), background.end());
    return out;
}

// True when `key` names the main GPU - the card the plan puts games on. False for an empty key, so an
// undecidable plan names no main GPU.
inline bool IsMainGpuKey(const GpuPlan& plan, const std::wstring& key) {
    return !key.empty() && key == plan.gameKey;
}

// The default background GPU: the candidate the USER has already pinned the most applications
// to, else the plan's own pick. Empty when the plan is undecidable.
//
// 🔴 FOUND ON THE OPERATOR'S MACHINE. PlanGpuIsolation takes the FIRST display-less adapter in
// DXGI order, and there that is the AMD Radeon integrated GPU with 2 GB - so the bulk action
// would have moved sixty-odd applications onto it. Every preference the operator set by hand
// points at the RTX 4090. The registry is the one place a user has already told us which card
// they use for this, so it outranks enumeration order; the plan's pick is only the fallback for
// a machine with no history. A tie keeps the plan's pick, so this never overrides it on noise.
//
// The window offers every candidate in a picker, so this is a default, never a decision.
inline std::wstring ChooseBackgroundKey(
    const std::vector<GpuAdapter>& adapters, const GpuPlan& plan,
    const std::vector<std::pair<std::wstring, std::wstring> >& registry) {
    if (plan.backgroundKey.empty()) return std::wstring();

    const std::vector<std::wstring> candidates = CandidateBackgroundKeys(adapters, plan);
    std::map<std::wstring, size_t> votes;
    for (size_t i = 0; i < candidates.size(); ++i) votes[candidates[i]] = 0;
    for (size_t i = 0; i < registry.size(); ++i) {
        std::map<std::wstring, size_t>::iterator it = votes.find(registry[i].second);
        if (it != votes.end()) ++it->second;
    }

    std::wstring best = plan.backgroundKey;
    size_t bestVotes = votes.count(best) ? votes[best] : 0;
    for (size_t i = 0; i < candidates.size(); ++i) {
        const size_t v = votes[candidates[i]];
        if (v > bestVotes) { best = candidates[i]; bestVotes = v; }
    }
    return best;
}

}  // namespace cd
