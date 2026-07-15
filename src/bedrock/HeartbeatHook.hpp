#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string_view>
#include <thread>

#include <pl/Mod.hpp>
#include <pl/memory/Hook.hpp>

namespace aeronautics::bedrock {

class HeartbeatHook final {
public:
    explicit HeartbeatHook(ll::mod::NativeMod& mod) noexcept;
    ~HeartbeatHook();

    HeartbeatHook(const HeartbeatHook&) = delete;
    HeartbeatHook& operator=(const HeartbeatHook&) = delete;

    [[nodiscard]] bool install();
    void uninstall() noexcept;

    [[nodiscard]] bool installed() const noexcept { return mHook.installed(); }
    [[nodiscard]] std::uint64_t callCount() const noexcept {
        return mCallCount.load(std::memory_order_relaxed);
    }

private:
    struct DiscoveryState;
    using Callback = bool (*)(void*);

    static bool detour(void* instance);
    void sample();
    void scanStaticReferences() noexcept;
    void clearActiveRegistration() noexcept;
    void writeDiscoveryProfile(
        std::string_view state,
        std::uint64_t totalCallbacks) noexcept;
    void writeStatusSnapshot(
        std::string_view state,
        std::uint64_t sequence,
        std::uint64_t totalCallbacks,
        std::uint64_t callbackDelta) noexcept;

    ll::mod::NativeMod& mMod;
    pl::memory::FuncPtr mOriginalStorage{};
    std::atomic<Callback> mOriginalCallable{nullptr};
    pl::memory::HookHandle mHook;
    std::atomic<std::uint64_t> mCallCount{0};
    std::atomic<std::uint32_t> mInFlightCallbacks{0};
    std::atomic_bool mStopRequested{false};
    std::atomic_bool mFirstCallbackLogged{false};
    std::thread mSampler;
    std::filesystem::path mStatusPath;
    std::filesystem::path mDiscoveryPath;
    std::unique_ptr<DiscoveryState> mDiscovery;

    static std::atomic<HeartbeatHook*> sActive;
};

}  // namespace aeronautics::bedrock
