#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <thread>

#include <pl/Mod.hpp>

#if defined(__ANDROID__) && !defined(__cpp_lib_atomic_ref)
namespace std {
template <typename T>
class atomic_ref final {
public:
    explicit atomic_ref(T& value) noexcept : mValue(value) {}

    void store(T value, memory_order order = memory_order_seq_cst) const noexcept {
        const int builtinOrder = order == memory_order_relaxed
            ? __ATOMIC_RELAXED
            : (order == memory_order_release ? __ATOMIC_RELEASE : __ATOMIC_SEQ_CST);
        __atomic_store_n(&mValue, value, builtinOrder);
    }

private:
    T& mValue;
};
}  // namespace std
#endif

namespace aeronautics::bedrock {

class HeartbeatHook;

class VtableSlotProbeV3 final {
public:
    VtableSlotProbeV3(ll::mod::NativeMod& mod, HeartbeatHook& heartbeat) noexcept;
    ~VtableSlotProbeV3();

    VtableSlotProbeV3(const VtableSlotProbeV3&) = delete;
    VtableSlotProbeV3& operator=(const VtableSlotProbeV3&) = delete;

    [[nodiscard]] bool install();
    void uninstall() noexcept;

    [[nodiscard]] bool safeToUnload() const noexcept {
        return !mPatchInstalled.load(std::memory_order_acquire);
    }

    static void recordActive() noexcept;

private:
    void record() noexcept;
    void workerLoop();
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
    std::atomic<std::uint32_t> mCallbacksInFlight{0};
    std::atomic_bool mStopRequested{false};
    std::atomic_bool mPatchInstalled{false};
    std::thread mWorker;

    std::filesystem::path mProfilePath;
    std::filesystem::path mTimelinePath;
    std::string mModuleBuildId;
    std::uintmax_t mModuleFileSize{};
    std::uintptr_t mModuleLoadBase{};
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
    std::uint64_t mHeartbeatCallsAtArm{};
    std::uint64_t mHeartbeatCallsAtPatch{};
    std::uint64_t mPreviousTotal{};
    std::string mFailureReason;
    std::chrono::steady_clock::time_point mArmedAt{};
    std::chrono::steady_clock::time_point mPatchedAt{};

    static std::atomic<VtableSlotProbeV3*> sActive;
};

}  // namespace aeronautics::bedrock
