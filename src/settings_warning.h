// Game Optimizer - pure wording helpers used by the Settings UI and unit tests.
#pragma once

#include <string>

#include "settings_environment.h"

namespace cd {

inline std::wstring FormatFullyParkedMaskWarning(const std::wstring& maskName,
                                                 int processorCount,
                                                 bool amdVCacheAgentRunning,
                                                 bool amdVCacheServiceRunning,
                                                 bool amdVCacheDriverRunning,
                                                 bool amdVCachePresent) {
    const std::wstring prefix =
        L"Warning: all " + std::to_wstring(processorCount) + L" processors in \"" +
        maskName + L"\" are currently parked. ";
    // The agent (amd3dvcacheUser.exe) is the only component that actively steers the kernel driver.
    // The service (amd3dvcacheSvc) is a launcher only and cannot steer. Test the agent first.
    if (amdVCacheAgentRunning) {
        return prefix +
               L"The running AMD 3D V-Cache optimizer (amd3dvcacheUser.exe) is the likely cause. " +
               AmdVCacheRunningEffectText() +
               L" Stopping the AMD service also stops this agent, and takes effect immediately. "
               L"Windows can accept assignments to that CCD and then ignore them.";
    }
    if (amdVCacheServiceRunning || amdVCacheDriverRunning) {
        return prefix +
               L"AMD's 3D V-Cache optimizer service or driver is running, but the part that "
               L"actively steers (amd3dvcacheUser.exe) is not, so this is probably not coming "
               L"from the optimizer. A BIOS option can park a CCD below the operating system. "
               L"Look for a game-aware or adaptive CCD parking setting - not the CCD or SMT "
               L"controls that disable a CCD at boot, which are a different feature. Windows can "
               L"accept assignments to a parked CCD and then ignore them.";
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

// The Stop box is checked when the optimizer is NOT active. One definition, so the checkbox
// and the startup warning can never disagree.
inline bool VCacheStopBoxChecked(bool optimizerActive) { return !optimizerActive; }

// The restore control exists only for users stranded by the OLD disable feature, i.e. only
// when a driver Start value was recorded.
inline bool ShowVCacheRestoreControl(int vcacheOriginalStart) { return vcacheOriginalStart >= 0; }

// AMD's INF installs this service as SERVICE_AUTO_START (2). Stopping it means Disabled (4),
// and clearing the box must restore AMD's own default rather than guess at Manual.
inline int VCacheServiceStartTypeFor(bool stopRequested) { return stopRequested ? 4 : 2; }

}  // namespace cd
