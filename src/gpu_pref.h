// Game Optimizer - GPU preference and isolation.
//
// WHY THIS FILE EXISTS. When an app updates to a versioned folder (Claude Desktop
// app-1.52386.3 → app-1.52387.0), Windows' per-app GPU preference registry key is keyed to
// the FULL PATH - so the setting silently orphans and the app reverts to the machine's
// default GPU choice, usually the same one the game is on. This module detects that drift by
// comparing the RUNNING process' full path with the paths the registry names
// (FindOrphanedAssignments, gpu_rows.h), and the window writes the preference again for the new path.
//
// 🔴 IT DOES NOT MEASURE WHICH GPU A PROCESS IS ACTUALLY ON. Nothing in this product fills
// GpuRow::runningKey. An earlier version of this comment promised that it did; adversarial review
// of v0.5.5 caught it. Which GPU Windows hands an application is visible only from inside it.
//
// This file provides the PURE decision logic
// inlined here so it is unit-testable without Win32. All Win32 calls live in gpu_pref.cpp.
//
// The Windows API surface is wrapped by plan and state classifiers that take abstract
// adapter lists and registry values. These form a firewall: the rule remains unit-testable,
// and Win32 failures are caught by higher layers that ask for specific things.
#pragma once
#include <string>
#include <utility>
#include <vector>
#include <algorithm>
#include <cctype>
#include <cwctype>

namespace cd {

// Whether a GPU is the processor's built-in graphics or a card of its own. THREE answers, like
// hasDisplay/displayUnknown: Unknown means nobody could tell, never "discrete".
//
// 🔴 MEMORY SIZE CANNOT ANSWER IT. [M] igpuprobe, the operator's machine: the AMD Radeon integrated GPU
// reports 2,119,966,720 bytes of dedicated memory, and every adapter reports the same shared memory, so an
// "under N GB is integrated" rule would misfire. gpu_pref.cpp asks Windows' own DXCore instead.
enum class GpuKind { Unknown, Integrated, Discrete };

// One GPU in the system, enumerated by Win32 (DXGI + SetupAPI or PnP).
struct GpuAdapter {
    std::wstring name;           // e.g. "NVIDIA GeForce RTX 5090"
    std::wstring pnpId;          // full PnP device ID, e.g. PCI\VEN_10DE&DEV_2B85&SUBSYS_53021462&REV_A1\...
    std::wstring adapterKey;     // extracted as VEN&DEV&SUBSYS, e.g. "10DE&2B85&53021462", UPPERCASE
    bool hasDisplay = false;     // a monitor is attached to this adapter
    // True when asking the adapter for its outputs FAILED with an error other than "there are
    // none". Whether a monitor hangs off it is then unknown, and PlanGpuIsolation refuses.
    bool displayUnknown = false;
    unsigned long long vram = 0; // VRAM in bytes, 0 if unknown
    // DXCore's IsIntegrated for this entry; Unknown when DXCore is absent or does not know the entry -
    // [M] it does not list the operator's second RTX 5090 entry at all.
    GpuKind kind = GpuKind::Unknown;
};

// The kind of GPU an adapter KEY names, over every entry with that key: Integrated if any entry says so,
// else Discrete if any entry says so, else Unknown.
//
// 🔴 BY KEY, NOT BY ENTRY, BECAUSE THE PICKER AND THE REGISTRY WORK BY KEY. DXGI lists the operator's RTX 5090
// twice and DXCore answers for only one of the two, so an entry-by-entry answer would call that card Unknown.
inline GpuKind KindForKey(const std::vector<GpuAdapter>& adapters, const std::wstring& key) {
    GpuKind kind = GpuKind::Unknown;
    for (size_t i = 0; i < adapters.size(); ++i) {
        if (adapters[i].adapterKey != key) continue;
        if (adapters[i].kind == GpuKind::Integrated) return GpuKind::Integrated;
        if (adapters[i].kind == GpuKind::Discrete) kind = GpuKind::Discrete;
    }
    return kind;
}

// How many distinct adapter keys KindForKey leaves Unknown; `keysOut` (optional) receives how many distinct keys there
// are. gpu_pref.cpp logs an Unknown kind only when this is not zero.
//
// 🔴 A KEY, NOT AN ENTRY, IS WHAT CAN LOSE A WARNING (Council review, v0.5.6). The log counted entries, so the operator's
// second RTX 5090 entry - which DXCore does not know, and whose key reads Discrete through its twin - wrote "Unknown" on
// every visit to the tab while no GPU the picker offers was unknown.
// ponytail: quadratic in the adapter count, which is a handful; a set of keys if that ever changes.
inline size_t UnknownKindKeyCount(const std::vector<GpuAdapter>& adapters, size_t* keysOut = nullptr) {
    std::vector<std::wstring> seen;
    size_t unknown = 0;
    for (size_t i = 0; i < adapters.size(); ++i) {
        if (std::find(seen.begin(), seen.end(), adapters[i].adapterKey) != seen.end()) continue;
        seen.push_back(adapters[i].adapterKey);
        if (KindForKey(adapters, adapters[i].adapterKey) == GpuKind::Unknown) ++unknown;
    }
    if (keysOut) *keysOut = seen.size();
    return unknown;
}

// Convert a full PnP ID to the Windows GPU preference adapter key format.
// Input:  "PCI\VEN_10DE&DEV_2684&SUBSYS_40BF1458&REV_A1\4&15A5C264&0&000B"
// Output: "10DE&2684&40BF1458" (uppercase, no prefix/revision/etc)
// Returns empty string on parse failure. Case-insensitive input, UPPERCASE output.
inline std::wstring AdapterKeyFromPnpId(const std::wstring& pnpId) {
    if (pnpId.empty()) return std::wstring();

    // Search an UPPERCASED copy. Real PnP ids are conventionally upper case, but the
    // casing is not guaranteed by anything, and a case-sensitive find here silently
    // returned empty for a lower-case id - which reads as "no GPU preference" rather
    // than as a parse failure. Uppercasing once up front makes the search and the
    // output casing the same decision.
    std::wstring up = pnpId;
    std::transform(up.begin(), up.end(), up.begin(), ::towupper);

    // Find VEN_
    size_t venPos = up.find(L"VEN_");
    if (venPos == std::wstring::npos) return std::wstring();
    venPos += 4;  // skip "VEN_"
    size_t venEnd = up.find(L'&', venPos);
    if (venEnd == std::wstring::npos) return std::wstring();
    std::wstring ven = up.substr(venPos, venEnd - venPos);

    // Find DEV_
    size_t devPos = up.find(L"DEV_", venEnd);
    if (devPos == std::wstring::npos) return std::wstring();
    devPos += 4;  // skip "DEV_"
    size_t devEnd = up.find(L'&', devPos);
    if (devEnd == std::wstring::npos) return std::wstring();
    std::wstring dev = up.substr(devPos, devEnd - devPos);

    // Find SUBSYS_
    size_t subsysPos = up.find(L"SUBSYS_", devEnd);
    if (subsysPos == std::wstring::npos) return std::wstring();
    subsysPos += 7;  // skip "SUBSYS_"
    size_t subsysEnd = up.find(L'&', subsysPos);
    if (subsysEnd == std::wstring::npos) return std::wstring();
    std::wstring subsys = up.substr(subsysPos, subsysEnd - subsysPos);

    // Uppercase all components
    std::transform(ven.begin(), ven.end(), ven.begin(), ::towupper);
    std::transform(dev.begin(), dev.end(), dev.begin(), ::towupper);
    std::transform(subsys.begin(), subsys.end(), subsys.begin(), ::towupper);

    // Combine as VEN&DEV&SUBSYS
    return ven + L"&" + dev + L"&" + subsys;
}

// Format the Windows GPU preference registry value for a specific adapter.
// The preference is stored as REG_SZ in HKCU\Software\Microsoft\DirectX\UserGpuPreferences
// for the full exe path, containing a semicolon-delimited attribute string.
// Input:  "10DE&2B85&53021462" (adapter key)
// Output: "SpecificAdapter=10DE&2B85&53021462;GpuPreference=1073741824;" (0x40000000)
//         where 1073741824 is the magic marker for SpecificAdapter mode.
inline std::wstring FormatPreferenceValue(const std::wstring& adapterKey) {
    if (adapterKey.empty()) return std::wstring();
    // 1073741824 = 0x40000000 (magic constant for SpecificAdapter mode)
    return L"SpecificAdapter=" + adapterKey + L";GpuPreference=1073741824;";
}

// Parse the registry value back to the adapter key (inverse of FormatPreferenceValue).
// Input:  "SpecificAdapter=10DE&2684&40BF1458;GpuPreference=1073741824;" or variations
// Input:  "GpuPreference=2;" or other non-SpecificAdapter forms
// Output: adapter key if SpecificAdapter form, empty string otherwise
// Tolerates:
//   - key order (GpuPreference before SpecificAdapter, etc)
//   - missing trailing semicolon
//   - extra unknown fields
// The text of the first field named `fieldName` - whole name, case-insensitive - and whether one
// exists. Fields are separated by ';', the last may lack one, and a field's name is everything before
// its first '='.
//
// 🔴 WHOLE FIELDS, NOT A SUBSTRING. Found by adversarial review of v0.5.5: the parser searched for the
// text "SpecificAdapter=" anywhere, so a field named NotSpecificAdapter satisfied it, and a last field
// without ';' was refused despite the comment promising otherwise.
inline bool FindPreferenceField(const std::wstring& value, const std::wstring& fieldName,
                                std::wstring& textOut) {
    std::wstring want = fieldName;
    for (size_t i = 0; i < want.size(); ++i) want[i] = static_cast<wchar_t>(::towlower(want[i]));
    size_t start = 0;
    while (start < value.size()) {
        size_t semi = value.find(L';', start);
        if (semi == std::wstring::npos) semi = value.size();
        const std::wstring field = value.substr(start, semi - start);
        const size_t eq = field.find(L'=');
        if (eq != std::wstring::npos) {
            std::wstring name = field.substr(0, eq);
            for (size_t i = 0; i < name.size(); ++i) name[i] = static_cast<wchar_t>(::towlower(name[i]));
            if (name == want) {
                textOut = field.substr(eq + 1);
                return true;
            }
        }
        start = semi + 1;
    }
    return false;
}

inline std::wstring AdapterKeyFromPreferenceValue(const std::wstring& value) {
    std::wstring key;
    if (!FindPreferenceField(value, L"SpecificAdapter", key)) return std::wstring();
    // VVVV&DDDD&SSSSSSSS: hexadecimal, widths 4, 4 and 8 - the form DXGI's ids print to. Anything
    // else is not a card this can name, so it is not an assignment.
    if (key.size() != 18 || key[4] != L'&' || key[9] != L'&') return std::wstring();
    for (size_t i = 0; i < key.size(); ++i) {
        if (i == 4 || i == 9) continue;
        if (!::iswxdigit(key[i])) return std::wstring();
        key[i] = static_cast<wchar_t>(::towupper(key[i]));
    }
    return key;
}

// ---- What an entry says about the GPU, as one key the window can compare and label ----------
//
// An adapter key when the entry names a card; one of the two keys below when it holds Windows' own
// "Power saving" (GpuPreference=1) or "High performance" (GpuPreference=2) setting and names no card;
// empty for no choice - absent, "Let Windows decide" (GpuPreference=0), or anything else.
//
// 🔴 WINDOWS' OWN SETTING IS A CHOICE THE USER ALREADY MADE. Found by adversarial review of v0.5.5:
// such an entry parsed to "no key", so the window showed it as "not assigned", the bulk action ticked
// it without the confirm counting it, and Remove could not touch it. It is now shown by name, counted
// as replaced, and removable.
inline std::wstring WindowsPowerSavingKey() { return L"WINDOWS:POWER-SAVING"; }
inline std::wstring WindowsHighPerformanceKey() { return L"WINDOWS:HIGH-PERFORMANCE"; }
inline bool IsWindowsModeKey(const std::wstring& key) { return key.compare(0, 8, L"WINDOWS:") == 0; }
// A value that is there but could not be read back after a change: shown as exactly that, never as
// "not assigned" (adversarial review, round 4).
inline std::wstring UnreadableChoiceKey() { return L"UNREADABLE"; }

// 🔴 A CARD COUNTS ONLY BESIDE ITS OWN MODE. Found by adversarial review of v0.5.5, round 3: a
// SpecificAdapter field was taken as an assignment whatever GpuPreference said. Windows writes a card
// together with GpuPreference=1073741824 [M, all ten such values on the operator's machine]; beside 1 or
// 2 the entry is read as that Windows setting, and anything else as no choice. What Windows itself does
// with a card beside 1 or 2 is [A] - not measured - so the entry is shown as the setting it names.
inline std::wstring PreferenceChoiceKey(const std::wstring& value) {
    std::wstring mode;
    if (!FindPreferenceField(value, L"GpuPreference", mode)) return std::wstring();
    if (mode == L"1073741824") return AdapterKeyFromPreferenceValue(value);
    if (mode == L"1") return WindowsPowerSavingKey();
    if (mode == L"2") return WindowsHighPerformanceKey();
    return std::wstring();
}

// THE GPU ISOLATION POLICY. Pure: given a list of available adapters, decide which
// GPU should run the game and which should run background apps.
//
// RULES, in priority order:
//   1. Fewer than 2 adapters -> both results empty (feature not applicable)
//   2. Game GPU MUST have a display attached. NEVER put the game on a display-less adapter
//      (its output would need a cross-adapter copy to reach the monitor)
//   3. Background GPU is preferably WITHOUT a display; if all have displays, pick any that
//      is not the game GPU
//   4. If multiple adapters qualify for game GPU, prefer the one with more VRAM
//   5. If NO adapter has a display, return both empty (cannot decide safely)
//   6. Output a human-readable explanation in `why`
struct GpuPlan {
    std::wstring gameKey;       // adapter key for game (empty if undecidable)
    std::wstring backgroundKey; // adapter key for background (empty if undecidable)
    std::wstring why;           // human-readable reason (e.g. "RTX 5090 as game, RTX 4090 as background")
};

inline GpuPlan PlanGpuIsolation(const std::vector<GpuAdapter>& adapters) {
    GpuPlan plan;

    // Rule 1: need at least 2 adapters
    if (adapters.size() < 2) {
        plan.why = L"fewer than 2 GPUs";
        return plan;
    }

    // 🔴 AN UNANSWERED DISPLAY QUERY IS NOT A DISPLAY, AND IT IS NOT "NO DISPLAY" EITHER. Found by
    // adversarial review of v0.5.5: the enumerator counted every EnumOutputs error as a display,
    // so one failed query could crown the wrong card as the game GPU and move background apps
    // onto the card the monitor is really on. Guessing the other way can do the same, so the
    // plan refuses and names the card that could not answer.
    for (const auto& a : adapters) {
        if (a.displayUnknown) {
            plan.why = L"could not tell whether " + a.name + L" drives a display";
            return plan;
        }
    }

    // Rule 5: need at least one adapter with a display
    bool anyHasDisplay = false;
    for (const auto& a : adapters) {
        if (a.hasDisplay) {
            anyHasDisplay = true;
            break;
        }
    }
    if (!anyHasDisplay) {
        plan.why = L"no GPU has a display attached";
        return plan;
    }

    // Find the best game GPU: must have display, prefer more VRAM
    const GpuAdapter* gameGpu = nullptr;
    for (const auto& a : adapters) {
        if (a.hasDisplay) {
            if (!gameGpu || a.vram > gameGpu->vram) {
                gameGpu = &a;
            }
        }
    }

    if (!gameGpu) {
        plan.why = L"no GPU with display found";
        return plan;
    }

    // Find the background GPU: prefer one without display, or any other if all have displays
    const GpuAdapter* bgGpu = nullptr;

    // 🔴 BOTH PASSES SKIP EVERY ADAPTER THAT SHARES THE GAME GPU'S KEY - not just the game
    // GPU's own pointer. Found on the operator's machine, where DXGI lists the RTX 5090 TWICE
    // (same key, same memory, one entry with a display and one without). A pointer-only skip
    // lets that second entry win the display-less pass whenever it is enumerated first, and the
    // plan then either pins apps to the game's own card or refuses outright - so the feature
    // switched itself off, or did the wrong thing, depending on DXGI enumeration order.
    //
    // A same-key adapter is useless as a background GPU whichever it is - the same card listed
    // twice, or a physically identical card the registry format cannot tell apart - so it is
    // skipped here, and the plan refuses only when NOTHING with a different key is left.

    // First pass: an adapter without a display
    for (const auto& a : adapters) {
        if (!a.hasDisplay && a.adapterKey != gameGpu->adapterKey) {
            bgGpu = &a;
            break;
        }
    }

    // Second pass: if every other adapter has a display, any adapter with a different key
    if (!bgGpu) {
        for (const auto& a : adapters) {
            if (a.adapterKey != gameGpu->adapterKey) {
                bgGpu = &a;
                break;
            }
        }
    }

    if (!bgGpu) {
        // Every other adapter shares the game GPU's key. There is no key that separates two
        // identical cards - the registry format has no field for it - so refuse and say why.
        bool twin = false;
        for (const auto& a : adapters)
            if (&a != gameGpu && a.adapterKey == gameGpu->adapterKey) twin = true;
        plan.why = twin ? L"both GPUs report the same adapter id (" + gameGpu->adapterKey
                              + L"), so a preference cannot tell them apart"
                        : std::wstring(L"no background GPU found");
        return plan;
    }

    // By construction of the passes above, bgGpu never shares the game GPU's key. That
    // invariant is what stops the plan pinning background apps to the card the game is on;
    // the key is VendorId&DeviceId&SubSysId, so two identical cards collide on it.
    // ponytail: identical cards are refused; per-instance pinning would need a different
    // registry mechanism than SpecificAdapter, not a better key.

    plan.gameKey = gameGpu->adapterKey;
    plan.backgroundKey = bgGpu->adapterKey;
    plan.why = gameGpu->name + L" as game, " + bgGpu->name + L" as background";

    return plan;
}

// GPU PREFERENCE STATE - why the setting does not match intent.
// The registry value `haveValue` is what we stored last time.
// The running process `runningOnKey` is which adapter it is ACTUALLY on right now.
// The `wantKey` is what we want it on.
enum class GpuPrefState {
    Correct,         // wantKey matches what is stored AND what is running
    Missing,         // haveValue is empty (preference was never written for this path)
    WrongAdapter,    // haveValue is not empty but contains wrong adapter key (manual edit? config changed?)
    StaleNotApplied, // haveValue has the CORRECT key but runningOnKey differs (the orphaned-path case:
                     // app updated to new versioned folder, new path was never written, old path
                     // setting was correct but process is on a different adapter). This is the
                     // MAIN BUG this feature exists to solve.
    Unknown,         // cannot determine running adapter or some other essential data is missing
};

// Classify the GPU preference state for a given executable path and intent.
// wantKey:      the adapter key we want (e.g. "10DE&2B85&53021462")
// haveValue:    the raw registry value stored for this exe path (empty = never written)
// runningOnKey: the adapter key the process is ACTUALLY on right now (empty = unknown/not running)
//
// Returns:
//   Correct         -> stored value matches intent AND running process is on that adapter
//   Missing         -> haveValue is empty
//   WrongAdapter    -> haveValue has a SpecificAdapter value but it is not wantKey
//   StaleNotApplied -> haveValue has wantKey AND stored is for SpecificAdapter form, but
//                      runningOnKey is different (process not yet restarted, or waiting for
//                      the new path to be written)
//   Unknown         -> wantKey or runningOnKey is empty (cannot classify)
inline GpuPrefState ClassifyGpuPref(const std::wstring& wantKey,
                                    const std::wstring& haveValue,
                                    const std::wstring& runningOnKey) {
    // Rule 1: if want or running is unknown, we cannot classify
    if (wantKey.empty() || runningOnKey.empty()) {
        return GpuPrefState::Unknown;
    }

    // Rule 2: if no stored value, Missing
    if (haveValue.empty()) {
        return GpuPrefState::Missing;
    }

    // Read the stored value exactly as the window does (adversarial review, round 4): a card counts only
    // beside GpuPreference=1073741824, and a Windows setting is not a card.
    std::wstring storedKey = PreferenceChoiceKey(haveValue);
    if (IsWindowsModeKey(storedKey)) storedKey.clear();

    // Rule 3: if stored value is not SpecificAdapter form, it's WrongAdapter
    if (storedKey.empty()) {
        return GpuPrefState::WrongAdapter;
    }

    // Rule 4: if stored adapter key is wrong, WrongAdapter
    if (storedKey != wantKey) {
        return GpuPrefState::WrongAdapter;
    }

    // Rule 5: stored is correct, but is process running on that adapter?
    if (runningOnKey == wantKey) {
        return GpuPrefState::Correct;
    }

    // Stored is correct but process is on a different adapter: StaleNotApplied
    return GpuPrefState::StaleNotApplied;
}

// ---- Editing an existing preference without destroying what else it carries ----------------
//
// 🔴 A UserGpuPreferences VALUE IS NOT ONLY A GPU CHOICE. Found by adversarial review of v0.5.5:
// Apply wrote FormatPreferenceValue over the WHOLE value and Remove deleted the WHOLE value, so any
// other "name=value;" field kept in that same string for that application would have been
// silently destroyed by a feature that only meant to change which GPU it uses. The functions below
// touch exactly the two fields this feature owns - SpecificAdapter and GpuPreference - and carry
// every other field through unchanged, in its original order.

inline bool IsOwnedGpuField(const std::wstring& name) {
    std::wstring n = name;
    for (size_t i = 0; i < n.size(); ++i) n[i] = static_cast<wchar_t>(::towlower(n[i]));
    return n == L"specificadapter" || n == L"gpupreference";
}

// Every field of `value` this feature owns (`owned` true) or does NOT own (`owned` false), each written back as
// "name=value;" in its original order. A fragment with no '=' or an empty name carries nothing and is dropped.
// One walk for both halves, so a field can never count as neither or as both.
inline std::wstring GpuPreferenceFieldsOwned(const std::wstring& value, bool owned) {
    std::wstring out;
    size_t start = 0;
    while (start <= value.size()) {
        size_t semi = value.find(L';', start);
        if (semi == std::wstring::npos) semi = value.size();
        const std::wstring field = value.substr(start, semi - start);
        const size_t eq = field.find(L'=');
        if (eq != std::wstring::npos && eq > 0 && IsOwnedGpuField(field.substr(0, eq)) == owned) {
            out += field;
            out += L';';
        }
        if (semi >= value.size()) break;
        start = semi + 1;
    }
    return out;
}

// Every field of `value` this feature does NOT own, each written back as "name=value;".
inline std::wstring OtherGpuPreferenceFields(const std::wstring& value) {
    return GpuPreferenceFieldsOwned(value, false);
}

// THE GPU CHOICE A VALUE HOLDS, AS TEXT TWO READS CAN COMPARE: its SpecificAdapter and GpuPreference fields, in order and
// exactly as written - empty for no value AND for a value with neither field, and UnreadableChoiceKey() for a value that
// is there and could not be read. Windows' own other fields (AppStatus, AutoHDREnable, ...) are not part of it.
//
// 🔴 A VALUE WITH NO GPU FIELD IS NOT A GPU CHOICE (Council review, v0.5.6). Windows writes values such as
// "AppStatus=1;AutoHDREnable=2097;" for an executable it has only seen; counting one as an assignment hid a main-GPU pin
// lost to an auto-update (GpuChoicePairs). And a change to Windows' own fields is not a change of GPU, so it never
// refuses an Apply (PrepareVerdict, gpu_edit.h). "GpuPreference=0;" - "Let Windows decide" - is a GPU field, so it is part
// of this text and a change to or from it refuses a row, although it names no card and GpuChoicePairs leaves it out.
inline std::wstring GpuChoiceText(bool present, const std::wstring& value, bool unreadable) {
    if (unreadable) return UnreadableChoiceKey();
    return present ? GpuPreferenceFieldsOwned(value, true) : std::wstring();
}

// What Apply writes: this feature's two fields for `adapterKey`, then everything else `existing`
// already carried. An absent value gives exactly FormatPreferenceValue, and re-applying is
// idempotent - the old choice is replaced, never duplicated.
inline std::wstring MergeGpuPreferenceValue(const std::wstring& existing,
                                            const std::wstring& adapterKey) {
    return FormatPreferenceValue(adapterKey) + OtherGpuPreferenceFields(existing);
}

// What Remove leaves: everything EXCEPT this feature's two fields. Empty means nothing else was
// there, and only then is the whole value deleted.
inline std::wstring StripGpuAssignment(const std::wstring& existing) {
    return OtherGpuPreferenceFields(existing);
}

// True when `value` is nothing but well-formed "name=value;" fields: every field has a non-empty
// name and an '=', and ends in ';'. Only such a value is rebuilt by the two helpers above without
// losing or reshaping a byte, so Apply and Remove leave any other value exactly as they found it and
// name the row as not written. An empty value is editable - there is nothing in it to lose.
//
// 🔴 Found by adversarial review of v0.5.5: the helpers drop a fragment with no '=' and add a missing
// final ';', so an unfamiliar value would have been silently reshaped - or, on Remove, emptied and
// deleted.
inline bool IsEditablePreferenceValue(const std::wstring& value) {
    if (value.empty()) return true;
    // A control character cannot be carried through the restore file's quoted .reg string and read
    // back unchanged, so a value holding one is never edited (adversarial review, round 3).
    for (size_t i = 0; i < value.size(); ++i) {
        if (value[i] < 0x20 || value[i] == 0x7F) return false;
    }
    if (value[value.size() - 1] != L';') return false;
    size_t start = 0;
    while (start < value.size()) {
        const size_t semi = value.find(L';', start);   // never npos: the value ends in ';'
        const std::wstring field = value.substr(start, semi - start);
        const size_t eq = field.find(L'=');
        if (eq == std::wstring::npos || eq == 0) return false;
        start = semi + 1;
    }
    return true;
}

// ---- The restore file Apply and Remove save before their first write -------------------------

struct GpuPreferenceBefore {
    std::wstring exePath;
    bool present = false;   // false: there was no value, and restoring deletes the one now there
    std::wstring value;     // the REG_SZ text it held
};

// The opening of a version-5 .reg file for the preference key: the line regedit requires first, then the key.
inline std::wstring FormatRegRestoreHeader() {
    return L"Windows Registry Editor Version 5.00\r\n\r\n"
           L"[HKEY_CURRENT_USER\\Software\\Microsoft\\DirectX\\UserGpuPreferences]\r\n";
}

// One value as a .reg line: a present value is set to its old text, an absent one is deleted. Backslashes
// and quotes are escaped as the format requires; the line ends in CRLF.
inline std::wstring FormatRegRestoreRow(const GpuPreferenceBefore& row) {
    struct Esc {
        static std::wstring Of(const std::wstring& s) {
            std::wstring o;
            for (size_t i = 0; i < s.size(); ++i) {
                if (s[i] == L'\\' || s[i] == L'"') o += L'\\';
                o += s[i];
            }
            return o;
        }
    };
    return L"\"" + Esc::Of(row.exePath) + L"\"=" +
           (row.present ? L"\"" + Esc::Of(row.value) + L"\"" : std::wstring(L"-")) + L"\r\n";
}

// A .reg file that puts every listed value back as it was when the file was made - over any change made
// to the same value since, which is what "put back" means. Windows' regedit imports it on a double-click,
// after asking.
inline std::wstring FormatRegRestoreFile(const std::vector<GpuPreferenceBefore>& before) {
    std::wstring out = FormatRegRestoreHeader();
    for (size_t i = 0; i < before.size(); ++i) out += FormatRegRestoreRow(before[i]);
    return out;
}

// The RECORD of every value a change planned to touch, kept while the change runs: the same lines as a .reg
// file, under a heading that is not the one regedit requires - so it cannot be imported, by a double-click
// or by accident.
//
// 🔴 A PLANNED LIST MUST NEVER BE AN IMPORTABLE FILE. Found by adversarial review, round 5: the restore file
// was saved with every planned row and trimmed only after the last write, so a change stopped part-way - a
// crash, a kill, a power cut - left a .reg that would also put back rows never changed, and delete a value
// another program had created meanwhile.
//
// 🔴 AND EVERY VALUE LINE IS A COMMENT (adversarial review, round 6): the heading alone kept regedit out, so
// pasting regedit's heading on top would have imported the planned list - the round-5 blocker, one edit away. A
// line starting with ';' is a comment to regedit, so even a "repaired" copy changes nothing.
inline std::wstring FormatPendingRestoreFile(const std::vector<GpuPreferenceBefore>& planned) {
    std::wstring out =
        L"Game Optimizer - GPU preference values saved before a change. THIS IS NOT A RESTORE FILE, AND\r\n"
        L"WINDOWS WILL NOT IMPORT IT: it lists every application the change planned to touch, including any it\r\n"
        L"then left alone. The restore file beside it - the same name ending in .reg - lists only the\r\n"
        L"applications that really changed. If this file is still here, Game Optimizer stopped before the\r\n"
        L"change finished: use the .reg file, and look here only for an application it does not list.\r\n"
        L"Every value line below starts with a semicolon, so even with regedit's heading added it changes nothing.\r\n\r\n";
    for (size_t i = 0; i < planned.size(); ++i) out += L"; " + FormatRegRestoreRow(planned[i]);
    return out;
}

// True when `text` - a restore file's contents after its byte-order mark - is exactly regedit's heading followed by
// one or more COMPLETE rows as FormatRegRestoreRow writes them, each ending in CRLF, and nothing else.
//
// 🔴 A FILE THAT EXISTS IS NOT A FILE THAT IS COMPLETE (adversarial review, round 6): the window recommended opening
// any .reg that was there, and a write stopped part-way leaves one cut off inside a row.
inline bool IsCompleteRegRestoreText(const std::wstring& text) {
    const std::wstring head = FormatRegRestoreHeader();
    if (text.compare(0, head.size(), head) != 0) return false;
    struct Quoted {
        // Moves `i` past a quoted string that starts at text[i]; false when it is not one or never closes.
        static bool Skip(const std::wstring& s, size_t& i) {
            if (i >= s.size() || s[i] != L'"') return false;
            for (++i; i < s.size(); ++i) {
                if (s[i] == L'\\') {
                    if (i + 1 >= s.size()) return false;
                    ++i;
                } else if (s[i] == L'"') {
                    ++i;
                    return true;
                } else if (s[i] == L'\r' || s[i] == L'\n') {
                    return false;
                }
            }
            return false;
        }
    };
    size_t i = head.size();
    size_t rows = 0;
    while (i < text.size()) {
        if (!Quoted::Skip(text, i)) return false;
        if (i >= text.size() || text[i] != L'=') return false;
        ++i;
        if (i < text.size() && text[i] == L'-') ++i;
        else if (!Quoted::Skip(text, i)) return false;
        if (text.compare(i, 2, L"\r\n") != 0) return false;
        i += 2;
        ++rows;
    }
    return rows > 0;
}

// ---- Win32 implementations (in gpu_pref.cpp) ----
// These are NOT inline because they require Win32 headers and DXGI libraries

bool EnumerateGpuAdapters(std::vector<GpuAdapter>& out, std::wstring* error);
// false with *unreadable == false: there is no such value. false with *unreadable == true: a value
// IS there and could not be read as a string - a caller must not write over it, because a merge into
// "nothing" replaces the whole value.
bool ReadGpuPreference(const std::wstring& exePath, std::wstring& valueOut, bool* unreadable = nullptr);
bool WriteGpuPreference(const std::wstring& exePath, const std::wstring& value);
bool ClearGpuPreference(const std::wstring& exePath);
// ---- The restore file: a record kept while a change runs, and a .reg of what really changed ---------------
//
// 🔴 A RESTORE FILE MUST NOT PUT BACK WHAT WAS LEFT ALONE - NOT EVEN WHEN THE CHANGE STOPS PART-WAY. Round 4 of
// adversarial review found the file listed every planned row; round 5 found the trim that fixed it ran only
// after the last write, so a crash or a kill in between still left that list as an importable .reg. Now the
// planned list is kept only as a .pending record regedit refuses, and the .reg grows one row at a time, each
// added right after that row's change was committed.
//
// BeginGpuRestore writes FormatPendingRestoreFile(planned) to a NEW "<name>.pending" in `dir` - UTF-16 with a
// byte-order mark, flushed - and names the "<name>.reg" beside it, which does not exist yet. False: nothing may
// be written, and a partial .pending is deleted if it can be.
// RecordGpuRestoreRow adds one row: the whole .reg - its heading and every row recorded so far - is written to
// "<name>.reg.tmp", flushed, and moved over the .reg in one step. False: the .reg on disk is still exactly the
// previous complete file, or still absent, and lacks that row - change nothing more.
//
// 🔴 THE .reg IS NEVER APPENDED TO (adversarial review, round 6): an append that failed or was stopped part-way left
// a file cut off inside a row, which the next opening of the window then recommended importing.
// FinishGpuRestore deletes the .pending once every change is over and every changed row is recorded.
struct GpuRestoreJournal {
    std::wstring pendingPath;
    std::wstring regPath;
    bool regStarted = false;                    // the .reg exists and holds every row in `recorded`
    std::vector<GpuPreferenceBefore> recorded;  // the rows the .reg holds, in order
};
bool BeginGpuRestore(const std::wstring& dir, const std::vector<GpuPreferenceBefore>& planned,
                     GpuRestoreJournal& journal);
bool RecordGpuRestoreRow(GpuRestoreJournal& journal, const GpuPreferenceBefore& row);
bool FinishGpuRestore(const GpuRestoreJournal& journal);
// The .pending records in `dir` left by changes that stopped before they finished: full paths, oldest first.
std::vector<std::wstring> UnfinishedGpuRestores(const std::wstring& dir);
// A restore file's text without its byte-order mark. False when it cannot be read or is not UTF-16 with a mark.
bool ReadRestoreFileText(const std::wstring& path, std::wstring& textOut);

// ---- A change that cannot replace another program's change -------------------------------------------
enum class GuardedWriteResult {
    Written,          // committed: the value now holds exactly what was asked
    Changed,          // it was no longer what the caller read, so nothing was written
    CheckUnreadable,  // it could not be read again to compare, so nothing was written
    NotCommitted,     // Windows did not commit it - error 6704 when another writer changed it meanwhile
    Unavailable,      // a registry transaction could not be started, so nothing was written
    Failed            // the change itself was refused, so nothing was written
};

// Called between the steps of a guarded write, for the write probe only: stage 1 after the change is made
// inside the transaction, stage 2 after the comparison and before the commit. The product passes none.
typedef void (*GuardedWriteHook)(int stage, void* context);

// Sets `exePath`'s value to `newValue`, or deletes it when `deleteValue`, ONLY if it is still exactly what
// the caller read (`expectPresent`, `expectValue`).
//
// 🔴 THE ORDER IS THE GUARANTEE. The change is made inside a registry transaction; the committed value is
// then read OUTSIDE the transaction and compared; only then is it committed. A change another writer made
// before ours is seen by that comparison; one made after ours makes the commit fail. [M] txprobe2, 10 of 10
// interleavings - set, delete and create - the other writer's value survived every one. Read-then-write
// without this lost it (round 2 to round 4 of adversarial review). A plain transaction around the read did
// not help either: [M] txprobe, a write between the transacted read and write was lost and the commit
// still succeeded.
// `*error` (optional) receives the Windows error code behind Unavailable, Failed and NotCommitted, else 0.
// ponytail: the other writer in the probes was a second handle in the same process, one step at a time.
GuardedWriteResult GuardedWriteGpuPreference(const std::wstring& exePath, bool expectPresent,
                                             const std::wstring& expectValue, bool deleteValue,
                                             const std::wstring& newValue, unsigned long* error = nullptr,
                                             GuardedWriteHook hook = nullptr, void* hookContext = nullptr);

// EVERY preference the registry holds, as (full exe path, PreferenceChoiceKey) pairs: an adapter
// key, a Windows-setting key, or empty for no choice - and an entry with no choice is still
// returned. `*complete` (optional) is set false when the walk could not see everything: the key
// could not be opened, an entry was too large to read, or the walk stopped on an error. The caller
// then has a partial list and must say so rather than present it as the whole picture.
//
// 🔴 IT REPLACES FindOrphanedPreferencePaths, WHICH COULD NOT FEED THE DETECTOR IT WAS
// WRITTEN FOR. That function returned value NAMES only - it passed nullptr for the data
// buffers - while FindOrphanedAssignments (gpu_rows.h) takes (path, key) PAIRS. It also
// pre-filtered to paths that no longer exist, so it could not supply the live-path side of
// the comparison either, and it BROKE its enumeration loop on ERROR_MORE_DATA, silently
// dropping every entry after the first over-long name - which is precisely the deep
// versioned-folder path this feature exists to notice.
//
// This one pass is also what populates every dialog row's assignedKey, so there is no second
// registry walk anywhere in the feature.
std::vector<std::pair<std::wstring, std::wstring> > EnumerateGpuPreferences(bool* complete = nullptr);

// One UserGpuPreferences value, as a walk of the key read it.
struct GpuPreferenceEntry {
    std::wstring path;         // the full exe path the value is named by
    std::wstring choiceKey;    // PreferenceChoiceKey of its text, or UnreadableChoiceKey
    std::wstring choiceText;   // GpuChoiceText of its text: its GPU fields as written, empty, or UnreadableChoiceKey
};

inline GpuPreferenceEntry MakeGpuPreferenceEntry(const std::wstring& path, const std::wstring& value, bool unreadable) {
    GpuPreferenceEntry e;
    e.path = path;
    e.choiceKey = unreadable ? UnreadableChoiceKey() : PreferenceChoiceKey(value);
    e.choiceText = GpuChoiceText(true, value, unreadable);
    return e;
}

// The same walk as EnumerateGpuPreferences, with each value's GPU fields kept: what the GPU Assignment tab reads.
std::vector<GpuPreferenceEntry> EnumerateGpuPreferenceEntries(bool* complete = nullptr);

// WHAT THE GPU ASSIGNMENT TAB WORKS FROM: (path, choice key) for every entry that names a GPU choice by key - a card, one of
// Windows' two modes, or a value that could not be read, which is never taken for no choice. An entry with no key - only
// Windows' own fields, or a GpuPreference that names no card - is left out, so it reads exactly as a path with no value: in
// the rows' assignedKey, in ChooseBackgroundKey's votes, and - the reason this exists - in FindOrphanedAssignments, where
// such a value at an updated app's new path hid the pin its old path lost (Council review, v0.5.6; AJ42).
//
// 🔴 "GpuPreference=0" IS LEFT OUT TOO (Council round 2, v0.5.6). It used to be kept here as a choice, while every other
// reader - PreferenceChoiceKey, Auto assign, Apply's replacing count - read it as none; so at an updated app's new path it
// hid a lost main-GPU pin, and Auto assign ticked that app for the background GPU. [M] Of the 59 values in the operator's
// key, the one holding GpuPreference=0 carries SwapEffectUpgradeEnable beside it and no card - a field of Windows' own
// graphics options, not a GPU pick [A: that the user never picked "Let Windows decide" there is inferred, not measured].
// Apply still compares it: GpuChoiceText keeps it, so a change to or from it refuses a row (PrepareVerdict, AJ41).
inline std::vector<std::pair<std::wstring, std::wstring> > GpuChoicePairs(const std::vector<GpuPreferenceEntry>& entries) {
    std::vector<std::pair<std::wstring, std::wstring> > out;
    for (size_t i = 0; i < entries.size(); ++i)
        if (!entries[i].choiceKey.empty()) out.push_back(std::make_pair(entries[i].path, entries[i].choiceKey));
    return out;
}

}  // namespace cd
