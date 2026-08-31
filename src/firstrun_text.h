#pragma once

#include <string>

#include "topology.h"
#include "util.h"

namespace cd {

inline std::wstring Page2GameModeText(const EnvironmentInfo& env, const Topology& t,
                                      bool& warnOut) {
    const bool multiDomain = t.domains.size() > 1;
    warnOut = env.gameModeState == GameModeState::On && multiDomain && env.isAmd;

    std::wstring s = L"CPU: ";
    s += env.cpuBrand.empty() ? std::wstring(L"(not reported by the registry)") : env.cpuBrand;
    s += L"\r\n\r\n";

    if (env.gameModeState == GameModeState::NotDeterminable) {
        s += L"Windows Game Mode is not determinable because the AutoGameModeEnabled "
             L"registry value could not be read for this account.";
    } else if (warnOut) {
        s += L"Windows Game Mode is ON "
             L"(HKCU\\Software\\Microsoft\\GameBar\\AutoGameModeEnabled = 1).\r\n\r\n"
             L"On a multi-CCD AMD part Game Mode applies its own GLOBAL CCD preference to "
             L"whatever it decides is the game. That is machine-wide, it is not per-game, "
             L"you do not choose which processes it covers, and it fights the per-game CPU "
             L"Sets this tool applies. If your game ends up on the wrong CCD anyway, this "
             L"is the first thing to check.\r\n\r\n"
             L"Game Optimizer will not change this setting for you. The button below just "
             L"opens the Windows page so you can decide.";
    } else if (env.gameModeState == GameModeState::Off) {
        s += L"Windows Game Mode is OFF "
             L"(HKCU\\Software\\Microsoft\\GameBar\\AutoGameModeEnabled = 0). It is not "
             L"applying a global CCD preference, so it is not competing with the per-game "
             L"CPU Sets this tool applies.";
    } else {
        s += L"Windows Game Mode is ON "
             L"(HKCU\\Software\\Microsoft\\GameBar\\AutoGameModeEnabled = 1), but this "
             L"machine is not a multi-CCD AMD part, so there is no CCD preference for it "
             L"to express. Nothing on this page needs your attention.";
    }
    return s;
}

inline std::wstring Page2VCacheText(const EnvironmentInfo& env,
                                    bool followsGameModeSection) {
    std::wstring s;
    if (followsGameModeSection)
        s = L"Separately, and regardless of the Game Mode setting above:\r\n\r\n";
    // THE AGENT IS TESTED FIRST, BEFORE THE SERVICE-STATE BRANCHES, AND THE ORDER IS THE POINT.
    //
    // Seeing amd3dvcacheUser.exe in our own session is a DIRECT OBSERVATION of the thing that
    // actually steers. Every branch below it is metadata ABOUT that thing, read from the service
    // control manager - and the SCM can fail to open, which yields NotDeterminable. Answering
    // "not determinable" while the agent is visibly running would be reporting ignorance we do
    // not have.
    //
    // The service only launches the agent, and the agent is not job-bound to it, so a stopped or
    // unreadable service says nothing about whether the optimizer is working.
    if (env.amdVCacheAgentRunning) {
        s += L"AMD's 3D V-Cache Performance Optimizer is ACTIVE.\r\n\r\n"
             L"The part that does the work is a background process, amd3dvcacheUser.exe, and it is "
             L"running now. It watches which window has focus and tells firmware which CCD to prefer - a "
             L"second, independent influence on where your game runs. It is not Windows Game Mode, and "
             L"turning Game Mode off does not stop it. If the core map shows a parked CCD, this is the "
             L"likeliest reason.\r\n\r\n"
             L"Game Optimizer does not stop, start or configure it.";
    } else if (env.amdVCacheServiceState == AmdVCacheServiceState::NotDeterminable) {
        // The agent was not seen AND the service could not be read, so we genuinely do not know.
        // Not seeing a process is weaker evidence than seeing one: it can also mean the snapshot
        // failed, so it is not promoted into a claim that nothing is running.
        s += L"The AMD 3D V-Cache Performance Optimizer state is not determinable.";
    } else if (env.amdVCacheServiceState == AmdVCacheServiceState::NotInstalled) {
        s += L"AMD's 3D V-Cache Performance Optimizer is NOT installed on this machine, so it is not "
             L"influencing scheduling here.";
    } else {
        s += L"AMD's 3D V-Cache Performance Optimizer is INSTALLED but NOT ACTIVE.\r\n\r\n"
             L"The part that actually steers, amd3dvcacheUser.exe, is not running, so nothing here is "
             L"expressing a CCD preference. Its service and kernel driver can both be running without "
             L"it - they only launch it and carry its decision to firmware.";
    }
    return s;
}

}  // namespace cd
