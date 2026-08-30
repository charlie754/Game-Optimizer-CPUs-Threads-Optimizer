// Game Optimizer - pure environment-status wording shared by Settings and unit tests.
#pragma once

#include <string>

#include "util.h"

namespace cd {

inline std::wstring FormatGameModeEnvironmentStatus(GameModeState state) {
    switch (state) {
        case GameModeState::On:
            return L"Windows Game Mode: On";
        case GameModeState::Off:
            return L"Windows Game Mode: Off";
        default:
            return L"Windows Game Mode: Not determinable";
    }
}

inline std::wstring FormatAmdVCacheEnvironmentStatus(AmdVCacheServiceState state) {
    switch (state) {
        case AmdVCacheServiceState::NotInstalled:
            return L"AMD 3D V-Cache Performance Optimizer: Not installed";
        case AmdVCacheServiceState::InstalledButStopped:
            return L"AMD 3D V-Cache Performance Optimizer: Installed but stopped";
        case AmdVCacheServiceState::Running:
            return L"AMD 3D V-Cache Performance Optimizer: Running";
        default:
            return L"AMD 3D V-Cache Performance Optimizer: Not determinable";
    }
}

inline std::wstring AmdVCacheRunningEffectText() {
    return L"It normally parks the non-cache CCD while a game is running. While that CCD "
           L"is parked, a background mask pointing at it is effectively inert.";
}

}  // namespace cd
