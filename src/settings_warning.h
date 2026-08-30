// Game Optimizer - pure wording helpers used by the Settings UI and unit tests.
#pragma once

#include <string>

#include "settings_environment.h"

namespace cd {

inline std::wstring FormatFullyParkedMaskWarning(const std::wstring& maskName,
                                                 int processorCount,
                                                 bool amdVCacheServiceRunning) {
    const std::wstring prefix =
        L"Warning: all " + std::to_wstring(processorCount) + L" processors in \"" +
        maskName + L"\" are currently parked. ";
    if (!amdVCacheServiceRunning) {
        return prefix +
               L"Windows can accept an assignment to a fully parked mask and then ignore "
               L"it - the process keeps running elsewhere.";
    }
    return prefix +
           L"The running AMD 3D V-Cache Performance Optimizer service is the likely cause. " +
           AmdVCacheRunningEffectText() +
           L" Windows can accept assignments to that CCD and then ignore them.";
}

}  // namespace cd
