#pragma once

#include <atomic>
#include <cstdint>
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
    using Callback = bool (*)(void*);

    static bool detour(void* instance);
    void sample(std::stop_token stopToken);

    ll::mod::NativeMod& mMod;
    pl::memory::FuncPtr mOriginal{};
    pl::memory::HookHandle mHook;
    std::atomic<std::uint64_t> mCallCount{0};
    std::jthread mSampler;

    static std::atomic<HeartbeatHook*> sActive;
};

}  // namespace aeronautics::bedrock
