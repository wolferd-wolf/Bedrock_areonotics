#pragma once

#include <atomic>
#include <filesystem>
#include <thread>

#include <pl/Mod.hpp>

namespace aeronautics::bedrock {

class LevelClassDiscovery final {
public:
    explicit LevelClassDiscovery(ll::mod::NativeMod& mod) noexcept;
    ~LevelClassDiscovery();

    LevelClassDiscovery(const LevelClassDiscovery&) = delete;
    LevelClassDiscovery& operator=(const LevelClassDiscovery&) = delete;

    [[nodiscard]] bool start();
    void stop() noexcept;

private:
    void scan() noexcept;

    ll::mod::NativeMod& mMod;
    std::atomic_bool mStopRequested{false};
    std::thread mWorker;
    std::filesystem::path mReportPath;
};

}  // namespace aeronautics::bedrock
