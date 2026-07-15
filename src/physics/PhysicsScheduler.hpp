#pragma once

#include "bedrock/ClientLevelTickEvent.hpp"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <string_view>
#include <thread>

#include <pl/Mod.hpp>

namespace aeronautics::physics {

struct PhysicsRenderSnapshot final {
    std::int64_t previousPositionYMicrometers{};
    std::int64_t currentPositionYMicrometers{};
    std::int64_t lastPhysicsTickNanoseconds{};
    std::uint64_t simulationStep{};
    std::uint64_t worldGeneration{};
    std::uintptr_t activeClientLevel{};
    bool grounded{};
    bool coherent{};
};

class PhysicsScheduler final : public bedrock::ClientLevelTickListener {
public:
    PhysicsScheduler(
        ll::mod::NativeMod& mod,
        bedrock::ClientLevelTickBus& tickBus) noexcept;
    ~PhysicsScheduler() override;

    PhysicsScheduler(const PhysicsScheduler&) = delete;
    PhysicsScheduler& operator=(const PhysicsScheduler&) = delete;

    [[nodiscard]] bool start();
    void stop() noexcept;

    void onClientLevelTick(
        const bedrock::ClientLevelTickEvent& event) noexcept override;

    [[nodiscard]] PhysicsRenderSnapshot renderSnapshot() const noexcept;

private:
    void writerLoop();
    void resetBodyState() noexcept;
    void publishRenderSnapshot(
        std::int64_t previousPosition,
        std::int64_t currentPosition,
        std::int64_t tickNanoseconds,
        std::uint64_t simulationStep) noexcept;
    void createTimelineHeader() noexcept;
    void appendTimeline(std::uint64_t sequence, std::uint64_t elapsedMs) noexcept;
    void writeStatus(std::string_view state) noexcept;

    ll::mod::NativeMod& mMod;
    bedrock::ClientLevelTickBus& mTickBus;

    std::filesystem::path mStatusPath;
    std::filesystem::path mTimelinePath;
    std::thread mWriter;
    std::atomic_bool mRunning{false};
    std::atomic_bool mStopRequested{false};
    std::atomic_bool mSubscribed{false};

    std::atomic<std::uint64_t> mTotalEvents{0};
    std::atomic<std::uint64_t> mSimulationSteps{0};
    std::atomic<std::uint64_t> mLastEventSequence{0};
    std::atomic<std::uint32_t> mLastThreadId{0};
    std::atomic<std::uintptr_t> mActiveClientLevel{0};
    std::atomic<std::uint64_t> mWorldGeneration{0};
    std::atomic<std::uint64_t> mWorldTransitions{0};
    std::atomic<std::uint64_t> mWorldTick{0};
    std::atomic<std::uint64_t> mImpactCount{0};
    std::atomic<std::uint64_t> mBodySleepCount{0};
    std::atomic<std::uint64_t> mBodyWakeCount{0};
    std::atomic<std::uint64_t> mGroundedTicks{0};
    std::atomic<std::uint64_t> mBodyCycleResets{0};
    std::atomic<std::uint64_t> mNullInstanceEvents{0};
    std::atomic<std::int64_t> mPositionYMicrometers{10'000'000};
    std::atomic<std::int64_t> mVelocityYMicrometersPerSecond{0};
    std::atomic_bool mGrounded{false};

    std::atomic<std::uint64_t> mRenderSnapshotSequence{0};
    std::atomic<std::int64_t> mRenderPreviousPositionYMicrometers{10'000'000};
    std::atomic<std::int64_t> mRenderCurrentPositionYMicrometers{10'000'000};
    std::atomic<std::int64_t> mRenderLastPhysicsTickNanoseconds{0};
    std::atomic<std::uint64_t> mRenderSimulationStep{0};
    std::atomic<std::uint64_t> mRenderWorldGeneration{0};
    std::atomic<std::uintptr_t> mRenderActiveClientLevel{0};
    std::atomic_bool mRenderGrounded{false};
};

}  // namespace aeronautics::physics
