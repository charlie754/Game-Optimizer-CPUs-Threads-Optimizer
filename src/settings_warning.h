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

// THE BOX REFLECTS THE SERVICE START TYPE, NOT A RUNNING PROCESS, AND THAT DISTINCTION IS A
// FIXED BUG. It used to key on IsAmdVCacheAgentRunning() while a click writes the SERVICE
// start type through --vcache-run. Those are different objects: stop the service and its
// per-session agent can still be alive, which pinned the box to "unchecked" and turned every
// click into a STOP. The operator disabled the service three times in a row that way, and only
// the log showed it. Checked now means exactly what the click means: "configured Disabled".
//
// A start value this code could not read comes back as -1, which is NOT 4, so the box shows
// unchecked. That is the safe direction: it never claims a stop that was not configured.
//
// It reports CONFIGURATION and nothing else. It is handed a start type, never a service or
// process state, so "checked" means "configured Disabled" and not "the optimizer is stopped
// right now" - the per-session agent can outlive a disabled service, which is the whole
// reason the paragraph above exists.
inline bool VCacheStopBoxChecked(int serviceStartValue) { return serviceStartValue == 4; }

// The restore control exists only for users stranded by the OLD disable feature, i.e. only
// when a driver Start value was recorded.
inline bool ShowVCacheRestoreControl(int vcacheOriginalStart) { return vcacheOriginalStart >= 0; }

// AMD's INF installs this service as SERVICE_AUTO_START (2). Stopping it means Disabled (4),
// and clearing the box must restore AMD's own default rather than guess at Manual.
inline int VCacheServiceStartTypeFor(bool stopRequested) { return stopRequested ? 4 : 2; }

// The driver's restore value: what we recorded, or AMD's documented DEMAND_START default.
// The service always restores to AMD's own SERVICE_AUTO_START (2), independent of what the
// driver originally was. They differ in AMD's own INF (service is 2, driver is 3).
inline int VCacheRestoreDriverStart(int recordedOriginal) {
    return recordedOriginal >= 0 ? recordedOriginal : 3;
}

}  // namespace cd
