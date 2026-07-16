#pragma once

#include "bedrock/LevelRenderEvent.hpp"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <string>
#include <thread>

#include <pl/Mod.hpp>
#include <pl/memory/Hook.hpp>

namespace aeronautics::physics {
class PhysicsScheduler;
}

namespace aeronautics::bedrock {

class HeartbeatHook;

class LevelRenderHook final {
public:
    LevelRenderHook(
        ll::mod::NativeMod& mod,
        HeartbeatHook& heartbeat,
        LevelRenderBus& eventBus,
        physics::PhysicsScheduler& physicsScheduler) noexcept;
    ~LevelRenderHook();

    LevelRenderHook(const LevelRenderHook&) = delete;
    LevelRenderHook& operator=(const LevelRenderHook&) = delete;

    [[nodiscard]] bool install();
    void uninstall() noexcept;
    [[nodiscard]] bool safeToUnload() const noexcept;

    static void recordMinecraftRenderPre(
        void* renderer,
        void* context,
        const void* view,
        void* client) noexcept;
    static void recordMinecraftRenderPost(
        void* renderer,
        void* context,
        const void* view,
        void* client) noexcept;
    static void recordChunkLayerTaskBegin(
        const void* closure,
        const void* taskContext) noexcept;
    static void recordChunkLayerTaskEnd() noexcept;

private:
    void workerLoop() noexcept;
    bool installHooks() noexcept;
    void removeHooks() noexcept;
    void createTimeline() noexcept;
    void appendTimeline(const char* state) noexcept;
    void writeStatus(const char* state) noexcept;

    ll::mod::NativeMod& mMod;
    HeartbeatHook& mHeartbeat;
    LevelRenderBus& mEventBus;
    physics::PhysicsScheduler& mPhysicsScheduler;
    std::filesystem::path mStatusPath;
    std::filesystem::path mTimelinePath;
    std::thread mWorker;
    std::atomic_bool mStopRequested{false};
    std::atomic_bool mRestoreSucceeded{false};
    std::atomic<std::uint32_t> mCallbacksInFlight{0};

    std::atomic<std::uint64_t> mMinecraftPreCalls{0};
    std::atomic<std::uint64_t> mMinecraftPostCalls{0};
    std::atomic<std::uint64_t> mChunkLayerTaskCalls{0};
    std::atomic<std::uint32_t> mMinecraftThreadId{0};
    std::atomic<std::uint32_t> mChunkLayerThreadId{0};
    std::atomic<std::uint64_t> mOtherMinecraftThreadCalls{0};
    std::atomic<std::uint64_t> mOtherChunkLayerThreadCalls{0};

    std::atomic<std::uintptr_t> mFirstRenderer{0};
    std::atomic<std::uintptr_t> mLastContext{0};
    std::atomic<std::uintptr_t> mLastView{0};
    std::atomic<std::uintptr_t> mLastClient{0};
    std::atomic<std::uintptr_t> mFirstChunkLayerClosure{0};
    std::atomic<std::uintptr_t> mLastChunkLayerClosure{0};
    std::atomic<std::uintptr_t> mLastChunkLayerTaskContext{0};

    std::atomic<std::uintptr_t> mClosureQword220{0};
    std::atomic<std::uintptr_t> mClosureQword228{0};
    std::atomic<std::uintptr_t> mClosureQword230{0};
    std::atomic<std::uintptr_t> mClosureQword238{0};
    std::atomic<std::uint32_t> mClosureDword240{0};
    std::atomic<std::uint64_t> mClosureQword248{0};
    std::atomic<std::uint64_t> mClosureQword250{0};
    std::atomic<std::uintptr_t> mClosureQword258{0};
    std::atomic<float> mClosureFloat260{0.0F};

    std::atomic_bool mFingerprintValidated{false};
    std::atomic_bool mMinecraftPrefixValidated{false};
    std::atomic_bool mChunkLayerPrefixValidated{false};
    std::string mFailureReason;

    pl::memory::FuncPtr mMinecraftOriginalStorage{};
    pl::memory::FuncPtr mChunkLayerOriginalStorage{};
    pl::memory::HookHandle mMinecraftHook;
    pl::memory::HookHandle mChunkLayerHook;

    static std::atomic<LevelRenderHook*> sActive;
};

}  // namespace aeronautics::bedrock
