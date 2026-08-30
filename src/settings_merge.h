#pragma once

#include <cstddef>
#include <string>
#include <vector>

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

}  // namespace cd
