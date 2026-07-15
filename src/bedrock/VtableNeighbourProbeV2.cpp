#include "bedrock/VtableNeighbourProbeV2.hpp"

#include "bedrock/CompatibilityProfile.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <dlfcn.h>
#include <fstream>
#include <limits>
#include <string>
#include <system_error>

#include <sys/syscall.h>
#include <unistd.h>

#include <pl/memory/Signature.hpp>

namespace aeronautics::bedrock {
namespace {

constexpr std::string_view expectedBuildId{
    "2e318db12824cadb2618754ab7c82fa96fb30659"};
constexpr std::uintmax_t expectedModuleFileSize = 349243744;
constexpr std::uintptr_t expectedHeartbeatOffset = 0x9d80fac;
constexpr std::uintptr_t expectedVtableRunStartOffset = 0x140545a0;
constexpr std::uint32_t expectedVtableEntryCount = 438;
constexpr std::uint32_t heartbeatSlotIndex = 152;
constexpr int glossArm64InstructionSet = 3;
constexpr std::uint64_t samplerSliceMilliseconds = 100;
constexpr int samplerSlicesPerInterval = 20;

struct ProbeSpec final {
    std::uint32_t slotIndex;
    std::int32_t relativeIndex;
    std::uintptr_t targetOffset;
    std::string_view label;
};

constexpr std::array<ProbeSpec, 5> probeSpecs{{
    {144, -8, 0x9d80558, "slot_144_large_neighbour"},
    {146, -6, 0x9d80bbc, "slot_146_medium_neighbour"},
    {151, -1, 0x9d80eac, "slot_151_pre_heartbeat"},
    {153, 1, 0x9d8129c, "slot_153_post_heartbeat"},
    {160, 8, 0x9d82094, "slot_160_local_virtual_caller"},
}};

[[nodiscard]] const MemoryRegion* findContainingRegion(
    const ModuleFingerprint& module,
    std::uintptr_t address,
    std::size_t byteCount) noexcept {
    for (const MemoryRegion& region : module.regions) {
        if (!region.readable || address < region.start || address >= region.end) {
            continue;
        }
        if (byteCount <= region.end - address) {
            return &region;
        }
    }
    return nullptr;
}

[[nodiscard]] bool readPointer(
    const ModuleFingerprint& module,
    std::uintptr_t address,
    std::uintptr_t& value) noexcept {
    if (findContainingRegion(module, address, sizeof(value)) == nullptr) {
        return false;
    }
    std::memcpy(&value, reinterpret_cast<const void*>(address), sizeof(value));
    return true;
}

[[nodiscard]] std::uint32_t currentThreadId() noexcept {
    const long raw = ::syscall(SYS_gettid);
    if (raw <= 0 ||
        static_cast<unsigned long>(raw) >
            static_cast<unsigned long>(std::numeric_limits<std::uint32_t>::max())) {
        return 0;
    }
    return static_cast<std::uint32_t>(raw);
}

template <typename FunctionPointer>
[[nodiscard]] bool loadFunctionSymbol(
    const char* name,
    FunctionPointer& destination) noexcept {
    void* symbol = ::dlsym(RTLD_DEFAULT, name);
    if (symbol == nullptr || sizeof(symbol) != sizeof(destination)) {
        destination = nullptr;
        return false;
    }
    std::memcpy(&destination, &symbol, sizeof(destination));
    return destination != nullptr;
}

[[nodiscard]] const char* menuStateName(int state) noexcept {
    if (state == 1) {
        return "showing_menu";
    }
    if (state == 0) {
        return "gameplay_or_no_menu";
    }
    return "unknown";
}

[[nodiscard]] std::uint64_t rateMilliHertz(
    std::uint64_t calls,
    std::uint64_t observedMilliseconds) noexcept {
    if (observedMilliseconds == 0) {
        return 0;
    }
    return (calls * 1000000ULL) / observedMilliseconds;
}

}  // namespace

std::atomic<VtableNeighbourProbeV2*> VtableNeighbourProbeV2::sActive{nullptr};

VtableNeighbourProbeV2::VtableNeighbourProbeV2(ll::mod::NativeMod& mod) noexcept
    : mMod(mod) {}

VtableNeighbourProbeV2::~VtableNeighbourProbeV2() {
    uninstall();
}

bool VtableNeighbourProbeV2::install() {
    if (mSampler.joinable() || mMenuHook.installed() || mActiveProbeCount != 0) {
        return true;
    }

    mProfilePath = mMod.getDataDir() / "vtable-probe-profile.txt";
    mTimelinePath = mMod.getDataDir() / "vtable-probe-timeline.txt";
    mFailureReason.clear();
    mModuleBuildId.clear();
    mModuleFileSize = 0;
    mModuleLoadBase = 0;
    mHeartbeatTarget = 0;
    mSuccessfulProbeCount = 0;
    mActiveProbeCount = 0;
    mMenuTrueObservedMilliseconds = 0;
    mMenuFalseObservedMilliseconds = 0;
    mMenuUnknownObservedMilliseconds = 0;
    mPreviousTotals.fill(0);
    mProbeHandles.fill(nullptr);
    mGlossHookInternal = nullptr;
    mGlossHookDelete = nullptr;
    mMenuOriginalStorage = nullptr;
    mMenuOriginalCallable.store(nullptr, std::memory_order_release);
    mMenuState.store(-1, std::memory_order_release);
    mMenuObserverCalls.store(0, std::memory_order_relaxed);
    mCallbacksInFlight.store(0, std::memory_order_relaxed);
    mStopRequested.store(false, std::memory_order_release);

    for (std::size_t index = 0; index < probeCount; ++index) {
        Counter& counter = mCounters[index];
        counter.total.store(0, std::memory_order_relaxed);
        counter.menuTrue.store(0, std::memory_order_relaxed);
        counter.menuFalse.store(0, std::memory_order_relaxed);
        counter.menuUnknown.store(0, std::memory_order_relaxed);
        counter.firstThreadId.store(0, std::memory_order_relaxed);
        counter.otherThreadCalls.store(0, std::memory_order_relaxed);
        mMetadata[index] = Metadata{};
        mMenuTrueIntervals[index] = IntervalStats{};
        mMenuFalseIntervals[index] = IntervalStats{};
    }

    createTimelineHeader();

    if (!validateTargets()) {
        writeProfile("validation_failed");
        mMod.getLogger().warn(
            "Vtable probe validation failed: {}",
            mFailureReason);
        return false;
    }

    if (!resolveGlossApi()) {
        mFailureReason =
            "GlossHookInternal or GlossHookDelete is not exported by the active preloader";
        writeProfile("gloss_api_unavailable");
        mMod.getLogger().warn("{}", mFailureReason);
        return false;
    }

    VtableNeighbourProbeV2* expected = nullptr;
    if (!sActive.compare_exchange_strong(
            expected,
            this,
            std::memory_order_acq_rel,
            std::memory_order_acquire)) {
        mFailureReason = "another vtable probe instance is already active";
        writeProfile("registration_failed");
        return false;
    }

    mMenuHook = pl::memory::HookHandle(
        reinterpret_cast<pl::memory::FuncPtr>(mHeartbeatTarget),
        reinterpret_cast<pl::memory::FuncPtr>(&VtableNeighbourProbeV2::menuDetour),
        &mMenuOriginalStorage,
        pl::memory::HookPriority::Lowest);
    if (!mMenuHook.installed() || mMenuOriginalStorage == nullptr) {
        mFailureReason =
            "menu-state observer could not join the proven heartbeat hook chain";
        mMenuHook.reset();
        clearActiveRegistration();
        writeProfile("menu_observer_failed");
        return false;
    }
    mMenuOriginalCallable.store(
        reinterpret_cast<MenuCallback>(mMenuOriginalStorage),
        std::memory_order_release);

    mSuccessfulProbeCount = installFullHooks();
    mActiveProbeCount = mSuccessfulProbeCount;
    if (mSuccessfulProbeCount == 0) {
        mFailureReason =
            "all five normal full-size register-preserving hooks failed to install";
        mMenuHook.reset();
        mMenuOriginalCallable.store(nullptr, std::memory_order_release);
        clearActiveRegistration();
        writeProfile("probe_install_failed");
        return false;
    }
    if (mSuccessfulProbeCount != probeCount) {
        mFailureReason = "one or more full-size probes failed; partial sampling active";
    }

    mStartedAt = std::chrono::steady_clock::now();
    writeProfile("installed");

    try {
        mSampler = std::thread([this] {
            samplerLoop();
        });
    } catch (const std::system_error& error) {
        mFailureReason = std::string("probe sampler thread failed: ") + error.what();
        uninstall();
        return false;
    }

    mMod.getLogger().info(
        "Full-size counter-only vtable probes active: installed={}/{}; profile={}; timeline={}",
        mSuccessfulProbeCount,
        probeCount,
        mProfilePath,
        mTimelinePath);
    return true;
}

void VtableNeighbourProbeV2::uninstall() noexcept {
    mStopRequested.store(true, std::memory_order_release);
    if (mSampler.joinable()) {
        mSampler.join();
    }

    if (mGlossHookDelete != nullptr) {
        for (std::size_t index = probeCount; index > 0; --index) {
            GlossHookHandle& handle = mProbeHandles[index - 1];
            if (handle != nullptr) {
                mGlossHookDelete(handle);
                handle = nullptr;
            }
        }
    }
    mActiveProbeCount = 0;

    mMenuHook.reset();
    mMenuOriginalCallable.store(nullptr, std::memory_order_release);

    using namespace std::chrono_literals;
    constexpr int maximumDrainAttempts = 1000;
    int drainAttempt = 0;
    while (mCallbacksInFlight.load(std::memory_order_acquire) != 0 &&
           drainAttempt < maximumDrainAttempts) {
        std::this_thread::sleep_for(1ms);
        ++drainAttempt;
    }

    writeProfile("stopped");
    clearActiveRegistration();
    mMenuOriginalStorage = nullptr;
}

bool VtableNeighbourProbeV2::validateTargets() {
    const auto module = inspectLoadedModule(CompatibilityProfile::moduleName);
    if (!module) {
        mFailureReason = "libminecraftpe.so is not loaded";
        return false;
    }

    mModuleBuildId = module->buildId;
    mModuleFileSize = module->fileSize;
    mModuleLoadBase = module->loadBase;

    if (module->buildId != expectedBuildId ||
        module->fileSize != expectedModuleFileSize) {
        mFailureReason =
            "Minecraft binary fingerprint does not match the accepted 1.26.33.1 ARM64 build";
        return false;
    }

    mHeartbeatTarget = pl::memory::resolveSignature(
        CompatibilityProfile::heartbeatSignature,
        CompatibilityProfile::moduleName);
    if (mHeartbeatTarget == 0 ||
        mHeartbeatTarget - module->loadBase != expectedHeartbeatOffset ||
        !isExecutableAddress(*module, mHeartbeatTarget)) {
        mFailureReason = "heartbeat target did not match exact offset 0x9d80fac";
        return false;
    }

    const std::uintptr_t tableStart =
        module->loadBase + expectedVtableRunStartOffset;
    const std::size_t tableBytes =
        static_cast<std::size_t>(expectedVtableEntryCount) * sizeof(std::uintptr_t);
    const MemoryRegion* const tableRegion =
        findContainingRegion(*module, tableStart, tableBytes);
    if (tableRegion == nullptr || tableRegion->executable) {
        mFailureReason =
            "accepted vtable run is not inside readable non-executable Minecraft memory";
        return false;
    }

    std::uintptr_t heartbeatEntry = 0;
    if (!readPointer(
            *module,
            tableStart + heartbeatSlotIndex * sizeof(std::uintptr_t),
            heartbeatEntry) ||
        heartbeatEntry != mHeartbeatTarget) {
        mFailureReason =
            "vtable slot 152 does not point to the proven heartbeat function";
        return false;
    }

    for (std::size_t index = 0; index < probeCount; ++index) {
        const ProbeSpec& spec = probeSpecs[index];
        Metadata& metadata = mMetadata[index];
        metadata.slotIndex = spec.slotIndex;
        metadata.relativeIndex = spec.relativeIndex;
        metadata.targetOffset = spec.targetOffset;
        metadata.label = std::string(spec.label);

        const std::uintptr_t entryAddress =
            tableStart + spec.slotIndex * sizeof(std::uintptr_t);
        const std::uintptr_t expectedTarget =
            module->loadBase + spec.targetOffset;
        std::uintptr_t entryValue = 0;
        if (!readPointer(*module, entryAddress, entryValue) ||
            entryValue != expectedTarget ||
            !isExecutableAddress(*module, expectedTarget)) {
            mFailureReason =
                "vtable neighbour entry does not match the accepted exact offset set";
            return false;
        }

        metadata.instructionPrefix =
            readInstructionPrefix(*module, expectedTarget, 16);
        if (metadata.instructionPrefix.empty()) {
            mFailureReason = "could not read a candidate instruction prefix";
            return false;
        }
        metadata.validated = true;
    }

    return true;
}

bool VtableNeighbourProbeV2::resolveGlossApi() noexcept {
    return loadFunctionSymbol("GlossHookInternal", mGlossHookInternal) &&
        loadFunctionSymbol("GlossHookDelete", mGlossHookDelete);
}

std::size_t VtableNeighbourProbeV2::installFullHooks() noexcept {
    constexpr std::array<GlossInternalCallback, probeCount> callbacks{{
        &VtableNeighbourProbeV2::probe0,
        &VtableNeighbourProbeV2::probe1,
        &VtableNeighbourProbeV2::probe2,
        &VtableNeighbourProbeV2::probe3,
        &VtableNeighbourProbeV2::probe4,
    }};

    std::size_t installed = 0;
    for (std::size_t index = 0; index < probeCount; ++index) {
        Metadata& metadata = mMetadata[index];
        metadata.fourByteAttempted = false;
        metadata.fullHookAttempted = true;

        const std::uintptr_t target =
            mModuleLoadBase + metadata.targetOffset;
        mProbeHandles[index] = mGlossHookInternal(
            reinterpret_cast<void*>(target),
            callbacks[index],
            nullptr,
            false,
            glossArm64InstructionSet);
        metadata.hookInstalled = mProbeHandles[index] != nullptr;
        if (metadata.hookInstalled) {
            metadata.installedMode = "full_size_default";
            ++installed;
        }
    }
    return installed;
}

bool VtableNeighbourProbeV2::menuDetour(void* instance) {
    VtableNeighbourProbeV2* const active =
        sActive.load(std::memory_order_acquire);
    if (active == nullptr) {
        return false;
    }

    active->mCallbacksInFlight.fetch_add(1, std::memory_order_acq_rel);
    const MenuCallback original =
        active->mMenuOriginalCallable.load(std::memory_order_acquire);
    if (original == nullptr) {
        active->mCallbacksInFlight.fetch_sub(1, std::memory_order_acq_rel);
        return false;
    }

    const bool result = original(instance);
    active->mMenuState.store(result ? 1 : 0, std::memory_order_release);
    active->mMenuObserverCalls.fetch_add(1, std::memory_order_relaxed);
    active->mCallbacksInFlight.fetch_sub(1, std::memory_order_acq_rel);
    return result;
}

void VtableNeighbourProbeV2::probe0(
    GlossRegisterOpaqueV2*,
    GlossHookHandle) {
    if (VtableNeighbourProbeV2* const active =
            sActive.load(std::memory_order_acquire)) {
        active->record(0);
    }
}

void VtableNeighbourProbeV2::probe1(
    GlossRegisterOpaqueV2*,
    GlossHookHandle) {
    if (VtableNeighbourProbeV2* const active =
            sActive.load(std::memory_order_acquire)) {
        active->record(1);
    }
}

void VtableNeighbourProbeV2::probe2(
    GlossRegisterOpaqueV2*,
    GlossHookHandle) {
    if (VtableNeighbourProbeV2* const active =
            sActive.load(std::memory_order_acquire)) {
        active->record(2);
    }
}

void VtableNeighbourProbeV2::probe3(
    GlossRegisterOpaqueV2*,
    GlossHookHandle) {
    if (VtableNeighbourProbeV2* const active =
            sActive.load(std::memory_order_acquire)) {
        active->record(3);
    }
}

void VtableNeighbourProbeV2::probe4(
    GlossRegisterOpaqueV2*,
    GlossHookHandle) {
    if (VtableNeighbourProbeV2* const active =
            sActive.load(std::memory_order_acquire)) {
        active->record(4);
    }
}

void VtableNeighbourProbeV2::record(std::size_t index) noexcept {
    if (index >= probeCount) {
        return;
    }

    mCallbacksInFlight.fetch_add(1, std::memory_order_acq_rel);
    Counter& counter = mCounters[index];
    counter.total.fetch_add(1, std::memory_order_relaxed);

    const int menuState = mMenuState.load(std::memory_order_acquire);
    if (menuState == 1) {
        counter.menuTrue.fetch_add(1, std::memory_order_relaxed);
    } else if (menuState == 0) {
        counter.menuFalse.fetch_add(1, std::memory_order_relaxed);
    } else {
        counter.menuUnknown.fetch_add(1, std::memory_order_relaxed);
    }

    const std::uint32_t threadId = currentThreadId();
    if (threadId != 0) {
        std::uint32_t expected = 0;
        if (!counter.firstThreadId.compare_exchange_strong(
                expected,
                threadId,
                std::memory_order_acq_rel,
                std::memory_order_acquire) &&
            expected != threadId) {
            counter.otherThreadCalls.fetch_add(1, std::memory_order_relaxed);
        }
    }

    mCallbacksInFlight.fetch_sub(1, std::memory_order_acq_rel);
}

void VtableNeighbourProbeV2::samplerLoop() {
    while (!mStopRequested.load(std::memory_order_acquire)) {
        const int intervalState = mMenuState.load(std::memory_order_acquire);
        bool stableState = true;
        int completedSlices = 0;

        for (int slice = 0;
             slice < samplerSlicesPerInterval &&
             !mStopRequested.load(std::memory_order_acquire);
             ++slice) {
            std::this_thread::sleep_for(
                std::chrono::milliseconds(samplerSliceMilliseconds));
            const int currentState = mMenuState.load(std::memory_order_acquire);
            stableState = stableState && currentState == intervalState;
            if (currentState == 1) {
                mMenuTrueObservedMilliseconds += samplerSliceMilliseconds;
            } else if (currentState == 0) {
                mMenuFalseObservedMilliseconds += samplerSliceMilliseconds;
            } else {
                mMenuUnknownObservedMilliseconds += samplerSliceMilliseconds;
            }
            ++completedSlices;
        }

        if (completedSlices == 0) {
            break;
        }

        std::array<std::uint64_t, probeCount> deltas{};
        for (std::size_t index = 0; index < probeCount; ++index) {
            const std::uint64_t total =
                mCounters[index].total.load(std::memory_order_relaxed);
            deltas[index] = total - mPreviousTotals[index];
            mPreviousTotals[index] = total;
            if (stableState && (intervalState == 0 || intervalState == 1)) {
                updateIntervalStats(index, intervalState, deltas[index]);
            }
        }

        static std::uint64_t sequence = 0;
        ++sequence;
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - mStartedAt);
        appendTimeline(
            sequence,
            static_cast<std::uint64_t>(elapsed.count()),
            intervalState,
            stableState,
            deltas);
        writeProfile("sampling");
    }
}

void VtableNeighbourProbeV2::updateIntervalStats(
    std::size_t index,
    int menuState,
    std::uint64_t delta) noexcept {
    IntervalStats& stats = menuState == 1
        ? mMenuTrueIntervals[index]
        : mMenuFalseIntervals[index];
    ++stats.intervals;
    if (!stats.initialized) {
        stats.minimum = delta;
        stats.maximum = delta;
        stats.initialized = true;
        return;
    }
    stats.minimum = std::min(stats.minimum, delta);
    stats.maximum = std::max(stats.maximum, delta);
}

void VtableNeighbourProbeV2::createTimelineHeader() noexcept {
    std::ofstream output(mTimelinePath, std::ios::trunc);
    if (!output) {
        return;
    }
    output
        << "schema=2\n"
        << "hook_mode=full_size_default\n"
        << "interval_ms=2000\n"
        << "columns=sequence,elapsed_ms,menu_state,stable_state,slot_144_delta,slot_146_delta,slot_151_delta,slot_153_delta,slot_160_delta\n";
}

void VtableNeighbourProbeV2::appendTimeline(
    std::uint64_t sequence,
    std::uint64_t elapsedMilliseconds,
    int menuState,
    bool stableState,
    const std::array<std::uint64_t, probeCount>& deltas) noexcept {
    std::ofstream output(mTimelinePath, std::ios::app);
    if (!output) {
        return;
    }

    output << sequence << ','
           << elapsedMilliseconds << ','
           << menuStateName(menuState) << ','
           << (stableState ? "true" : "false");
    for (const std::uint64_t delta : deltas) {
        output << ',' << delta;
    }
    output << '\n';
}

void VtableNeighbourProbeV2::writeProfile(std::string_view state) noexcept {
    if (mProfilePath.empty()) {
        return;
    }

    std::ofstream output(mProfilePath, std::ios::trunc);
    if (!output) {
        return;
    }

    output << "schema=5\n";
    output << "state=" << state << '\n';
    output << "minecraft_version=" << CompatibilityProfile::minecraftVersion << '\n';
    output << "module_build_id="
           << (mModuleBuildId.empty() ? "unavailable" : mModuleBuildId) << '\n';
    output << "module_file_size=" << mModuleFileSize << '\n';
    output << "discovery_method=vtable_neighbour_full_hook_probe\n";
    output << "previous_failure=all_4_byte_internal_hooks_rejected\n";
    output << "hook_mode=full_size_default\n";
    output << "source_vtable_run_start_offset=0x"
           << std::hex << expectedVtableRunStartOffset << std::dec << '\n';
    output << "source_vtable_entry_count=" << expectedVtableEntryCount << '\n';
    output << "heartbeat_slot_index=" << heartbeatSlotIndex << '\n';
    output << "heartbeat_target_offset=0x"
           << std::hex << expectedHeartbeatOffset << std::dec << '\n';
    output << "menu_observer_calls="
           << mMenuObserverCalls.load(std::memory_order_relaxed) << '\n';
    output << "menu_state_current="
           << menuStateName(mMenuState.load(std::memory_order_acquire)) << '\n';
    output << "menu_true_observed_ms=" << mMenuTrueObservedMilliseconds << '\n';
    output << "menu_false_observed_ms=" << mMenuFalseObservedMilliseconds << '\n';
    output << "menu_unknown_observed_ms=" << mMenuUnknownObservedMilliseconds << '\n';
    output << "gloss_internal_api_resolved="
           << (mGlossHookInternal != nullptr && mGlossHookDelete != nullptr
                   ? "true"
                   : "false")
           << '\n';
    output << "successful_probe_count=" << mSuccessfulProbeCount << '\n';
    output << "active_probe_count=" << mActiveProbeCount << '\n';
    output << "probe_count=" << probeCount << '\n';
    output << "callbacks_in_flight="
           << mCallbacksInFlight.load(std::memory_order_acquire) << '\n';
    output << "failure_reason="
           << (mFailureReason.empty() ? "none" : mFailureReason) << '\n';
    output << "timeline_file=vtable-probe-timeline.txt\n";

    for (std::size_t index = 0; index < probeCount; ++index) {
        const Metadata& metadata = mMetadata[index];
        const Counter& counter = mCounters[index];
        const std::uint64_t menuTrueCalls =
            counter.menuTrue.load(std::memory_order_relaxed);
        const std::uint64_t menuFalseCalls =
            counter.menuFalse.load(std::memory_order_relaxed);
        const IntervalStats& trueStats = mMenuTrueIntervals[index];
        const IntervalStats& falseStats = mMenuFalseIntervals[index];

        output << "probe." << index << ".label=" << metadata.label << '\n';
        output << "probe." << index << ".slot_index="
               << metadata.slotIndex << '\n';
        output << "probe." << index << ".relative_index="
               << metadata.relativeIndex << '\n';
        output << "probe." << index << ".target_offset=0x"
               << std::hex << metadata.targetOffset << std::dec << '\n';
        output << "probe." << index << ".instruction_prefix="
               << metadata.instructionPrefix << '\n';
        output << "probe." << index << ".validated="
               << (metadata.validated ? "true" : "false") << '\n';
        output << "probe." << index << ".four_byte_attempted="
               << (metadata.fourByteAttempted ? "true" : "false") << '\n';
        output << "probe." << index << ".full_hook_attempted="
               << (metadata.fullHookAttempted ? "true" : "false") << '\n';
        output << "probe." << index << ".hook_installed="
               << (metadata.hookInstalled ? "true" : "false") << '\n';
        output << "probe." << index << ".installed_mode="
               << metadata.installedMode << '\n';
        output << "probe." << index << ".total_calls="
               << counter.total.load(std::memory_order_relaxed) << '\n';
        output << "probe." << index << ".menu_true_calls="
               << menuTrueCalls << '\n';
        output << "probe." << index << ".menu_false_calls="
               << menuFalseCalls << '\n';
        output << "probe." << index << ".menu_unknown_calls="
               << counter.menuUnknown.load(std::memory_order_relaxed) << '\n';
        output << "probe." << index << ".menu_true_rate_millihz="
               << rateMilliHertz(menuTrueCalls, mMenuTrueObservedMilliseconds)
               << '\n';
        output << "probe." << index << ".menu_false_rate_millihz="
               << rateMilliHertz(menuFalseCalls, mMenuFalseObservedMilliseconds)
               << '\n';
        output << "probe." << index << ".first_thread_id="
               << counter.firstThreadId.load(std::memory_order_relaxed) << '\n';
        output << "probe." << index << ".other_thread_calls="
               << counter.otherThreadCalls.load(std::memory_order_relaxed) << '\n';
        output << "probe." << index << ".stable_menu_true_intervals="
               << trueStats.intervals << '\n';
        output << "probe." << index << ".stable_menu_true_delta_min="
               << (trueStats.initialized ? trueStats.minimum : 0) << '\n';
        output << "probe." << index << ".stable_menu_true_delta_max="
               << (trueStats.initialized ? trueStats.maximum : 0) << '\n';
        output << "probe." << index << ".stable_menu_false_intervals="
               << falseStats.intervals << '\n';
        output << "probe." << index << ".stable_menu_false_delta_min="
               << (falseStats.initialized ? falseStats.minimum : 0) << '\n';
        output << "probe." << index << ".stable_menu_false_delta_max="
               << (falseStats.initialized ? falseStats.maximum : 0) << '\n';
    }
}

void VtableNeighbourProbeV2::clearActiveRegistration() noexcept {
    VtableNeighbourProbeV2* expected = this;
    sActive.compare_exchange_strong(
        expected,
        nullptr,
        std::memory_order_acq_rel,
        std::memory_order_acquire);
}

}  // namespace aeronautics::bedrock
