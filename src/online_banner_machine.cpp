#include "online_banner_machine.h"

#include <algorithm>
#include <cstddef>
#include <limits>
#include <utility>

namespace ob {
namespace {

constexpr std::int64_t kH = 3600000ll;
constexpr std::int64_t kBackoff[] = {6ll * kH, 6ll * kH, 12ll * kH,
                                     24ll * kH};
constexpr std::int64_t kCheck = 24ll * kH;
constexpr std::int64_t kMinTimerAhead = 60000ll;

std::int64_t SaturatingAdd(std::int64_t value, std::int64_t addend)
{
    if (addend > 0 && value > std::numeric_limits<std::int64_t>::max() - addend) {
        return std::numeric_limits<std::int64_t>::max();
    }
    if (addend < 0 && value < std::numeric_limits<std::int64_t>::min() - addend) {
        return std::numeric_limits<std::int64_t>::min();
    }
    return value + addend;
}

} // namespace

Machine::Machine(std::int64_t floor, std::vector<Kind> kinds)
    : clientKinds_(std::move(kinds)), record_(EmptyRecord(floor)),
      hwmHigh_(floor), floor_(floor)
{
}

Record Machine::EmptyRecord(std::int64_t floor)
{
    Record record;
    record.hwm = floor;
    return record;
}

bool Machine::Showable() const
{
    return record_.hasCurrent && record_.current.enabled &&
           record_.current.revision == record_.hwm &&
           nowMs_ < record_.current.expiresMs && setVerified_;
}

Machine::Displayed Machine::Desired() const
{
    if (Showable()) {
        Displayed displayed;
        displayed.online = true;
        displayed.sha256 = record_.current.sha256;
        displayed.expiresMs = record_.current.expiresMs;
        return displayed;
    }
    return Displayed{};
}

bool Machine::Due() const
{
    const int failureIndex = std::max(0, std::min(3, record_.failures));
    return nowMs_ >= SaturatingAdd(record_.lastCheckMs, kBackoff[failureIndex]) ||
           record_.lastCheckMs > nowMs_;
}

std::int64_t Machine::TimerTarget() const
{
    if (!visible_) {
        return 0;
    }

    bool hasTarget = false;
    std::int64_t target = 0;
    if (displayed_.online) {
        target = displayed_.expiresMs;
        hasTarget = true;
    }
    if (Showable() && (!hasTarget || record_.current.expiresMs < target)) {
        target = record_.current.expiresMs;
        hasTarget = true;
    }

    if (!attempting_ && lastMayFetch_) {
        const std::int64_t lower = SaturatingAdd(nowMs_, kMinTimerAhead);
        const std::int64_t upper = SaturatingAdd(nowMs_, kCheck);
        const std::int64_t check = SaturatingAdd(record_.lastCheckMs, kCheck);
        const std::int64_t checkTarget =
            std::min(upper, std::max(lower, check));
        if (!hasTarget || checkTarget < target) {
            target = checkTarget;
            hasTarget = true;
        }
    }

    return hasTarget ? target : 0;
}

void Machine::StartAttempt(std::vector<Effect>* effects)
{
    ++gen_;
    attempting_ = true;
    phase_ = Phase::Signed;
    Effect effect;
    effect.type = EffectType::FetchSigned;
    effect.gen = gen_;
    effects->push_back(std::move(effect));
}

void Machine::SwapNow(std::vector<Effect>* effects)
{
    const Displayed next = Desired();
    Effect effect;
    effect.type = EffectType::Display;
    effect.online = next.online;
    effect.immediate = true;
    effects->push_back(std::move(effect));
    displayed_ = next;
    pendingSwap_ = false;
}

void Machine::FailAttempt(std::vector<Effect>* effects)
{
    record_.lastCheckMs = nowMs_;
    record_.failures = std::min(3, record_.failures + 1);
    hwmHigh_ = std::max(hwmHigh_, record_.hwm);
    attempting_ = false;
    phase_ = Phase::None;
    pending_ = Pending{};

    Effect effect;
    effect.type = EffectType::WriteRecord;
    effect.record = record_;
    effects->push_back(std::move(effect));
}

CurrentSet Machine::PhotosFor(const Result& result) const
{
    CurrentSet photos;
    if (result.action == Action::KillSwitch) {
        return photos;
    }
    for (const Kind kind : clientKinds_) {
        if (kind == Kind::Webp) {
            photos.photoWebp = result.banner.sha256Webp;
        } else {
            photos.photoJpg = result.banner.sha256Jpg;
        }
    }
    return photos;
}

void Machine::PreCommit(const Result& result, std::vector<Effect>* effects)
{
    const Result incoming = result;
    const std::int64_t expiresMs = incoming.banner.expiresMs;
    bool missingPhotoHash = false;
    if (incoming.action == Action::Show) {
        for (const Kind kind : clientKinds_) {
            const std::string& hash = kind == Kind::Webp
                                          ? incoming.banner.sha256Webp
                                          : incoming.banner.sha256Jpg;
            if (hash.empty()) {
                missingPhotoHash = true;
                break;
            }
        }
    }
    if (!(nowMs_ < expiresMs) || incoming.banner.revision < record_.hwm ||
        missingPhotoHash) {
        FailAttempt(effects);
        return;
    }

    pending_ = Pending{};
    pending_.present = true;
    pending_.result = incoming;
    pending_.expiresMs = expiresMs;
    pending_.photos = PhotosFor(incoming);
    phase_ = Phase::WriteSet;

    Effect effect;
    effect.type = EffectType::WriteSet;
    effect.gen = gen_;
    effect.sha256 = incoming.sha256;
    effects->push_back(std::move(effect));
}

bool Machine::SameDisplayed(const Displayed& next) const
{
    if (displayed_.online != next.online) {
        return false;
    }
    return !next.online || displayed_.sha256 == next.sha256;
}

void Machine::ReconcileDisplay(std::vector<Effect>* effects)
{
    if (!visible_) {
        return;
    }

    const Displayed next = Desired();
    if (SameDisplayed(next)) {
        pendingSwap_ = false;
        return;
    }

    if (!next.online && displayed_.online) {
        SwapNow(effects);
        return;
    }

    if (pointerOver_ || focusInside_) {
        pendingSwap_ = true;
        return;
    }

    Effect effect;
    effect.type = EffectType::Display;
    effect.online = true;
    effect.immediate = false;
    effects->push_back(std::move(effect));
    displayed_ = next;
    pendingSwap_ = false;
}

void Machine::ReconcileTimer(std::vector<Effect>* effects)
{
    const std::int64_t target = TimerTarget();
    const bool targetArmed = target != 0;
    if (targetArmed == timerArmed_ && (!targetArmed || target == timerAtMs_)) {
        return;
    }

    Effect effect;
    if (!targetArmed) {
        effect.type = EffectType::DisarmTimer;
        timerArmed_ = false;
        timerAtMs_ = 0;
    } else {
        effect.type = EffectType::ArmTimer;
        effect.atMs = target;
        timerArmed_ = true;
        timerAtMs_ = target;
    }
    effects->push_back(std::move(effect));
}

std::vector<Effect> Machine::Dispatch(const Event& event, std::int64_t nowMs)
{
    std::vector<Effect> effects;
    try {
        if (!started_ && event.type != EventType::Start) {
            return effects;
        }

        if (event.type == EventType::Start) {
            Record nextRecord = event.hasRecord ? event.record : EmptyRecord(floor_);
            bool recordChanged = false;
            if (nextRecord.hwm < floor_) {
                nextRecord.hwm = floor_;
                recordChanged = true;
            }
            if (nextRecord.hasCurrent &&
                !(event.setValid &&
                  nextRecord.current.revision == nextRecord.hwm &&
                  nowMs < nextRecord.current.expiresMs)) {
                nextRecord.hasCurrent = false;
                nextRecord.current = CurrentSet{};
                recordChanged = true;
            }

            nowMs_ = nowMs;
            visible_ = false;
            attempting_ = false;
            phase_ = Phase::None;
            pending_ = Pending{};
            displayed_ = Displayed{};
            pendingSwap_ = false;
            pointerOver_ = false;
            focusInside_ = false;
            timerArmed_ = false;
            timerAtMs_ = 0;
            lastMayFetch_ = false;

            started_ = true;
            record_ = nextRecord;

            setVerified_ = record_.hasCurrent;
            hwmHigh_ = record_.hwm;
            if (recordChanged) {
                Effect effect;
                effect.type = EffectType::WriteRecord;
                effect.record = record_;
                effects.push_back(std::move(effect));
            }
            Effect effect;
            effect.type = EffectType::DeleteUnnamedSets;
            effects.push_back(std::move(effect));
            return effects;
        }

        nowMs_ = nowMs;
        bool expirySwapped = false;

        if (event.type == EventType::Timer || event.type == EventType::Resume) {
            timerArmed_ = false;
            timerAtMs_ = 0;
        }

        if (event.type != EventType::Hidden && visible_ && displayed_.online &&
            nowMs_ >= displayed_.expiresMs) {
            SwapNow(&effects);
            expirySwapped = true;
        }

        switch (event.type) {
        case EventType::Shown:
            lastMayFetch_ = event.mayFetch;
            if (!visible_) {
                visible_ = true;
                const Displayed next = Desired();
                Effect effect;
                effect.type = EffectType::Display;
                effect.online = next.online;
                effect.immediate = true;
                effects.push_back(std::move(effect));
                displayed_ = next;
            }
            if (!attempting_ && event.mayFetch && Due()) {
                StartAttempt(&effects);
            }
            break;

        case EventType::Hidden:
            if (!visible_) {
                break;
            }
            visible_ = false;
            if (attempting_) {
                Effect effect;
                effect.type = EffectType::Cancel;
                effect.gen = gen_;
                effects.push_back(std::move(effect));
                attempting_ = false;
                phase_ = Phase::None;
                pending_ = Pending{};
            }
            pointerOver_ = false;
            focusInside_ = false;
            pendingSwap_ = false;
            break;

        case EventType::Signed:
            if (!attempting_ || event.gen != gen_ || phase_ != Phase::Signed) {
                break;
            }
            if (event.result.action == Action::Reject) {
                FailAttempt(&effects);
            } else if (event.result.action == Action::KillSwitch) {
                PreCommit(event.result, &effects);
            } else if (event.result.action == Action::Show) {
                pending_ = Pending{};
                pending_.present = true;
                pending_.result = event.result;
                phase_ = Phase::Photos;
                Effect effect;
                effect.type = EffectType::FetchPhotos;
                effect.gen = gen_;
                effect.kinds = clientKinds_;
                effects.push_back(std::move(effect));
            } else {
                FailAttempt(&effects);
            }
            break;

        case EventType::Photos:
            if (!attempting_ || event.gen != gen_ || phase_ != Phase::Photos) {
                break;
            }
            if (!event.ok) {
                FailAttempt(&effects);
            } else if (pending_.present) {
                PreCommit(pending_.result, &effects);
            }
            break;

        case EventType::Committed:
            if (!attempting_ || event.gen != gen_ ||
                phase_ != Phase::WriteSet || !pending_.present) {
                break;
            }
            record_.hwm = pending_.result.banner.revision;
            record_.hasCurrent = true;
            record_.current.revision = pending_.result.banner.revision;
            record_.current.sha256 = pending_.result.sha256;
            record_.current.enabled = pending_.result.action == Action::Show;
            record_.current.expiresMs = pending_.expiresMs;
            record_.current.photoWebp = pending_.photos.photoWebp;
            record_.current.photoJpg = pending_.photos.photoJpg;
            record_.lastCheckMs = nowMs_;
            record_.failures = 0;
            hwmHigh_ = std::max(hwmHigh_, record_.hwm);
            setVerified_ = true;
            attempting_ = false;
            phase_ = Phase::None;
            pending_ = Pending{};
            {
                Effect effect;
                effect.type = EffectType::WriteRecord;
                effect.record = record_;
                effects.push_back(std::move(effect));
            }
            break;

        case EventType::Failed:
            if (attempting_ && event.gen == gen_) {
                FailAttempt(&effects);
            }
            break;

        case EventType::Timer:
        case EventType::Resume:
            if (!visible_) {
                break;
            }
            lastMayFetch_ = event.mayFetch;
            if (!attempting_ && event.mayFetch && Due()) {
                StartAttempt(&effects);
            }
            break;

        case EventType::Click:
            if (!visible_ || expirySwapped) {
                break;
            }
            {
                Effect effect;
                effect.type = EffectType::OpenLink;
                effects.push_back(std::move(effect));
            }
            break;

        case EventType::Pointer:
            if (!visible_) {
                break;
            }
            pointerOver_ = event.over;
            break;

        case EventType::Focus:
            if (!visible_) {
                break;
            }
            focusInside_ = event.inside;
            break;

        case EventType::Start:
            break;
        }

        ReconcileDisplay(&effects);
        ReconcileTimer(&effects);
        return effects;
    } catch (...) {
        return std::vector<Effect>();
    }
}

Machine::State Machine::GetState() const
{
    State state;
    state.record = record_;
    state.visible = visible_;
    state.gen = gen_;
    state.attempting = attempting_;
    state.shownOnline = displayed_.online;
    state.pendingSwap = pendingSwap_;
    state.floor = floor_;
    state.timerArmed = timerArmed_;
    state.timerAtMs = timerAtMs_;
    return state;
}

std::vector<std::string> Machine::CheckInvariants() const
{
    std::vector<std::string> broken;
    const Displayed desiredNow = Desired();

    if (record_.hwm < floor_) {
        broken.push_back("record hwm below floor");
    }
    if (record_.hwm < hwmHigh_) {
        broken.push_back("record hwm below high-water mark");
    }
    if (visible_ && displayed_.online &&
        !(nowMs_ < displayed_.expiresMs && Showable())) {
        broken.push_back("visible online display is not showable");
    }
    if (!visible_ && attempting_) {
        broken.push_back("hidden machine is attempting");
    }
    if (timerArmed_ && !visible_) {
        broken.push_back("timer is armed while hidden");
    }
    if (visible_ && !attempting_ && lastMayFetch_ &&
        (!timerArmed_ || timerAtMs_ > SaturatingAdd(nowMs_, kCheck))) {
        broken.push_back("visible idle fetch gate has no valid timer");
    }
    if (pendingSwap_ &&
        (!visible_ || (!pointerOver_ && !focusInside_) || !desiredNow.online)) {
        broken.push_back("pending swap lacks an active hold or online target");
    }
    if (record_.failures < 0 || record_.failures > 3) {
        broken.push_back("failure count out of range");
    }
    if (attempting_ != (phase_ != Phase::None)) {
        broken.push_back("attempting and phase disagree");
    }
    if (visible_ && !pendingSwap_ && !SameDisplayed(desiredNow)) {
        broken.push_back("displayed value differs from desired");
    }
    if (visible_ && !attempting_ && lastMayFetch_ && timerArmed_) {
        std::int64_t earliest = SaturatingAdd(nowMs_, kMinTimerAhead);
        if (displayed_.online) {
            earliest = std::min(earliest, displayed_.expiresMs);
        }
        if (Showable()) {
            earliest = std::min(earliest, record_.current.expiresMs);
        }
        if (timerAtMs_ < earliest) {
            broken.push_back("timer is earlier than its expiry lower bound");
        }
    }
    if (!visible_ && (pointerOver_ || focusInside_)) {
        broken.push_back("hidden machine has an active hold");
    }
    return broken;
}

} // namespace ob
