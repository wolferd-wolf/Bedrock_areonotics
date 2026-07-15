#include "bedrock/HeartbeatHook.hpp"

#include "bedrock/CompatibilityProfile.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <new>
#include <string>
#include <system_error>
#include <vector>

#include <pl/memory/Signature.hpp>

namespace aeronautics::bedrock {
namespace {

constexpr std::size_t maximumRecordedReferences = 256;
constexpr std::uintptr_t arm64InstructionAlignment = 4;
constexpr std::uintptr_t pointerAlignment = 8;
constexpr std::uint32_t arm64BranchOpcodeMask = 0xFC000000U;
constexpr std::uint32_t arm64BranchOpcode = 0x14000000U;
constexpr std::uint32_t arm64BranchLinkOpcode = 0x94000000U;
constexpr std::uint32_t arm64ImmediateMask = 0x03FFFFFFU;
constexpr std::uint32_t arm64ImmediateSignBit = 0x02000000U;
constexpr std::uint64_t cancellationCheckStride = 1024U * 1024U;

[[nodiscard]] std::uintptr_t alignUp(
    std::uintptr_t value,
    std::uintptr_t alignment) noexcept {
    return (value + alignment - 1U) & ~(alignment - 1U);
}

[[nodiscard]] std::uintptr_t decodeArm64BranchDestination(
    std::uintptr_t instructionAddress,
    std::uint32_t instruction) noexcept {
    std::int64_t immediate =
        static_cast<std::int64_t>(instruction & arm64ImmediateMask);
    if ((instruction & arm64ImmediateSignBit) != 0U) {
        immediate |= ~static_cast<std::int64_t>(arm64ImmediateMask);
    }
    const std::int64_t displacement = immediate * 4;
    return static_cast<std::uintptr_t>(
        static_cast<std::int64_t>(instructionAddress) + displacement);
}

}  // namespace

struct HeartbeatHook::DiscoveryState final {
    struct BranchReference final {
        std::uintptr_t address{};
        bool link{};
    };

    std::uintptr_t moduleLoadBase{};
    std::uintptr_t targetAddress{};
    std::uintmax_t moduleFileSize{};
    std::string moduleBuildId;
    std::vector<MemoryRegion> regions;
    std::vector<BranchReference> branchReferences;
    std::vector<std::uintptr_t> pointerReferences;
    std::uint64_t executableBytesScanned{};
    std::uint64_t readableDataBytesScanned{};
    std::uint64_t directBranchReferenceCount{};
    std::uint64_t pointerReferenceCount{};
    std::uint64_t scanDurationMilliseconds{};
    bool scanStarted{};
    bool scanComplete{};
    bool scanCancelled{};
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
        mDiscovery->moduleLoadBase = module->loadBase;
        mDiscovery->targetAddress = target;
        mDiscovery->moduleFileSize = module->fileSize;
        mDiscovery->moduleBuildId = module->buildId;
        mDiscovery->regions = module->regions;
        mDiscovery->branchReferences.reserve(maximumRecordedReferences);
        mDiscovery->pointerReferences.reserve(maximumRecordedReferences);
    } catch (const std::bad_alloc&) {
        mMod.getLogger().error(
            "Compatibility gate passed, but static discovery storage could not be allocated");
        return false;
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
        "Read-only heartbeat hook installed; static ARM64 reference discovery active; no world or render state is modified");
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

void HeartbeatHook::scanStaticReferences() noexcept {
    if (mDiscovery == nullptr) {
        return;
    }

    auto& discovery = *mDiscovery;
    discovery.scanStarted = true;
    const auto startedAt = std::chrono::steady_clock::now();

    for (const MemoryRegion& region : discovery.regions) {
        if (mStopRequested.load(std::memory_order_acquire)) {
            discovery.scanCancelled = true;
            break;
        }
        if (!region.readable || !region.executable || region.end <= region.start) {
            continue;
        }

        const std::uintptr_t first =
            alignUp(region.start, arm64InstructionAlignment);
        for (std::uintptr_t address = first;
             address <= region.end - sizeof(std::uint32_t);
             address += sizeof(std::uint32_t)) {
            if ((discovery.executableBytesScanned % cancellationCheckStride) == 0U &&
                mStopRequested.load(std::memory_order_acquire)) {
                discovery.scanCancelled = true;
                break;
            }

            std::uint32_t instruction = 0;
            std::memcpy(
                &instruction,
                reinterpret_cast<const void*>(address),
                sizeof(instruction));
            discovery.executableBytesScanned += sizeof(instruction);

            const std::uint32_t opcode =
                instruction & arm64BranchOpcodeMask;
            if (opcode != arm64BranchOpcode &&
                opcode != arm64BranchLinkOpcode) {
                continue;
            }
            if (decodeArm64BranchDestination(address, instruction) !=
                discovery.targetAddress) {
                continue;
            }

            ++discovery.directBranchReferenceCount;
            if (discovery.branchReferences.size() < maximumRecordedReferences) {
                discovery.branchReferences.push_back({
                    .address = address,
                    .link = opcode == arm64BranchLinkOpcode,
                });
            }
        }
        if (discovery.scanCancelled) {
            break;
        }
    }

    if (!discovery.scanCancelled) {
        for (const MemoryRegion& region : discovery.regions) {
            if (mStopRequested.load(std::memory_order_acquire)) {
                discovery.scanCancelled = true;
                break;
            }
            if (!region.readable || region.executable || region.end <= region.start) {
                continue;
            }

            const std::uintptr_t first = alignUp(region.start, pointerAlignment);
            for (std::uintptr_t address = first;
                 address <= region.end - sizeof(std::uintptr_t);
                 address += sizeof(std::uintptr_t)) {
                if ((discovery.readableDataBytesScanned % cancellationCheckStride) == 0U &&
                    mStopRequested.load(std::memory_order_acquire)) {
                    discovery.scanCancelled = true;
                    break;
                }

                std::uintptr_t value = 0;
                std::memcpy(
                    &value,
                    reinterpret_cast<const void*>(address),
                    sizeof(value));
                discovery.readableDataBytesScanned += sizeof(value);

                if (value != discovery.targetAddress) {
                    continue;
                }

                ++discovery.pointerReferenceCount;
                if (discovery.pointerReferences.size() < maximumRecordedReferences) {
                    discovery.pointerReferences.push_back(address);
                }
            }
            if (discovery.scanCancelled) {
                break;
            }
        }
    }

    discovery.scanComplete = !discovery.scanCancelled;
    discovery.scanDurationMilliseconds =
        static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - startedAt)
                .count());

    mMod.getLogger().info(
        "Static ARM64 heartbeat reference scan {}: direct_branches={}, pointer_references={}, executable_bytes={}, readable_data_bytes={}, duration_ms={}",
        discovery.scanComplete ? "completed" : "cancelled",
        discovery.directBranchReferenceCount,
        discovery.pointerReferenceCount,
        discovery.executableBytesScanned,
        discovery.readableDataBytesScanned,
        discovery.scanDurationMilliseconds);
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

    const auto& discovery = *mDiscovery;
    const std::uintptr_t targetOffset =
        discovery.targetAddress >= discovery.moduleLoadBase
            ? discovery.targetAddress - discovery.moduleLoadBase
            : 0;

    output << "schema=2\n";
    output << "state=" << state << '\n';
    output << "minecraft_version=" << CompatibilityProfile::minecraftVersion << '\n';
    output << "module_build_id="
           << (discovery.moduleBuildId.empty() ? "unavailable" : discovery.moduleBuildId)
           << '\n';
    output << "module_file_size=" << discovery.moduleFileSize << '\n';
    output << "discovery_method=arm64_static_reference_scan\n";
    output << "bridge_caller_capture=disabled_confirmed_hook_bridge\n";
    output << "heartbeat_target_offset=0x"
           << std::hex << targetOffset << std::dec << '\n';
    output << "total_callbacks=" << totalCallbacks << '\n';
    output << "scan_state="
           << (discovery.scanComplete
                   ? "complete"
                   : discovery.scanCancelled
                         ? "cancelled"
                         : discovery.scanStarted ? "running" : "not_started")
           << '\n';
    output << "scan_duration_ms=" << discovery.scanDurationMilliseconds << '\n';
    output << "executable_bytes_scanned="
           << discovery.executableBytesScanned << '\n';
    output << "readable_data_bytes_scanned="
           << discovery.readableDataBytesScanned << '\n';
    output << "direct_branch_reference_count="
           << discovery.directBranchReferenceCount << '\n';
    output << "direct_branch_reference_recorded="
           << discovery.branchReferences.size() << '\n';
    output << "pointer_reference_count="
           << discovery.pointerReferenceCount << '\n';
    output << "pointer_reference_recorded="
           << discovery.pointerReferences.size() << '\n';

    for (std::size_t index = 0;
         index < discovery.branchReferences.size();
         ++index) {
        const auto& reference = discovery.branchReferences[index];
        const std::uintptr_t offset =
            reference.address >= discovery.moduleLoadBase
                ? reference.address - discovery.moduleLoadBase
                : 0;
        output << "direct_branch." << index << ".offset=0x"
               << std::hex << offset << std::dec << '\n';
        output << "direct_branch." << index << ".kind="
               << (reference.link ? "bl" : "b") << '\n';
    }

    for (std::size_t index = 0;
         index < discovery.pointerReferences.size();
         ++index) {
        const std::uintptr_t address = discovery.pointerReferences[index];
        const std::uintptr_t offset =
            address >= discovery.moduleLoadBase
                ? address - discovery.moduleLoadBase
                : 0;
        output << "pointer_reference." << index << ".offset=0x"
               << std::hex << offset << std::dec << '\n';
    }
}

void HeartbeatHook::sample() {
    using namespace std::chrono_literals;

    std::uint64_t sequence = 1;
    std::uint64_t previous = 0;
    writeStatusSnapshot("sampler_started", sequence, 0, 0);
    writeDiscoveryProfile("scanning", 0);
    scanStaticReferences();
    writeDiscoveryProfile(
        mDiscovery != nullptr && mDiscovery->scanComplete
            ? "scan_complete"
            : "scan_cancelled",
        mCallCount.load(std::memory_order_relaxed));

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
