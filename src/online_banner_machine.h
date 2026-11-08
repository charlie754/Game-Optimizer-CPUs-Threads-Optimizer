#pragma once

#include "online_banner.h"

#include <cstdint>
#include <string>
#include <vector>

namespace ob {

enum class Kind { Webp, Jpg };

struct CurrentSet {
    std::int64_t revision = 0;
    std::string sha256;
    bool enabled = false;
    std::int64_t expiresMs = 0;
    std::string photoWebp;
    std::string photoJpg;
};

struct Record {
    std::int64_t hwm = 0;
    bool hasCurrent = false;
    CurrentSet current;
    std::int64_t lastCheckMs = 0;
    int failures = 0;
};

enum class EventType {
    Start,
    Shown,
    Hidden,
    Signed,
    Photos,
    Committed,
    Failed,
    Timer,
    Resume,
    Click,
    Pointer,
    Focus
};

struct Event {
    EventType type = EventType::Start;
    bool hasRecord = false;
    Record record;
    bool setValid = false;
    bool mayFetch = false;
    std::int64_t gen = 0;
    Result result;
    bool ok = false;
    bool over = false;
    bool inside = false;
};

enum class EffectType {
    FetchSigned,
    FetchPhotos,
    Cancel,
    WriteSet,
    WriteRecord,
    DeleteUnnamedSets,
    Display,
    ArmTimer,
    DisarmTimer,
    OpenLink
};

struct Effect {
    EffectType type = EffectType::FetchSigned;
    std::int64_t gen = 0;
    std::vector<Kind> kinds;
    std::string sha256;
    Record record;
    bool online = false;
    bool immediate = false;
    std::int64_t atMs = 0;
};

class Machine {
public:
    Machine(std::int64_t floor, std::vector<Kind> kinds);
    std::vector<Effect> Dispatch(const Event& event, std::int64_t nowMs);
    std::vector<std::string> CheckInvariants() const;

    struct State {
        Record record;
        bool visible = false;
        std::int64_t gen = 0;
        bool attempting = false;
        bool shownOnline = false;
        bool pendingSwap = false;
        std::int64_t floor = 0;
        bool timerArmed = false;
        std::int64_t timerAtMs = 0;
    };

    State GetState() const;

private:
    struct Displayed {
        bool online = false;
        std::string sha256;
        std::int64_t expiresMs = 0;
    };

    enum class Phase { None, Signed, Photos, WriteSet };

    struct Pending {
        bool present = false;
        Result result;
        std::int64_t expiresMs = 0;
        CurrentSet photos;
    };

    static Record EmptyRecord(std::int64_t floor);
    bool Showable() const;
    Displayed Desired() const;
    bool Due() const;
    std::int64_t TimerTarget() const;
    void StartAttempt(std::vector<Effect>* effects);
    void SwapNow(std::vector<Effect>* effects);
    void FailAttempt(std::vector<Effect>* effects);
    CurrentSet PhotosFor(const Result& result) const;
    void PreCommit(const Result& result, std::vector<Effect>* effects);
    bool SameDisplayed(const Displayed& next) const;
    void ReconcileDisplay(std::vector<Effect>* effects);
    void ReconcileTimer(std::vector<Effect>* effects);

    std::vector<Kind> clientKinds_;
    bool started_ = false;
    Record record_;
    bool visible_ = false;
    std::int64_t gen_ = 0;
    bool attempting_ = false;
    Phase phase_ = Phase::None;
    Pending pending_;
    Displayed displayed_;
    bool pendingSwap_ = false;
    bool pointerOver_ = false;
    bool focusInside_ = false;
    bool timerArmed_ = false;
    std::int64_t timerAtMs_ = 0;
    bool lastMayFetch_ = false;
    bool setVerified_ = false;
    std::int64_t nowMs_ = 0;
    std::int64_t hwmHigh_ = 0;
    std::int64_t floor_ = 0;
};

} // namespace ob
