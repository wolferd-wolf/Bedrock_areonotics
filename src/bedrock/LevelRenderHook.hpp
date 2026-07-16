#pragma once

#include "bedrock/LevelRenderEvent.hpp"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <thread>

#include <pl/Mod.hpp>

namespace aeronautics::bedrock {

class HeartbeatHook;

class LevelRenderHook final {
public:
    LevelRenderHook(ll::mod::NativeMod& mod, HeartbeatHook& heartbeat, LevelRenderBus& eventBus) noexcept;
    ~LevelRenderHook();

    LevelRenderHook(const LevelRenderHook&) = delete;
    LevelRenderHook& operator=(const LevelRenderHook&) = delete;

    [[nodiscard]] bool install();
    void uninstall() noexcept;
    [[nodiscard]] bool safeToUnload() const noexcept;

    static void recordMinecraftRender(void* renderer, void* context, const void* view, void* client) noexcept;
    static void recordPresent(bool vulkan) noexcept;

private:
    void workerLoop() noexcept;
    bool installHooks() noexcept;
    void removeHooks() noexcept;
    void writeStatus(const char* state) noexcept;

    ll::mod::NativeMod& mMod;
    HeartbeatHook& mHeartbeat;
    LevelRenderBus& mEventBus;
    std::filesystem::path mStatusPath;
    std::thread mWorker;
    std::atomic_bool mStopRequested{false};
    std::atomic_bool mHooksInstalled{false};
    std::atomic_bool mRestoreSucceeded{false};
    std::atomic<std::uint32_t> mCallbacksInFlight{0};
    std::atomic<std::uint64_t> mMinecraftCalls{0};
    std::atomic<std::uint64_t> mPresentCalls{0};
    std::atomic<std::uint64_t> mVulkanPresentCalls{0};
    std::atomic<std::uint64_t> mEglPresentCalls{0};
    std::atomic<std::uint32_t> mMinecraftThreadId{0};
    std::atomic<std::uint32_t> mPresentThreadId{0};
    std::atomic<std::uint64_t> mOtherMinecraftThreadCalls{0};
    std::atomic<std::uint64_t> mOtherPresentThreadCalls{0};
    std::atomic<std::uintptr_t> mFirstRenderer{0};
    std::atomic<std::uintptr_t> mLastContext{0};
    std::atomic<std::uintptr_t> mLastView{0};
    std::atomic<std::uintptr_t> mLastClient{0};
    std::atomic_bool mFingerprintValidated{false};
    std::atomic_bool mPrefixValidated{false};
    std::atomic_bool mMinecraftHookInstalled{false};
    std::atomic_bool mVulkanHookInstalled{false};
    std::atomic_bool mEglHookInstalled{false};
    std::string mFailureReason;

    static std::atomic<LevelRenderHook*> sActive;
};

}  // namespace aeronautics::bedrock
