#pragma once

#include "bedrock/LevelRenderEvent.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <thread>

#include <pl/Mod.hpp>

namespace aeronautics::bedrock {

class HeartbeatHook;

class LevelRenderHook final {
public:
    LevelRenderHook(
        ll::mod::NativeMod& mod,
        HeartbeatHook& heartbeat,
        LevelRenderBus& eventBus) noexcept;
    ~LevelRenderHook();

    LevelRenderHook(const LevelRenderHook&) = delete;
    LevelRenderHook& operator=(const LevelRenderHook&) = delete;

    [[nodiscard]] bool install();
    void uninstall() noexcept;

    [[nodiscard]] bool safeToUnload() const noexcept {
        return !mPatchInstalled.load(std::memory_order_acquire);
    }

    static void recordActive(
        void* levelRendererCamera,
        void* baseActorRenderContext,
        const void* viewRenderObject,
        void* clientInstance) noexcept;

private:
    void record(
        void* levelRendererCamera,
        void* baseActorRenderContext,
        const void* viewRenderObject,
        void* clientInstance) noexcept;
    void workerLoop();
    void writeStatus(std::string_view state) noexcept;
    void clearActiveRegistration() noexcept;

    [[nodiscard]] bool validateTarget();
    [[nodiscard]] bool patchSlot() noexcept;
    [[nodiscard]] bool restoreSlot() noexcept;
    [[nodiscard]] bool writeSlotPointer(
        std::uintptr_t value,
        int& writableErrno,
        int& restoreErrno) noexcept;
    [[nodiscard]] std::uintptr_t readSlotPointer() const noexcept;

    ll::mod::NativeMod& mMod;
    HeartbeatHook& mHeartbeat;
    LevelRenderBus& mEventBus;

    std::filesystem::path mStatusPath;
    std::thread mWorker;
    std::atomic_bool mStopRequested{false};
    std::atomic_bool mPatchInstalled{false};
    std::atomic<std::uint32_t> mCallbacksInFlight{0};
    std::atomic<std::uint64_t> mTotalCalls{0};
    std::atomic<std::uint64_t> mDeliveredCallbacks{0};
    std::atomic<std::uint32_t> mFirstThreadId{0};
    std::atomic<std::uint64_t> mOtherThreadCalls{0};
    std::atomic<std::uintptr_t> mFirstRenderer{0};
    std::atomic<std::uintptr_t> mLastRenderer{0};
    std::atomic<std::uint64_t> mRendererTransitions{0};
    std::atomic<std::uintptr_t> mLastRenderContext{0};
    std::atomic<std::uintptr_t> mLastViewRenderObject{0};
    std::atomic<std::uintptr_t> mLastClientInstance{0};

    std::string mModuleBuildId;
    std::uintmax_t mModuleFileSize{};
    std::uintptr_t mModuleLoadBase{};
    std::uintptr_t mSlotAddress{};
    std::uintptr_t mOriginalTarget{};
    std::string mObservedFunctionPrefix;
    std::size_t mPageSize{};
    int mOriginalProtection{};
    std::string mOriginalPermissions;

    bool mTargetValidated{};
    bool mRttiValidated{};
    bool mFunctionPrefixValidated{};
    bool mPatchAttempted{};
    bool mPatchEverInstalled{};
    bool mPatchRestoreAttempted{};
    bool mPatchRestoreSucceeded{};
    bool mRollbackAttempted{};
    bool mRollbackSucceeded{};
    int mInstallWritableErrno{};
    int mInstallRestoreErrno{};
    int mRollbackWritableErrno{};
    int mRollbackRestoreErrno{};
    int mUninstallWritableErrno{};
    int mUninstallRestoreErrno{};

    std::uint64_t mHeartbeatCallsAtArm{};
    std::uint64_t mHeartbeatCallsAtFirstActivity{};
    std::uint64_t mHeartbeatCallsAtPatch{};
    std::string mFailureReason;

    static std::atomic<LevelRenderHook*> sActive;
};

}  // namespace aeronautics::bedrock
