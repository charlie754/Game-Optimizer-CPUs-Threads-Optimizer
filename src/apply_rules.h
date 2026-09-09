// Game Optimizer - the four rules the restore journal actually rests on, as PURE
// predicates with no Win32 and no state.
//
// WHY THIS FILE EXISTS. Every one of these rules used to live as an inline condition inside
// Engine::Impl::Tick, which is welded to Win32 - a process snapshot, the foreground window,
// the applier - and is therefore not reachable from the unit harness at all. A three-vendor
// review of v0.4.3 found that three of the four were WRONG, and the reason none of them had
// ever failed a test is that none of them could be tested. The engine now calls these and
// nothing else, so the rule and the check are the same sentence in two places.
//
// The rules, in the order a tick meets them:
//
//   1. Does this pid need a recovery record before a setter may touch it?
//   2. May this assignment reach the setter at all?
//   3. Must this pid be cleared when it stops being wanted?
//   4. May its recovery record be dropped now?
//   5. Is this pid's assignment still the one that is wanted?
//
// Rule 5 is numbered last because it arrived last, NOT because a tick meets it last: it is
// the FIRST question the apply loop asks, ahead of rules 1 and 2. The numbers are the
// order they were written, and renumbering them would silently re-label four tests that
// name the rule they cover.
#pragma once
#include <algorithm>
#include <string>
#include <vector>

#include "applier.h"

namespace cd {

// ---- 1 -----------------------------------------------------------------------------
// A recovery record is needed whenever the journal does not already carry one for this
// pid. THE SECOND HALF OF THAT SENTENCE IS THE BUG THIS REPLACES: the engine used to ask
// only "is this pid new to the applied map", so a pid whose FIRST assignment failed - which
// leaves a map entry behind, with blocked = true, and removes its journal entry - looked
// like an already-recorded pid on the next attempt. A later attempt with a different mask
// name then succeeded and was never journalled, and the assignment it made could not be
// undone after an unclean exit.
//
// haveRecord         an `applied` entry exists for this pid
// recordIsJournalled that entry's assignment is the one currently written to the journal
inline bool NeedsRecoveryRecord(bool haveRecord, bool recordIsJournalled) {
    return !haveRecord || !recordIsJournalled;
}

// ---- 2 -----------------------------------------------------------------------------
// 🔴 THE BLOCKER. An assignment may only reach the setter once its recovery record is on
// disk. The journal was always written FIRST, which established the ORDER; it never
// established that the write SUCCEEDED, because JournalAddMany returned void and merely
// logged. A failed write therefore still pinned the process, with nothing on disk to undo
// it - which is precisely the state the journal exists to make unreachable.
//
// The safe half of a failed write is that the process is left on ALL CORES. That is the
// machine's default, it is what the user had before the app ran, and it is recoverable by
// doing nothing. The unsafe half - a pinned process no launch can find - is the one this
// returns false for.
inline bool MayApplyAssignment(bool needsRecoveryRecord, bool recordWriteSucceeded) {
    return !needsRecoveryRecord || recordWriteSucceeded;
}

// ---- 3 -----------------------------------------------------------------------------
// A pid that leaves the desired set must be cleared when an assignment of ours ever landed
// on it - NOT when the last attempt happened to succeed.
//
// The old test was "the last attempt was not blocked", and it is wrong in the one case that
// matters: a process that was successfully moved to Cache and then FAILED to move to Freq
// still carries Cache. Its record says blocked, so the old test skipped the clear, erased
// the record and dropped the journal entry - stranding a live process on half the machine
// with nothing left that knew about it.
inline bool NeedsClearOnLeaving(bool everApplied) {
    return everApplied;
}

// ---- 4 -----------------------------------------------------------------------------
// A recovery record may only be dropped once there is provably nothing left to recover.
//
//   nothing ever landed            -> nothing to undo; drop
//   the clear succeeded            -> nothing left; drop
//   the process is gone / recycled -> nothing left to undo, and a recycled pid must never
//                                     be cleared on a later run either; drop
//   the clear FAILED               -> the assignment is still on a live process. KEEP.
//
// Keeping it is what makes the failure survivable: the next tick tries the clear again, and
// if the app dies first, the next launch's recovery finds the record and undoes it.
inline bool MayDropRecoveryRecord(bool everApplied, ApplyResult clearResult) {
    if (!everApplied) return true;
    return clearResult == ApplyResult::Ok || clearResult == ApplyResult::Gone;
}

// ---- 5's two helpers, defined first because rule 5 is written in terms of them ------
// A mask's ids as a SET: ascending, no duplicates. Not cosmetic - it is what makes the
// comparison in rule 5 answer a question about processors rather than about a vector.
//
// config.ini is documented as hand-editable and ParseMaskValue keeps a mask's ids in FILE
// ORDER, so "1,3,8" and "8,1,3" both reach ResolveMask verbatim. topology.cpp's
// MaskNameForIds already normalises both sides for exactly this reason; this is the same
// rule at the other end of the same data.
inline std::vector<ULONG> NormalizedMaskIds(std::vector<ULONG> ids) {
    std::sort(ids.begin(), ids.end());
    ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
    return ids;
}

// Do these two id lists name the same processors? The literal compare FIRST, deliberately:
// every id list the app itself produces is already sorted and unique (DeriveMasks and the
// Core map's IdsForLps both end in SortedUnique), so the fast path is the only one a normal
// config ever takes and it allocates nothing. The normalising path costs two small vectors
// and is reached only when the lists genuinely differ - i.e. on a real edit, or on a
// hand-written config whose ids are out of order.
inline bool SameMaskIds(const std::vector<ULONG>& a, const std::vector<ULONG>& b) {
    if (a == b) return true;
    return NormalizedMaskIds(a) == NormalizedMaskIds(b);
}

// ---- 5 -----------------------------------------------------------------------------
// 🔴 A MASK IS A NAME AND A SET OF PROCESSORS, AND THE GATE USED TO COMPARE ONLY THE NAME.
//
// The apply loop must not re-issue an identical mask four times a second, so it skips any
// pid whose record already says what is wanted. Until v0.4.4 that test was
//
//     if (haveRecord && a->second.maskName == want) continue;
//
// and AppliedRec stored no ids at all, so "what is wanted" could only ever mean the NAME.
// Edit mask "Cache" on the Core map from LPs 0-15 down to 0-7 and KEEP THE NAME - which is
// the ordinary edit, not an exotic one - and every already-governed process stays on the
// old ids for the life of the run. The UI says the profile is on Cache, Cache now means
// 0-7, and the processes are on 0-15. Nothing in the product can notice: the CPU-Set
// getters echo stored intent, never effective placement, so there is no reading anywhere
// that disagrees with the wrong one.
//
// Both halves matter and they pull opposite ways, so both are asserted in the tests:
//   * CONTENT changed, name the same  -> re-issue, or the edit never reaches the machine;
//   * NOTHING changed                 -> do NOT re-issue, or every governed process is
//                                        re-pinned at the poll rate. Under extreme game
//                                        mode that is ~200 setter calls every 250 ms.
//
// haveRecord    an `applied` entry exists for this pid
// recordedMask  / recordedIds  what that entry was last asked to move to
// wantMask      / wantIds      what this tick wants, resolved through the CURRENT config
inline bool NeedsReissue(bool haveRecord,
                         const std::wstring& recordedMask,
                         const std::vector<ULONG>& recordedIds,
                         const std::wstring& wantMask,
                         const std::vector<ULONG>& wantIds) {
    if (!haveRecord) return true;
    if (recordedMask != wantMask) return true;
    return !SameMaskIds(recordedIds, wantIds);
}

}  // namespace cd
