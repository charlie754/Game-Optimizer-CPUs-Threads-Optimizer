// Game Optimizer - what Apply and Remove do to each ticked row, as PURE logic.
//
// WHY THIS FILE EXISTS. Round 5 of adversarial review found the window's two write loops right on the
// ordinary path and wrong on the failure paths - a refused row kept a stale GPU label, a read failure was
// reported as another program's change, a no-op was counted as a change, and the restore file was trimmed
// only after the last write. None of that could be tested while the loops lived inside gpuwindow.cpp. Here
// every registry and file operation is passed in, so the unit suite drives each failure the window can meet.
#pragma once

#include <functional>
#include <string>
#include <vector>

#include "gpu_pref.h"

namespace cd {

// Why a guarded write did not happen, in the words the result dialog uses. `error` is the Windows error
// code the write reported, or 0.
inline std::wstring GuardedReason(GuardedWriteResult g, unsigned long error) {
    const std::wstring code = error ? L" (error " + std::to_wstring(error) + L")" : std::wstring();
    switch (g) {
        case GuardedWriteResult::Written:
            return std::wstring();
        case GuardedWriteResult::Changed:
            return L"it changed after it was read, so it was left alone";
        case GuardedWriteResult::CheckUnreadable:
            return L"it could not be read again to check that nothing had changed, so it was left alone";
        case GuardedWriteResult::NotCommitted:
            // [M] txprobe2: a write by another handle to this same value between the check and the commit made
            // CommitTransaction fail with 6704, "the Transaction has already been aborted"; [M] txprobe3: so did a
            // write to ANY other value of this key after the transacted change. A transaction timing out is aborted
            // too, so the cause is said as likely, never as certain.
            if (error == 6704)
                return L"Windows cancelled the change (error 6704) - most likely another program changed Windows' GPU "
                       L"settings at the same moment - so it was left alone";
            return L"Windows did not complete the change" + code + L", so it was left alone";
        case GuardedWriteResult::Unavailable:
            return L"Windows could not start a protected change" + code + L", so it was left alone";
        case GuardedWriteResult::NoLongerAllowed:
            // v0.5.7: asked only for a row Auto assign ticked, whose check is Auto assign's own rule read again inside the
            // write. That check says no for a sibling pinned to the main GPU, AND for a read of Windows' GPU preferences
            // that could not see everything or another version's value that could not be read; the result does not say
            // which, so the text names both and asserts neither. [A] which one a real refusal was is not recorded.
            return L"another version of it may now be pinned to the main GPU, or Windows' GPU settings could not all "
                   L"be read again, so it was left alone; tick it by hand to change it anyway";
        default:
            return L"Windows refused the change" + code + L", so it was left alone";
    }
}

// One ticked row as pass one read it. Only a row whose value was readable and editable gets this far.
struct GpuEditItem {
    std::wstring exePath;
    bool present = false;
    std::wstring existing;
    // Ticked by Auto assign, not by hand (v0.5.7): its write is checked against Auto assign's rule once more, inside the
    // guarded write. A hand tick is the user's override and is never checked that way.
    bool automatic = false;
};

enum class GpuEditOutcome {
    Done,          // changed, and the change reads back
    AlreadyDone,   // there was nothing to change: nothing written, nothing recorded
    Refused,       // not written - see the reason
    Unconfirmed,   // committed, but what reads back is not what was written
    NotAttempted   // never tried, because the restore file stopped recording
};

struct GpuEditResult {
    GpuEditOutcome outcome = GpuEditOutcome::NotAttempted;
    std::wstring reason;      // empty only for Done
    std::wstring choiceKey;   // what the row shows now: PreferenceChoiceKey of what was read, or UnreadableChoiceKey
    std::wstring choiceText;  // the same read's GpuChoiceText - what the NEXT Apply or Remove compares with (PrepareVerdict)
    bool recorded = false;    // the row's previous value is in the restore file
};

// 🔴 WHAT THE CUDA HALF OF ONE ROW ANSWERS, AND WHY IT IS A THREE-WAY ANSWER (v0.5.8, E4 and E5).
//   Go       - carry on with this row's registry change, and with the rest of the run.
//   SkipRow  - leave this row's registry value exactly where it is, and say why. Remove uses it: taking
//              the Windows GPU assignment away while the NVIDIA settings entry has to stay would grey out
//              Remove assignment itself, and with it the only way back this product offers.
//   StopRun  - a change reached NVIDIA's database and could NOT be taken back out. NO LATER APPLICATION is
//              attempted at all, for the GPU setting or for CUDA: a run that has lost track of one
//              application's CUDA setting may not start on the next.
enum class GpuRowGate { Go, SkipRow, StopRun };

// Everything that touches Windows, passed in.
struct GpuEditOps {
    // GuardedWriteGpuPreference, with the error it reports. `automatic` is the item's own flag (GpuEditItem::automatic).
    std::function<GuardedWriteResult(const std::wstring& exePath, bool expectPresent, const std::wstring& expectValue,
                                     bool deleteValue, const std::wstring& newValue, unsigned long& error,
                                     bool automatic)> write;
    // ReadGpuPreference
    std::function<bool(const std::wstring& exePath, std::wstring& value, bool& unreadable)> read;
    // RecordGpuRestoreRow
    std::function<bool(const GpuPreferenceBefore& row)> record;
    // called after every row; may be empty
    std::function<void()> afterRow;
    // 🔴 THE CUDA HALF OF ROW `index`, AND WHEN IT RUNS IS THE WHOLE OF E4. On REMOVE it is called BEFORE
    // the registry change, because the undo button greys out the moment the assignment is gone; on APPLY
    // it is called after a row whose Windows GPU preference IS what this run wants - one just written
    // (Done) or one that already held it (AlreadyDone, R5-3) - because CUDA follows a GPU preference that
    // really landed and may never fail one. `reason` is what the result says about the row when the answer
    // is not Go. May be empty, and then nothing CUDA-related happens at all.
    std::function<GpuRowGate(size_t index, std::wstring& reason)> cudaRow;
};

// Apply (`removing` false: merge `targetKey` in) or Remove (`removing` true: strip this feature's fields, and
// delete the value when nothing else is left) for every item, in order.
//
// 🔴 THE ORDER PER ROW IS THE RECOVERY GUARANTEE: guarded write -> the row's previous value recorded in the
// restore file -> read back -> the next row. A row is recorded only after its change was committed, so a run
// stopped at ANY point leaves a restore file listing nothing that did not change. If recording fails, no
// further row is changed - its change would have nothing to put it back.
//
// 🔴 AND THE CUDA HALF SITS AT OPPOSITE ENDS OF THAT ORDER FOR THE TWO ACTIONS (v0.5.8). On APPLY it runs
// AFTER a row whose GPU preference is what this run wants - Done or AlreadyDone (R5-3) - because CUDA
// follows a GPU preference that really landed and may never fail one. On REMOVE it runs BEFORE the row is
// touched, because the button that could try again is enabled only while the row still HAS an assignment
// (E4). Both go through ops.cudaRow.
//
// Every row's choiceKey is read from the registry after it is handled, never assumed - so a row another
// program changed shows what that program wrote, and a row that could not be read says so.
inline std::vector<GpuEditResult> RunGpuEdits(const std::vector<GpuEditItem>& items, bool removing,
                                             const std::wstring& targetKey, const GpuEditOps& ops) {
    std::vector<GpuEditResult> out(items.size());
    bool stopped = false;
    // Why nothing after a stop was tried. The restore file is the original reason; v0.5.8 adds the one
    // E5 names, and the two must not be confused in a result.
    std::wstring stopReason = L"not tried: the restore file could not record the change before it, so nothing "
                              L"more was changed";
    for (size_t k = 0; k < items.size(); ++k) {
        const GpuEditItem& it = items[k];
        GpuEditResult& res = out[k];
        if (stopped) {
            res.outcome = GpuEditOutcome::NotAttempted;
            res.reason = stopReason;
            // What is there NOW, not what pass one read (adversarial review, round 6).
            std::wstring cur;
            bool unr = false;
            const bool pres = ops.read(it.exePath, cur, unr);
            res.choiceKey = unr ? UnreadableChoiceKey() : (pres ? PreferenceChoiceKey(cur) : std::wstring());
            res.choiceText = GpuChoiceText(pres, cur, unr);
            continue;
        }

        const std::wstring after = removing ? StripGpuAssignment(it.existing)
                                            : MergeGpuPreferenceValue(it.existing, targetKey);
        const bool deleting = removing && after.empty();
        const bool nothingToDo = removing ? (!it.present || after == it.existing)
                                          : (it.present && after == it.existing);

        std::wstring now;
        bool unreadable = false;
        bool present = false;
        const auto show = [&](GpuEditResult& r) {
            r.choiceKey = unreadable ? UnreadableChoiceKey() : (present ? PreferenceChoiceKey(now) : std::wstring());
            r.choiceText = GpuChoiceText(present, now, unreadable);
        };

        // 🔴 E4: ON REMOVE THE CUDA UNDO GOES FIRST, AND A ROW IT REFUSES KEEPS ITS GPU ASSIGNMENT.
        // Remove assignment is enabled on rows that HAVE an assignment (RemovableCount), so stripping the
        // assignment of a row whose NVIDIA settings entry could not be taken away would grey out the only
        // button that could ever try again - which is exactly the trap Council round 3 found.
        if (removing && ops.cudaRow) {
            std::wstring why;
            const GpuRowGate gate = ops.cudaRow(k, why);
            if (gate != GpuRowGate::Go) {
                res.outcome = GpuEditOutcome::Refused;
                res.reason = why;
                present = ops.read(it.exePath, now, unreadable);
                show(res);
                if (gate == GpuRowGate::StopRun) {
                    stopped = true;
                    stopReason = why;
                }
                if (ops.afterRow) ops.afterRow();
                continue;
            }
        }

        if (nothingToDo) {
            // NOTHING TO CHANGE IS NOT A CHANGE (adversarial review, round 5): not counted, not written, not
            // recorded - and still looked at again, so a value changed meanwhile is not called "already done".
            present = ops.read(it.exePath, now, unreadable);
            if (unreadable) {
                res.outcome = GpuEditOutcome::Refused;
                res.reason = GuardedReason(GuardedWriteResult::CheckUnreadable, 0);
            } else if (present != it.present || (present && now != it.existing)) {
                res.outcome = GpuEditOutcome::Refused;
                res.reason = GuardedReason(GuardedWriteResult::Changed, 0);
            } else {
                res.outcome = GpuEditOutcome::AlreadyDone;
                res.reason = removing ? L"it has no GPU assignment to remove, so nothing was written"
                                      : L"it already has exactly that setting, so nothing was written";
            }
            show(res);
        } else {
            unsigned long error = 0;
            const GuardedWriteResult g = ops.write(it.exePath, it.present, it.existing, deleting, after, error, it.automatic);
            if (g != GuardedWriteResult::Written) {
                res.outcome = GpuEditOutcome::Refused;
                res.reason = GuardedReason(g, error);
                present = ops.read(it.exePath, now, unreadable);
                show(res);
            } else {
                GpuPreferenceBefore b;
                b.exePath = it.exePath;
                b.present = it.present;
                b.value = it.existing;
                res.recorded = ops.record(b);
                if (!res.recorded) stopped = true;

                present = ops.read(it.exePath, now, unreadable);
                if (unreadable) {
                    res.outcome = GpuEditOutcome::Unconfirmed;
                    res.reason = L"changed, but it could not be read back";
                } else if (deleting ? present : (!present || now != after)) {
                    res.outcome = GpuEditOutcome::Unconfirmed;
                    res.reason = L"changed, but a different value reads back";
                } else {
                    res.outcome = GpuEditOutcome::Done;
                }
                show(res);
            }
        }
        // 🔴 E5: ON APPLY THE CUDA HALF FOLLOWS A GPU WRITE THAT REALLY LANDED, AND A CHANGE IT COULD NOT
        // TAKE BACK OUT STOPS THE WHOLE RUN - not just the CUDA half, which was a blocker in adversarial review, round 3:
        // the CUDA path stopped and this loop carried on writing later applications' GPU preferences while
        // the result said "nothing after that was tried".
        //
        // 🔴 R5-3, AND IT IS THE FINDING THE OPERATOR WOULD HAVE MET FIRST (Council round 4, BOTH seats).
        // AlreadyDone is a row whose Windows GPU pin ALREADY equals what this Apply intends - which is every
        // application v0.5.6 and v0.5.7 pinned. Gating the CUDA half on Done alone meant "GPU assignment
        // shall also change the CUDA dedication GPU" did nothing at all for those, silently, and a user
        // could only get it by unpinning and pinning again. The rule the gate is really enforcing is "the
        // Windows GPU preference is what this run wants it to be", and BOTH outcomes satisfy it; Refused,
        // Unconfirmed and NotAttempted do not, and still drag no CUDA change.
        if (!removing && ops.cudaRow &&
            (res.outcome == GpuEditOutcome::Done || res.outcome == GpuEditOutcome::AlreadyDone)) {
            std::wstring why;
            if (ops.cudaRow(k, why) == GpuRowGate::StopRun) {
                stopped = true;
                stopReason = L"not tried: which GPU CUDA uses was changed for an application before it and "
                             L"could not be put back, so nothing more was changed";
            }
        }
        if (ops.afterRow) ops.afterRow();
    }
    return out;
}

// ---- Pass one: may a ticked row be changed at all? --------------------------------------------------------------
//
// `listedChoice` is the row's GPU choice as the list showed it - GpuChoiceText of what the tab read, or of what the
// tab's last change read back (GpuEditResult::choiceText) - and `present`, `value` and `unreadable` are ReadGpuPreference
// now. In this order: a value that cannot be read; a GPU choice that is not the one the list showed; a value this does
// not edit.
//
// 🔴 THE LIST IS WHAT THE USER SAID YES TO (Council review, v0.5.6). Each ticked row - ticked by hand or by Auto assign,
// from the list - used to be prepared from a fresh read, and that read became the guarded write's expected value. So an
// application the user pinned to the main GPU in Windows Settings while this tab was open was moved to the background
// GPU: the guard compared with the NEW pin and found nothing changed. Only the GPU fields are compared - Windows rewrites
// AppStatus and AutoHDREnable on its own - so a change to those never refuses a row. The guarded write keeps its own
// comparison, which covers the time from this read to the write; this one covers the time from the list to this read.
enum class GpuPrepareVerdict { Ready, Unreadable, ChangedSinceListed, NotEditable };

inline GpuPrepareVerdict PrepareVerdict(const std::wstring& listedChoice, bool present, const std::wstring& value,
                                        bool unreadable) {
    if (unreadable) return GpuPrepareVerdict::Unreadable;
    if (GpuChoiceText(present, value, false) != listedChoice) return GpuPrepareVerdict::ChangedSinceListed;
    if (!IsEditablePreferenceValue(value)) return GpuPrepareVerdict::NotEditable;
    return GpuPrepareVerdict::Ready;
}

// Why pass one refused a row, in the words the result dialog uses; empty for Ready. The first and the last are v0.5.5's.
inline std::wstring PrepareRefusalReason(GpuPrepareVerdict v) {
    switch (v) {
        case GpuPrepareVerdict::Unreadable:
            return L"its current value could not be read, so it was left alone";
        case GpuPrepareVerdict::ChangedSinceListed:
            return L"its GPU setting changed after the list was shown, so it was left alone";
        case GpuPrepareVerdict::NotEditable:
            return L"its current value is in a form this does not edit, so it was left alone";
        default:
            return std::wstring();
    }
}

// ---- After pass two: may a row that is still ticked keep its tick? ---------------------------------------------
//
// The GPU choice a run means to leave on an item, as GpuChoiceText: Apply's merged value, or what Remove leaves - none when
// it deletes the value.
inline std::wstring IntendedChoiceText(const GpuEditItem& item, bool removing, const std::wstring& targetKey) {
    const std::wstring after = removing ? StripGpuAssignment(item.existing) : MergeGpuPreferenceValue(item.existing, targetKey);
    return GpuChoiceText(!after.empty(), after, false);
}

// True when a row pass two did not finish - Refused, Unconfirmed or NotAttempted - must be UNTICKED: the GPU choice read
// after the run (`choiceAfter`, GpuEditResult::choiceText) is neither the one the list showed when the user said yes
// (`listedBefore`) nor the one this run meant to leave (`intended`, IntendedChoiceText). Done and AlreadyDone rows are
// unticked by their own rule and never counted here.
//
// 🔴 PASS TWO RE-LISTED A ROW AT ANOTHER PROGRAM'S VALUE AND LEFT IT TICKED (Council round 2, v0.5.6). Every row takes the
// choice its run read back as its listed choice, so the next Apply compares with it - and a row another program pinned to
// the main GPU between pass one's read and the row's guarded write was refused, kept its tick, and a second Apply wrote over
// that pin with no refusal. Such a row is unticked and counted with pass one's changed rows, so the result says so and the
// tab is read again. A row whose GPU choice is unchanged - Windows rewrote AppStatus, say - keeps its tick as in v0.5.5, and
// so does one that reads back as what this run meant to write: a retry of it writes no other GPU. A read-back that failed is
// no evidence of a change, and keeps its tick too: its listed choice is then UnreadableChoiceKey, which the next Apply's
// PrepareVerdict refuses against any value it can read. AJ41 pins it.
inline bool UntickAfterRun(GpuEditOutcome outcome, const std::wstring& listedBefore, const std::wstring& choiceAfter,
                           const std::wstring& intended) {
    if (outcome == GpuEditOutcome::Done || outcome == GpuEditOutcome::AlreadyDone) return false;
    if (choiceAfter == UnreadableChoiceKey()) return false;
    return choiceAfter != listedBefore && choiceAfter != intended;
}

// ---- The panel's state across a message box -------------------------------------------------------------------
//
// A message box runs its own message loop, so the panel can be destroyed while one is open: tray Exit closes Settings,
// and the box goes with its owner. `busy` counts the boxes open for the panel, a box raised inside another included;
// `detached` is set by WM_NCDESTROY. The state is freed exactly once - by WM_NCDESTROY when no box is open, otherwise by
// the return of the last one - and an action that finds `detached` set when a box returns stops there, touching no
// window, no focus and no registry value.
//
// 🔴 A HANDLE THAT IS ALIVE IS NOT THIS PANEL (Council review, v0.5.6). This replaces IsWindow(panel) after each box:
// Windows reuses a destroyed window's handle, so another window could answer for it and the action carried on with
// freed state. The count lives in the state, is reached through the pointer taken before the box, and needs no handle.
// AJ43 pins it.
struct PanelLifetime {
    int busy = 0;
    bool detached = false;
};

inline void EnterModal(PanelLifetime& life) { ++life.busy; }

// After a box returns. True when the state must be freed now: it was detached while boxes were open, and this was the last.
inline bool LeaveModal(PanelLifetime& life) {
    --life.busy;
    return life.detached && life.busy == 0;
}

// From WM_NCDESTROY. True when the state may be freed now; otherwise the return of the last open box frees it.
inline bool DetachPanel(PanelLifetime& life) {
    life.detached = true;
    return life.busy == 0;
}

// ---- The notice for a change that did not finish ------------------------------------------------------------
//
// `records` holds each leftover .pending record's full path, oldest first, and whether the .reg of the same name
// exists.
//
// 🔴 IT NAMES ONLY FILES THAT EXIST (adversarial review, round 6): the first version always spoke of "the .reg
// restore file it saved" - also after a stop before any row was recorded, when there is none, in a folder that
// holds older restore files a user could open instead. And a row committed in the instant before the stop can be
// missing from the .reg; its previous value is in the .pending, so the notice says where.
struct UnfinishedRecord {
    std::wstring pendingPath;
    bool regExists = false;
    bool regValid = false;   // the .reg reads as a complete restore file (IsCompleteRegRestoreText)
};

inline std::wstring FormatUnfinishedNotice(const std::vector<UnfinishedRecord>& records) {
    if (records.empty()) return std::wstring();
    const std::wstring nl = L"\r\n";
    // "LEFT ITS RECORD BEHIND", NOT "STOPPED": a change that finished but could not delete its record leaves the same
    // file, and the notice cannot tell the two apart (adversarial review, round 6).
    std::wstring s = records.size() == 1
                         ? std::wstring(L"An earlier GPU change left its record behind: Game Optimizer stopped while it "
                                        L"was writing, or could not delete the record when it finished.")
                         : std::to_wstring(records.size()) +
                               L" earlier GPU changes left their records behind: Game Optimizer stopped while it was "
                               L"writing, or could not delete a record when a change finished.";
    const size_t shown = records.size() < 3 ? records.size() : 3;
    for (size_t i = 0; i < shown; ++i) {
        const UnfinishedRecord& r = records[records.size() - 1 - i];   // newest first
        std::wstring stem = r.pendingPath;
        const std::wstring ext = L".pending";
        if (stem.size() > ext.size() && stem.compare(stem.size() - ext.size(), ext.size(), ext) == 0)
            stem.erase(stem.size() - ext.size());
        if (r.regExists && r.regValid) {
            s += nl + nl + L"Its restore file lists what it changed - open it to put those values back:" + nl + L"    " +
                 stem + L".reg" + nl +
                 L"An application it changed in the moment before it stopped may be missing from that file; that "
                 L"application's previous value is in:" + nl + L"    " + r.pendingPath;
        } else if (r.regExists) {
            s += nl + nl + L"It left a restore file that could not be checked as complete, so do not open it:" + nl +
                 L"    " + stem + L".reg" + nl +
                 L"The previous value of every application it planned to change is in:" + nl + L"    " + r.pendingPath;
        } else {
            s += nl + nl + L"It wrote no restore file, so it most likely changed nothing. If it changed an application "
                           L"in the moment before it stopped, that application's previous value is in:" + nl + L"    " +
                 r.pendingPath;
        }
    }
    if (records.size() > shown)
        s += nl + nl + L"and " + std::to_wstring(records.size() - shown) + L" more in the same folder.";
    s += nl + nl + L"A .pending file is not a restore file and cannot be imported: it also lists applications that "
                   L"were never changed - do not rename it or edit it into one. Do not open an older restore file from "
                   L"the same folder to undo this. Delete each .pending file once you no longer need it.";
    return s;
}

// What a result dialog says about the restore file and the record kept while the change ran. `unrecordedLines` is
// the list of applications whose change could not be recorded, already formatted for the dialog.
//
// 🔴 A LIST IS NEVER CALLED "MISSING FROM THAT FILE" WHEN NO FILE WAS WRITTEN (adversarial review, round 6).
inline std::wstring FormatRestoreLines(bool regStarted, const std::wstring& regPath, const std::wstring& pendingPath,
                                       bool anyUnrecorded, const std::wstring& unrecordedLines, bool finished) {
    std::wstring s;
    if (regStarted) {
        s += L"\r\n\r\nThe previous values of the applications that changed were saved to:\r\n    " + regPath +
             L"\r\nOpening that file puts those values back as they were before this change.";
    }
    if (anyUnrecorded) {
        s += (regStarted ? std::wstring(L"\r\n\r\nMissing from that file, so nothing more was changed after it:")
                         : std::wstring(L"\r\n\r\nNo restore file could be written, so nothing more was changed "
                                        L"after this:")) +
             unrecordedLines;
    }
    if (!finished) {
        s += L"\r\n\r\nThe record of every planned value was kept at:\r\n    " + pendingPath +
             (anyUnrecorded
                  ? std::wstring(L"\r\nIt cannot be opened as a restore file; the previous value of each application "
                                 L"named just above is in it.")
                  : std::wstring(L"\r\nIt cannot be opened as a restore file. Delete it once you no longer need it."));
    }
    return s;
}

// ---- The question Apply asks --------------------------------------------------------------------------------
//
// The whole text of Apply's confirm, built here so the unit suite can pin every sentence of it (AJ37).
//
// 🔴 THE FIRST LINE AND THE "ALREADY ... ANOTHER GPU SETTING" SENTENCES STAY BYTE-IDENTICAL TO v0.5.5's: end-to-end
// scripts match on them.
//
// 🔴 R6-4: THE EXPLAINING PARAGRAPH IS NOT ONE OF THEM, AND IT WAS UNTRUE. It promised to write Windows'
// own GPU preference "for each one" - and a ticked row that already carries exactly that preference is
// AlreadyDone: nothing is written for it, which is what the result then says. On a machine upgrading from
// v0.5.6 or v0.5.7 that is EVERY ticked row, so the question promised a write for all of them and the
// result reported none. It now says what it really does.
//
// Two warnings are added in v0.5.6, each only when it is true:
//   - the MAIN GPU: the applications will share the card games run on. The picker offers it from v0.5.6 on.
//   - an INTEGRATED GPU: some applications can overload it. Founder decision: this warning is shown ONLY here - no
//     line on the tab, no suffix in the picker. A GPU whose kind could not be read gets no sentence: nothing is said
//     that was not measured.
//
// v0.5.8 adds ONE more, last and only when a CUDA change is really planned: FormatCudaConfirmLine
// (gpu_cuda.h) names the GPU CUDA is being asked to use - asked, never promised, because NVIDIA can refuse
// to make a settings entry for an executable - and says plainly that a program NVIDIA already keeps its own
// settings for is left to NVIDIA Control Panel. Empty leaves this question byte-identical to v0.5.7's,
// which is what AJ37 pins.
struct AssignConfirm {
    size_t count = 0;          // ticked rows
    std::wstring targetName;   // NameForKey of the target, no prefix
    size_t replacing = 0;      // ticked rows that already have another GPU setting
    bool mainGpu = false;      // the target is the main GPU
    bool integrated = false;   // KindForKey(target) == Integrated
    std::wstring cudaLine;     // FormatCudaConfirmLine, or empty when nothing CUDA-related is planned
};

inline std::wstring FormatAssignConfirm(const AssignConfirm& c) {
    const std::wstring gap = L"\r\n\r\n";
    std::wstring s = L"Assign " + std::to_wstring(c.count) + L" application" + (c.count == 1 ? L"" : L"s") + L" to "
                   + c.targetName + L"?" + gap +
                     L"This writes Windows' own per-application GPU preference for each one that does not "
                     L"already have it, and keeps "
                     L"the previous value of each one it changes in a restore file. It takes effect the "
                     L"next time each application starts. Remove assignment later returns an application "
                     L"to Windows' default GPU choice.";
    if (c.mainGpu)
        s += gap + c.targetName + L" is the main GPU, where your games run: these applications will share it with them.";
    if (c.integrated)
        s += gap + c.targetName +
             L" is an integrated GPU. Some applications can overload it - for example a browser showing a 3D model - "
             L"and run slowly.";
    if (c.replacing > 0)
        s += gap + std::to_wstring(c.replacing) + (c.replacing == 1
                                                       ? L" of them already has another GPU setting; it is replaced."
                                                       : L" of them already have another GPU setting; those are replaced.");
    if (!c.cudaLine.empty()) s += gap + c.cudaLine;
    return s;
}

// ---- The question Remove asks -------------------------------------------------------------------------
//
// 🔴 IT MOVED OUT OF THE WINDOW SO EVERY SENTENCE OF IT CAN BE PINNED, the same reason FormatAssignConfirm
// did. Its first two sentences stay byte-identical to v0.5.7's, which is what AJ37b pins.
//
// v0.5.8 adds one line, and only when there is really something to take away, or when nothing can be
// removed at all: FormatCudaRestoreConfirmLine (gpu_cuda.h). Remove changes NVIDIA's own settings just as
// Apply does - it takes away the entry Apply made - and the old question said nothing about that at all
// (Council round 1, F8). It also says so when the CUDA half cannot run, because then E4 removes NOTHING.
struct RemoveConfirm {
    size_t count = 0;          // ticked rows that really have an assignment to remove (RemovableCount)
    std::wstring cudaLine;     // FormatCudaRestoreConfirmLine, or empty when no CUDA setting goes back
};

inline std::wstring FormatRemoveConfirm(const RemoveConfirm& c) {
    const std::wstring gap = L"\r\n\r\n";
    std::wstring s = L"Remove the GPU assignment from " + std::to_wstring(c.count) + L" application" +
                     (c.count == 1 ? L"" : L"s") + L"?" + gap +
                     L"Each returns to Windows' default GPU choice the next time it starts, and the "
                     L"previous value of each one it changes is kept in a restore file. "
                     L"Ticked applications with no assignment are left alone.";
    if (!c.cudaLine.empty()) s += gap + c.cudaLine;
    return s;
}

// ---- Filling a list or a picker -----------------------------------------------------------------------
//
// `add(i)` inserts item i and answers its position, or a negative number when it was not inserted;
// `tie(at, i)` ties that position to item i; `untie(at)` takes back an item whose tie failed. `shown[i]` is
// true only for an item really on screen with its tie.
//
// 🔴 AN ITEM THAT COULD NOT BE TAKEN BACK BREAKS THE CONTROL (adversarial review, round 5): a line is then on
// screen tied to nothing known, and the window must offer nothing rather than guess what it means.
struct PopulateResult {
    std::vector<bool> shown;
    bool broken = false;
};

inline PopulateResult PopulateControl(size_t count, const std::function<long long(size_t)>& add,
                                      const std::function<bool(long long, size_t)>& tie,
                                      const std::function<bool(long long)>& untie) {
    PopulateResult r;
    r.shown.assign(count, false);
    for (size_t i = 0; i < count; ++i) {
        const long long at = add(i);
        if (at < 0) continue;
        if (tie(at, i)) {
            r.shown[i] = true;
        } else if (!untie(at)) {
            r.broken = true;
            break;
        }
    }
    return r;
}

// The candidate a picker is really showing. `selection` and `tie` are what the control answered for its
// current selection and that item's tie, negative for its error. False when either is not a real candidate.
inline bool VisibleCandidate(long long selection, long long tie, size_t candidates, size_t& index) {
    if (selection < 0 || tie < 0 || static_cast<unsigned long long>(tie) >= candidates) return false;
    index = static_cast<size_t>(tie);
    return true;
}

}  // namespace cd
