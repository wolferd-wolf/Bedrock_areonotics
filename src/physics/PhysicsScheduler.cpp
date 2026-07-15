#include "physics/PhysicsScheduler.hpp"

#include <chrono>
#include <fstream>
#include <iomanip>
#include <string>
#include <system_error>

namespace aeronautics::physics {
namespace {

constexpr std::int64_t fixedStepMilliseconds = 50;
constexpr std::int64_t gravityMicrometersPerSecondSquared = -9'810'000;
constexpr std::int64_t initialHeightMicrometers = 10'000'000;
constexpr std::int64_t minimumBounceSpeedMicrometersPerSecond = 100'000;
constexpr std::int64_t restitutionNumerator = 35;
constexpr std::int64_t restitutionDenominator = 100;
constexpr std::uint64_t testCycleTicks = 200;
constexpr std::uint64_t sampleIntervalMilliseconds = 2000;

[[nodiscard]] double micrometersToMeters(std::int64_t value) noexcept {
    return static_cast<double>(value) / 1'000'000.0;
}

}  // namespace

PhysicsScheduler::PhysicsScheduler(
    ll::mod::NativeMod& mod,
    bedrock::ClientLevelTickBus& tickBus) noexcept
    : mMod(mod), mTickBus(tickBus) {}

PhysicsScheduler::~PhysicsScheduler() {
    stop();
}

bool PhysicsScheduler::start() {
    if (mRunning.load(std::memory_order_acquire)) {
        return true;
    }

    mStatusPath = mMod.getDataDir() / "physics-scheduler-status.txt";
    mTimelinePath = mMod.getDataDir() / "physics-scheduler-timeline.txt";
    mStopRequested.store(false, std::memory_order_release);
    mTotalEvents.store(0, std::memory_order_relaxed);
    mSimulationSteps.store(0, std::memory_order_relaxed);
    mLastEventSequence.store(0, std::memory_order_relaxed);
    mLastThreadId.store(0, std::memory_order_relaxed);
    mActiveClientLevel.store(0, std::memory_order_relaxed);
    mWorldGeneration.store(0, std::memory_order_relaxed);
    mWorldTransitions.store(0, std::memory_order_relaxed);
    mWorldTick.store(0, std::memory_order_relaxed);
    mImpactCount.store(0, std::memory_order_relaxed);
    mBodyCycleResets.store(0, std::memory_order_relaxed);
    mNullInstanceEvents.store(0, std::memory_order_relaxed);
    resetBodyState();
    createTimelineHeader();

    if (!mTickBus.subscribe(*this)) {
        writeStatus("subscription_failed");
        mMod.getLogger().error(
            "Physics scheduler could not subscribe to ClientLevel tick bus");
        return false;
    }
    mSubscribed.store(true, std::memory_order_release);

    try {
        mWriter = std::thread([this] { writerLoop(); });
    } catch (const std::system_error& error) {
        mTickBus.unsubscribe(*this);
        mSubscribed.store(false, std::memory_order_release);
        writeStatus("writer_start_failed");
        mMod.getLogger().error(
            "Physics scheduler telemetry writer failed: {}",
            error.what());
        return false;
    }

    mRunning.store(true, std::memory_order_release);
    writeStatus("waiting_for_tick_events");
    mMod.getLogger().info(
        "Physics scheduler started; fixed_step_ms=50; integration=semi_implicit_euler_fixed_point");
    return true;
}

void PhysicsScheduler::stop() noexcept {
    mStopRequested.store(true, std::memory_order_release);
    if (mWriter.joinable()) {
        mWriter.join();
    }

    if (mSubscribed.exchange(false, std::memory_order_acq_rel)) {
        mTickBus.unsubscribe(*this);
    }
    mRunning.store(false, std::memory_order_release);
    writeStatus("stopped");
}

void PhysicsScheduler::onClientLevelTick(
    const bedrock::ClientLevelTickEvent& event) noexcept {
    mTotalEvents.fetch_add(1, std::memory_order_relaxed);
    mLastEventSequence.store(event.sequence, std::memory_order_relaxed);
    mLastThreadId.store(event.threadId, std::memory_order_relaxed);

    const std::uintptr_t currentInstance =
        reinterpret_cast<std::uintptr_t>(event.clientLevel);
    if (currentInstance == 0) {
        mNullInstanceEvents.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    const std::uintptr_t previousInstance =
        mActiveClientLevel.load(std::memory_order_relaxed);
    if (previousInstance != currentInstance) {
        mActiveClientLevel.store(currentInstance, std::memory_order_relaxed);
        mWorldGeneration.fetch_add(1, std::memory_order_relaxed);
        if (previousInstance != 0) {
            mWorldTransitions.fetch_add(1, std::memory_order_relaxed);
        }
        mWorldTick.store(0, std::memory_order_relaxed);
        resetBodyState();
    }

    const std::uint64_t worldTick =
        mWorldTick.fetch_add(1, std::memory_order_relaxed) + 1U;
    if (worldTick > 1U && ((worldTick - 1U) % testCycleTicks) == 0U) {
        resetBodyState();
        mBodyCycleResets.fetch_add(1, std::memory_order_relaxed);
    }

    std::int64_t velocity =
        mVelocityYMicrometersPerSecond.load(std::memory_order_relaxed);
    std::int64_t position =
        mPositionYMicrometers.load(std::memory_order_relaxed);

    velocity +=
        gravityMicrometersPerSecondSquared * fixedStepMilliseconds / 1000;
    position += velocity * fixedStepMilliseconds / 1000;

    if (position <= 0) {
        position = 0;
        if (velocity < -minimumBounceSpeedMicrometersPerSecond) {
            velocity =
                (-velocity * restitutionNumerator) / restitutionDenominator;
            mImpactCount.fetch_add(1, std::memory_order_relaxed);
        } else {
            velocity = 0;
        }
    }

    mVelocityYMicrometersPerSecond.store(velocity, std::memory_order_relaxed);
    mPositionYMicrometers.store(position, std::memory_order_relaxed);
    mSimulationSteps.fetch_add(1, std::memory_order_relaxed);
}

void PhysicsScheduler::writerLoop() {
    using namespace std::chrono_literals;
    const auto startedAt = std::chrono::steady_clock::now();
    std::uint64_t sequence = 0;

    while (!mStopRequested.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(
            std::chrono::milliseconds(sampleIntervalMilliseconds));
        if (mStopRequested.load(std::memory_order_acquire)) {
            break;
        }

        ++sequence;
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - startedAt);
        appendTimeline(sequence, static_cast<std::uint64_t>(elapsed.count()));
        writeStatus(
            mTotalEvents.load(std::memory_order_relaxed) == 0
                ? "waiting_for_tick_events"
                : "running_fixed_step_simulation");
    }
}

void PhysicsScheduler::resetBodyState() noexcept {
    mPositionYMicrometers.store(
        initialHeightMicrometers,
        std::memory_order_relaxed);
    mVelocityYMicrometersPerSecond.store(0, std::memory_order_relaxed);
}

void PhysicsScheduler::createTimelineHeader() noexcept {
    std::ofstream output(mTimelinePath, std::ios::trunc);
    if (output) {
        output << "schema=1\n"
               << "interval_ms=" << sampleIntervalMilliseconds << "\n"
               << "fixed_step_ms=" << fixedStepMilliseconds << "\n"
               << "columns=sequence,elapsed_ms,total_events,simulation_steps,world_generation,world_transitions,world_tick,active_clientlevel,position_y_um,velocity_y_um_per_s,impact_count,body_cycle_resets,last_event_sequence,last_thread_id\n";
    }
}

void PhysicsScheduler::appendTimeline(
    std::uint64_t sequence,
    std::uint64_t elapsedMs) noexcept {
    std::ofstream output(mTimelinePath, std::ios::app);
    if (!output) {
        return;
    }

    output << sequence << ','
           << elapsedMs << ','
           << mTotalEvents.load(std::memory_order_relaxed) << ','
           << mSimulationSteps.load(std::memory_order_relaxed) << ','
           << mWorldGeneration.load(std::memory_order_relaxed) << ','
           << mWorldTransitions.load(std::memory_order_relaxed) << ','
           << mWorldTick.load(std::memory_order_relaxed) << ",0x"
           << std::hex << mActiveClientLevel.load(std::memory_order_relaxed)
           << std::dec << ','
           << mPositionYMicrometers.load(std::memory_order_relaxed) << ','
           << mVelocityYMicrometersPerSecond.load(std::memory_order_relaxed) << ','
           << mImpactCount.load(std::memory_order_relaxed) << ','
           << mBodyCycleResets.load(std::memory_order_relaxed) << ','
           << mLastEventSequence.load(std::memory_order_relaxed) << ','
           << mLastThreadId.load(std::memory_order_relaxed) << '\n';
}

void PhysicsScheduler::writeStatus(std::string_view state) noexcept {
    std::ofstream output(mStatusPath, std::ios::trunc);
    if (!output) {
        return;
    }

    const std::int64_t position =
        mPositionYMicrometers.load(std::memory_order_relaxed);
    const std::int64_t velocity =
        mVelocityYMicrometersPerSecond.load(std::memory_order_relaxed);

    output << "schema=1\n"
           << "state=" << state << '\n'
           << "scheduler=clientlevel_tick_event_fixed_step\n"
           << "integration=semi_implicit_euler_fixed_point\n"
           << "fixed_step_ms=" << fixedStepMilliseconds << '\n'
           << "gravity_m_per_s2=-9.81\n"
           << "test_body_initial_height_m=10.0\n"
           << "test_body_restitution=0.35\n"
           << "test_cycle_ticks=" << testCycleTicks << '\n'
           << "tick_bus_listener_count=" << mTickBus.listenerCount() << '\n'
           << "tick_bus_published_events=" << mTickBus.publishedEvents() << '\n'
           << "tick_bus_delivered_callbacks=" << mTickBus.deliveredCallbacks() << '\n'
           << "total_events=" << mTotalEvents.load(std::memory_order_relaxed) << '\n'
           << "simulation_steps="
           << mSimulationSteps.load(std::memory_order_relaxed) << '\n'
           << "last_event_sequence="
           << mLastEventSequence.load(std::memory_order_relaxed) << '\n'
           << "last_thread_id="
           << mLastThreadId.load(std::memory_order_relaxed) << '\n'
           << "active_clientlevel=0x" << std::hex
           << mActiveClientLevel.load(std::memory_order_relaxed) << std::dec << '\n'
           << "world_generation="
           << mWorldGeneration.load(std::memory_order_relaxed) << '\n'
           << "world_transitions="
           << mWorldTransitions.load(std::memory_order_relaxed) << '\n'
           << "world_tick=" << mWorldTick.load(std::memory_order_relaxed) << '\n'
           << "position_y_um=" << position << '\n'
           << "velocity_y_um_per_s=" << velocity << '\n'
           << std::fixed << std::setprecision(6)
           << "position_y_m=" << micrometersToMeters(position) << '\n'
           << "velocity_y_m_per_s=" << micrometersToMeters(velocity) << '\n'
           << "impact_count=" << mImpactCount.load(std::memory_order_relaxed) << '\n'
           << "body_cycle_resets="
           << mBodyCycleResets.load(std::memory_order_relaxed) << '\n'
           << "null_instance_events="
           << mNullInstanceEvents.load(std::memory_order_relaxed) << '\n'
           << "timeline_file=physics-scheduler-timeline.txt\n";
}

}  // namespace aeronautics::physics
