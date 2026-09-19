// Game Optimizer - the Settings window's tab labels, in page order.
//
// A PURE HEADER FOR ONE REASON: settings.cpp is not in the unit-test build (tools\build-tests.bat
// compiles neither it nor any window code), so a label typed there as a literal can be checked by a
// screenshot and by nothing else. The tab bar, its fallback button row and every page heading read
// this one array, and test AJ38 pins it word for word.
//
// THE ORDER IS THE PAGE ORDER. settings.cpp indexes it with PAGE_* and static_asserts that it holds
// PAGE_COUNT entries, so a page added without a label - or a label without a page - does not compile.
#pragma once

namespace cd {

// v0.5.6, operator request: "Core map" became "CPU Core Map", and "GPU Assignment" - the old separate
// GPU window, now a tab - sits between it and "Setting".
inline constexpr const wchar_t* kSettingsPageLabels[] = {
    L"Profiles", L"CPU Core Map", L"GPU Assignment", L"Setting"
};

}  // namespace cd
