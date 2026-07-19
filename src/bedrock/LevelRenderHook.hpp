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
    static void recordTerrainTaskBegin(
        const void* closure,
        const void* taskContext) noexcept;
    static void recordTerrainTaskEnd() noexcept;
    static void recordTerrainCommandHelper(
        const void* destination,
        const void* commandVector,
        const void* commandContext,
        const void* renderObject,
        const void* view,
        const void* sharedOwner,
        const void* descriptor,
        std::uint32_t mode,
        float scale) noexcept;
    static void recordTerrainCommandHelperEnd() noexcept;

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
    std::atomic<std::uint64_t> mTerrainTaskCalls{0};
    std::atomic<std::uint64_t> mTerrainHelperCalls{0};
    std::atomic<std::uint64_t> mTerrainHelperInsideTaskCalls{0};
    std::atomic<std::uint64_t> mTerrainHelperOutsideTaskCalls{0};
    std::atomic<std::uint32_t> mMinecraftThreadId{0};
    std::atomic<std::uint32_t> mTerrainThreadId{0};
    std::atomic<std::uint32_t> mTerrainHelperThreadId{0};
    std::atomic<std::uint64_t> mOtherMinecraftThreadCalls{0};
    std::atomic<std::uint64_t> mOtherTerrainThreadCalls{0};
    std::atomic<std::uint64_t> mOtherTerrainHelperThreadCalls{0};

    std::atomic<std::uintptr_t> mFirstRenderer{0};
    std::atomic<std::uintptr_t> mLastContext{0};
    std::atomic<std::uintptr_t> mLastView{0};
    std::atomic<std::uintptr_t> mLastClient{0};
    std::atomic<std::uintptr_t> mFirstTerrainClosure{0};
    std::atomic<std::uintptr_t> mLastTerrainClosure{0};
    std::atomic<std::uintptr_t> mLastTerrainTaskContext{0};

    std::atomic_bool mFingerprintValidated{false};
    std::atomic_bool mMinecraftPrefixValidated{false};
    std::atomic_bool mTerrainPrefixValidated{false};
    std::atomic_bool mTerrainHelperPrefixValidated{false};
    std::string mFailureReason;

    pl::memory::FuncPtr mMinecraftOriginalStorage{};
    pl::memory::FuncPtr mTerrainOriginalStorage{};
    pl::memory::FuncPtr mTerrainHelperOriginalStorage{};
    pl::memory::HookHandle mMinecraftHook;
    pl::memory::HookHandle mTerrainHook;
    pl::memory::HookHandle mTerrainHelperHook;

    static std::atomic<LevelRenderHook*> sActive;
};

}  // namespace aeronautics::bedrock
