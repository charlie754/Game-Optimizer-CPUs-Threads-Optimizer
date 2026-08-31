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

inline const wchar_t* AmdVCacheComponentStateText(AmdVCacheServiceState state) {
    switch (state) {
        case AmdVCacheServiceState::NotInstalled:
            return L"Not installed";
        case AmdVCacheServiceState::InstalledButStopped:
            return L"Installed but stopped";
        case AmdVCacheServiceState::Running:
            return L"Running";
        default:
            return L"Not determinable";
    }
}

inline std::wstring FormatAmdVCacheEnvironmentStatus(AmdVCacheServiceState state) {
    return std::wstring(L"AMD 3D V-Cache Performance Optimizer: ") +
           AmdVCacheComponentStateText(state);
}

// ONLY THE SERVICE IS REPORTED, AND THE OMISSION OF THE DRIVER IS DELIBERATE - do not
// "restore" it. amd3dvcache.sys is PnP-loaded onto the ACPI device node, so it runs whether
// or not amd3dvcacheSvc does; measured live, Running with the service Disabled AND Stopped.
// It relays one value to firmware and carries no policy of its own, so it is inert without
// the per-session agent the service launches. Its state therefore tells the user nothing they
// can act on, and printing it beside the service invited exactly the wrong conclusion - that
// a still-Running driver meant stopping the service had failed. Stopping the SERVICE is the
// whole lever, and it takes effect immediately.
inline std::wstring FormatAmdVCacheComponentsEnvironmentStatus(
    AmdVCacheServiceState serviceState) {
    return std::wstring(L"AMD 3D V-Cache Performance Optimizer\r\n") +
           L"  service (amd3dvcacheSvc): " + AmdVCacheComponentStateText(serviceState);
}

// Returned text is empty when there is nothing recorded (originalStart < 0), so the caller
// can skip the row entirely rather than showing a placeholder.
inline std::wstring FormatVCacheRestoreHint(int originalStart) {
    if (originalStart < 0) return {};
    return L"Game Optimizer recorded this driver's original Start value as " +
           std::to_wstring(originalStart) +
           L". Turn this setting off before deleting the app's settings folder, or that value is "
           L"lost and the driver stays disabled.";
}

inline const wchar_t* ServiceStartTypeText(int startValue) {
    switch (startValue) {
        case 0: return L"Boot";
        case 1: return L"System";
        case 2: return L"Automatic";
        case 3: return L"Manual";
        case 4: return L"Disabled";
        default: return L"Not determinable";
    }
}

inline bool AmdVCacheRestartRequired(AmdVCacheServiceState state, int configuredStart) {
    if (configuredStart < 0) return false;
    return (configuredStart == 4 && state == AmdVCacheServiceState::Running) ||
           (configuredStart != 4 && state == AmdVCacheServiceState::InstalledButStopped);
}

inline std::wstring FormatAmdVCacheComponentEnvironmentLine(
    const wchar_t* component, const wchar_t* serviceName,
    AmdVCacheServiceState state, int configuredStart) {
    std::wstring line = std::wstring(component) + L" (" + serviceName + L"): " +
                        AmdVCacheComponentStateText(state) + L", start type " +
                        ServiceStartTypeText(configuredStart);
    if (AmdVCacheRestartRequired(state, configuredStart)) line += L" - restart required";
    return line;
}

inline std::wstring AmdVCacheRunningEffectText() {
    return L"It normally parks the non-cache CCD while a game is running. While that CCD "
           L"is parked, a background mask pointing at it is effectively inert.";
}

}  // namespace cd
