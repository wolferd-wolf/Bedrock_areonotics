#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <thread>

#include <pl/Mod.hpp>
#include <pl/memory/Hook.hpp>

namespace aeronautics::bedrock {

struct GlossRegisterOpaque;

class VtableNeighbourProbe final {
public:
    explicit VtableNeighbourProbe(ll::mod::NativeMod& mod) noexcept;
    ~VtableNeighbourProbe();

    VtableNeighbourProbe(const VtableNeighbourProbe&) = delete;
    VtableNeighbourProbe& operator=(const VtableNeighbourProbe&) = delete;

    [[nodiscard]] bool install();
    void uninstall() noexcept;

private:
    static constexpr std::size_t probeCount = 5;
    using MenuCallback = bool (*)(void*);
    using GlossHookHandle = void*;
    using GlossInternalCallback = void (*)(GlossRegisterOpaque*, GlossHookHandle);
    using GlossHookInternalFn = GlossHookHandle (*)(
        void*,
        GlossInternalCallback,
        void*,
        bool,
        int);
    using GlossHookDeleteFn = void (*)(GlossHookHandle);

    struct ProbeCounter final {
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

    struct ProbeMetadata final {
        std::uint32_t slotIndex{};
        std::int32_t relativeIndex{};
        std::uintptr_t targetOffset{};
        std::string label;
        std::string instructionPrefix;
        bool validated{};
        bool hookInstalled{};
    };

    static bool menuDetour(void* instance);
    static void probe0(GlossRegisterOpaque*, GlossHookHandle);
    static void probe1(GlossRegisterOpaque*, GlossHookHandle);
    static void probe2(GlossRegisterOpaque*, GlossHookHandle);
    static void probe3(GlossRegisterOpaque*, GlossHookHandle);
    static void probe4(GlossRegisterOpaque*, GlossHookHandle);

    void recordProbe(std::size_t index) noexcept;
    void samplerLoop();
    void updateIntervalStats(
        std::size_t probeIndex,
        int menuState,
        std::uint64_t delta) noexcept;
    void appendTimeline(
        std::uint64_t sequence,
        std::uint64_t elapsedMilliseconds,
        int menuState,
        bool stableState,
        const std::array<std::uint64_t, probeCount>& deltas) noexcept;
    void writeProfile(std::string_view state) noexcept;
    void clearActiveRegistration() noexcept;
    [[nodiscard]] bool resolveGlossApi() noexcept;
    [[nodiscard]] bool validateTargets();
    [[nodiscard]] std::size_t installInternalHooks() noexcept;

    ll::mod::NativeMod& mMod;
    pl::memory::FuncPtr mMenuOriginalStorage{};
    std::atomic<MenuCallback> mMenuOriginalCallable{nullptr};
    pl::memory::HookHandle mMenuHook;
    std::atomic_int mMenuState{-1};
    std::atomic<std::uint64_t> mMenuObserverCalls{0};
    std::atomic<std::uint32_t> mCallbacksInFlight{0};
    std::atomic_bool mStopRequested{false};
    std::thread mSampler;

    GlossHookInternalFn mGlossHookInternal{};
    GlossHookDeleteFn mGlossHookDelete{};
    std::array<GlossHookHandle, probeCount> mProbeHandles{};
    std::array<ProbeCounter, probeCount> mCounters{};
    std::array<ProbeMetadata, probeCount> mMetadata{};
    std::array<IntervalStats, probeCount> mMenuTrueIntervals{};
    std::array<IntervalStats, probeCount> mMenuFalseIntervals{};
    std::array<std::uint64_t, probeCount> mPreviousTotals{};

    std::filesystem::path mProfilePath;
    std::filesystem::path mTimelinePath;
    std::string mModuleBuildId;
    std::uintmax_t mModuleFileSize{};
    std::uintptr_t mModuleLoadBase{};
    std::uintptr_t mHeartbeatTarget{};
    std::uint64_t mMenuTrueObservedMilliseconds{};
    std::uint64_t mMenuFalseObservedMilliseconds{};
    std::uint64_t mMenuUnknownObservedMilliseconds{};
    std::size_t mInstalledProbeCount{};
    std::string mFailureReason;
    std::chrono::steady_clock::time_point mStartedAt{};

    static std::atomic<VtableNeighbourProbe*> sActive;
};

}  // namespace aeronautics::bedrock
