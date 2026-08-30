// Game Optimizer - pure so the heavy-app ordering rule can be tested without a window.
// The caller owns lookup-key construction because the process table uses
// ToLower/CharLowerBuffW. Folding differently here would let a non-ASCII executable sort as
// inactive while the same row is drawn as running.
#pragma once

#include <algorithm>
#include <cstddef>
#include <set>
#include <string>
#include <vector>

namespace cd {

inline std::vector<std::wstring> OrderHeavyByActivity(
    const std::vector<std::wstring>& items,
    const std::vector<std::wstring>& itemKeys,
    const std::set<std::wstring>& runningKeys) {
    if (itemKeys.size() != items.size()) {
        // Mismatched parallel arrays are a caller error. Leave every item in place rather than
        // silently returning a half-correct order based on only some lookup keys.
        return items;
    }

    std::vector<std::size_t> order;
    order.reserve(items.size());
    for (std::size_t i = 0; i < items.size(); ++i) order.push_back(i);
    std::stable_partition(order.begin(), order.end(), [&](std::size_t i) {
        return runningKeys.find(itemKeys[i]) != runningKeys.end();
    });

    std::vector<std::wstring> ordered;
    ordered.reserve(items.size());
    for (std::size_t i : order) ordered.push_back(items[i]);
    return ordered;
}

}  // namespace cd
