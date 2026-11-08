#pragma once

#include <cstddef>

namespace cd::webview_message {

constexpr std::size_t kMaxJsonChars = 1024;

inline std::size_t BoundedLength(const wchar_t* value) noexcept {
    if (value == nullptr) return kMaxJsonChars + 1;
    for (std::size_t i = 0; i <= kMaxJsonChars; ++i) {
        if (value[i] == L'\0') return i;
    }
    return kMaxJsonChars + 1;
}

inline bool WithinLimit(const wchar_t* value) noexcept {
    return BoundedLength(value) <= kMaxJsonChars;
}

}  // namespace cd::webview_message
