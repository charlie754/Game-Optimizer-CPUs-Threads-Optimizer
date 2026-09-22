// Game Optimizer - which GPU CUDA uses for one application (v0.5.8).
//
// WHY THIS FILE EXISTS. Windows' per-application GPU preference decides which adapter an application
// DRAWS on. It says nothing about CUDA: a program that opens a CUDA context still sees every NVIDIA
// card, so moving a browser to the background GPU left its CUDA work on the card the game runs on.
// NVIDIA keeps that choice somewhere else entirely - in its own driver profile database, as setting
// CUDA_EXCLUDED_GPUS_ID (0x10354FF8), a wide string LISTING THE GPUs TO EXCLUDE from CUDA for one
// application. [M] 2026-09-19: the CUDA driver obeys
// it per application on the next launch, and writing it needs no administrator rights.
//
// 🔴 THE ROOT THREE COUNCIL ROUNDS FOUND, AND WHAT v0.5.8 THEREFORE IS (founder decision 22).
// NVIDIA's CUDA setting belongs to a PROFILE, and one profile can cover MANY applications - [M] this
// machine's "Microsoft Edge Beta" profile carries six under one 0x10354FF8 value. Game Optimizer assigns
// per APPLICATION, because Windows' own preference is per executable path. THE TWO MODELS DO NOT MAP:
// two members of one NVIDIA profile cannot be given different CUDA GPUs, and three rounds of ever more
// careful record-keeping were attempts to paper over that. They could not, and the blockers kept coming
// back in a new corner of the same domain.
//
// So THIS PRODUCT ONLY WRITES CUDA WHERE IT OWNS THE PROFILE EXCLUSIVELY (E1): a profile it created
// itself, named after the executable, covering that one executable and nothing else. Everything else -
// NVIDIA's own entry for Chrome, Edge or Claude, a profile the user made, a profile with a second
// member, a membership the driver would not finish listing, or -167 - IS NOT WRITTEN. The row is
// skipped, the Windows GPU assignment still happens, and the result names the entry, says how many
// other programs it covers, and sends the user to NVIDIA Control Panel, which is where they set those
// today anyway.
//
// AND THE RECORD COLLAPSES WITH IT (E2). One line per application IS one line per profile, and the
// state before this product touched anything is ALWAYS "there was no profile at all". There is no
// original value to keep, no ownership flag to prove and no foreign profile to fail closed on. The undo
// is not "write the old value back" - it is "take away the entry we made" (E3).
//
// EVERY PURE DECISION IS HERE, exactly as gpu_rows.h and gpu_policy.h keep theirs, so the unit suite
// reaches all of it with no driver on the machine. The one thing that is NOT here is the driver
// itself: CudaOps is a bundle of function objects, so the panel regressions drive Apply and Remove
// end to end against a fake that never opens nvapi64.dll.
//
// WHAT THIS DELIBERATELY REFUSES TO DO:
//   * It never touches CUDA for a target that is not an NVIDIA card - an AMD or Intel GPU, or one of
//     Windows' own power/performance modes. Founder decision 18, verbatim: "ignore all non-nvidia GPU
//     by our CUDA config".
//   * It never SYNTHESISES a GPU id string. The tail of one encodes architecture, revision and memory
//     size, it is undocumented, and it has changed between driver generations [S]. The string is copied
//     verbatim from what the driver itself enumerates, or nothing is written.
//   * It refuses to exclude MORE THAN ONE GPU. [A] The separator between two ids inside that one string
//     is unknown and has never been measured, and a guess would be a silently wrong driver setting.
//   * It never fails an Apply. Everything there happens AFTER the GPU preference was written; a refusal
//     costs one sentence in the result and nothing else.
#pragma once
#include <algorithm>
#include <functional>
#include <string>
#include <vector>

#include "gpu_pref.h"
#include "util.h"

namespace cd {

// NVIDIA's own "nothing is excluded" value: CUDA_EXCLUDED_GPUS_NONE in NvApiDriverSettings.h, which is
// also the setting's default. Written when the target is the only NVIDIA card there is - CUDA may then
// use every GPU, and a stale exclusion left by an earlier assignment is undone rather than kept.
inline std::wstring CudaNoneValue() { return L"none"; }

// NVIDIA's own id for the one setting this product ever writes: CUDA_EXCLUDED_GPUS_ID in
// NvApiDriverSettings.h.
// 🔴 R6-1: IT IS DECLARED HERE, NOT ONLY IN gpu_cuda.cpp, BECAUSE THE DECISION THAT NEEDS IT IS A
// DECISION. "Are this entry's own settings exactly ours?" decides whether a whole NVIDIA profile is
// deleted, so it belongs where the unit suite reaches it, and gpu_cuda.cpp reports what the driver
// enumerated rather than ruling on it.
inline unsigned long CudaSettingId() { return 0x10354FF8ul; }

// ---- The driver's own name for one GPU -------------------------------------------------------
//
// [M] The string looks like: id,2.0:2B8510DE,00000100,GF - (432,2,161,32607) @ (0)
// Only two of its fields join to anything this product knows:
//   field 1, between ':' and the next ',', is NvAPI_GPU_GetPCIIdentifiers' deviceId;
//   field 2, shifted right by 8, is NvAPI_GPU_GetBusId.
// Everything after that is the driver's business and is carried, never parsed and never rebuilt.
struct UniversalGpuId {
    unsigned long deviceId = 0;
    unsigned long busId = 0;
    bool ok = false;         // false: not a GPU id string at all - "autoselect" and "none" land here
};

// Hexadecimal, at most eight digits, nothing else accepted. A field with a stray character is not a
// number we may join on, so the whole string is refused rather than half-read.
inline bool CudaParseHex(const std::wstring& s, unsigned long& out) {
    if (s.empty() || s.size() > 8) return false;
    unsigned long v = 0;
    for (size_t i = 0; i < s.size(); ++i) {
        const wchar_t c = s[i];
        unsigned long d;
        if (c >= L'0' && c <= L'9') d = static_cast<unsigned long>(c - L'0');
        else if (c >= L'a' && c <= L'f') d = 10ul + static_cast<unsigned long>(c - L'a');
        else if (c >= L'A' && c <= L'F') d = 10ul + static_cast<unsigned long>(c - L'A');
        else return false;
        v = (v << 4) | d;
    }
    out = v;
    return true;
}

inline UniversalGpuId ParseUniversalGpuId(const std::wstring& text) {
    UniversalGpuId id;
    const size_t colon = text.find(L':');
    if (colon == std::wstring::npos) return id;
    const size_t first = text.find(L',', colon + 1);
    if (first == std::wstring::npos) return id;
    const size_t second = text.find(L',', first + 1);
    if (second == std::wstring::npos) return id;
    unsigned long f1 = 0, f2 = 0;
    if (!CudaParseHex(text.substr(colon + 1, first - colon - 1), f1)) return id;
    if (!CudaParseHex(text.substr(first + 1, second - first - 1), f2)) return id;
    id.deviceId = f1;
    id.busId = f2 >> 8;
    id.ok = true;
    return id;
}

// ---- One NVIDIA GPU, in the two terms the join needs -----------------------------------------
//
// 🔴 THE KEY CARRIES THE DEVICE ID, WHICH IS WHY THIS STRUCT HAS ONLY TWO FIELDS. This product's
// adapter key is "VVVV&DDDD&SSSSSSSS" (gpu_pref.h), and NVAPI rebuilds exactly that as
// "%04X&%04X&%08X" of (deviceId & 0xFFFF, deviceId >> 16, subSystemId) [M] nvmap. So the deviceId an
// id string carries is (device << 16) | vendor, read back out of the key itself - there is no third
// number to keep in step.
struct NvidiaGpu {
    std::wstring adapterKey;    // as AdapterKeyFromPnpId writes it: UPPERCASE, "VVVV&DDDD&SSSSSSSS"
    unsigned long busId = 0;    // NvAPI_GPU_GetBusId
};

inline bool DeviceIdFromAdapterKey(const std::wstring& key, unsigned long& out) {
    if (key.size() != 18 || key[4] != L'&' || key[9] != L'&') return false;
    unsigned long vendor = 0, device = 0;
    if (!CudaParseHex(key.substr(0, 4), vendor)) return false;
    if (!CudaParseHex(key.substr(5, 4), device)) return false;
    out = (device << 16) | vendor;
    return true;
}

// True only for a card whose PCI vendor is NVIDIA's 0x10DE. Windows' two mode keys and
// UnreadableChoiceKey are not adapter keys at all and answer false, as does an empty key.
inline bool IsNvidiaAdapterKey(const std::wstring& key) {
    unsigned long id = 0;
    return DeviceIdFromAdapterKey(key, id) && (id & 0xFFFFul) == 0x10DEul;
}

// ---- What a read of the CUDA setting answered -------------------------------------------------
//
// 🔴 THREE ANSWERS, NEVER TWO. A read that FAILED is not "there was none". This product has shipped
// that exact confusion before, in the registry half: a value longer than the read buffer came back as
// absent, and merging over "absent" overwrote the whole thing. Here the cost
// is worse, because the mistake is recorded: Apply would write a line claiming the driver holds what it
// does not, and Remove assignment would then take away an entry on that belief. Failed refuses the row.
enum class CudaRead { Value, Absent, Failed };

// ---- What a lookup of the application's own NVIDIA profile answered ---------------------------
//
// 🔴 THREE ANSWERS HERE TOO, FOR THE SAME REASON ONE LEVEL UP (Council round 2, F13). CudaRead keeps a
// failed read of the SETTING from being written over. This keeps a failed lookup of the PROFILE from
// being read as "NVIDIA has no settings entry for this application yet" - which is worse, because that
// answer does not stop the row, it MAKES a new profile: the application ends up governed by an entry
// nobody compared with the one the driver may already have had. Absent is the driver saying so, and
// nothing else is ([M] -166 NVAPI_EXECUTABLE_NOT_FOUND); every other unhappy answer is Failed, and
// Failed never writes.
enum class CudaLookup { Found, Absent, Failed };

// ---- Why nothing was written -----------------------------------------------------------------
//
// None means "there was nothing to do", which is NOT a refusal and is never said. Every other value
// is one sentence in the result dialog and nothing else: on Apply the GPU preference this followed has
// already been written and no refusal here may undo or fail it; on Remove the GPU preference is left
// exactly where it is, so the user can try the whole thing again (E4).
enum class CudaRefusal {
    None,
    NoNvidiaDriver,            // nvapi64.dll is absent, or it enumerated no GPU
    NoNvidiaGpuForKey,         // the driver does not list a card with that adapter key
    AmbiguousIdenticalCards,   // two cards share the key, so which one to exclude cannot be said
    NoIdForGpu,                // the driver enumerated no id string for that card
    SeveralIdsForGpu,          // more than one id string matched it: the join is not unique
    SeveralToExclude,          // more than one GPU would have to be excluded - separator unmeasured
    SessionRefused,            // the driver's settings could not be opened
    // 🔴 E1, AND THE WHOLE POINT OF v0.5.8. NVIDIA - or the user - already keeps this application's
    // settings in an entry this product does not own, so the entry is left alone. Its sentence is the
    // ONE that has to name the profile and count its other programs, so it is built by
    // FormatCudaNvidiaManagesLine rather than fetched from CudaRefusalReason.
    NvidiaManagesIt,
    NameAlreadyInUse,          // -167: another profile claims that executable's base name
    ProfileNameTaken,          // an entry already carries the name this uses, and no line of ours claims it
    ProfileNotCreated,         // a profile for it could not be made
    // 🔴 R5-9: THIS DRIVER OFFERS NO WAY TO TAKE A PROFILE AWAY AGAIN, so none is made in the first place.
    // Asked in the PLAN, before any create call: a profile made on a driver that cannot delete one is a
    // profile nothing in this product could ever remove, and Remove assignment would meet it forever.
    NoWayToUndo,
    // 🔴 THE SAME RULE, FOR THE OTHER HALF OF AN UNDO (pre-publish review of v0.5.9). Remove assignment
    // takes an entry of ours away only after listing which settings it holds (R6-1), so a driver that does
    // not offer NvAPI_DRS_EnumSettings makes an entry Remove refuses forever (SettingsUnknown). It is asked
    // where NoWayToUndo is, in the plan, before any create call - with a sentence that says which call. And
    // unlike NoWayToUndo it refuses an entry that ALREADY EXISTS as well: Remove could not undo a write on it
    // either, while a missing delete still leaves an existing entry's setting clearable (AK25b).
    NoWayToListSettings,
    SettingNotWritten,         // the driver refused the value
    // 🔴 R5-1: Remove, on an entry carrying OTHER settings a user put there: only our own CUDA setting is
    // taken out and the entry is left standing - and the driver refused even that.
    SettingNotRemoved,
    // 🔴 R6-1: THE DRIVER WOULD NOT LIST WHICH SETTINGS THAT ENTRY HOLDS. Round 5 read numOfSettings - a
    // COUNT - as an IDENTITY: "one setting" was taken to mean "the one setting is ours". A count says
    // nothing about WHICH, exactly as round 4 proved for applications, so the ids are enumerated instead
    // and an enumeration nobody could make is never guessed at.
    SettingsUnknown,
    // 🔴 R6-1: THE ENUMERATION AND THE READ DISAGREE. NvAPI_DRS_GetSetting can answer with a value the
    // profile INHERITS from the global profile, so a value coming back is not proof the setting is that
    // profile's own. When the enumeration does not list 0x10354FF8 among the entry's own settings while
    // the read just returned what this product wrote, the value is not local - and taking the entry away
    // over it would destroy an application entry, and any other setting on it, for nothing.
    SettingNotOurs,
    NotSaved,                  // the driver would not save
    CouldNotRead,              // the setting that is there now could not be read - never "there was none"
    CouldNotLookUp,            // the profile it already has could not be looked up - never "it has none"
    MembershipUnknown,         // the driver would not list every application that profile covers
    RecordUnreadable,          // the note of what this product changed could not be read, or is an older format
    RecordUnusable,            // that note holds something that is not a CUDA setting
    RecordedProfileChanged,    // Remove: NVIDIA now answers with a different entry than the line names
    ChangedSinceWritten,       // it no longer holds what this product wrote, so it is left alone
    ProfileNotRemoved,         // Remove: the entry this product made could not be taken away again
    NotRecorded,               // the note could not be written, so the change was not left behind
    NotConfirmed,              // the driver saved, and the change did not read back
    NotUndone                  // 🔴 a change this product made could not be taken back out again (E5)
};

// The words the result dialog uses, after "<application> - ". Empty for None, and empty for
// NvidiaManagesIt, whose sentence needs the profile and the count (CudaRowRefusalText).
inline std::wstring CudaRefusalReason(CudaRefusal r) {
    switch (r) {
        case CudaRefusal::NoNvidiaDriver:
            return L"this computer has no NVIDIA driver to ask";
        case CudaRefusal::NoNvidiaGpuForKey:
            return L"NVIDIA's driver does not list that card";
        case CudaRefusal::AmbiguousIdenticalCards:
            return L"two NVIDIA cards report the same id, so they cannot be told apart";
        case CudaRefusal::NoIdForGpu:
            return L"NVIDIA's driver gave no name for the other card";
        case CudaRefusal::SeveralIdsForGpu:
            return L"NVIDIA's driver gave more than one name for the other card";
        case CudaRefusal::SeveralToExclude:
            return L"there are more than two NVIDIA cards, which this cannot set yet";
        case CudaRefusal::SessionRefused:
            return L"NVIDIA's own settings could not be opened";
        case CudaRefusal::NameAlreadyInUse:
            // 🔴 R5-7: THE FALLBACK, FOR WHEN THE ENTRY COULD NOT BE NAMED. The named form is built by
            // FormatCudaNameInUseLine, which this sentence is the honest "I looked and could not" half of.
            return L"NVIDIA already keeps a settings entry for a program with that file name and that entry "
                   L"could not be named, so set which GPU CUDA uses for it in NVIDIA Control Panel";
        case CudaRefusal::ProfileNameTaken:
            return L"an NVIDIA settings entry already carries the name Game Optimizer gives its own, and "
                   L"Game Optimizer has no record of making it";
        case CudaRefusal::ProfileNotCreated:
            return L"an NVIDIA settings entry for it could not be made";
        case CudaRefusal::NoWayToUndo:
            return L"this NVIDIA driver offers no way to take a settings entry away again, so Game Optimizer "
                   L"made none for it";
        case CudaRefusal::NoWayToListSettings:
            return L"this NVIDIA driver offers no way to list which settings an entry holds, so Game Optimizer "
                   L"could never safely take one away again and made none for it";
        case CudaRefusal::SettingNotWritten:
            return L"NVIDIA refused the change";
        case CudaRefusal::SettingNotRemoved:
            return L"the NVIDIA settings entry for it holds other settings too, and NVIDIA refused to take "
                   L"Game Optimizer's own CUDA setting out of it";
        case CudaRefusal::SettingsUnknown:
            return L"NVIDIA's driver would not list which settings its entry for it holds, so Game Optimizer "
                   L"could not tell whether taking that entry away would take somebody else's settings too";
        case CudaRefusal::SettingNotOurs:
            return L"NVIDIA's settings entry for it does not hold Game Optimizer's own CUDA setting of its "
                   L"own - the value read back comes from NVIDIA's general settings - so the entry was left "
                   L"exactly as it is";
        case CudaRefusal::NotSaved:
            return L"NVIDIA's own settings could not be saved";
        case CudaRefusal::CouldNotRead:
            return L"NVIDIA's driver would not say which GPU CUDA uses for it now";
        case CudaRefusal::CouldNotLookUp:
            return L"NVIDIA's driver would not say whether it already has settings of its own for it";
        case CudaRefusal::MembershipUnknown:
            // 🔴 R5-7 again: the un-named half. FormatCudaMembershipUnknownLine names the entry when the
            // lookup that refused could say which one it was.
            return L"NVIDIA's driver would not list every program that shares its settings";
        case CudaRefusal::RecordUnreadable:
            return L"the note of what Game Optimizer changed could not be read";
        case CudaRefusal::RecordUnusable:
            return L"the note of what Game Optimizer changed is damaged";
        case CudaRefusal::RecordedProfileChanged:
            return L"NVIDIA now keeps its settings under a different entry than the note names";
        case CudaRefusal::ChangedSinceWritten:
            return L"something else has changed which GPU CUDA uses for it since, so it was left alone";
        case CudaRefusal::ProfileNotRemoved:
            return L"the NVIDIA settings entry Game Optimizer made for it could not be taken away again";
        case CudaRefusal::NotRecorded:
            return L"the note Game Optimizer keeps for it could not be written, so which GPU CUDA uses was "
                   L"left as it was";
        case CudaRefusal::NotConfirmed:
            return L"NVIDIA saved the change, and it did not read back";
        case CudaRefusal::NotUndone:
            return L"which GPU CUDA uses was changed for it, and the change could not be taken back out";
        default:
            return std::wstring();
    }
}

// ---- Matching one adapter key to the driver's id string ---------------------------------------
struct CudaMatch {
    std::wstring id;                              // copied VERBATIM from the driver, never built
    CudaRefusal refusal = CudaRefusal::None;
};

// `ids` is every value NvAPI_DRS_EnumAvailableSettingValues(0x20D0F3E6) enumerated, in its order.
// 0x10354FF8 itself enumerates only "none" [M], which is why the OpenGL setting is the source.
inline CudaMatch MatchUniversalId(const std::wstring& adapterKey, const std::vector<NvidiaGpu>& gpus,
                                  const std::vector<std::wstring>& ids) {
    CudaMatch m;
    if (gpus.empty()) {
        m.refusal = CudaRefusal::NoNvidiaDriver;
        return m;
    }
    size_t found = 0, at = 0;
    for (size_t i = 0; i < gpus.size(); ++i) {
        if (gpus[i].adapterKey != adapterKey) continue;
        ++found;
        at = i;
    }
    if (found == 0) {
        m.refusal = CudaRefusal::NoNvidiaGpuForKey;
        return m;
    }
    if (found > 1) {
        // 🔴 TWO PHYSICALLY IDENTICAL CARDS COLLIDE ON THIS KEY, exactly as PlanGpuIsolation already
        // refuses them: the key has no field that separates them, so excluding "the one with this key"
        // is not a thing that can be said.
        m.refusal = CudaRefusal::AmbiguousIdenticalCards;
        return m;
    }
    unsigned long deviceId = 0;
    if (!DeviceIdFromAdapterKey(adapterKey, deviceId)) {
        m.refusal = CudaRefusal::NoNvidiaGpuForKey;
        return m;
    }
    size_t hits = 0, hit = 0;
    for (size_t i = 0; i < ids.size(); ++i) {
        const UniversalGpuId parsed = ParseUniversalGpuId(ids[i]);
        if (!parsed.ok || parsed.deviceId != deviceId || parsed.busId != gpus[at].busId) continue;
        ++hits;
        hit = i;
    }
    if (hits == 0) {
        m.refusal = CudaRefusal::NoIdForGpu;
        return m;
    }
    if (hits > 1) {
        m.refusal = CudaRefusal::SeveralIdsForGpu;
        return m;
    }
    m.id = ids[hit];
    return m;
}

// ---- Which GPUs a target implies must be excluded ---------------------------------------------
struct CudaPlan {
    bool act = false;                        // false: nothing CUDA-related happens, and usually nothing is said
    CudaRefusal refusal = CudaRefusal::None; // said when it is not None, whatever `act` says
    std::vector<std::wstring> excludeKeys;   // every OTHER NVIDIA card's adapter key, each once
};

// 🔴 A NON-NVIDIA TARGET IS SILENCE, NOT A REFUSAL (founder decision 18). An application assigned to the
// AMD integrated GPU, or to one of Windows' own modes, has its CUDA setting left exactly as it is and
// nothing is said about it: this product has no opinion about compute on a card CUDA cannot use.
// A target that IS an NVIDIA card and that the driver cannot account for is a different thing, and it
// IS said - silence there would hide a feature that quietly stopped working.
inline CudaPlan CudaPlanFor(const std::wstring& targetAdapterKey, const std::vector<NvidiaGpu>& gpus) {
    CudaPlan plan;
    if (!IsNvidiaAdapterKey(targetAdapterKey)) return plan;   // empty, a Windows mode, AMD, Intel: do nothing
    if (gpus.empty()) {
        plan.refusal = CudaRefusal::NoNvidiaDriver;
        return plan;
    }
    size_t listed = 0;
    for (size_t i = 0; i < gpus.size(); ++i)
        if (gpus[i].adapterKey == targetAdapterKey) ++listed;
    if (listed == 0) {
        plan.refusal = CudaRefusal::NoNvidiaGpuForKey;
        return plan;
    }
    if (listed > 1) {
        // 🔴 TWO IDENTICAL CARDS ARE NOT A "NOTHING TO EXCLUDE" PLAN. The loop below skips every GPU
        // carrying the target key, so a pair of them would leave excludeKeys empty and write NVIDIA's
        // "nothing is excluded" - the opposite of what the user asked for, silently. The key has no field
        // that separates two of the same card, so which one to keep cannot be said, and it is refused
        // exactly as MatchUniversalId already refuses the same pair on the other side.
        plan.refusal = CudaRefusal::AmbiguousIdenticalCards;
        return plan;
    }
    for (size_t i = 0; i < gpus.size(); ++i) {
        const std::wstring& k = gpus[i].adapterKey;
        if (k == targetAdapterKey) continue;
        if (std::find(plan.excludeKeys.begin(), plan.excludeKeys.end(), k) == plan.excludeKeys.end())
            plan.excludeKeys.push_back(k);
    }
    if (plan.excludeKeys.size() > 1) {
        // [A] The separator between two ids inside the one wide string is unknown and untested, so a
        // three-card machine is refused rather than guessed at (ledger, "Not in scope").
        plan.refusal = CudaRefusal::SeveralToExclude;
        return plan;
    }
    plan.act = true;
    return plan;
}

// ---- The one value a target implies, ready to write -------------------------------------------
struct CudaTarget {
    bool act = false;                         // false: write nothing for any row
    CudaRefusal refusal = CudaRefusal::None;  // said once, for the whole run, when it is not None
    std::wstring value;                       // what 0x10354FF8 must hold: one driver id, or "none"
};

inline CudaTarget CudaTargetFor(const std::wstring& targetAdapterKey, const std::vector<NvidiaGpu>& gpus,
                                const std::vector<std::wstring>& ids) {
    CudaTarget t;
    const CudaPlan plan = CudaPlanFor(targetAdapterKey, gpus);
    if (!plan.act) {
        t.refusal = plan.refusal;
        return t;
    }
    if (plan.excludeKeys.empty()) {
        // The target is the only NVIDIA card here: CUDA may use everything, which is also what undoes an
        // exclusion an earlier assignment to another card left behind.
        t.act = true;
        t.value = CudaNoneValue();
        return t;
    }
    const CudaMatch m = MatchUniversalId(plan.excludeKeys[0], gpus, ids);
    if (m.refusal != CudaRefusal::None) {
        t.refusal = m.refusal;
        return t;
    }
    t.act = true;
    t.value = m.id;
    return t;
}

// ---- Everything that touches the driver, passed in --------------------------------------------
//
// One NVIDIA profile, as the driver answers for an executable. `appEntry` is the application entry
// that matched - [M] either the full path in lower case with forward slashes, or a bare file name
// that matches that file at any path. Only the first form is ever one of ours (CudaAppKeyFor).
// 🔴 THERE IS NO `found` FIELD, AND ITS ABSENCE IS THE POINT (Council round 2, F13). Whether the driver
// has an entry at all is the LOOKUP's answer (CudaLookup), not a flag on the thing looked up: a bool here
// reads false for "NVIDIA has none" and false again for "nobody could find out", which is exactly the
// confusion this file removed from the setting read one level down.
struct CudaProfile {
    std::wstring profileName;
    std::wstring appEntry;
    bool isPredefined = false;                // NVIDIA shipped the profile itself
    // 🔴 ONLY EVER SET BY createProfileForExe, AND ONLY WHEN THE CREATE CALL REALLY CREATED ONE.
    // "It is called Game Optimizer - ..." is a claim about a string, not proof that this product made it.
    bool createdNow = false;
    // 🔴 THE COUNT OF OTHER APPLICATIONS THE PROFILE COVERS - AND E1 ONLY EVER WRITES ON ZERO. It used to
    // be a LIST, because a shared profile's neighbours had to be named in the question before the user
    // agreed to move their CUDA setting too. v0.5.8 never moves them, so the only thing left to say is
    // how many programs the entry it is REFUSING covers.
    size_t otherApps = 0;
    // 🔴 FALSE MEANS "THAT COUNT IS NOT THE WHOLE COUNT", and the row is then refused rather than written.
    // The enumeration can stop short two ways - a page the driver refuses, and this reader's own cap - and
    // neither is distinguishable from "that is all of them" unless it is carried out.
    bool appsComplete = true;
    // NVDRS_PROFILE::numOfSettings, as the driver states it.
    // 🔴 R6-1: IT IS REPORTED AND NOTHING IS DECIDED ON IT, AND THAT IS DELIBERATE. Round 5 branched on
    // this number - "more than one setting means somebody else's is in here too" - which is a COUNT
    // standing in for an IDENTITY, the exact mistake round 4 had already found for applications: one
    // setting says nothing about WHICH setting. Whether an entry may be taken away is decided by
    // enumerating the ids (CudaOps::listSettingIds, JudgeCudaSettings). Leave this a diagnostic; do not
    // branch on it again.
    size_t numSettings = 0;
};

// ---- R6-1: WHICH SETTINGS AN ENTRY HOLDS OF ITS OWN, AND WHAT MAY BE DONE ABOUT THEM ----------
//
// 🔴 A COUNT IS NOT AN IDENTITY, AND THIS IS THE SECOND TIME THAT SENTENCE HAS HAD TO BE WRITTEN.
// Round 4 proved it for APPLICATIONS: "this profile covers one application" says nothing about WHICH, so
// a profile that lost our entry and gained somebody else's passed the count. Round 5 made exactly the
// same mistake one level down, for SETTINGS: NVDRS_PROFILE::numOfSettings == 1 was read as "the one
// setting is ours", so an entry of ours carrying the user's "Vertical sync" and NO CUDA setting of its
// own was deleted whole. The ids are enumerated now and the decision is taken on the ids.
//
// 🔴 AND ZERO IS NOT A SAFE ANSWER EITHER. NvAPI_DRS_GetSetting answers from the profile when the profile
// has the setting and from NVIDIA's GENERAL settings when it does not, so a value coming back is not
// proof that the setting is that entry's own. An enumeration that lists no 0x10354FF8 while the read just
// returned what this product wrote is a contradiction between two driver answers, and the entry is left
// exactly as it is rather than deleted on the weaker of them.
enum class CudaSettingsVerdict {
    OnlyOurs,    // the entry's own settings are exactly {CUDA_EXCLUDED_GPUS_ID}: it may be taken away whole
    AlsoOthers,  // ours AND settings this product did not write: only ours comes out, the entry stays
    NotOurs      // ours is not among them at all - including the empty enumeration. Change nothing.
};

inline CudaSettingsVerdict JudgeCudaSettings(const std::vector<unsigned long>& ids) {
    bool ours = false, others = false;
    for (size_t i = 0; i < ids.size(); ++i) {
        if (ids[i] == CudaSettingId()) ours = true;
        else others = true;
    }
    if (!ours) return CudaSettingsVerdict::NotOurs;   // an EMPTY enumeration lands here, which is the point
    return others ? CudaSettingsVerdict::AlsoOthers : CudaSettingsVerdict::OnlyOurs;
}

// ---- R6-6: WHETHER AN APPLICATION ENUMERATION CAN BE JUDGED AT ALL ----------------------------
//
// 🔴 THE COUNT AND THE NAMES USED TO BE MEASURED DIFFERENTLY, AND THAT IS THE DEFECT. gpu_cuda.cpp
// totalled the RAW rows the driver handed back to decide "did we see all of them", and de-duplicated the
// NAMES to decide how many other programs there are. A driver that returned one entry twice therefore
// satisfied "we saw them all" while a real member had never been seen at all - and that entry's row was
// then written on, or deleted, as though its membership were known.
//
// `listed` is every application name the driver handed back, IN ORDER, empties and repeats included -
// gpu_cuda.cpp reports, this rules. `expected` is NVDRS_PROFILE::numOfApps, which the driver states
// before any page is read.
//
// 🔴 THE TEST IS ARITHMETIC AGAINST `expected`, NOT THE LOOP'S OWN EXIT CONDITION, and it has to be: a
// profile holding exactly one full page cannot be told from a truncated one by the loop, and calling that
// incomplete would refuse a perfectly ordinary row. An EMPTY profile therefore answers complete, which
// the half-finished-create clean-up (E7) depends on.
struct CudaMembership {
    size_t otherApps = 0;      // unique entries that are not `except`
    bool complete = false;     // false: that count is not the whole count, so the row is refused
};

inline CudaMembership JudgeCudaMembership(const std::vector<std::wstring>& listed, size_t expected,
                                          const std::wstring& except) {
    CudaMembership m;
    std::vector<std::wstring> unique;
    bool sane = true;
    for (size_t i = 0; i < listed.size(); ++i) {
        // 🔴 R6-6: AN EMPTY NAME IS "CANNOT JUDGE", NOT A ROW TO STEP OVER. It used to be skipped in
        // silence, so a page of blanks counted towards "we saw them all" and named nobody.
        if (listed[i].empty()) {
            sane = false;
            continue;
        }
        bool already = false;
        for (size_t j = 0; j < unique.size(); ++j) {
            if (!IEquals(unique[j], listed[i])) continue;
            already = true;
            break;
        }
        if (already) {
            // A driver that lists one entry twice has not told us about the member it left out.
            sane = false;
            continue;
        }
        unique.push_back(listed[i]);
    }
    for (size_t i = 0; i < unique.size(); ++i)
        if (!IEquals(unique[i], except)) ++m.otherApps;
    // 🔴 UNIQUE ENTRIES, NOT RAW ROWS. This is the whole of R6-6.
    m.complete = sane && unique.size() >= expected;
    return m;
}

// ---- ONE DRIVER ENUMERATION, PAGE BY PAGE, WHERE THE UNIT SUITE REACHES IT ---------------------
//
// NvAPI_DRS_EnumApplications and NvAPI_DRS_EnumSettings both hand back a page at a time: the caller
// passes a buffer and its capacity, and the driver writes the page and says how many entries it wrote.
// gpu_cuda.cpp's two readers (OtherAppCountOf, SettingIdsOf) share this walk; only the call and the copy
// out of the page are theirs.
//
// 🔴 A PAGE LONGER THAN ITS BUFFER IS A FAILED ENUMERATION, NEVER A SHORT ONE (pre-publish review of
// v0.5.9). Both readers used to copy at most the buffer's worth and then advance by the count the DRIVER
// reported, so a driver that over-reported a page had the extra entries skipped in silence while the walk
// still answered success. For the settings that is a delete decision taken on a partial list: a profile
// that looked like it held nothing but this product's own setting would be taken away whole, the user's
// setting with it. An over-reported page is answered Overran BEFORE anything is copied or the walk
// advances, and both callers read Overran as "could not list".
enum class CudaPage { Filled, End, Refused };      // what one page request answered
enum class CudaWalk { Complete, Refused, Overran, Capped };

// `fetch(start, count)` asks for the page at `start`: `count` goes in as the page's capacity and comes back
// as the number of entries the driver says it wrote. `take(k)` keeps entry k of the page just fetched, and
// is only ever called for k < pageSize. `cap` is where the walk gives up (Capped), because a list that long
// is not one this product reads whole.
template <class Fetch, class Take>
inline CudaWalk WalkCudaPages(unsigned long pageSize, unsigned long cap, Fetch fetch, Take take) {
    if (pageSize == 0) return CudaWalk::Refused;   // a walk that could never advance
    for (unsigned long start = 0; start < cap;) {
        unsigned long count = pageSize;
        const CudaPage got = fetch(start, count);
        if (got == CudaPage::End) return CudaWalk::Complete;   // the driver saying "that is all of them"
        if (got != CudaPage::Filled) return CudaWalk::Refused;
        if (count > pageSize) return CudaWalk::Overran;       // nothing copied, nothing skipped
        for (unsigned long k = 0; k < count; ++k) take(k);
        if (count < pageSize) return CudaWalk::Complete;       // a short page, the empty one included
        start += count;
    }
    return CudaWalk::Capped;
}

// The one name this feature gives a profile it makes, and the test that recognises one again later.
// Nothing else in the driver database may be deleted by this product, so both live in one place.
inline std::wstring CudaProfileNameFor(const std::wstring& exePath) { return L"Game Optimizer - " + exePath; }

// 🔴 R5-10: WITHOUT CASE, LIKE EVERY OTHER PROFILE-NAME COMPARISON IN THIS FILE. NVIDIA's own Control
// Panel lets a user rename a profile, and a driver that answered "GAME OPTIMIZER - C:\..." would have made
// this false - which would have refused every delete of an entry that really is ours.
inline bool IsCudaProfileWeMade(const std::wstring& profileName) {
    const std::wstring stem = L"Game Optimizer - ";
    return profileName.size() > stem.size() && IEquals(profileName.substr(0, stem.size()), stem);
}

// 🔴 THE ONE FORM OUR OWN APPLICATION ENTRY TAKES, IN ONE PLACE. [M] the driver stores an application
// entry either as the full path in lower case with forward slashes, or as a BARE FILE NAME that matches
// that file wherever it lives - 13,009 entries on this machine are the second kind. createProfileForExe
// only ever writes the first, so "is this entry ours?" is a string comparison with this and nothing else,
// and a bare-name entry can never be mistaken for one of ours.
inline std::wstring CudaAppKeyFor(const std::wstring& exePath) {
    std::wstring k = ToLower(exePath);
    for (size_t i = 0; i < k.size(); ++i)
        if (k[i] == L'\\') k[i] = L'/';
    return k;
}

// Why a profile could not be made for an executable. AlreadyInUse is NVAPI's -167
// (NVAPI_EXECUTABLE_ALREADY_IN_USE), which [M] a full path does not avoid. NameTaken is an entry already
// carrying the name this product uses, which the caller only lets it adopt when a line of ours names it.
enum class CudaCreate { Created, NameTaken, AlreadyInUse, Failed };

struct CudaOps {
    // False: no driver was reached at all. Every row is then refused with NoNvidiaDriver, once.
    bool available = false;
    // Set when nvapi64.dll IS there but its settings could not be opened - a different sentence.
    CudaRefusal openRefusal = CudaRefusal::NoNvidiaDriver;
    std::function<std::vector<NvidiaGpu>()> listGpus;
    std::function<std::vector<std::wstring>()> listIds;
    // Three answers, and the caller must tell them apart, exactly as readSetting's are: Found fills
    // `profile`, Absent means the driver really has no settings entry for that executable, Failed means
    // nobody knows - see CudaLookup. Absent is the only one a new profile may be made on.
    std::function<CudaLookup(const std::wstring& exePath, CudaProfile& profile)> findProfileForExe;
    // 🔴 R5-2: THE SAME THREE ANSWERS ABOUT A PROFILE ASKED FOR BY NAME, AND IT EXISTS SO THE ADOPT RULING
    // CAN LIVE IN THIS FILE. Until round 5 the decision "an entry already carries our name, so put our
    // application into it" was made inside createProfileForExe - in gpu_cuda.cpp, which no test reaches -
    // and it asked the driver nothing at all before writing. `profile.otherApps` is EVERY application entry
    // that profile covers: the caller only asks when the lookup for the executable answered Absent, so none
    // of them can be ours. Found means an entry of that name exists; Absent means none does; Failed means
    // nobody knows, and nothing is adopted or created on Failed.
    std::function<CudaLookup(const std::wstring& profileName, CudaProfile& profile)> findProfileByName;
    // Three answers, and the caller must tell them apart: Value fills `value`, Absent means the profile
    // really holds no such setting, Failed means nobody knows - see CudaRead.
    std::function<CudaRead(const std::wstring& profileName, std::wstring& value)> readSetting;
    std::function<bool(const std::wstring& profileName, bool clear, const std::wstring& value)> writeSetting;
    // 🔴 `adoptName` IS THE CALLER'S OWNERSHIP RULING, NOT A CONVENIENCE (E1). A profile already carrying
    // this feature's name may be ADOPTED only when the record line for this executable names it - which is
    // the only evidence there is that an earlier run of this product made it. Without that the call must
    // change NOTHING and answer NameTaken, because adding our application entry to an entry we do not own
    // is exactly the write E1 forbids. EMPTY means "adopt nothing": make one under the name you compute.
    // 🔴 AND IT CARRIES THE NAME RATHER THAN A FLAG BECAUSE THE DRIVER'S LOOKUP HAS CASE. [M] probe S23:
    // NvAPI_DRS_FindProfileByName is CASE-SENSITIVE. The caller ruled on an entry it found by the name THE
    // RECORD STORES, so this call has to open that same entry - recomputing one from today's path would
    // miss it and make a second entry differing only in case. See WriteCudaForRow.
    // 🔴 AND `made` IS FILLED EVEN WHEN THE ANSWER IS NOT Created, because a call that made the profile and
    // then failed to put the application in it has changed the driver - `made.createdNow` is how it says
    // so, and taking that profile away again is the CALLER's decision (E7, in WriteCudaForRow).
    std::function<CudaCreate(const std::wstring& exePath, const std::wstring& adoptName, CudaProfile& made)>
        createProfileForExe;
    // Takes away a profile THIS FEATURE made, and nothing else: the implementation re-reads the profile
    // and refuses unless the name is still ours, NVIDIA did not ship it, and the ONE application it
    // covers is `appEntry` - the same guards tests\cuda_assign_probe.cpp cleans up behind itself with.
    // 🔴 `appEntry` IS PART OF THE GUARD, NOT DECORATION: "it covers one application" says nothing about
    // WHICH, so a profile that lost our entry and gained somebody else's would have passed a count.
    // May be null when the driver does not offer the calls it needs; Remove then refuses the row and says
    // so rather than half-undoing it.
    std::function<bool(const std::wstring& profileName, const std::wstring& appEntry)> deleteProfile;
    // 🔴 R6-1: THE IDS OF THE SETTINGS THAT PROFILE HOLDS OF ITS OWN, so the decision about deleting it
    // can be taken on WHICH settings rather than on HOW MANY. False means the driver would not list them,
    // which is never "it has none": the row is refused with SettingsUnknown and nothing is taken away.
    // May be null when the driver does not offer NvAPI_DRS_EnumSettings; that is the same answer, and
    // ApplyCudaDriverCalls below is what makes it null rather than a lambda that cannot work.
    std::function<bool(const std::wstring& profileName, std::vector<unsigned long>& ids)> listSettingIds;
    std::function<bool()> save;
    std::function<void()> close;   // ends the driver session; NEVER NvAPI_Unload (see gpu_cuda.cpp)
};

// ---- R6-2: WHAT THE DRIVER CAN ACTUALLY DO, DECIDED WHERE THE TESTS REACH IT -------------------
//
// 🔴 THE ROUND-5 DEFECT BOTH SEATS FOUND, AND IT IS THE ROUND-4 SHAPE WEARING NEW CLOTHES. PlanCudaForRow
// refuses a row with NoWayToUndo when `ops.deleteProfile` is null - a rule the unit suite exercises by
// setting a flag on its fake. MakeCudaOps then installed that lambda UNCONDITIONALLY, even when
// NvAPI_DRS_DeleteProfile and NvAPI_DRS_DeleteApplicationEx had not resolved at all, so on a real driver
// the null could never happen and the rule could never fire. A rule written where the tests reach,
// against a seam the .cpp always fills, is not a rule: it is a test that passes.
//
// So the capability gate is ONE function, it lives here, and gpu_cuda.cpp's only job is to say which
// entry points resolved. A lambda that would call a null function pointer is removed rather than
// installed, and the refusals that ask "is this operation there?" then mean what they say.
struct CudaDriverCalls {
    bool deleteProfile = false;      // NvAPI_DRS_DeleteProfile
    bool deleteApplication = false;  // NvAPI_DRS_DeleteApplicationEx
    bool enumSettings = false;       // NvAPI_DRS_EnumSettings
};

// Taking an entry away needs BOTH calls: the application comes out of the profile first, and then the
// profile goes. One without the other cannot finish the job, so one without the other is no capability.
inline bool CudaCanDeleteProfile(const CudaDriverCalls& c) { return c.deleteProfile && c.deleteApplication; }
inline bool CudaCanListSettings(const CudaDriverCalls& c) { return c.enumSettings; }

inline void ApplyCudaDriverCalls(CudaOps& ops, const CudaDriverCalls& calls) {
    if (!CudaCanDeleteProfile(calls)) ops.deleteProfile = nullptr;
    if (!CudaCanListSettings(calls)) ops.listSettingIds = nullptr;
}

// The live driver. Declared here, implemented in gpu_cuda.cpp, and substituted wholesale by the panel
// regressions - which is why nothing else in this header knows that NVAPI exists.
CudaOps MakeCudaOps();

// ---- One application, as the driver answers for it RIGHT NOW ----------------------------------
//
// 🔴 ONE RESOLVER, THREE CALLERS, AND THE SHARING IS THE POINT. Apply, Remove and Remove's own read-back
// each decide something about what NVIDIA keeps for one executable, so all of them have to be looking at
// the same answer. Council round 2 found them looking at different ones: Apply failed closed on a lookup
// it could not make, while Remove wrote on a profile NAME out of a file and asked the driver nothing.
struct CudaResolved {
    CudaLookup state = CudaLookup::Failed;
    std::wstring profileName;
    std::wstring appEntry;      // the entry that matched: a full path in lower case, or a bare file name
    bool isPredefined = false;
    bool appsComplete = true;   // false: the count below is not the whole count, so the row is refused
    size_t otherApps = 0;
    // R6-1: reported, never branched on - the delete decision reads the setting IDS. See CudaProfile.
    size_t numSettings = 0;
};

inline CudaResolved ResolveCudaProfile(const std::wstring& exePath, const CudaOps& ops) {
    CudaResolved r;
    // No driver, or no way to ask it, is Failed - never "NVIDIA has no entry for this program", which is
    // the confusion CudaLookup exists to prevent.
    if (!ops.available || !ops.findProfileForExe) return r;
    CudaProfile p;
    r.state = ops.findProfileForExe(exePath, p);
    if (r.state != CudaLookup::Found) return r;
    r.profileName = p.profileName;
    r.appEntry = p.appEntry;
    r.isPredefined = p.isPredefined;
    r.appsComplete = p.appsComplete;
    r.otherApps = p.otherApps;
    r.numSettings = p.numSettings;
    return r;
}

// 🔴 E1, IN ONE PLACE: IS THIS ENTRY OURS TO WRITE ON? Everything that is not - NVIDIA's own profile, a
// profile the user made, one with a second member, one whose membership could not be stated - answers with
// the refusal that says so. `recordedProfile` is the entry the record line for this executable names, and
// EMPTY when there is no line: an entry nothing of ours names is never ours, whatever it is called, which
// is E1's "a profile that the record says WE created". A line always names an entry, so empty is never a
// line (CudaRecordRowIsWritable). Absent is the caller's to handle first: it is not a foreign entry, it is
// no entry at all.
inline CudaRefusal CudaOwnershipRefusal(const CudaResolved& r, const std::wstring& exePath,
                                        const std::wstring& recordedProfile) {
    if (r.state == CudaLookup::Failed) return CudaRefusal::CouldNotLookUp;
    if (r.state != CudaLookup::Found) return CudaRefusal::NvidiaManagesIt;
    // FIRST, BECAUSE EVERY OTHER ANSWER WOULD STATE A COUNT NOBODY MEASURED. A membership that stopped
    // short cannot say whether that entry covers one program or six, and the sentence E1 requires is
    // built out of exactly that number.
    if (!r.appsComplete) return CudaRefusal::MembershipUnknown;
    if (r.isPredefined || r.otherApps != 0) return CudaRefusal::NvidiaManagesIt;
    // R5-10: without case, exactly as the entry comparison below it and the delete guard already are.
    if (!IEquals(r.profileName, CudaProfileNameFor(exePath))) return CudaRefusal::NvidiaManagesIt;
    if (!IEquals(r.appEntry, CudaAppKeyFor(exePath))) return CudaRefusal::NvidiaManagesIt;
    if (recordedProfile.empty() || !IEquals(recordedProfile, r.profileName)) return CudaRefusal::NvidiaManagesIt;
    return CudaRefusal::None;
}

// 🔴 R5-2: MAY AN ENTRY THAT ALREADY CARRIES OUR NAME BE ADOPTED? THE SECOND HALF OF THE ONE OWNERSHIP
// CHECK, AND IT LIVES HERE FOR THE SAME REASON THE FIRST HALF DOES.
//
// The lookup for the executable answered Absent - NVIDIA has no settings entry covering this application -
// and yet a profile spelled "Game Optimizer - <path>" is in the database. There are exactly two ways that
// happens: an earlier run of this product died between CreateProfile and CreateApplication and left an
// EMPTY one (E7's own failure mode), or somebody else made it. `named` is what the driver says about it
// RIGHT NOW, and `claimedByRecord` is whether a line of ours names it - the only evidence an earlier run of
// this product made it (E1).
//
// 🔴 THE ROUND-4 FINDING THIS CLOSES (adversarial review, round 4): the adopt path went straight to CreateApplication with no
// GetProfileInfo at all and then reported otherApps = 0 as a CONSTANT. A profile another program had joined
// since was therefore written on as though it were ours alone, and the refusal sentence that should have
// named it never ran. Adoption is now allowed on ONE shape only: not NVIDIA's own, membership fully
// readable, and NO application entry in it at all.
inline CudaRefusal CudaAdoptRefusal(const CudaProfile& named, bool claimedByRecord) {
    if (!claimedByRecord) return CudaRefusal::ProfileNameTaken;
    // FIRST, as in CudaOwnershipRefusal: a membership that stopped short cannot say whether that entry is
    // the empty leftover this may adopt or one six programs are already in.
    if (!named.appsComplete) return CudaRefusal::MembershipUnknown;
    if (named.isPredefined || named.otherApps != 0) return CudaRefusal::NvidiaManagesIt;
    return CudaRefusal::None;
}

// ---- The record: ONE LINE PER APPLICATION, IN ONE FILE THAT OUTLIVES THE RUN ------------------
//
// 🔴 E2: IT IS A NOTE OF WHAT WE MADE, NOT A BACKUP OF WHAT WAS THERE. Because this product only ever
// writes on a profile it created itself, the state before it touched anything is ALWAYS "NVIDIA had no
// entry for this application" - there is no previous value to keep, and no ownership flag to prove,
// because a line only exists for a profile this product made. One line per application IS one line per
// profile, so the oldest-versus-newest choice that produced the round-2 blocker has nothing to choose
// between any more.
//
// THE LINE, in order, tab separated:
//   NVIDIA's profile | the application entry | the value this product LAST wrote | when
//
// Remove compares the driver with the LAST-WRITTEN value - is this still what WE left here? - and then
// takes the whole entry away (E3).
inline std::wstring CudaRecordMagic() { return L"Game Optimizer gpu-cuda-record"; }
inline std::wstring CudaRecordVersion() { return L"2"; }

// A plain name, in the config folder beside config.ini - not beside the .reg, because it is not a per-run
// file and has to be findable without knowing when a change was made.
inline std::wstring CudaRecordFileName() { return L"gpu-cuda-record.txt"; }

struct CudaRecordRow {
    std::wstring profileName;
    std::wstring appEntry;
    std::wstring lastWrote;     // what this product wrote most recently, which is what Remove compares
    std::wstring when;          // local time, for a human reading the file
};

// A CUDA GPU setting the driver itself could have written: its own word for "nothing is excluded", or an
// id string in the form ParseUniversalGpuId reads. Anything else is damage - a truncated line, a hand
// edit, a file from another product - and is never handed to the driver.
inline bool CudaValueIsLegal(const std::wstring& v) {
    return v == CudaNoneValue() || ParseUniversalGpuId(v).ok;
}

// 🔴 R6-7: THE VALUE A LINE CARRIES WHEN THE ENTRY IS STILL OURS AND HOLDS NOTHING OF OURS.
//
// The clear-only restore (R5-1) leaves an entry Game Optimizer made standing, because the user has put
// other NVIDIA settings on it. Until round 6 it also SPENT the line - and the entry then existed, carried
// this product's own name, and had no line claiming it, which is exactly the state CudaOwnershipRefusal
// answers "NVIDIA manages this one" to. So every future Apply refused it, forever, and the only way out
// was NVIDIA Control Panel. The line stays instead, with this in place of a value: "we made this entry,
// and there is nothing of ours in it now". A later Apply may own it again; a later Remove finds nothing
// to take away and keeps the line; the line goes only when the entry itself does.
//
// It is deliberately NOT a legal driver value, so it can never be handed to the driver by any path that
// checks CudaValueIsLegal first, and it carries no tab or line break, so the record format is unchanged.
inline std::wstring CudaNothingWritten() { return L"-"; }

inline bool CudaRowHoldsNothingOfOurs(const CudaRecordRow& r) {
    return r.lastWrote == CudaNothingWritten();
}

// A field carrying a tab or a line break cannot survive one line of a tab-separated record, so such a row
// is never written and the change that produced it is reported as unrecorded.
// ponytail: tab-separated and so limited to fields without tabs; a quoted or length-tagged format would
// lift that, and [M] no NVIDIA profile name or application entry on this machine contains one.
inline bool CudaRecordRowIsWritable(const CudaRecordRow& r) {
    const std::wstring fields[4] = { r.profileName, r.appEntry, r.lastWrote, r.when };
    for (size_t f = 0; f < 4; ++f)
        for (size_t i = 0; i < fields[f].size(); ++i)
            if (fields[f][i] == L'\t' || fields[f][i] == L'\r' || fields[f][i] == L'\n') return false;
    return !r.profileName.empty() && !r.appEntry.empty() && !r.lastWrote.empty() && !r.when.empty();
}

// 🔴 AND WHAT A ROW HAS TO BE BEFORE REMOVE ACTS ON IT. A row can be perfectly well formed as a LINE and
// still hold nonsense as a SETTING; the driver would take that nonsense.
inline bool CudaRecordRowIsUsable(const CudaRecordRow& r) {
    if (r.profileName.empty() || r.appEntry.empty()) return false;
    // R6-7: a line that says "the entry is ours and holds nothing of ours" is a perfectly good line. It
    // is never compared with the driver and never written to it - RestoreCudaForRow answers it before it
    // reads anything - so it does not have to be a value the driver could hold.
    if (CudaRowHoldsNothingOfOurs(r)) return true;
    return CudaValueIsLegal(r.lastWrote);
}

// 🔴 E6: THE FIRST LINE IS A MACHINE-READABLE HEADER, AND A FILE WITHOUT IT IS UNREADABLE. An empty file,
// a file of nothing but comments, and a file in the format an earlier version wrote all answer the same
// way: "this is not a record I understand", never "nothing was ever changed here". The comments below it
// are for the person who opens it; the parser skips them and demands the line above them.
inline std::wstring FormatCudaRecordHeader() {
    return CudaRecordMagic() + L"\t" + CudaRecordVersion() + L"\r\n" +
           L"; Game Optimizer - gpu-cuda-record.txt: the NVIDIA settings entries Game Optimizer made to\r\n"
           L"; choose which GPU CUDA uses, and has not taken away again. ONE LINE PER APPLICATION.\r\n"
           L";\r\n"
           L"; A line is, separated by tabs: the NVIDIA settings entry Game Optimizer made, the application\r\n"
           L"; entry inside it, the value Game Optimizer wrote there most recently, and when it wrote it.\r\n"
           L";\r\n"
           L"; Game Optimizer only ever changes an entry it made itself, for one application, so there is\r\n"
           L"; no previous value to keep here: before it, NVIDIA had no entry for that application at all.\r\n"
           L"; \"Remove assignment\" on the GPU Assignment tab reads this file. It checks that the entry is\r\n"
           L"; still the one Game Optimizer made and still holds what Game Optimizer wrote, and then puts\r\n"
           L"; it back ONE OF TWO WAYS. If that entry holds nothing but Game Optimizer's own CUDA setting,\r\n"
           L"; the whole entry is taken away and the line goes out of this file. If NVIDIA Control Panel\r\n"
           L"; has also put YOUR settings in the same entry, only Game Optimizer's own CUDA setting is\r\n"
           L"; taken out, the entry and your settings stay, and the line stays here with \"-\" in place of\r\n"
           L"; a value - which means \"Game Optimizer made this entry and there is nothing of its own in it\r\n"
           L"; now\". Assigning that application again may then use that entry once more.\r\n"
           L"; It is a record, not a file Windows or NVIDIA can import. Deleting it does not undo anything;\r\n"
           L"; it only makes Remove assignment unable to take those entries away.\r\n";
}

inline std::wstring FormatCudaRecordRow(const CudaRecordRow& r) {
    return r.profileName + L"\t" + r.appEntry + L"\t" + r.lastWrote + L"\t" + r.when + L"\r\n";
}

inline std::wstring FormatCudaRecordFile(const std::vector<CudaRecordRow>& rows) {
    std::wstring out = FormatCudaRecordHeader();
    for (size_t i = 0; i < rows.size(); ++i) out += FormatCudaRecordRow(rows[i]);
    return out;
}

// 🔴 NO HEADER, OR ONE LINE THIS CANNOT READ, MAKES THE WHOLE FILE UNREADABLE - AND NEVER "THERE WAS
// NOTHING" (E6). The old parser skipped a line it did not understand, so a record written by an older
// format read back as an empty one, and Remove then told the user their application had never had its
// CUDA GPU changed here while the entry this product made was still there. False is that answer, and the
// caller says it out loud rather than acting on a file it does not understand.
inline bool ParseCudaRecordFile(const std::wstring& text, std::vector<CudaRecordRow>& out) {
    out.clear();
    bool header = false;
    size_t start = 0;
    while (start < text.size()) {
        size_t nl = text.find(L'\n', start);
        if (nl == std::wstring::npos) nl = text.size();
        std::wstring line = text.substr(start, nl - start);
        start = nl + 1;
        if (!line.empty() && line[line.size() - 1] == L'\r') line.erase(line.size() - 1);
        if (line.empty()) continue;
        if (!header) {
            // THE FIRST LINE THAT IS NOT BLANK IS THE HEADER, OR THIS IS NOT OUR FILE. A comment before it
            // is not allowed either: "somebody put something in front of it" is exactly the state this
            // refuses to guess at.
            if (line != CudaRecordMagic() + L"\t" + CudaRecordVersion()) return false;
            header = true;
            continue;
        }
        if (line[0] == L';') continue;
        std::vector<std::wstring> f;
        size_t at = 0;
        for (;;) {
            const size_t tab = line.find(L'\t', at);
            if (tab == std::wstring::npos) {
                f.push_back(line.substr(at));
                break;
            }
            f.push_back(line.substr(at, tab - at));
            at = tab + 1;
            if (f.size() > 4) break;   // more fields than this format has: another format, or damage
        }
        if (f.size() != 4) {
            out.clear();
            return false;
        }
        CudaRecordRow r;
        r.profileName = f[0];
        r.appEntry = f[1];
        r.lastWrote = f[2];
        r.when = f[3];
        if (!CudaRecordRowIsWritable(r)) {
            out.clear();
            return false;
        }
        // 🔴 R5-8: TWO LINES FOR ONE APPLICATION IS A FILE THIS CODE DID NOT WRITE, AND IT IS UNREADABLE -
        // NEVER "the first one wins". E2 is one line per application, so a second is a hand edit, a merge of
        // two records, or damage; and the two lines can name DIFFERENT entries and DIFFERENT values, so
        // taking either one on trust is Remove acting on an entry the other line contradicts. CudaLineFor
        // returns the first match, which is exactly the silent wrong answer this refuses to give.
        for (size_t i = 0; i < out.size(); ++i) {
            if (!IEquals(out[i].appEntry, r.appEntry)) continue;
            out.clear();
            return false;
        }
        out.push_back(r);
    }
    return header;
}

// 🔴 THREE STATES, FOR THE REASON EVERY OTHER READ IN THIS FILE HAS THREE. Missing is the driver's
// equivalent of "there really is nothing of ours to take away"; Unreadable may name the very application
// being removed, and is said rather than acted on.
enum class CudaRecordState { Ok, Missing, Unreadable };

struct CudaRecord {
    CudaRecordState state = CudaRecordState::Missing;
    std::wstring path;
    std::vector<CudaRecordRow> rows;
};

// The line for one executable. Our own application entry has exactly one form (CudaAppKeyFor), so this is
// a comparison and not a search: there is no bare-name line to prefer a path line over any more.
inline const CudaRecordRow* CudaLineFor(const CudaRecord& record, const std::wstring& exePath) {
    if (exePath.empty()) return nullptr;
    const std::wstring key = CudaAppKeyFor(exePath);
    for (size_t i = 0; i < record.rows.size(); ++i)
        if (IEquals(record.rows[i].appEntry, key)) return &record.rows[i];
    return nullptr;
}

// The rows with `row` put in place of the line for its application entry, or added at the end. There is
// never more than one line per entry, which is the whole of E2.
inline std::vector<CudaRecordRow> CudaRowsWith(const std::vector<CudaRecordRow>& rows, const CudaRecordRow& row) {
    std::vector<CudaRecordRow> next;
    bool replaced = false;
    for (size_t i = 0; i < rows.size(); ++i) {
        if (IEquals(rows[i].appEntry, row.appEntry)) {
            next.push_back(row);
            replaced = true;
        } else {
            next.push_back(rows[i]);
        }
    }
    if (!replaced) next.push_back(row);
    return next;
}

inline std::vector<CudaRecordRow> CudaRowsWithout(const std::vector<CudaRecordRow>& rows,
                                                  const std::wstring& entry) {
    std::vector<CudaRecordRow> kept;
    for (size_t i = 0; i < rows.size(); ++i)
        if (!IEquals(rows[i].appEntry, entry)) kept.push_back(rows[i]);
    return kept;
}

// All three in gpu_cuda.cpp.
// Reads the one record in `dir`, and reports which of the three states it is in. 🔴 A per-run
// gpu-cuda-before-*.txt left by v0.5.8's earlier rounds makes the record UNREADABLE rather than absent
// (E6): it is a record of changes this product made, in a format this code no longer understands, and
// reading that as "nothing was ever changed" is exactly the mistake E6 names.
CudaRecord ReadCudaRecord(const std::wstring& dir);
// Puts `row` in the record - replacing the line for its application entry, or adding one - and rewrites
// the WHOLE file through a temporary, exactly as the .reg is written. `when` is filled in if it is empty.
// False: the file on disk is still the previous complete record, and `record` is unchanged.
bool SaveCudaRecordRow(CudaRecord& record, CudaRecordRow row);
// Takes the line for `appEntry` out, on disk as well as in `record`. False: the line is still there, so a
// later Remove may try the same undo again; the caller says so rather than leaving it quiet.
bool ForgetCudaRecordRow(CudaRecord& record, const std::wstring& appEntry);
// The record put back to exactly these rows. It exists for one case: a row that was recorded and then
// TAKEN BACK OUT of the driver (E5's verified undo). The driver no longer holds what the line claims, so
// leaving the line would make the next Remove report a conflict that never happened.
bool SetCudaRecordRows(CudaRecord& record, const std::vector<CudaRecordRow>& rows);

// ---- Is the CUDA setting an entry's OWN? One rule, for every path that reads one before writing ----
//
// 🔴 NvAPI_DRS_GetSetting ANSWERS WITH A VALUE THE ENTRY INHERITS FROM NVIDIA'S GENERAL SETTINGS when it has
// none of its own (R6-1), so "the read returned a value" is not "the entry holds that setting". The entry's
// own setting ids decide it. Both the adopt branch of WriteCudaForRow and PlanCudaForRow ask this, and they
// used to decide it separately - the plan from the read alone, so an entry of ours emptied by a clear-only
// Remove (R6-7) read an inherited value, and a value equal to the target was taken as "already set" and
// nothing was written.
//   * ids nobody could list, a read that failed, a value the driver could not have written, or ids naming
//     our setting while the read says there is none -> Undecidable, and the caller REFUSES (fail closed);
//   * ids naming our setting and a legal value      -> Present: that value is the entry's own;
//   * ids not naming it                             -> Absent, whatever the read answered.
enum class CudaLocalSetting { Present, Absent, Undecidable };

inline CudaLocalSetting JudgeCudaLocalSetting(bool idsListed, const std::vector<unsigned long>& ownIds,
                                              CudaRead read, const std::wstring& value) {
    if (!idsListed || read == CudaRead::Failed) return CudaLocalSetting::Undecidable;
    if (read == CudaRead::Value && !CudaValueIsLegal(value)) return CudaLocalSetting::Undecidable;
    bool own = false;
    for (size_t i = 0; i < ownIds.size(); ++i)
        if (ownIds[i] == CudaSettingId()) own = true;
    if (own && read != CudaRead::Value) return CudaLocalSetting::Undecidable;   // two driver answers disagree
    return own ? CudaLocalSetting::Present : CudaLocalSetting::Absent;
}

// ---- One row, before anything is written ------------------------------------------------------
struct CudaRowPlan {
    bool act = false;
    CudaRefusal refusal = CudaRefusal::None;
    bool profileExists = false;            // the driver already answers with an entry for this executable
    std::wstring profileName;
    std::wstring appEntry;
    size_t otherApps = 0;                  // E1/E9: how many OTHER programs that entry covers
    bool hadPrevious = false;              // that entry holds 0x10354FF8 right now
    std::wstring previousValue;
    std::wstring newValue;
    bool alreadySet = false;               // it already holds exactly what this would write
    bool lineMatches = false;              // and the record line for it already says so
};

// `record` is the one this run read at its start; the line for this application, if there is one, is the
// only evidence that an entry carrying our name is ours (E1).
inline CudaRowPlan PlanCudaForRow(const std::wstring& exePath, const CudaTarget& target, const CudaOps& ops,
                                  const CudaRecord& record) {
    CudaRowPlan p;
    if (!target.act) {
        p.refusal = target.refusal;
        return p;
    }
    if (!ops.available) {
        p.refusal = ops.openRefusal;
        return p;
    }
    p.newValue = target.value;
    const CudaRecordRow* line = CudaLineFor(record, exePath);
    const CudaResolved res = ResolveCudaProfile(exePath, ops);
    if (res.state == CudaLookup::Failed) {
        // 🔴 WHAT NVIDIA ALREADY KEEPS FOR THIS APPLICATION IS UNKNOWN. Going on would MAKE an entry for
        // it as though the driver had none - so an entry that may well exist is neither seen nor compared,
        // and a program NVIDIA manages would be given a second entry of ours. Nothing is created here.
        p.refusal = CudaRefusal::CouldNotLookUp;
        return p;
    }
    if (res.state == CudaLookup::Absent) {
        // E1, FIRST CASE: the driver has no entry for this executable, so one of ours may be made for it.
        // 🔴 R5-9: BUT NOT ON A DRIVER THAT CANNOT TAKE ONE AWAY AGAIN. NvAPI_DRS_DeleteProfile and
        // NvAPI_DRS_DeleteApplicationEx are deliberately NOT part of NvApi::ready (gpu_cuda.cpp), because a
        // driver missing them can still SET the CUDA GPU - but a profile MADE on such a driver is one
        // neither this row's own rollback nor any later Remove assignment could ever remove. The row is
        // refused HERE, in the plan, so nothing has been created when it is.
        if (!ops.deleteProfile) {
            p.refusal = CudaRefusal::NoWayToUndo;
            return p;
        }
        // 🔴 AND NOT ON ONE THAT CANNOT LIST AN ENTRY'S SETTINGS EITHER (pre-publish review of v0.5.9).
        // Remove assignment lists them before it takes anything away, and refuses when it cannot
        // (SettingsUnknown), so an entry made here would be one no Remove could ever take back.
        if (!ops.listSettingIds) {
            p.refusal = CudaRefusal::NoWayToListSettings;
            return p;
        }
        p.act = true;
        return p;
    }
    p.profileExists = true;
    p.profileName = res.profileName;
    p.appEntry = res.appEntry;
    p.otherApps = res.otherApps;
    const CudaRefusal bad = CudaOwnershipRefusal(res, exePath, line ? line->profileName : std::wstring());
    if (bad != CudaRefusal::None) {
        // E1, THIRD CASE. The profile name and the count survive on purpose: they are the sentence.
        if (bad == CudaRefusal::MembershipUnknown) p.otherApps = 0;   // never state a count nobody measured
        p.refusal = bad;
        return p;
    }
    // 🔴 AN ENTRY THAT ALREADY EXISTS IS REFUSED ON THIS DRIVER TOO (pre-publish review of v0.5.9). Remove
    // cannot undo ANY write on an entry whose settings it cannot list (SettingsUnknown), so a write here is a
    // change nothing could take back. Only the CUDA half is refused; the Windows GPU preference is not.
    if (!ops.listSettingIds) {
        p.refusal = CudaRefusal::NoWayToListSettings;
        return p;
    }
    std::wstring value;
    const CudaRead read = ops.readSetting ? ops.readSetting(p.profileName, value) : CudaRead::Failed;
    std::vector<unsigned long> ownIds;
    const bool idsListed = ops.listSettingIds && ops.listSettingIds(p.profileName, ownIds);
    const CudaLocalSetting local = JudgeCudaLocalSetting(idsListed, ownIds, read, value);
    if (local == CudaLocalSetting::Undecidable) {
        // 🔴 WHAT IS THERE NOW IS UNKNOWN - a failed read, a value the driver could not have written, or ids
        // that could not be listed or contradict the read - so the compare Remove will make cannot be set up
        // honestly. It counts as could-not-read, and the row is refused.
        p.refusal = CudaRefusal::CouldNotRead;
        return p;
    }
    if (local == CudaLocalSetting::Present) {   // an INHERITED value leaves the previous state absent
        p.hadPrevious = true;
        p.previousValue = value;
        p.alreadySet = (value == target.value);
    }
    p.lineMatches = line != nullptr && line->lastWrote == target.value;
    p.act = true;
    return p;
}

// ---- One row, after it was written ------------------------------------------------------------
enum class CudaOutcome {
    NotAsked,     // nothing was planned for this row - no NVIDIA target, or the feature is off
    AlreadySet,   // it already had exactly this value: nothing written to the driver
    Written,      // Apply: the driver holds the new value and saved it. Remove: the entry is gone
    Absent,       // Remove only: NVIDIA has no entry for it any more, so there is nothing to take away
    // 🔴 R6-7: Remove only. The entry is STILL OURS and holds nothing of ours - an earlier Remove took our
    // CUDA setting out of it and left it standing because the user's own settings are in it too. There is
    // nothing to take away, nothing failed, and the line is KEPT so a later Apply may own it again.
    NothingOfOurs,
    Refused       // see `refusal`
};

struct CudaRowResult {
    CudaOutcome outcome = CudaOutcome::NotAsked;
    CudaRefusal refusal = CudaRefusal::None;
    std::wstring profileName;
    std::wstring appEntry;
    size_t otherApps = 0;          // E1: how many other programs the entry named above covers
    bool hadPrevious = false;
    std::wstring previousValue;
    std::wstring newValue;
    bool undone = false;           // the row's own change was taken back out, and that was read back
    // 🔴 THE ROW CHANGED THE DRIVER AND THE CHANGE COULD NOT BE TAKEN BACK OUT (E5). The WHOLE run stops
    // here: no later application is attempted, for GPU or for CUDA, and the result names this one.
    bool unresolved = false;
    // 🔴 THE DRIVER SESSION MAY HOLD A CHANGE OF THIS ROW THAT WAS NEITHER SAVED NOR TAKEN BACK OUT.
    // NvAPI_DRS_SaveSettings commits the WHOLE session, so a later row's save would commit it with no line
    // naming it. Nothing reached disk, so this is not `unresolved` - the row can simply be tried again -
    // but no later row of this run may save, which is what the caller does with it.
    bool sessionDirty = false;
    bool profileDeleted = false;   // Remove: the entry this feature made was taken away
    // 🔴 R5-1: Remove, THE OTHER WAY. The entry held settings this product did not write, so only our own
    // CUDA setting was taken out of it and the entry - with those other settings and its application entry -
    // was left standing. Both are a successful restore; the result has to say WHICH happened.
    bool settingCleared = false;
    bool recorded = false;         // Apply: the line was in the record BEFORE the save
};

// A profile that exists only because this feature made it, taken away again. Nothing else is ever
// deleted: IsCudaProfileWeMade is asked here as well as inside the operation, because two guards over a
// delete cost one line. THE CALLER decides when it may ask at all - Apply only when the create call
// really created the profile this run, Remove only when a record line of ours names it (E1).
inline bool DropCudaProfileWeMade(const std::wstring& profile, const std::wstring& appEntry,
                                  const CudaOps& ops) {
    if (!ops.deleteProfile || appEntry.empty() || !IsCudaProfileWeMade(profile)) return false;
    return ops.deleteProfile(profile, appEntry);
}

struct CudaUndo {
    bool ok = false;               // the driver is back where it was, and that was READ BACK
    bool profileDeleted = false;
    // 🔴 R5-4: THE OPEN SESSION STILL HOLDS SOMETHING OF THIS ROW'S. Set when the profile this row created
    // could not be taken away again: it is sitting in the session, and NvAPI_DRS_SaveSettings commits the
    // WHOLE session, so the NEXT row's save would put an empty "Game Optimizer - <path>" in NVIDIA's own
    // Control Panel that no record names and no Remove assignment will ever find.
    bool sessionDirty = false;
};

// 🔴 ONE APPLY ROW'S CHANGE, TAKEN BACK OUT OF THE DRIVER - AND THE ANSWER IS CHECKED (E5).
// NvAPI_DRS_SaveSettings does not save one setting, it writes the WHOLE session, so a row still sitting in
// that session is committed by the NEXT row's save: a change on disk that no line names. The old code
// wrote the undo and reported `undone` whatever the driver answered, so a rollback that itself failed
// looked exactly like one that worked. Every step is checked here, and an undo that cannot be confirmed
// stops the run.
//
// `onDisk` says whether this row's change was saved. A row whose own save FAILED never reached the
// database, so its undo needs no save - and asking for one would be asking the call that just failed.
inline CudaUndo UndoCudaWrite(const CudaRowPlan& p, const std::wstring& profile, bool createdHere, bool onDisk,
                              const CudaOps& ops) {
    CudaUndo u;
    if (!ops.writeSetting) return u;
    // NOT the same as clearing: an entry of ours that an earlier run left a value in must get that value
    // back. An entry this row made itself held nothing, so for that one clearing IS the old state.
    const bool clear = createdHere || !p.hadPrevious;
    if (!ops.writeSetting(profile, clear, clear ? std::wstring() : p.previousValue)) return u;
    if (onDisk && (!ops.save || !ops.save())) return u;
    std::wstring after;
    const CudaRead back = ops.readSetting ? ops.readSetting(profile, after) : CudaRead::Failed;
    u.ok = clear ? (back == CudaRead::Absent) : (back == CudaRead::Value && after == p.previousValue);
    if (!u.ok) return u;
    // LAST, AND ONLY ONCE THE SETTING IS CONFIRMED BACK.
    // 🔴 R5-4: AND A FAILED DELETE *IS* A FAILED UNDO. This line used to read "a failed delete is not a
    // failed undo" and threw the answer away: the setting went back, the row was reported undone, and the
    // empty profile this row created stayed in the open session for the next row's save to commit. The row
    // is now unresolved - the session is dirty and the run stops, exactly as E5 stops it for a change that
    // reached the database, because the consequence is the same: a change nothing records.
    if (createdHere) {
        u.profileDeleted = DropCudaProfileWeMade(profile, p.appEntry, ops);
        if (!u.profileDeleted) {
            u.ok = false;
            u.sessionDirty = true;
            return u;
        }
    }
    // 🔴 R6-5: THE SAVE THAT FOLLOWS THE DELETE IS CHECKED, AND IT USED TO BE THROWN AWAY. `ops.save()`
    // stood here as a bare statement: the profile came out of the session, the save refused, and the undo
    // still answered ok - so the row was reported taken back out while the delete sat in the open session
    // for the NEXT row's save to commit. That is the same "a change nothing records" the rest of this
    // function exists to prevent, one call later. A save that refuses makes the undo unresolved, marks the
    // session dirty and stops the run (E5).
    if (u.profileDeleted && onDisk) {
        if (!ops.save || !ops.save()) {
            u.ok = false;
            u.sessionDirty = true;
            return u;
        }
    }
    return u;
}

// What one Apply row needs besides the driver: the record it writes into, before it saves.
struct CudaApplyInputs {
    // 🔴 CALLED AFTER THE DRIVER TAKES THE VALUE AND BEFORE THE SAVE. The compare in E3 makes a leftover
    // record line harmless - Remove sees a value that is not the one the line claims and leaves it alone -
    // while a change with no line at all is one Remove cannot even see. So the risk is put on the harmless
    // side: the line goes down first, and a row whose line could not be written is taken back out of the
    // driver instead of being left there unrecorded.
    std::function<bool(const CudaRecordRow&)> record;
};

// 🔴 THE ORDER IS THE RECOVERY GUARANTEE, AS IT IS FOR THE REGISTRY WRITE (gpu_edit.h): look up what
// NVIDIA already keeps, refuse unless the entry is ours, write, RECORD, save, read back. A row whose save
// failed changed nothing on disk, and a row whose record failed is taken back out before it can.
//
// ponytail: one save per row, and NvAPI_DRS_SaveSettings rewrites the whole driver database (about 1.9 MB
// [M]). That is the price of knowing exactly which rows landed when one of them fails; one save at the end
// of the run would be faster and could not say which rows it lost, so batching needs a per-row read-back
// first, not just a moved call.
inline CudaRowResult WriteCudaForRow(const std::wstring& exePath, const CudaTarget& target, const CudaOps& ops,
                                     const CudaRecord& record, const CudaApplyInputs& in) {
    CudaRowResult r;
    const CudaRowPlan p = PlanCudaForRow(exePath, target, ops, record);
    r.profileName = p.profileName;
    r.appEntry = p.appEntry;
    r.otherApps = p.otherApps;
    r.newValue = p.newValue;
    if (!p.act) {
        r.refusal = p.refusal;
        r.outcome = p.refusal == CudaRefusal::None ? CudaOutcome::NotAsked : CudaOutcome::Refused;
        return r;
    }
    if (p.alreadySet) {
        r.outcome = CudaOutcome::AlreadySet;
        r.hadPrevious = p.hadPrevious;
        r.previousValue = p.previousValue;
        if (p.lineMatches) return r;   // the note already says exactly this: nothing at all to do
        // 🔴 THE NOTE STILL HAS TO SAY WHAT THE DRIVER HOLDS (Council round 3). Our own entry
        // can come to hold the value this Apply would write without this Apply writing it - the user set
        // it in NVIDIA Control Panel. Leaving the line at the older value would make the next Remove
        // compare against a value nobody holds and report a conflict this product itself caused.
        CudaRecordRow line;
        line.profileName = p.profileName;
        line.appEntry = p.appEntry;
        line.lastWrote = target.value;
        if (!in.record || !in.record(line)) {
            r.outcome = CudaOutcome::Refused;
            r.refusal = CudaRefusal::NotRecorded;
            return r;
        }
        r.recorded = true;
        return r;
    }
    CudaRowPlan state = p;                 // what the undo has to put back, profile included
    std::wstring profile = p.profileName;
    bool createdHere = false;
    if (!p.profileExists) {
        CudaProfile made;
        // E1: AN ENTRY ALREADY CARRYING OUR NAME MAY ONLY BE ADOPTED WHEN A LINE OF OURS NAMES IT. That
        // line is the only evidence an earlier run of this product made it; without one it is somebody's
        // else's, however it is spelled.
        const CudaRecordRow* line = CudaLineFor(record, exePath);
        const std::wstring computed = CudaProfileNameFor(exePath);
        const bool claimed = line != nullptr && IEquals(line->profileName, computed);
        // 🔴 R6-12: A CLAIMED ENTRY IS LOOKED UP BY THE NAME THE RECORD STORES, NEVER BY ONE RECOMPUTED
        // FROM TODAY'S PATH. [M] probe S23, on the real driver: NvAPI_DRS_FindProfileByName is
        // CASE-SENSITIVE - the exact name answers Found, the same name in UPPER or lower case answers
        // Absent with an empty name back. Windows hands the same executable back in whatever case it
        // likes, so a path spelled differently than when the entry was made still satisfies every
        // ownership compare in this file (they are all IEquals, R5-10) while the DRIVER lookup misses -
        // and a SECOND entry differing only in case is then created, which no line claims and every later
        // Apply therefore refuses forever. That is R6-7's trap arriving through another door. The recorded
        // name is the one that was actually used when the entry was made; the recomputed one is a guess
        // about today's spelling. Where no line claims the entry there is nothing else to use, and the
        // recomputed name is correct.
        const std::wstring wanted = claimed ? line->profileName : computed;
        // 🔴 R5-2: THE OWNERSHIP CHECK RUNS ON THE ADOPT PATH TOO, AND IT RUNS HERE - BEFORE ANY CREATE
        // CALL - BECAUSE THE DRIVER HAS TO BE ASKED FIRST. Adversarial review, round 4, found this path taking
        // `handle != nullptr` straight to CreateApplication with no GetProfileInfo at all, so a profile
        // another program had joined since was written on as ours alone.
        CudaProfile named;
        const CudaLookup namedState =
            ops.findProfileByName ? ops.findProfileByName(wanted, named) : CudaLookup::Failed;
        if (namedState == CudaLookup::Failed) {
            // Whether an entry of that name exists, and who is in it, is unknown. Making one would risk a
            // second entry beside a real one; adopting would risk writing on somebody else's.
            r.outcome = CudaOutcome::Refused;
            r.refusal = CudaRefusal::CouldNotLookUp;
            return r;
        }
        bool reuseNamed = false;
        bool adoptedHadPrevious = false;
        std::wstring adoptedPrevious;
        if (namedState == CudaLookup::Found) {
            const CudaRefusal no = CudaAdoptRefusal(named, claimed);
            if (no != CudaRefusal::None) {
                r.outcome = CudaOutcome::Refused;
                r.refusal = no;
                r.profileName = named.profileName.empty() ? wanted : named.profileName;
                // NEVER A COUNT NOBODY MEASURED, and never a constant: E9's sentence is built out of it.
                r.otherApps = named.appsComplete ? named.otherApps : 0;
                return r;
            }
            // 🔴 R6-8: READ BEFORE ADDING, NOT AFTER. An adopted entry is one nobody has read yet - the
            // lookup answered Absent about the APPLICATION, not about the entry, and an earlier run of
            // this product may have left a value in it. That read used to happen AFTER
            // createProfileForExe had already put our application entry into the profile, so a read that
            // failed refused the row and left that entry behind: an application NVIDIA now resolves to an
            // entry this run never wrote and never recorded. Asking first costs one call and leaves the
            // driver untouched when the answer is one this cannot use.
            std::wstring value;
            const CudaRead read = ops.readSetting ? ops.readSetting(wanted, value) : CudaRead::Failed;
            // 🔴 WHETHER THAT VALUE IS THE ENTRY'S OWN IS DECIDED BY ITS SETTING IDS, NEVER BY THE READ ALONE
            // (pre-publish review of v0.5.9). NvAPI_DRS_GetSetting answers with a value the entry INHERITS from
            // NVIDIA's general settings when it has none of its own (R6-1), so the read alone recorded an
            // inherited value as the previous one - and a failed Apply's rollback then WROTE it into the entry
            // as a local setting that had never existed. Ids nobody could list refuse, and so does an
            // enumeration naming our setting while the read says there is none: two driver answers disagree.
            std::vector<unsigned long> ownIds;
            const bool idsListed = ops.listSettingIds && ops.listSettingIds(wanted, ownIds);
            const CudaLocalSetting local = JudgeCudaLocalSetting(idsListed, ownIds, read, value);
            if (local == CudaLocalSetting::Undecidable) {
                r.outcome = CudaOutcome::Refused;
                r.refusal = CudaRefusal::CouldNotRead;
                r.profileName = named.profileName.empty() ? wanted : named.profileName;
                return r;   // NOTHING was added: the adoption never happened
            }
            if (local == CudaLocalSetting::Present) {   // the entry's OWN; an inherited one leaves it ABSENT
                adoptedHadPrevious = true;
                adoptedPrevious = value;
            }
            reuseNamed = true;
        }
        // The ADOPTED entry is named, so the call opens the one this plan ruled on rather than looking for
        // a name of its own (R6-12, above). Empty is "adopt nothing", and then the name it computes is the
        // only one there is.
        const CudaCreate c = ops.createProfileForExe
                                 ? ops.createProfileForExe(exePath, reuseNamed ? wanted : std::wstring(), made)
                                 : CudaCreate::Failed;
        if (c != CudaCreate::Created) {
            // 🔴 R5-7: NAME THE ENTRY THAT HOLDS THE FILE NAME, OR SAY PLAINLY THAT IT COULD NOT BE NAMED.
            // [M] -167 arrives when ANOTHER profile already claims this executable's BASE NAME, and the old
            // sentence named nothing - so the user was sent to NVIDIA Control Panel to find an entry among
            // thousands. The base name is exactly the form the driver stores those entries under, so asking
            // the same lookup for it answers which profile that is. Read before the clean-up below, because
            // the clean-up changes the session. An empty name leaves the honest un-named sentence.
            if (c == CudaCreate::AlreadyInUse) {
                const CudaResolved by = ResolveCudaProfile(BaseName(exePath), ops);
                if (by.state == CudaLookup::Found) {
                    r.profileName = by.profileName;
                    r.otherApps = by.appsComplete ? by.otherApps : 0;
                }
            }
            // 🔴 E7: A PROFILE MADE MOMENTS AGO GOES AGAIN WHEN THE APPLICATION WOULD NOT GO INTO IT.
            // NvAPI_DRS_SaveSettings commits the WHOLE session, so an empty "Game Optimizer - <path>" left
            // here is committed by the NEXT row's save - an entry in NVIDIA's own Control Panel that no
            // record names and no Remove will ever take away. `createdNow` is true only when this call
            // really made one, so nothing that was already there is touched.
            if (made.createdNow) {
                r.profileDeleted = DropCudaProfileWeMade(made.profileName, made.appEntry, ops);
                if (!r.profileDeleted) {
                    // 🔴 R5-4: THE CLEAN-UP ITSELF FAILED, so the empty profile is still in the open session
                    // and a later row's save would commit it. That is unresolved: the session is dirty and
                    // the whole run stops, exactly as E5 stops it. The reason that brought us here is given
                    // up in favour of this one, which is the louder and the actionable half.
                    r.unresolved = true;
                    r.sessionDirty = true;
                    r.outcome = CudaOutcome::Refused;
                    r.refusal = CudaRefusal::ProfileNotRemoved;
                    return r;
                }
            }
            r.outcome = CudaOutcome::Refused;
            r.refusal = c == CudaCreate::AlreadyInUse
                            ? CudaRefusal::NameAlreadyInUse
                            : (c == CudaCreate::NameTaken ? CudaRefusal::ProfileNameTaken
                                                          : CudaRefusal::ProfileNotCreated);
            // 🔴 R6-11: THE ADOPT RE-CHECK LOST A RACE, AND THE OLD SENTENCE WAS FALSE. The plan adopted
            // the entry - a line of ours named it and the driver said it held nobody - and then the second
            // guard inside createProfileForExe, taken against the handle actually about to be written,
            // found a member that arrived in between. ProfileNameTaken's sentence says Game Optimizer "has
            // no record of making" the entry, which is untrue here: a record line claims it, and that is
            // the only reason this path was taken at all. It is the ordinary "NVIDIA manages this one"
            // answer, with the count the second guard MEASURED - or the membership-unknown one when that
            // guard is the thing that could not be measured.
            if (c == CudaCreate::NameTaken && reuseNamed) {
                if (!made.profileName.empty()) r.profileName = made.profileName;
                if (!made.appsComplete) {
                    r.otherApps = 0;   // never state a count nobody measured
                    r.refusal = CudaRefusal::MembershipUnknown;
                } else {
                    r.otherApps = made.otherApps;
                    r.refusal = CudaRefusal::NvidiaManagesIt;
                }
            }
            return r;
        }
        createdHere = made.createdNow;
        profile = made.profileName;
        state.profileName = made.profileName;
        state.appEntry = made.appEntry;
        state.hadPrevious = false;
        state.previousValue.clear();
        r.profileName = made.profileName;
        r.appEntry = made.appEntry;
        // R5-2: what the call MEASURED, never a constant. Adoption above already refused anything but zero,
        // so this can only be zero on a driver that agrees with the lookup - and if it ever disagrees, the
        // number the result prints is the driver's and not this code's wish.
        r.otherApps = made.otherApps;
        if (!createdHere && adoptedHadPrevious) {
            // 🔴 R6-8: WHAT THE ADOPTED ENTRY HELD, READ BEFORE THE APPLICATION WENT INTO IT. The read
            // itself is above, on the branch that decided to adopt - so a read this cannot use refuses the
            // row while the driver is still untouched, instead of leaving the application entry behind in
            // an entry this run then walked away from.
            state.hadPrevious = true;
            state.previousValue = adoptedPrevious;
            r.hadPrevious = true;
            r.previousValue = adoptedPrevious;
        }
    } else {
        r.hadPrevious = p.hadPrevious;
        r.previousValue = p.previousValue;
    }
    if (!ops.writeSetting || !ops.writeSetting(profile, false, target.value)) {
        // Nothing of this row's own is in the session - except the entry it just made for it, which would
        // otherwise stay behind empty. The setting is NOT cleared here: on an entry that already existed,
        // clearing would delete a value this row never managed to replace.
        if (createdHere) {
            r.profileDeleted = DropCudaProfileWeMade(profile, state.appEntry, ops);
            if (!r.profileDeleted) {
                // 🔴 R5-4, the same shape one step later: the profile is still in the session.
                r.unresolved = true;
                r.sessionDirty = true;
                r.outcome = CudaOutcome::Refused;
                r.refusal = CudaRefusal::ProfileNotRemoved;
                return r;
            }
        }
        r.outcome = CudaOutcome::Refused;
        r.refusal = CudaRefusal::SettingNotWritten;
        return r;
    }
    CudaRecordRow line;
    line.profileName = profile;
    line.appEntry = state.appEntry;
    line.lastWrote = target.value;
    if (!in.record || !in.record(line)) {
        const CudaUndo u = UndoCudaWrite(state, profile, createdHere, false, ops);
        r.undone = u.ok;
        r.profileDeleted = u.profileDeleted;
        r.unresolved = !u.ok;
        r.sessionDirty = r.sessionDirty || u.sessionDirty;   // R5-4
        r.outcome = CudaOutcome::Refused;
        r.refusal = u.ok ? CudaRefusal::NotRecorded : CudaRefusal::NotUndone;
        return r;
    }
    r.recorded = true;
    if (!ops.save || !ops.save()) {
        const CudaUndo u = UndoCudaWrite(state, profile, createdHere, false, ops);
        r.undone = u.ok;
        r.profileDeleted = u.profileDeleted;
        r.unresolved = !u.ok;
        r.sessionDirty = r.sessionDirty || u.sessionDirty;   // R5-4
        r.outcome = CudaOutcome::Refused;
        r.refusal = u.ok ? CudaRefusal::NotSaved : CudaRefusal::NotUndone;
        return r;
    }
    // READ IT BACK BEFORE CALLING IT DONE. A save that answers yes is not the same as a value that is
    // there; this is the same read-back the registry half does after its own write (gpu_edit.h), and it
    // is what makes the record a statement about the driver rather than about our intent.
    std::wstring after;
    const CudaRead back = ops.readSetting ? ops.readSetting(profile, after) : CudaRead::Failed;
    if (back != CudaRead::Value || after != target.value) {
        // This one DID reach the database, so its undo has to as well.
        const CudaUndo u = UndoCudaWrite(state, profile, createdHere, true, ops);
        r.undone = u.ok;
        r.profileDeleted = u.profileDeleted;
        r.unresolved = !u.ok;
        r.sessionDirty = r.sessionDirty || u.sessionDirty;   // R5-4
        r.outcome = CudaOutcome::Refused;
        r.refusal = u.ok ? CudaRefusal::NotConfirmed : CudaRefusal::NotUndone;
        return r;
    }
    r.outcome = CudaOutcome::Written;
    return r;
}

// ---- Putting one row's CUDA setting back: E3 ---------------------------------------------------
//
// 🔴 THE UNDO IS THE ENTRY, NOT THE VALUE. This product only ever writes on an entry it made itself, and
// before it there was none, so "as it was" means "NVIDIA has no entry for this application" - which is
// reached by taking the entry away, setting and all. Every step answers a finding the Council made:
//   * the note has to make sense at all before it is handed to a driver;
//   * the driver has to still agree that this entry is the one that governs the application, that it is
//     ours, that its membership can be stated and that it still covers our application entry - Remove
//     used to ask none of that and write on a name out of a file;
//   * the setting has to still be THE VALUE THIS PRODUCT LAST WROTE, not something a user chose since;
//   * the driver has to agree afterwards that the entry is really gone;
//   * and a step that may have changed the open session without committing it says so (sessionDirty), so
//     no later row's save can commit it behind us.
inline CudaRowResult RestoreCudaForRow(const std::wstring& exePath, const CudaRecordRow& rec, const CudaOps& ops) {
    CudaRowResult r;
    r.profileName = rec.profileName;
    r.appEntry = rec.appEntry;
    r.newValue = rec.lastWrote;
    if (!CudaRecordRowIsUsable(rec)) {
        r.outcome = CudaOutcome::Refused;
        r.refusal = CudaRefusal::RecordUnusable;
        return r;
    }
    if (!ops.available) {
        r.outcome = CudaOutcome::Refused;
        r.refusal = ops.openRefusal;
        return r;
    }
    const CudaResolved res = ResolveCudaProfile(exePath, ops);
    if (res.state == CudaLookup::Failed) {
        r.outcome = CudaOutcome::Refused;
        r.refusal = CudaRefusal::CouldNotLookUp;
        return r;
    }
    if (res.state == CudaLookup::Absent) {
        // E3, THIRD CASE: the entry this product made is already gone - somebody removed it in NVIDIA's
        // own Control Panel, which is the same end. The state the note asks for is already true, so the
        // line is dropped and nothing is said about it as a failure.
        r.outcome = CudaOutcome::Absent;
        return r;
    }
    r.otherApps = res.otherApps;
    if (!res.appsComplete) {
        r.otherApps = 0;
        // R5-7: the entry IS known here - the lookup found it and only its membership was unreadable - so
        // the sentence names it rather than describing an anonymous failure.
        r.profileName = res.profileName;
        r.outcome = CudaOutcome::Refused;
        r.refusal = CudaRefusal::MembershipUnknown;
        return r;
    }
    if (!IEquals(res.profileName, rec.profileName) || !IEquals(res.appEntry, rec.appEntry) ||
        res.isPredefined || res.otherApps != 0 || !IsCudaProfileWeMade(res.profileName)) {
        // E3, SECOND CASE, at the identity level: the entry the note names is not the one the driver now
        // answers with, or it has gained a member since. Taking it away would take somebody else's
        // settings with it, so nothing is written and the line is kept.
        r.outcome = CudaOutcome::Refused;
        r.refusal = CudaRefusal::RecordedProfileChanged;
        return r;
    }
    // 🔴 R6-7: THE LINE SAYS THE ENTRY IS OURS AND HOLDS NOTHING OF OURS. An earlier Remove took our own
    // CUDA setting out of it and left the entry standing, because the user's own settings are in it too.
    // There is nothing to compare, nothing to take away and nothing that failed - and the LINE STAYS, so
    // a later Apply can own that entry again instead of meeting it forever as one NVIDIA manages. It is
    // answered here, after the identity checks, so the line is only kept while the entry really is still
    // the one it names; an entry that has gone answered Absent above and the line is dropped.
    if (CudaRowHoldsNothingOfOurs(rec)) {
        r.outcome = CudaOutcome::NothingOfOurs;
        return r;
    }
    std::wstring now;
    const CudaRead read = ops.readSetting ? ops.readSetting(rec.profileName, now) : CudaRead::Failed;
    if (read == CudaRead::Failed) {
        r.outcome = CudaOutcome::Refused;
        r.refusal = CudaRefusal::CouldNotRead;
        return r;
    }
    if (read != CudaRead::Value || now != rec.lastWrote) {
        // E3, SECOND CASE: somebody - the user in NVIDIA's Control Panel, NVIDIA's own app, another
        // program - has set this since. Taking the entry away would throw THEIR change away with it, so
        // it is left exactly as it is, the line stays in the note, and the result says which application.
        r.outcome = CudaOutcome::Refused;
        r.refusal = CudaRefusal::ChangedSinceWritten;
        return r;
    }
    // 🔴 R5-1, AND IT IS THE ROUND-4 BLOCKER: A PROFILE CARRYING SETTINGS THIS PRODUCT DID NOT WRITE IS
    // NEVER DELETED. NVIDIA keeps EVERY per-application setting in one profile, so the entry this product
    // made for an executable is also where NVIDIA Control Panel puts that executable's "Vertical sync",
    // "Power management mode" or anything else the user sets afterwards. Remove used to delete the whole
    // profile on four guards - predefined, one application, that application ours, membership readable -
    // and NVDRS_PROFILE::numOfSettings was in the struct and read by nobody, so every one of those settings
    // went with it.
    //
    // 🔴 R6-1: AND THE FIX WAS A COUNT, WHICH IS THE VERY MISTAKE ROUND 4 HAD ALREADY PROVED. `numSettings
    // > 1` reads "one setting" as "the one setting is MINE" - it is the application defect one level down.
    // An entry of ours holding the user's "Vertical sync" and no CUDA setting of its own counts ONE, and
    // went whole. The ids are enumerated instead, and the decision is taken on WHICH settings are there:
    //   * exactly ours          -> the entry is nothing but ours and goes whole, as before;
    //   * ours and others       -> only ours comes out and the entry stays standing (R5-1's branch);
    //   * ours NOT among them   -> the value read back a moment ago is INHERITED from NVIDIA's general
    //                             settings, not this entry's own. Two driver answers disagree, so nothing
    //                             is touched and it is said (SettingNotOurs);
    //   * no enumeration at all -> nobody knows, which is never "it has none" (SettingsUnknown).
    // Both restores are read back; neither refusal writes anything.
    std::vector<unsigned long> settingIds;
    if (!ops.listSettingIds || !ops.listSettingIds(rec.profileName, settingIds)) {
        r.outcome = CudaOutcome::Refused;
        r.refusal = CudaRefusal::SettingsUnknown;
        return r;
    }
    const CudaSettingsVerdict verdict = JudgeCudaSettings(settingIds);
    if (verdict == CudaSettingsVerdict::NotOurs) {
        // 🔴 THE CONTRADICTION. The read above answered with exactly what this product wrote, and the
        // enumeration says that setting is not this entry's own. A delete here would take away an
        // application entry - and any other setting on it - over a value that lives somewhere else
        // entirely, and a clear would delete nothing and then fail its own read-back. The row is refused
        // and the line is kept, so nothing is lost and it can be looked at.
        r.outcome = CudaOutcome::Refused;
        r.refusal = CudaRefusal::SettingNotOurs;
        return r;
    }
    if (verdict == CudaSettingsVerdict::AlsoOthers) {
        // NOT a delete of the profile: the same clear the rollback uses, which is
        // NvAPI_DRS_DeleteProfileSetting for 0x10354FF8 and nothing else (gpu_cuda.cpp, writeSetting).
        if (!ops.writeSetting || !ops.writeSetting(rec.profileName, true, std::wstring())) {
            r.outcome = CudaOutcome::Refused;
            r.refusal = CudaRefusal::SettingNotRemoved;
            return r;
        }
        if (!ops.save || !ops.save()) {
            // It is out of the SESSION and not out of the database: the line stays so Remove can try again,
            // and no later row may save or it would commit this with the line still naming it.
            r.sessionDirty = true;
            r.outcome = CudaOutcome::Refused;
            r.refusal = CudaRefusal::NotSaved;
            return r;
        }
        std::wstring gone;
        const CudaRead back = ops.readSetting ? ops.readSetting(rec.profileName, gone) : CudaRead::Failed;
        if (back != CudaRead::Absent) {
            // A save that answers yes is not a setting that is gone. It IS out of the database or it is not,
            // and "not" is said rather than recorded as done.
            r.sessionDirty = true;
            r.outcome = CudaOutcome::Refused;
            r.refusal = CudaRefusal::NotConfirmed;
            return r;
        }
        r.settingCleared = true;
        r.outcome = CudaOutcome::Written;
        return r;
    }
    if (!DropCudaProfileWeMade(rec.profileName, rec.appEntry, ops)) {
        // Nothing reached disk, and the line is kept, so this is a retry rather than a loss. It may still
        // have taken the application entry out of the session on its way to refusing, so nothing later in
        // this run may save.
        r.sessionDirty = ops.deleteProfile != nullptr;
        r.outcome = CudaOutcome::Refused;
        r.refusal = CudaRefusal::ProfileNotRemoved;
        return r;
    }
    if (!ops.save || !ops.save()) {
        // The entry is out of the SESSION and not out of the database. Nothing is committed and the line
        // stays, so Remove can try again - but no later row may save, or it would commit this with no line
        // naming it.
        r.sessionDirty = true;
        r.outcome = CudaOutcome::Refused;
        r.refusal = CudaRefusal::NotSaved;
        return r;
    }
    // READ IT BACK: the driver must no longer answer with OUR entry for this application. Another entry
    // answering instead is still success - ours is gone, which is what the line claimed to undo.
    const CudaResolved after = ResolveCudaProfile(exePath, ops);
    if (after.state == CudaLookup::Failed) {
        r.outcome = CudaOutcome::Refused;
        r.refusal = CudaRefusal::CouldNotLookUp;
        return r;
    }
    if (after.state == CudaLookup::Found && IEquals(after.profileName, rec.profileName)) {
        r.outcome = CudaOutcome::Refused;
        r.refusal = CudaRefusal::NotConfirmed;
        return r;
    }
    r.profileDeleted = true;
    r.outcome = CudaOutcome::Written;
    return r;
}

// ---- What Apply asks, and what both results say -----------------------------------------------

// 🔴 E9 + FOUNDER DECISION 22: THE ONE SENTENCE THAT NAMES WHAT NVIDIA MANAGES. It is the whole of what
// the user loses in v0.5.8 - Chrome, Edge, Firefox, Claude and anything else NVIDIA ships a profile for -
// so it says which entry, how many other programs go with it, and where to set it instead. It is true for
// every entry that is not ours: a predefined NVIDIA one, a user's own, one with a second member, and an
// entry left behind under our own name that no note of ours claims.
inline std::wstring FormatCudaNvidiaManagesLine(const std::wstring& profileName, size_t otherApps) {
    std::wstring s = L"NVIDIA keeps which GPU CUDA uses for it in its own settings entry \"" + profileName + L"\"";
    if (otherApps == 0) s += L", which Game Optimizer has no record of making";
    else if (otherApps == 1) s += L", which covers 1 other program as well";
    else s += L", which covers " + std::to_wstring(otherApps) + L" other programs as well";
    return s + L", so Game Optimizer left it alone; set that entry in NVIDIA Control Panel";
}

// 🔴 R5-7: -167's OWN SENTENCE, AND IT NAMES THE ENTRY OR SAYS IT COULD NOT. A refusal that sends the user
// to NVIDIA Control Panel without saying WHICH of the thousands of entries there to open is a generic
// sentence standing in for a specific one. The empty form is the honest statement of a search that failed,
// which is a different claim from "there is no such entry".
// 🔴 R6-9: AND IT IS GIVEN THE COUNT IT ALREADY MEASURED. WriteCudaForRow looks the base name up and fills
// `otherApps` for exactly this sentence, and the sentence never took the argument - so a refusal that knew
// the entry covers four other programs told the user about none of them. Zero, one and many each get their
// own words, as FormatCudaNvidiaManagesLine's do, because "covers 0 other programs as well" is not English
// and reads as a bug.
inline std::wstring FormatCudaNameInUseLine(const std::wstring& profileName, size_t otherApps) {
    if (profileName.empty()) return CudaRefusalReason(CudaRefusal::NameAlreadyInUse);
    std::wstring s = L"NVIDIA already keeps which GPU CUDA uses for a program with that file name in its own "
                     L"settings entry \"" + profileName + L"\"";
    if (otherApps == 1) s += L", which covers 1 other program as well";
    else if (otherApps > 1) s += L", which covers " + std::to_wstring(otherApps) + L" other programs as well";
    return s + L", so Game Optimizer left it alone; set that entry in NVIDIA Control Panel";
}

// 🔴 R5-7 again, for the other refusal that used to describe an anonymous failure. The entry is known
// whenever the lookup FOUND it and only its membership could not be listed; it is unknown when the adopt
// path could not name what it was refusing.
inline std::wstring FormatCudaMembershipUnknownLine(const std::wstring& profileName) {
    if (profileName.empty()) return CudaRefusalReason(CudaRefusal::MembershipUnknown);
    return L"NVIDIA's driver would not list every program that shares its settings entry \"" + profileName +
           L"\", so Game Optimizer left it alone; set that entry in NVIDIA Control Panel";
}

// 🔴 R5-5: REMOVE FOUND AN ENTRY OF OUR OWN NAME THAT NO LINE OF OURS CLAIMS. It is said and NOTHING is
// done about it - the removal is not blocked (blocking every Remove is the trap E4 exists to close) and the
// entry is not deleted (deleting an entry no record claims is what E1 forbids). The user is the one who
// decides what happens to it.
inline std::wstring FormatCudaOrphanEntryLine(const std::wstring& profileName) {
    return L"NVIDIA still keeps a settings entry named \"" + profileName +
           L"\" for it, which Game Optimizer has no note of making, so Game Optimizer left it alone and the "
           L"GPU assignment was removed anyway; set or remove that entry in NVIDIA Control Panel";
}

// 🔴 R5-6: THE ONE WHOLE-RUN REASON THAT HAS TO NAME A PLACE ON DISK. "The note could not be read" tells a
// user nothing they can act on: the note is a file, in a folder they have never had to find, and the
// commonest reason it cannot be read is a gpu-cuda-before-*.txt an earlier version of v0.5.8 left beside it
// (E6). Every other refusal keeps its fixed sentence.
inline std::wstring CudaWholeRunReason(CudaRefusal refusal, const std::wstring& configDir) {
    const std::wstring base = CudaRefusalReason(refusal);
    if (refusal != CudaRefusal::RecordUnreadable || configDir.empty()) return base;
    // 🔴 IT NAMES ONLY THE gpu-cuda-before-*.txt FILES AS THE ONES TO MOVE (pre-publish review of v0.5.9). The
    // record itself is the only note of what to undo, so moving it out would throw that away: the sentence
    // says to keep it, in so many words.
    return base + L" - Game Optimizer keeps it as " + CudaRecordFileName() + L" in " + configDir +
           L", and a file named gpu-cuda-before-*.txt in that folder counts as a note it cannot read; move any "
           L"gpu-cuda-before-*.txt file out of that folder, keep " + CudaRecordFileName() +
           L" where it is, and try again";
}

// The sentence one refused row gets in a result, after "<application> - ". Most refusals are a fixed
// sentence; the three that have to NAME an entry are built from the row's own answer rather than fetched
// from an enum.
inline std::wstring CudaRowRefusalText(const CudaRowResult& r) {
    if (r.refusal == CudaRefusal::NvidiaManagesIt)
        return FormatCudaNvidiaManagesLine(r.profileName, r.otherApps);
    if (r.refusal == CudaRefusal::NameAlreadyInUse) return FormatCudaNameInUseLine(r.profileName, r.otherApps);
    if (r.refusal == CudaRefusal::MembershipUnknown) return FormatCudaMembershipUnknownLine(r.profileName);
    return CudaRefusalReason(r.refusal);
}

// 🔴 IT ASKS, IT DOES NOT PROMISE (E9). An application NVIDIA already keeps its own settings entry for is
// left to NVIDIA, and one it has no entry for needs one made - which NVIDIA can still refuse. Neither is
// known until the write is tried, long after this question was answered, so the question must be true both
// ways. Which rows really changed is the result's job to say.
inline std::wstring FormatCudaConfirmLine(bool planned, const std::wstring& targetName) {
    if (!planned) return std::wstring();
    return L"This also asks NVIDIA to use " + targetName +
           L" for the CUDA work of any of them that use CUDA. Game Optimizer only does that through a "
           L"settings entry of its own, so a program NVIDIA already keeps its own settings for - Chrome, "
           L"Edge and others - is left to NVIDIA Control Panel. The result says which ones changed.";
}

// The same line for Remove, and it exists because Remove CHANGES NVIDIA's settings exactly as Apply does:
// it takes away the entry Apply made. Its old question spoke only of Windows' GPU preference.
//
// `count` is the ticked applications the note names, so there is really something to take away; zero says
// nothing at all. `settingOff` adds the one sentence the check box's state makes necessary: the box gates
// Apply's writes only, so a change made while it was ticked can still be undone after it is cleared.
// `blockedReason` is not empty when the CUDA half of this Remove cannot run at all - and then NOTHING is
// removed, because taking the Windows assignment away while the CUDA entry has to stay would leave the
// user no way back at all (E4).
inline std::wstring FormatCudaRestoreConfirmLine(size_t count, bool settingOff,
                                                 const std::wstring& blockedReason) {
    if (!blockedReason.empty())
        return L"Nothing is removed at all: " + blockedReason +
               L". Game Optimizer does not take a GPU assignment away while the NVIDIA settings entry it "
               L"made for it has to stay, because that would leave no way back. Try again when it can.";
    if (count == 0) return std::wstring();
    std::wstring s = L"This also takes away the NVIDIA settings entry Game Optimizer made for " +
                     std::to_wstring(count) +
                     (count == 1 ? L" of them, so which GPU CUDA uses goes back to what it was before."
                                 : L" of them, so which GPU CUDA uses goes back to what it was before.") +
                     L" The result says which ones changed.";
    if (settingOff)
        s += L" Game Optimizer does that whether or not \"Also set which GPU CUDA uses\" is ticked, so a "
             L"change it made can always be undone.";
    return s;
}

// What a result dialog says about CUDA. Every list is already formatted by the caller, the way
// FormatRestoreLines takes its lines, so the twelve-line cap lives in one place in the window.
struct CudaResultText {
    size_t changed = 0;                // rows whose CUDA GPU was set (Apply) or put back (Remove)
    // Apply: rows that already held exactly that value. Remove: rows whose NVIDIA settings entry is gone
    // already - the state the note asks for is true without anything being done.
    size_t already = 0;
    // Remove only: rows no note names, so this product never changed their CUDA setting and has nothing to
    // take away. Counted and said in one sentence rather than listed - a Remove over forty applications
    // that pre-date this feature would otherwise be forty lines of "nothing happened". It is still SAID,
    // because silence here reads exactly like an undo that quietly failed.
    size_t noRecord = 0;
    // 🔴 R5-1: Remove rows whose entry held OTHER settings too, so only Game Optimizer's own CUDA setting
    // was taken out and the entry itself was left standing. They are counted in `changed` as well - they
    // ARE a successful restore - and this says which of them happened the other way.
    size_t keptEntry = 0;
    // 🔴 R6-7: Remove rows whose line already said "the entry is ours and holds nothing of ours" - an
    // earlier Remove took our setting out of an entry it had to leave standing. Nothing to take away,
    // nothing failed, and the line was KEPT. Counted and said, because silence here reads exactly like an
    // undo that quietly did nothing.
    size_t nothingOfOurs = 0;
    // 🔴 R5-5: Remove rows with no line of ours, for which NVIDIA still holds an entry named for this
    // product. Nothing was done about them and nothing was blocked; they are named so the user can.
    bool anyOrphan = false;
    std::wstring orphanLines;
    std::wstring targetName;           // the GPU CUDA will use; empty on Remove
    // A refusal that stopped the WHOLE run before any row - no driver, no settings session, three NVIDIA
    // cards, a note nobody could read. Said once rather than repeated against every application.
    std::wstring wholeReason;
    bool anyRefused = false;
    std::wstring refusedLines;
    bool recordStarted = false;
    std::wstring recordPath;
    // 🔴 E5: a change this product made that could not be taken back out. It is the loudest thing either
    // dialog can say, and it names the applications.
    bool anyUnresolved = false;
    std::wstring unresolvedLines;
    // Remove only: the entry went and the line could not be taken out of the note, so a later Remove will
    // meet it again. Apply has no such state any more - the line goes down BEFORE the save, so a row whose
    // line could not be written is taken back out of the driver instead of left unrecorded.
    bool anyUnrecorded = false;
    std::wstring unrecordedLines;
};

// The sentence E5 requires, in both dialogs: plain, first among the bad news, and it names the record.
inline std::wstring FormatCudaUnresolvedLines(const CudaResultText& t) {
    if (!t.anyUnresolved) return std::wstring();
    std::wstring s = L"\r\n\r\nWhich GPU CUDA uses was changed and could NOT be put back for:" + t.unresolvedLines +
                     L"\r\nNothing after that was tried, for the GPU setting or for CUDA.";
    if (!t.recordPath.empty())
        s += L" What Game Optimizer made for each one is in:\r\n    " + t.recordPath +
             L"\r\nRemove assignment reads that file and can try again.";
    return s;
}

inline std::wstring FormatCudaApplyLines(const CudaResultText& t) {
    std::wstring s;
    if (!t.wholeReason.empty())
        s += L"\r\n\r\nWhich GPU CUDA uses was not changed for any of them: " + t.wholeReason + L".";
    if (t.changed > 0)
        s += L"\r\n\r\n" + std::to_wstring(t.changed) + L" of them will use " + t.targetName + L" for CUDA as well.";
    if (t.already > 0)
        s += L"\r\n\r\n" + std::to_wstring(t.already) +
             (t.already == 1 ? L" already used it for CUDA, so nothing was written there."
                             : L" already used it for CUDA, so nothing was written for those.");
    s += FormatCudaUnresolvedLines(t);
    if (t.anyRefused) s += L"\r\n\r\nWhich GPU CUDA uses was not changed for:" + t.refusedLines;
    if (t.recordStarted)
        s += L"\r\n\r\nThe NVIDIA settings entries Game Optimizer made are listed in:\r\n    " + t.recordPath +
             L"\r\nRemove assignment reads that file and takes them away again.";
    return s;
}

inline std::wstring FormatCudaRemoveLines(const CudaResultText& t) {
    std::wstring s;
    if (!t.wholeReason.empty())
        s += L"\r\n\r\nWhich GPU CUDA uses was not put back for any of them: " + t.wholeReason +
             L", so no GPU assignment was removed either.";
    if (t.changed > 0)
        s += L"\r\n\r\nWhich GPU CUDA uses was put back for " + std::to_wstring(t.changed) + L" of them.";
    if (t.already > 0)
        s += L"\r\n\r\n" + std::to_wstring(t.already) +
             (t.already == 1 ? L" of them no longer has NVIDIA settings of its own, so there was nothing to "
                               L"put back."
                             : L" of them no longer have NVIDIA settings of their own, so there was nothing "
                               L"to put back.");
    // 🔴 R5-1: SAY WHICH OF THE TWO RESTORES HAPPENED. "The entry was taken away" and "only our setting was
    // taken out of an entry that is still there" are different states of the user's driver database, and a
    // result that called both "put back" would leave the second invisible.
    // 🔴 R6-7: AND IT SAYS WHAT HAPPENS TO THE NOTE, because the note is why the entry is reachable again.
    // The line is kept with its value emptied, so assigning that application again may use that entry once
    // more; spending the line used to make the entry unreachable forever.
    if (t.keptEntry > 0)
        s += L"\r\n\r\n" + std::to_wstring(t.keptEntry) +
             (t.keptEntry == 1
                  ? L" of those had other NVIDIA settings in the same entry, so only Game Optimizer's own "
                    L"CUDA setting was taken out of it and the entry itself was left standing. Game "
                    L"Optimizer still notes that entry as one it made, so assigning that application again "
                    L"can use it."
                  : L" of those had other NVIDIA settings in the same entry, so only Game Optimizer's own "
                    L"CUDA setting was taken out of each and those entries were left standing. Game "
                    L"Optimizer still notes those entries as ones it made, so assigning those applications "
                    L"again can use them.");
    if (t.nothingOfOurs > 0)
        s += L"\r\n\r\n" + std::to_wstring(t.nothingOfOurs) +
             (t.nothingOfOurs == 1
                  ? L" of them already had nothing of Game Optimizer's in its NVIDIA settings entry, so "
                    L"there was nothing to put back and that entry was left alone."
                  : L" of them already had nothing of Game Optimizer's in their NVIDIA settings entries, so "
                    L"there was nothing to put back and those entries were left alone.");
    if (t.noRecord > 0)
        s += L"\r\n\r\n" + std::to_wstring(t.noRecord) +
             L" of them never had their CUDA GPU changed here, so there was nothing to put back.";
    if (t.anyOrphan)
        s += L"\r\n\r\nGame Optimizer has no note of changing which GPU CUDA uses for the applications "
             L"below, and NVIDIA still holds a settings entry named for Game Optimizer for each of them. "
             L"Nothing was done to those entries:" + t.orphanLines;
    s += FormatCudaUnresolvedLines(t);
    if (t.anyRefused)
        s += L"\r\n\r\nWhich GPU CUDA uses was not put back for the applications below, so their GPU "
             L"assignment was left in place and Remove assignment can try again:" + t.refusedLines;
    if (t.anyUnrecorded)
        s += L"\r\n\r\nPut back, but the record of it could not be updated, so Remove assignment may try "
             L"these again:" + t.unrecordedLines;
    return s;
}

}  // namespace cd
