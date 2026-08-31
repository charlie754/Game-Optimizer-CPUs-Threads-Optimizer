#pragma once

#include "topology.h"
#include "util.h"

namespace cd {

enum class WarningTone { Actionable, Informational };

struct StartupWarningDecision {
    bool showGameMode = false;
    bool showVCache   = false;
    WarningTone gameModeTone = WarningTone::Informational;
    bool Any() const { return showGameMode || showVCache; }
};

inline StartupWarningDecision DecideStartupWarning(const EnvironmentInfo& env,
                                                    const Topology& topo) {
    StartupWarningDecision decision;
    decision.showGameMode = env.gameModeState == GameModeState::On;
    decision.gameModeTone = topo.domains.size() > 1 && env.isAmd
        ? WarningTone::Actionable
        : WarningTone::Informational;
    // Key on the agent alone. amd3dvcacheUser.exe is the policy engine: it watches
    // the foreground window and issues the IOCTL. The service only launches that
    // agent, and the kernel driver is an ACPI relay, so neither one running means
    // anything is actually steering. The previous predicate was also vacuous: the
    // service DEPENDS ON the driver, so "service || driver" reduced to "driver",
    // which is PnP-pinned true on any X3D machine and fired the warning
    // unconditionally.
    decision.showVCache = env.amdVCacheAgentRunning;
    return decision;
}

}  // namespace cd
