#pragma once

#include <string>
#include <vector>

#include "topology.h"

namespace cd {

bool IEquals(const std::wstring& a, const std::wstring& b);

// Rebuild the mask list after the CPU topology changed, WITHOUT destroying masks the user
// made by hand.
//
// `derived`  - the freshly derived masks for the live hardware (DeriveMasks output).
// `existing` - the mask list as it was before the topology changed.
//
// Returns: every derived mask, in derived order, followed by every mask in `existing` whose
// `derived` flag is false and whose name does not collide with a derived mask.
inline std::vector<Mask> MergeMasksPreservingCustom(const std::vector<Mask>& derived,
                                                    const std::vector<Mask>& existing) {
    std::vector<Mask> merged = derived;
    merged.reserve(derived.size() + existing.size());
    for (const Mask& mask : existing) {
        if (mask.derived) continue;

        bool collides = false;
        for (const Mask& derivedMask : derived) {
            if (IEquals(derivedMask.name, mask.name)) {
                collides = true;
                break;
            }
        }
        if (!collides) merged.push_back(mask);
    }
    return merged;
}

// True when the merge carried at least one hand-made mask across a topology change, which is
// what the user must be told: the processor numbers inside those masks may now refer to
// different processors.
inline bool MergePreservedCustomMasks(const std::vector<Mask>& derived,
                                      const std::vector<Mask>& existing) {
    return MergeMasksPreservingCustom(derived, existing).size() > derived.size();
}

}  // namespace cd
