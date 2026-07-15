#include "bedrock/HeartbeatHook.hpp"

#include "bedrock/CompatibilityProfile.hpp"

#include <chrono>
#include <filesystem>
#include <string>

#include <pl/memory/Signature.hpp>

namespace aeronautics::bedrock {

std::atomic<HeartbeatHook*> HeartbeatHook::sActive{nullptr};

HeartbeatHook::HeartbeatHook(ll::mod::NativeMod& mod) noexcept
    : mMod(mod) {}

HeartbeatHook::~HeartbeatHook() {
    uninstall();
}

bool HeartbeatHook::install() {
    if (installed()) {
        return true;
    }

    const auto module = inspectLoadedModule(CompatibilityProfile::moduleName);
    if (!module) {
        mMod.getLogger().error(
            "Compatibility gate failed: {} is not loaded",
            CompatibilityProfile::moduleName);
        return false;
    }

    const std::uintptr_t target = pl::memory::resolveSignature(
        CompatibilityProfile::heartbeatSignature,
        CompatibilityProfile::moduleName);
    if (target == 0) {
        mMod.getLogger().error(
            "Compatibility gate failed: 1.26.33.1 heartbeat signature was not found");
        return false;
    }
    if ((target & 0x3U) != 0U || !isExecutableAddress(*module, target)) {
        mMod.getLogger().error(
            "Compatibility gate failed: heartbeat target 0x{:x} is not an aligned executable address",
            target);
        return false;
    }

    const std::string prefix = readInstructionPrefix(*module, target, 32);
    if (prefix.empty()) {
        mMod.getLogger().error(
            "Compatibility gate failed: could not read heartbeat target prefix");
        return false;
    }

    const std::filesystem::path reportPath =
        mMod.getDataDir() / "compatibility-report.txt";
    std::string reportError;
    if (!writeCompatibilityReport(*module, target, reportPath, reportError)) {
        mMod.getLogger().warn(
            "Could not write compatibility report {}: {}",
            reportPath,
            reportError);
    }

    mCallCount.store(0, std::memory_order_relaxed);
    mOriginal = nullptr;
    sActive.store(this, std::memory_order_release);
    mHook = pl::memory::HookHandle(
        reinterpret_cast<pl::memory::FuncPtr>(target),
        reinterpret_cast<pl::memory::FuncPtr>(&HeartbeatHook::detour),
        &mOriginal,
        pl::memory::HookPriority::Low);

    if (!mHook.installed() || mOriginal == nullptr) {
        mHook.reset();
        HeartbeatHook* expected = this;
        sActive.compare_exchange_strong(
            expected,
            nullptr,
            std::memory_order_acq_rel,
            std::memory_order_acquire);
        mOriginal = nullptr;
        mMod.getLogger().error(
            "Compatibility gate passed, but the heartbeat detour could not be installed");
        return false;
    }

    const std::uintptr_t relativeAddress =
        target >= module->loadBase ? target - module->loadBase : 0;
    mMod.getLogger().info(
        "Compatibility profile accepted: mc={}, build_id={}, module_size={}, heartbeat_offset=0x{:x}",
        CompatibilityProfile::minecraftVersion,
        module->buildId.empty() ? "unavailable" : module->buildId,
        module->fileSize,
        relativeAddress);
    mMod.getLogger().info("Heartbeat target prefix: {}", prefix);

    mSampler = std::jthread([this](std::stop_token stopToken) {
        sample(stopToken);
    });
    mMod.getLogger().info(
        "Read-only heartbeat hook installed; no world or render state is modified");
    return true;
}

void HeartbeatHook::uninstall() noexcept {
    if (mSampler.joinable()) {
        mSampler.request_stop();
        mSampler.join();
    }

    const bool wasInstalled = mHook.installed();
    mHook.reset();

    HeartbeatHook* expected = this;
    sActive.compare_exchange_strong(
        expected,
        nullptr,
        std::memory_order_acq_rel,
        std::memory_order_acquire);
    mOriginal = nullptr;

    if (wasInstalled) {
        mMod.getLogger().info(
            "Heartbeat hook removed; final callback count={}",
            mCallCount.load(std::memory_order_relaxed));
    }
}

bool HeartbeatHook::detour(void* instance) {
    HeartbeatHook* const active = sActive.load(std::memory_order_acquire);
    if (active == nullptr || active->mOriginal == nullptr) {
        return false;
    }

    const auto original = reinterpret_cast<Callback>(active->mOriginal);
    const bool result = original(instance);
    active->mCallCount.fetch_add(1, std::memory_order_relaxed);
    return result;
}

void HeartbeatHook::sample(std::stop_token stopToken) {
    using namespace std::chrono_literals;

    std::uint64_t previous = 0;
    while (!stopToken.stop_requested()) {
        for (int slice = 0; slice < 100 && !stopToken.stop_requested(); ++slice) {
            std::this_thread::sleep_for(100ms);
        }
        if (stopToken.stop_requested()) {
            break;
        }

        const std::uint64_t total = mCallCount.load(std::memory_order_relaxed);
        const std::uint64_t delta = total - previous;
        previous = total;

        if (total == 0) {
            mMod.getLogger().warn(
                "Heartbeat sample: no callbacks observed in the last 10 seconds");
        } else {
            mMod.getLogger().info(
                "Heartbeat sample: total_callbacks={}, callbacks_last_10s={}",
                total,
                delta);
        }
    }
}

}  // namespace aeronautics::bedrock
