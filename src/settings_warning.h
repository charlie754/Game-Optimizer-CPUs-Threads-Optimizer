// Game Optimizer - pure wording helpers used by the Settings UI and unit tests.
#pragma once

#include <string>

#include "settings_environment.h"

namespace cd {

inline std::wstring FormatFullyParkedMaskWarning(const std::wstring& maskName,
                                                 int processorCount,
                                                 bool amdVCacheServiceRunning,
                                                 bool amdVCacheDriverRunning,
                                                 bool amdVCachePresent) {
    const std::wstring prefix =
        L"Warning: all " + std::to_wstring(processorCount) + L" processors in \"" +
        maskName + L"\" are currently parked. ";
    if (amdVCacheServiceRunning) {
        return prefix +
               L"The running AMD 3D V-Cache Performance Optimizer service is the likely cause. " +
               AmdVCacheRunningEffectText() +
               L" Windows can accept assignments to that CCD and then ignore them.";
    }
    if (amdVCacheDriverRunning) {
        return prefix +
               L"The AMD 3D V-Cache Performance Optimizer service is stopped, but its kernel "
               L"driver is still running, so this may still be coming from Windows rather than "
               L"from firmware. That driver has no stop routine and only goes away after a "
               L"restart. Windows can accept assignments to a parked CCD and then ignore them.";
    }
    if (amdVCachePresent) {
        return prefix +
               L"AMD's 3D V-Cache optimizer is installed, and neither its service nor its driver "
               L"is running, so nothing on the Windows side explains this. A BIOS option can "
               L"park a CCD below the operating system. Look for a game-aware or adaptive CCD "
               L"parking setting - not the CCD or SMT controls that disable a CCD at boot, "
               L"which are a different feature. Windows can accept assignments to a parked CCD "
               L"and then ignore them.";
    }
    return prefix +
           L"Windows can accept an assignment to a fully parked mask and then ignore "
           L"it - the process keeps running elsewhere.";
}

}  // namespace cd
