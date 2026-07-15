#pragma once

#include "bedrock/LevelRenderEvent.hpp"
#include "physics/PhysicsScheduler.hpp"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <string_view>
#include <thread>

#include <pl/Mod.hpp>

namespace aeronautics::render {

class DiagnosticCubeRenderProbe final : public bedrock::LevelRenderListener {
public:
    DiagnosticCubeRenderProbe(
        ll::mod::NativeMod& mod,
        bedrock::LevelRenderBus& renderBus,
        physics::PhysicsScheduler& physicsScheduler) noexcept;
    ~DiagnosticCubeRenderProbe() override;

    DiagnosticCubeRenderProbe(const DiagnosticCubeRenderProbe&) = delete;
    DiagnosticCubeRenderProbe& operator=(const DiagnosticCubeRenderProbe&) = delete;

    [[nodiscard]] bool start();
    void stop() noexcept;

    void onLevelRender(const bedrock::LevelRenderEvent& event) noexcept override;

private:
    void writerLoop();
    void createTimelineHeader() noexcept;
    void appendTimeline(std::uint64_t sequence, std::uint64_t elapsedMs) noexcept;
    void writeStatus(std::string_view state) noexcept;

    ll::mod::NativeMod& mMod;
    bedrock::LevelRenderBus& mRenderBus;
    physics::PhysicsScheduler& mPhysicsScheduler;

    std::filesystem::path mStatusPath;
    std::filesystem::path mTimelinePath;
    std::thread mWriter;
    std::atomic_bool mRunning{false};
    std::atomic_bool mStopRequested{false};
    std::atomic_bool mSubscribed{false};

    std::atomic<std::uint64_t> mTotalRenderEvents{0};
    std::atomic<std::uint64_t> mPhysicsSnapshotReads{0};
    std::atomic<std::uint64_t> mIncoherentSnapshotReads{0};
    std::atomic<std::uint64_t> mFramesWithActiveWorld{0};
    std::atomic<std::uint64_t> mPhysicsStepTransitions{0};
    std::atomic<std::uint64_t> mLastPhysicsStep{0};
    std::atomic<std::uint64_t> mLastRenderEventSequence{0};
    std::atomic<std::uint32_t> mFirstThreadId{0};
    std::atomic<std::uint32_t> mLastThreadId{0};
    std::atomic<std::uint64_t> mOtherThreadCalls{0};

    std::atomic<std::uintptr_t> mFirstRenderer{0};
    std::atomic<std::uintptr_t> mLastRenderer{0};
    std::atomic<std::uint64_t> mRendererTransitions{0};
    std::atomic<std::uintptr_t> mLastRenderContext{0};
    std::atomic<std::uintptr_t> mLastViewRenderObject{0};
    std::atomic<std::uintptr_t> mLastClientInstance{0};
    std::atomic<std::uint64_t> mNullRendererEvents{0};
    std::atomic<std::uint64_t> mNullContextEvents{0};
    std::atomic<std::uint64_t> mNullViewEvents{0};
    std::atomic<std::uint64_t> mNullClientEvents{0};

    std::atomic<std::int64_t> mPreviousPositionYMicrometers{0};
    std::atomic<std::int64_t> mCurrentPositionYMicrometers{0};
    std::atomic<std::int64_t> mInterpolatedPositionYMicrometers{0};
    std::atomic<std::uint32_t> mInterpolationAlphaPartsPerMillion{0};
    std::atomic<std::uint64_t> mWorldGeneration{0};
    std::atomic<std::uintptr_t> mActiveClientLevel{0};
    std::atomic_bool mGrounded{false};
};

}  // namespace aeronautics::render
