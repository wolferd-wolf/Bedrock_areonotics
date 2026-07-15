#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <thread>

#include <pl/Mod.hpp>

namespace aeronautics::bedrock {

class HeartbeatHook;

class ClientLevelTickProbe final {
public:
    ClientLevelTickProbe(ll::mod::NativeMod& mod, HeartbeatHook& heartbeat) noexcept;
    ~ClientLevelTickProbe();

    ClientLevelTickProbe(const ClientLevelTickProbe&) = delete;
    ClientLevelTickProbe& operator=(const ClientLevelTickProbe&) = delete;

    [[nodiscard]] bool install();
    void uninstall() noexcept;

    [[nodiscard]] bool safeToUnload() const noexcept {
        return !mPatchInstalled.load(std::memory_order_acquire);
    }

    static void recordActive(void* instance) noexcept;

private:
    void record(void* instance) noexcept;
    void workerLoop();
    void sampleLoop();
    void createTimelineHeader() noexcept;
    void appendTimeline(
        std::uint64_t sequence,
        std::uint64_t elapsedMilliseconds,
        std::uint64_t heartbeatCalls,
        std::uint64_t totalDelta) noexcept;
    void writeProfile(std::string_view state) noexcept;
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

    std::atomic<std::uint64_t> mTotalCalls{0};
    std::atomic<std::uint32_t> mFirstThreadId{0};
    std::atomic<std::uint64_t> mOtherThreadCalls{0};
    std::atomic<std::uintptr_t> mFirstInstance{0};
    std::atomic<std::uintptr_t> mLastInstance{0};
    std::atomic<std::uint64_t> mInstanceTransitions{0};
    std::atomic<std::uint32_t> mCallbacksInFlight{0};
    std::atomic_bool mStopRequested{false};
    std::atomic_bool mPatchInstalled{false};
    std::thread mWorker;

    std::filesystem::path mProfilePath;
    std::filesystem::path mTimelinePath;
    std::string mModuleBuildId;
    std::uintmax_t mModuleFileSize{};
    std::uintptr_t mModuleLoadBase{};
    std::uintptr_t mVtableAddressPoint{};
    std::uintptr_t mTypeInfoAddress{};
    std::uintptr_t mTypeNameAddress{};
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
    std::uint64_t mPreviousTotal{};
    std::string mFailureReason;
    std::chrono::steady_clock::time_point mPatchedAt{};

    static std::atomic<ClientLevelTickProbe*> sActive;
};

}  // namespace aeronautics::bedrock
