#pragma once

#include <cwctype>
#include <string>
#include <vector>

#include "config.h"
#include "topology.h"

namespace cd {

bool IEquals(const std::wstring& a, const std::wstring& b);

// Pure helpers behind the "Add mask..." / "Remove mask" actions on the Core map page.
// Nothing here places a thread anywhere; these only decide whether a NAME for a set of
// logical processors is acceptable, and what to tell the user when one survives a topology
// change. See docs\superpowers\specs\2026-08-31-custom-masks-design.md section 5.

// Leading and trailing whitespace stripped. The caller stores THIS form, so a name typed
// as "  Streaming " cannot later fail to match "Streaming".
inline std::wstring TrimMaskName(const std::wstring& raw) {
    size_t begin = 0;
    size_t end = raw.size();
    while (begin < end && std::iswspace(raw[begin])) ++begin;
    while (end > begin && std::iswspace(raw[end - 1])) --end;
    return raw.substr(begin, end - begin);
}

// THE CAPTION OF THE "Add mask..." ROW THE PROFILES PAGE PUTS INSIDE ITS TWO MASK COMBOS,
// so a mask can be created without leaving the page for the Core map.
//
// It lives HERE, beside the validator, rather than in settings.cpp because both sides need the
// same string: the UI adds the row with it, and ValidateNewMaskName below refuses it as a mask
// NAME. Two literals would drift apart the first time the wording changed, and the drift would
// be invisible - the row would still draw and the name would silently become creatable again.
//
// The row itself is identified by its combo ITEM DATA, never by this text (settings.cpp,
// kMaskItemAdd). Reserving the name is the second belt: a config written by hand, or by a build
// older than this one, can still contain a mask called "Add mask...", and the item-data test is
// what keeps THAT mask distinguishable from the action.
inline const wchar_t* const kAddMaskEntryCaption = L"Add mask...";

enum class MaskNameProblem {
    None,                 // acceptable
    Empty,                // nothing left after trimming
    Duplicate,            // case-insensitively equal to a mask that already exists
    ReservedDerivedName,  // a name DeriveMasks emits on this or any other machine
};

// Checked in this order: Empty, Duplicate, ReservedDerivedName (which now covers the
// "Add mask..." combo caption as well as the derived vocabulary - see kAddMaskEntryCaption).
//
// `existing`              - the current mask list (Config::masks).
// `derivedForThisMachine` - DeriveMasks output for the live topology.
//
// Duplicate is case-insensitive because FindMask and OnResetMask compare names with IEquals:
// "cache" and "Cache" are one mask to the rest of the program and must be one mask here.
//
// ReservedDerivedName exists for the machine the user does NOT have yet. A custom "Cache"
// created on hardware with no cache domain would collide with the derived "Cache" the day
// the config moves to an X3D part, and the merge would then discard the user's work.
// Refusing the name today is far kinder than resolving that collision later. So the check
// covers EVERY machine, not only this one: `derivedForThisMachine` catches the live names,
// and IsDerivableMaskName (topology.h) refuses the whole vocabulary DeriveMasks can ever
// emit - All, All no SMT, Cache, CCDn, P-cores, E-cores and their " no SMT" variants -
// whatever the hardware under this config happens to be today.
inline MaskNameProblem ValidateNewMaskName(const std::wstring& raw,
                                           const std::vector<Mask>& existing,
                                           const std::vector<Mask>& derivedForThisMachine) {
    const std::wstring name = TrimMaskName(raw);
    if (name.empty()) return MaskNameProblem::Empty;

    for (const Mask& mask : existing) {
        if (IEquals(mask.name, name)) return MaskNameProblem::Duplicate;
    }
    // The Profiles page's mask combos carry kAddMaskEntryCaption as a ROW. A mask with that
    // exact name would sit in the same list, drawn identically to the action, so the name is
    // not the user's to take. ReservedDerivedName rather than a new problem code: the meaning
    // the caller has to convey - "that name belongs to Game Optimizer, choose another" - is
    // exactly what that code already means, and every existing switch already handles it.
    if (IEquals(kAddMaskEntryCaption, name)) return MaskNameProblem::ReservedDerivedName;
    for (const Mask& mask : derivedForThisMachine) {
        if (IEquals(mask.name, name)) return MaskNameProblem::ReservedDerivedName;
    }
    // Machine-independent: refuses every name DeriveMasks can emit on ANY hardware, so a
    // custom "Cache" cannot be created on a single-domain machine and collide the day the
    // config moves to an X3D part. Same problem code - the user-facing meaning is the same.
    if (IsDerivableMaskName(name)) return MaskNameProblem::ReservedDerivedName;
    return MaskNameProblem::None;
}

// Names of every profile whose game mask or heavy mask is `maskName` (case-insensitive),
// in config order, each profile listed once even when it uses the mask for both roles.
// A mask in use must not be removed silently: those profiles would drop onto a default
// mask, which is exactly the harm a custom mask was created to avoid.
inline std::vector<std::wstring> ProfilesReferencingMask(const Config& c,
                                                         const std::wstring& maskName) {
    std::vector<std::wstring> names;
    for (const Profile& p : c.profiles) {
        if (IEquals(p.gameMask, maskName) || IEquals(p.heavyMask, maskName)) {
            names.push_back(p.name);
        }
    }
    return names;
}

// Whether the "Remove mask" button is live for the mask the Core map is editing - and the
// SAME predicate OnRemoveMask runs before it acts, so what the button allows and what the
// command does cannot drift apart. `derivedForThisMachine` is DeriveMasks output for the live
// topology, exactly as ValidateNewMaskName receives it.
//
// Refused for a null selection (no mask chosen, or a name the working config does not hold),
// for a mask still flagged derived, and for any mask whose NAME a derived mask for this machine
// carries (case-insensitive, as FindMask and the merge compare). The name check is not
// redundant with the flag: the Core map's write-back clears `derived` on any hand edit, so an
// edited "CCD0" reports derived == false. Removing it would NOT make it reappear - main.cpp
// runs the merge that re-derives masks only when the topology signature changes, so on the
// same hardware the name is gone for good, and Add then refuses it as ReservedDerivedName.
// A name DeriveMasks emits for this machine is therefore never removable, whatever its flag
// says; "Reset to detected" is the action for an edited derived mask.
inline bool CanRemoveMask(const Mask* selected, const std::vector<Mask>& derivedForThisMachine) {
    if (!selected || selected->derived) return false;
    for (const Mask& mask : derivedForThisMachine) {
        if (IEquals(mask.name, selected->name)) return false;
    }
    return true;
}

// The extra sentence for the topology-changed message box when the merge kept
// `preservedCustomCount` hand-made masks. Empty when nothing was kept, so the caller can
// append it unconditionally. Preservation is deliberately NOT silent (operator decision:
// "Keep, and warn"): the CPU Set Ids inside a kept mask were chosen against the OLD
// topology and may now name different processors.
inline std::wstring TopologyChangedPreservedSentence(size_t preservedCustomCount) {
    if (preservedCustomCount == 0) return std::wstring();
    if (preservedCustomCount == 1) {
        return L"1 custom mask you created was kept, but the processor numbers inside it may "
               L"now refer to different cores - open the Core map and check it.";
    }
    return std::to_wstring(preservedCustomCount) +
           L" custom masks you created were kept, but the processor numbers inside them may "
           L"now refer to different cores - open the Core map and check each one.";
}

}  // namespace cd
