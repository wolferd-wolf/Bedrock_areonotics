#pragma once

#include "bedrock/ClientLevelTickEvent.hpp"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <string_view>
#include <thread>

#include <pl/Mod.hpp>

namespace aeronautics::physics {

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

private:
    void writerLoop();
    void resetBodyState() noexcept;
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
    std::atomic<std::uint64_t> mBodyCycleResets{0};
    std::atomic<std::uint64_t> mNullInstanceEvents{0};
    std::atomic<std::int64_t> mPositionYMicrometers{10'000'000};
    std::atomic<std::int64_t> mVelocityYMicrometersPerSecond{0};
};

}  // namespace aeronautics::physics
