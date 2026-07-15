#include "bedrock/HeartbeatHook.hpp"

#include "bedrock/CompatibilityProfile.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <memory>
#include <new>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include <sys/syscall.h>
#include <unistd.h>

#include <pl/memory/Signature.hpp>

namespace aeronautics::bedrock {
namespace {

constexpr std::uint64_t discoverySampleStride = 256;
constexpr std::size_t discoverySampleCapacity = 4096;

[[nodiscard]] std::uint32_t currentThreadId() noexcept {
    const long rawThreadId = ::syscall(SYS_gettid);
    if (rawThreadId <= 0 ||
        static_cast<unsigned long>(rawThreadId) >
            static_cast<unsigned long>(std::numeric_limits<std::uint32_t>::max())) {
        return 0;
    }
    return static_cast<std::uint32_t>(rawThreadId);
}

}  // namespace

struct HeartbeatHook::DiscoveryState final {
    struct SampleSlot final {
        std::atomic<std::uint64_t> readySequence{0};
        std::uintptr_t callerAddress{};
        std::uint32_t threadId{};
        bool menuShowing{};
    };

    struct Aggregate final {
        std::uint64_t samples{};
        std::uint64_t menuTrueSamples{};
        std::uint64_t menuFalseSamples{};
        std::map<std::uint32_t, std::uint64_t> threadSamples;
    };

    std::array<SampleSlot, discoverySampleCapacity> slots{};
    std::atomic<std::uint64_t> reservedSamples{0};
    std::atomic<std::uint64_t> droppedSamples{0};
    std::uint64_t consumedSamples{};
    std::uint64_t outsideModuleSamples{};
    std::uintptr_t moduleLoadBase{};
    std::uintptr_t moduleEnd{};
    std::map<std::uintptr_t, Aggregate> callSites;
};

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

    try {
        mDiscovery = std::make_unique<DiscoveryState>();
    } catch (const std::bad_alloc&) {
        mMod.getLogger().error(
            "Compatibility gate passed, but tick discovery storage could not be allocated");
        return false;
    }

    mDiscovery->moduleLoadBase = module->loadBase;
    for (const MemoryRegion& region : module->regions) {
        mDiscovery->moduleEnd = std::max(mDiscovery->moduleEnd, region.end);
    }

    mStatusPath = mMod.getDataDir() / "heartbeat-status.txt";
    mDiscoveryPath = mMod.getDataDir() / "tick-discovery-profile.txt";
    mCallCount.store(0, std::memory_order_relaxed);
    mInFlightCallbacks.store(0, std::memory_order_relaxed);
    mStopRequested.store(false, std::memory_order_release);
    mFirstCallbackLogged.store(false, std::memory_order_release);
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
        mDiscovery.reset();
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

    writeStatusSnapshot("installed", 0, 0, 0);
    writeDiscoveryProfile("installed", 0);
    mMod.getLogger().info("Heartbeat status file: {}", mStatusPath);
    mMod.getLogger().info("Tick discovery profile: {}", mDiscoveryPath);

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
        "Read-only heartbeat hook installed; tick discovery sampling active; no world or render state is modified");
    return true;
}

void HeartbeatHook::uninstall() noexcept {
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

    const std::uint64_t finalCount =
        mCallCount.load(std::memory_order_relaxed);
    consumeDiscoverySamples();
    writeDiscoveryProfile("stopped", finalCount);
    writeStatusSnapshot("stopped", 0, finalCount, 0);

    mOriginalCallable.store(nullptr, std::memory_order_release);
    clearActiveRegistration();
    mOriginalStorage = nullptr;

    if (wasInstalled) {
        mMod.getLogger().info(
            "Heartbeat hook removed; final callback count={}",
            finalCount);
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
    std::uintptr_t callerAddress = 0;
#if defined(__clang__) || defined(__GNUC__)
    void* const returnAddress = __builtin_return_address(0);
    callerAddress = reinterpret_cast<std::uintptr_t>(
        __builtin_extract_return_addr(returnAddress));
#endif

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
    const std::uint64_t total =
        active->mCallCount.fetch_add(1, std::memory_order_relaxed) + 1;
    active->recordDiscoverySample(total, result, callerAddress);

    bool expected = false;
    if (active->mFirstCallbackLogged.compare_exchange_strong(
            expected,
            true,
            std::memory_order_acq_rel,
            std::memory_order_acquire)) {
        active->mMod.getLogger().info(
            "Heartbeat callback observed for the first time; total_callbacks={}",
            total);
    }

    active->mInFlightCallbacks.fetch_sub(1, std::memory_order_acq_rel);
    return result;
}

void HeartbeatHook::recordDiscoverySample(
    std::uint64_t totalCallbacks,
    bool menuShowing,
    std::uintptr_t callerAddress) noexcept {
    if (mDiscovery == nullptr ||
        totalCallbacks % discoverySampleStride != 0) {
        return;
    }

    const std::uint64_t sampleIndex =
        mDiscovery->reservedSamples.fetch_add(1, std::memory_order_relaxed);
    if (sampleIndex >= discoverySampleCapacity) {
        mDiscovery->droppedSamples.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    auto& slot = mDiscovery->slots[static_cast<std::size_t>(sampleIndex)];
    slot.callerAddress = callerAddress;
    slot.threadId = currentThreadId();
    slot.menuShowing = menuShowing;
    slot.readySequence.store(sampleIndex + 1, std::memory_order_release);
}

void HeartbeatHook::consumeDiscoverySamples() noexcept {
    if (mDiscovery == nullptr) {
        return;
    }

    const std::uint64_t reserved = std::min<std::uint64_t>(
        mDiscovery->reservedSamples.load(std::memory_order_acquire),
        discoverySampleCapacity);

    while (mDiscovery->consumedSamples < reserved) {
        const std::uint64_t sampleIndex = mDiscovery->consumedSamples;
        auto& slot = mDiscovery->slots[static_cast<std::size_t>(sampleIndex)];
        if (slot.readySequence.load(std::memory_order_acquire) != sampleIndex + 1) {
            break;
        }

        if (slot.callerAddress < mDiscovery->moduleLoadBase ||
            slot.callerAddress >= mDiscovery->moduleEnd) {
            ++mDiscovery->outsideModuleSamples;
        } else {
            auto& aggregate = mDiscovery->callSites[slot.callerAddress];
            ++aggregate.samples;
            if (slot.menuShowing) {
                ++aggregate.menuTrueSamples;
            } else {
                ++aggregate.menuFalseSamples;
            }
            ++aggregate.threadSamples[slot.threadId];
        }

        ++mDiscovery->consumedSamples;
    }
}

void HeartbeatHook::writeDiscoveryProfile(
    std::string_view state,
    std::uint64_t totalCallbacks) noexcept {
    if (mDiscovery == nullptr || mDiscoveryPath.empty()) {
        return;
    }

    std::ofstream output(mDiscoveryPath, std::ios::trunc);
    if (!output) {
        return;
    }

    using Entry = std::pair<const std::uintptr_t, DiscoveryState::Aggregate>;
    std::vector<const Entry*> ranked;
    ranked.reserve(mDiscovery->callSites.size());
    for (const auto& entry : mDiscovery->callSites) {
        ranked.push_back(&entry);
    }
    std::sort(
        ranked.begin(),
        ranked.end(),
        [](const Entry* left, const Entry* right) {
            if (left->second.samples != right->second.samples) {
                return left->second.samples > right->second.samples;
            }
            return left->first < right->first;
        });

    output << "schema=1\n";
    output << "state=" << state << '\n';
    output << "minecraft_version=" << CompatibilityProfile::minecraftVersion << '\n';
    output << "sample_stride_callbacks=" << discoverySampleStride << '\n';
    output << "sample_capacity=" << discoverySampleCapacity << '\n';
    output << "total_callbacks=" << totalCallbacks << '\n';
    output << "samples_reserved="
           << mDiscovery->reservedSamples.load(std::memory_order_relaxed) << '\n';
    output << "samples_consumed=" << mDiscovery->consumedSamples << '\n';
    output << "samples_dropped="
           << mDiscovery->droppedSamples.load(std::memory_order_relaxed) << '\n';
    output << "samples_outside_minecraft_module="
           << mDiscovery->outsideModuleSamples << '\n';
    output << "call_site_count=" << ranked.size() << '\n';

    for (std::size_t index = 0; index < ranked.size(); ++index) {
        const Entry& entry = *ranked[index];
        const std::uintptr_t relativeAddress =
            entry.first - mDiscovery->moduleLoadBase;
        const auto& aggregate = entry.second;
        output << "call_site." << index << ".offset=0x"
               << std::hex << relativeAddress << std::dec << '\n';
        output << "call_site." << index << ".samples="
               << aggregate.samples << '\n';
        output << "call_site." << index << ".menu_true_samples="
               << aggregate.menuTrueSamples << '\n';
        output << "call_site." << index << ".menu_false_samples="
               << aggregate.menuFalseSamples << '\n';
        output << "call_site." << index << ".thread_count="
               << aggregate.threadSamples.size() << '\n';

        std::size_t threadIndex = 0;
        for (const auto& [threadId, samples] : aggregate.threadSamples) {
            output << "call_site." << index << ".thread." << threadIndex
                   << ".id=" << threadId << '\n';
            output << "call_site." << index << ".thread." << threadIndex
                   << ".samples=" << samples << '\n';
            ++threadIndex;
        }
    }
}

void HeartbeatHook::sample() {
    using namespace std::chrono_literals;

    std::uint64_t sequence = 1;
    std::uint64_t previous = 0;
    writeStatusSnapshot("sampler_started", sequence, 0, 0);
    consumeDiscoverySamples();
    writeDiscoveryProfile("sampler_started", 0);

    while (!mStopRequested.load(std::memory_order_acquire)) {
        for (int slice = 0;
             slice < 20 && !mStopRequested.load(std::memory_order_acquire);
             ++slice) {
            std::this_thread::sleep_for(100ms);
        }
        if (mStopRequested.load(std::memory_order_acquire)) {
            break;
        }

        const std::uint64_t total = mCallCount.load(std::memory_order_relaxed);
        const std::uint64_t delta = total - previous;
        previous = total;
        ++sequence;
        consumeDiscoverySamples();
        writeStatusSnapshot("sampling", sequence, total, delta);
        writeDiscoveryProfile("sampling", total);
    }
}

void HeartbeatHook::writeStatusSnapshot(
    std::string_view state,
    std::uint64_t sequence,
    std::uint64_t totalCallbacks,
    std::uint64_t callbackDelta) noexcept {
    if (mStatusPath.empty()) {
        return;
    }

    std::ofstream output(mStatusPath, std::ios::trunc);
    if (!output) {
        return;
    }

    const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();

    output << "schema=1\n";
    output << "state=" << state << '\n';
    output << "sampler_sequence=" << sequence << '\n';
    output << "timestamp_unix_ms=" << now << '\n';
    output << "total_callbacks=" << totalCallbacks << '\n';
    output << "callbacks_since_previous=" << callbackDelta << '\n';
}

}  // namespace aeronautics::bedrock
