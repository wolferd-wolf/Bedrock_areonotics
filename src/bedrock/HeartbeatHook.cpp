#include "bedrock/HeartbeatHook.hpp"

#include "bedrock/CompatibilityProfile.hpp"

#include <chrono>
#include <filesystem>
#include <string>
#include <system_error>

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
    mInFlightCallbacks.store(0, std::memory_order_relaxed);
    mStopRequested.store(false, std::memory_order_release);
    mDisabling.store(false, std::memory_order_release);
    mOriginalStorage = nullptr;
    mOriginalCallable.store(nullptr, std::memory_order_release);
    sActive.store(this, std::memory_order_release);

    mHook = pl::memory::HookHandle(
        reinterpret_cast<pl::memory::FuncPtr>(target),
        reinterpret_cast<pl::memory::FuncPtr>(&HeartbeatHook::detour),
        &mOriginalStorage,
        pl::memory::HookPriority::Low);

    if (!mHook.installed() || mOriginalStorage == nullptr) {
        mHook.reset();
        clearActiveRegistration();
        mOriginalStorage = nullptr;
        mMod.getLogger().error(
            "Compatibility gate passed, but the heartbeat detour could not be installed");
        return false;
    }

    mOriginalCallable.store(
        reinterpret_cast<Callback>(mOriginalStorage),
        std::memory_order_release);

    const std::uintptr_t relativeAddress =
        target >= module->loadBase ? target - module->loadBase : 0;
    mMod.getLogger().info(
        "Compatibility profile accepted: mc={}, build_id={}, module_size={}, heartbeat_offset=0x{:x}",
        CompatibilityProfile::minecraftVersion,
        module->buildId.empty() ? "unavailable" : module->buildId,
        module->fileSize,
        relativeAddress);
    mMod.getLogger().info("Heartbeat target prefix: {}", prefix);

    try {
        mSampler = std::thread([this] {
            sample();
        });
    } catch (const std::system_error& error) {
        mMod.getLogger().error(
            "Could not start heartbeat sampler thread: {}",
            error.what());
        uninstall();
        return false;
    }

    mMod.getLogger().info(
        "Read-only heartbeat hook installed; no world or render state is modified");
    return true;
}

void HeartbeatHook::uninstall() noexcept {
    mDisabling.store(true, std::memory_order_release);
    mStopRequested.store(true, std::memory_order_release);
    if (mSampler.joinable()) {
        mSampler.join();
    }

    const bool wasInstalled = mHook.installed();
    mHook.reset();

    using namespace std::chrono_literals;
    constexpr int maxDrainAttempts = 1000;
    int drainAttempt = 0;
    while (mInFlightCallbacks.load(std::memory_order_acquire) != 0 &&
           drainAttempt < maxDrainAttempts) {
        std::this_thread::sleep_for(1ms);
        ++drainAttempt;
    }

    const std::uint32_t remainingCallbacks =
        mInFlightCallbacks.load(std::memory_order_acquire);
    if (remainingCallbacks != 0) {
        mMod.getLogger().warn(
            "Heartbeat hook removal timed out with {} callback(s) still active",
            remainingCallbacks);
    }

    mOriginalCallable.store(nullptr, std::memory_order_release);
    clearActiveRegistration();
    mOriginalStorage = nullptr;

    if (wasInstalled) {
        mMod.getLogger().info(
            "Heartbeat hook removed; final callback count={}",
            mCallCount.load(std::memory_order_relaxed));
    }
}

void HeartbeatHook::clearActiveRegistration() noexcept {
    HeartbeatHook* expected = this;
    sActive.compare_exchange_strong(
        expected,
        nullptr,
        std::memory_order_acq_rel,
        std::memory_order_acquire);
}

bool HeartbeatHook::detour(void* instance) {
    HeartbeatHook* const active = sActive.load(std::memory_order_acquire);
    if (active == nullptr) {
        return false;
    }

    active->mInFlightCallbacks.fetch_add(1, std::memory_order_acq_rel);
    const Callback original =
        active->mOriginalCallable.load(std::memory_order_acquire);
    if (original == nullptr) {
        active->mInFlightCallbacks.fetch_sub(1, std::memory_order_acq_rel);
        return false;
    }

    const bool result = original(instance);
    active->mCallCount.fetch_add(1, std::memory_order_relaxed);
    active->mInFlightCallbacks.fetch_sub(1, std::memory_order_acq_rel);
    return result;
}

void HeartbeatHook::sample() {
    using namespace std::chrono_literals;

    std::uint64_t previous = 0;
    while (!mStopRequested.load(std::memory_order_acquire)) {
        for (int slice = 0;
             slice < 100 && !mStopRequested.load(std::memory_order_acquire);
             ++slice) {
            std::this_thread::sleep_for(100ms);
        }
        if (mStopRequested.load(std::memory_order_acquire)) {
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
