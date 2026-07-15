#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <thread>

#include <pl/Mod.hpp>
#include <pl/memory/Hook.hpp>

namespace aeronautics::bedrock {

class VtableSlotProbe final {
public:
    explicit VtableSlotProbe(ll::mod::NativeMod& mod) noexcept;
    ~VtableSlotProbe();

    VtableSlotProbe(const VtableSlotProbe&) = delete;
    VtableSlotProbe& operator=(const VtableSlotProbe&) = delete;

    [[nodiscard]] bool install();
    void uninstall() noexcept;

    [[nodiscard]] bool safeToUnload() const noexcept {
        return !mPatchInstalled.load(std::memory_order_acquire);
    }

    static void recordActive() noexcept;

private:
    using MenuCallback = bool (*)(void*);

    struct Counter final {
        std::atomic<std::uint64_t> total{0};
        std::atomic<std::uint64_t> menuTrue{0};
        std::atomic<std::uint64_t> menuFalse{0};
        std::atomic<std::uint64_t> menuUnknown{0};
        std::atomic<std::uint32_t> firstThreadId{0};
        std::atomic<std::uint64_t> otherThreadCalls{0};
    };

    struct IntervalStats final {
        std::uint64_t intervals{};
        std::uint64_t minimum{};
        std::uint64_t maximum{};
        bool initialized{};
    };

    static bool menuDetour(void* instance);

    void record() noexcept;
    void workerLoop();
    void sampleLoop();
    void createTimelineHeader() noexcept;
    void appendTimeline(
        std::uint64_t sequence,
        std::uint64_t elapsedMilliseconds,
        int menuState,
        bool stableState,
        std::uint64_t delta) noexcept;
    void updateIntervalStats(int menuState, std::uint64_t delta) noexcept;
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
    pl::memory::FuncPtr mMenuOriginalStorage{};
    std::atomic<MenuCallback> mMenuOriginalCallable{nullptr};
    pl::memory::HookHandle mMenuHook;

    std::atomic_int mMenuState{-1};
    std::atomic<std::uint64_t> mMenuObserverCalls{0};
    std::atomic<std::uint32_t> mCallbacksInFlight{0};
    std::atomic_bool mStopRequested{false};
    std::atomic_bool mPatchInstalled{false};
    std::thread mWorker;

    Counter mCounter;
    IntervalStats mMenuTrueIntervals;
    IntervalStats mMenuFalseIntervals;

    std::filesystem::path mProfilePath;
    std::filesystem::path mTimelinePath;
    std::string mModuleBuildId;
    std::uintmax_t mModuleFileSize{};
    std::uintptr_t mModuleLoadBase{};
    std::uintptr_t mHeartbeatTarget{};
    std::uintptr_t mTableStart{};
    std::uintptr_t mSlotAddress{};
    std::uintptr_t mOriginalTarget{};
    std::size_t mPageSize{};
    int mOriginalProtection{};
    std::string mOriginalPermissions;

    bool mTargetValidated{};
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

    std::uint64_t mMenuTrueObservedMilliseconds{};
    std::uint64_t mMenuFalseObservedMilliseconds{};
    std::uint64_t mMenuUnknownObservedMilliseconds{};
    std::uint64_t mPreviousTotal{};
    std::string mFailureReason;
    std::chrono::steady_clock::time_point mStartedAt{};
    std::chrono::steady_clock::time_point mPatchedAt{};

    static std::atomic<VtableSlotProbe*> sActive;
};

}  // namespace aeronautics::bedrock
