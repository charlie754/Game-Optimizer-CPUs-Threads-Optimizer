// Game Optimizer - when is a change in AMD's V-Cache agent worth one log line?
//
// Pure and header-only so the decision can be tested without a process snapshot, a thread or
// a clock. The engine probes IsAmdVCacheAgentRunning() on the tick, and a tick runs four times
// a second: without this the log would carry the same line 14,400 times an hour.
#pragma once

namespace cd {

// What the engine knows about the agent, INCLUDING "nothing yet".
//
// The third state is not decoration. A machine on which the agent has been running the whole
// time never changes, so a plain bool that starts false would record the very first probe as a
// change and a plain bool that starts true would record nothing at all. Neither answers the
// question the log is being kept for - what was true the first time we actually looked.
enum class AgentSeen {
    Never = -1,   // no probe has completed yet this run
    Absent = 0,   // probed, and the agent was not running
    Present = 1   // probed, and the agent was running
};

inline AgentSeen AgentSeenFrom(bool running) {
    return running ? AgentSeen::Present : AgentSeen::Absent;
}

// Returns true when a change from lastKnown to now is worth one log line.
// The first probe of a run always is, whichever way it lands.
inline bool ShouldLogAgentChange(AgentSeen lastKnown, bool now) {
    return lastKnown != AgentSeenFrom(now);
}

// The two-state form, for a caller that has already probed at least once and only wants to
// know whether the answer moved.
inline bool ShouldLogAgentChange(bool lastKnown, bool now) {
    return lastKnown != now;
}

}  // namespace cd
