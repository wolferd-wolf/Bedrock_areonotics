#pragma once

#include "bedrock/LevelRenderEvent.hpp"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <thread>

#include <pl/Mod.hpp>

namespace aeronautics::bedrock {

class HeartbeatHook;

// 0.0.16 deliberately keeps the historical class name so the Android module
// lifecycle does not change. It performs a read-only census and never patches
// a renderer vtable or publishes render events.
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

    [[nodiscard]] bool safeToUnload() const noexcept { return true; }

private:
    void workerLoop() noexcept;
    void runCensus() noexcept;
    void writeStatus(const char* state) noexcept;

    ll::mod::NativeMod& mMod;
    HeartbeatHook& mHeartbeat;
    LevelRenderBus& mEventBus;

    std::filesystem::path mStatusPath;
    std::filesystem::path mCensusPath;
    std::thread mWorker;
    std::atomic_bool mStopRequested{false};
    std::atomic_bool mRunning{false};

    std::atomic<std::uint64_t> mCensusRuns{0};
    std::atomic<std::uint64_t> mTypeNamesFound{0};
    std::atomic<std::uint64_t> mTypeInfosFound{0};
    std::atomic<std::uint64_t> mVtablesFound{0};
    std::atomic<std::uint64_t> mExecutableSlot24Targets{0};
    std::atomic<std::uint64_t> mWritableVptrReferences{0};
    std::atomic<std::uint64_t> mReadableBytesScanned{0};
    std::atomic<std::uint64_t> mWritableBytesScanned{0};
    std::atomic_bool mFingerprintValidated{false};
    std::atomic_bool mCompleted{false};
    std::atomic_bool mFailed{false};
};

}  // namespace aeronautics::bedrock
