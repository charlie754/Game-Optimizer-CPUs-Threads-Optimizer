#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "config.h"

namespace cd {

inline std::vector<std::size_t> IndicesAddedBehindTheWindow(
    const std::vector<std::wstring>& liveKeys,
    const std::vector<std::wstring>& baselineKeys,
    const std::vector<std::wstring>& workKeys) {
    std::vector<std::size_t> added;
    for (std::size_t i = 0; i < liveKeys.size(); ++i) {
        bool inBaseline = false;
        for (std::size_t j = 0; j < baselineKeys.size(); ++j) {
            if (liveKeys[i] == baselineKeys[j]) {
                inBaseline = true;
                break;
            }
        }
        if (inBaseline) continue;

        bool inWork = false;
        for (std::size_t j = 0; j < workKeys.size(); ++j) {
            if (liveKeys[i] == workKeys[j]) {
                inWork = true;
                break;
            }
        }
        if (!inWork) added.push_back(i);
    }
    return added;
}

// THE FIELDS SETTINGS NEVER EDITS, carried from the LIVE config into the snapshot the window
// has been holding since it opened. ApplyChanges calls this immediately before it writes that
// snapshot back, so the live value is the one that survives.
//
// Settings is modeless, and every field below is written from OUTSIDE it while it sits open:
//
//   paused              the tray's Pause item, and the game prompt's decline
//   unknown             sections and keys a newer build wrote that this one cannot parse
//   vcacheOriginalStart the driver's Start value before the app disabled it. It is written by
//                       the ELEVATED `--vcache-set` child process - never by this window - and
//                       the startup warning can now trigger that child while Settings is open
//                       behind it. It is also the ONLY record able to restore the driver, so a
//                       stale -1 written over a recorded 3 does not merely lose a number: it
//                       strands the driver disabled with nothing left that can put it back.
//
// The rule is "the snapshot never wins", NOT "the larger value wins" - a live -1 written after
// the driver was restored must overwrite a snapshot's stale 3 just as readily. Tests group Q.
inline void PreserveFieldsSettingsNeverEdits(const Config& live, Config& work) {
    work.paused = live.paused;
    work.unknown = live.unknown;
    work.vcacheOriginalStart = live.vcacheOriginalStart;
}

}  // namespace cd
