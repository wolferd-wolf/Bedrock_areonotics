#include "bedrock/VtableSlotProbe.hpp"

#include "bedrock/CompatibilityProfile.hpp"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <fstream>
#include <limits>
#include <sstream>
#include <string>
#include <system_error>

#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <pl/memory/Signature.hpp>

extern "C" {
__attribute__((visibility("hidden"), used, aligned(8)))
std::uintptr_t aeronautics_slot160_original_target = 0;
}

namespace aeronautics::bedrock {
namespace {

constexpr std::string_view expectedBuildId{
    "2e318db12824cadb2618754ab7c82fa96fb30659"};
constexpr std::uintmax_t expectedModuleFileSize = 349243744;
constexpr std::uintptr_t expectedHeartbeatOffset = 0x9d80fac;
constexpr std::uintptr_t expectedVtableRunStartOffset = 0x140545a0;
constexpr std::uint32_t expectedVtableEntryCount = 438;
constexpr std::uint32_t heartbeatSlotIndex = 152;
constexpr std::uint32_t probeSlotIndex = 160;
constexpr std::uintptr_t probeTargetOffset = 0x9d82094;
constexpr std::uintptr_t probeSlotOffsetBytes =
    static_cast<std::uintptr_t>(probeSlotIndex) * sizeof(std::uintptr_t);
constexpr std::uintptr_t probeSlotAddressOffset =
    expectedVtableRunStartOffset + probeSlotOffsetBytes;
constexpr std::uint64_t activationDelayMilliseconds = 8000;
constexpr std::uint64_t activationTimeoutMilliseconds = 30000;
constexpr std::uint64_t samplerSliceMilliseconds = 100;
constexpr int samplerSlicesPerInterval = 20;

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

[[nodiscard]] bool parseHexAddress(
    std::string_view value,
    std::uintptr_t& output) noexcept {
    output = 0;
    std::istringstream stream(std::string(value));
    stream >> std::hex >> output;
    return !stream.fail();
}

[[nodiscard]] bool queryMemoryProtection(
    std::uintptr_t address,
    int& protection,
    std::string& permissions) {
    std::ifstream maps("/proc/self/maps");
    if (!maps) {
        return false;
    }

    std::string line;
    while (std::getline(maps, line)) {
        std::istringstream stream(line);
        std::string range;
        std::string perms;
        if (!(stream >> range >> perms)) {
            continue;
        }
        const auto separator = range.find('-');
        if (separator == std::string::npos) {
            continue;
        }

        std::uintptr_t start = 0;
        std::uintptr_t end = 0;
        if (!parseHexAddress(std::string_view(range).substr(0, separator), start) ||
            !parseHexAddress(std::string_view(range).substr(separator + 1), end) ||
            address < start || address >= end) {
            continue;
        }

        protection = 0;
        if (!perms.empty() && perms[0] == 'r') {
            protection |= PROT_READ;
        }
        if (perms.size() >= 2 && perms[1] == 'w') {
            protection |= PROT_WRITE;
        }
        if (perms.size() >= 3 && perms[2] == 'x') {
            protection |= PROT_EXEC;
        }
        permissions = perms;
        return true;
    }

    return false;
}

[[nodiscard]] const char* slotStateName(
    std::uintptr_t value,
    std::uintptr_t original,
    std::uintptr_t trampoline) noexcept {
    if (value == original) {
        return "original";
    }
    if (value == trampoline) {
        return "trampoline";
    }
    return "other";
}

}  // namespace

std::atomic<VtableSlotProbe*> VtableSlotProbe::sActive{nullptr};

}  // namespace aeronautics::bedrock

extern "C" __attribute__((visibility("hidden"), used, noinline))
void aeronautics_slot160_record() noexcept {
    aeronautics::bedrock::VtableSlotProbe::recordActive();
}

extern "C" __attribute__((naked, visibility("hidden"), used))
void aeronautics_slot160_trampoline() noexcept {
    asm volatile(
        ".inst 0xd503245f\n"  // BTI c
        "sub sp, sp, #0x140\n"
        "stp x0, x1, [sp, #0x00]\n"
        "stp x2, x3, [sp, #0x10]\n"
        "stp x4, x5, [sp, #0x20]\n"
        "stp x6, x7, [sp, #0x30]\n"
        "stp x8, x9, [sp, #0x40]\n"
        "stp x10, x11, [sp, #0x50]\n"
        "stp x12, x13, [sp, #0x60]\n"
        "stp x14, x15, [sp, #0x70]\n"
        "stp x16, x17, [sp, #0x80]\n"
        "str x18, [sp, #0x90]\n"
        "str x30, [sp, #0x98]\n"
        "stp q0, q1, [sp, #0xa0]\n"
        "stp q2, q3, [sp, #0xc0]\n"
        "stp q4, q5, [sp, #0xe0]\n"
        "stp q6, q7, [sp, #0x100]\n"
        "mrs x16, nzcv\n"
        "str x16, [sp, #0x120]\n"
        "mrs x16, fpcr\n"
        "str x16, [sp, #0x128]\n"
        "mrs x16, fpsr\n"
        "str x16, [sp, #0x130]\n"
        "bl aeronautics_slot160_record\n"
        "ldr x16, [sp, #0x130]\n"
        "msr fpsr, x16\n"
        "ldr x16, [sp, #0x128]\n"
        "msr fpcr, x16\n"
        "ldr x16, [sp, #0x120]\n"
        "msr nzcv, x16\n"
        "ldp q6, q7, [sp, #0x100]\n"
        "ldp q4, q5, [sp, #0xe0]\n"
        "ldp q2, q3, [sp, #0xc0]\n"
        "ldp q0, q1, [sp, #0xa0]\n"
        "ldr x30, [sp, #0x98]\n"
        "ldr x18, [sp, #0x90]\n"
        "ldp x16, x17, [sp, #0x80]\n"
        "ldp x14, x15, [sp, #0x70]\n"
        "ldp x12, x13, [sp, #0x60]\n"
        "ldp x10, x11, [sp, #0x50]\n"
        "ldp x8, x9, [sp, #0x40]\n"
        "ldp x6, x7, [sp, #0x30]\n"
        "ldp x4, x5, [sp, #0x20]\n"
        "ldp x2, x3, [sp, #0x10]\n"
        "ldp x0, x1, [sp, #0x00]\n"
        "add sp, sp, #0x140\n"
        "adrp x16, aeronautics_slot160_original_target\n"
        "ldr x16, [x16, :lo12:aeronautics_slot160_original_target]\n"
        "br x16\n");
}

namespace aeronautics::bedrock {

VtableSlotProbe::VtableSlotProbe(ll::mod::NativeMod& mod) noexcept
    : mMod(mod) {}

VtableSlotProbe::~VtableSlotProbe() {
    uninstall();
}

bool VtableSlotProbe::install() {
    if (mWorker.joinable() || mMenuHook.installed() ||
        mPatchInstalled.load(std::memory_order_acquire)) {
        return true;
    }

    mProfilePath = mMod.getDataDir() / "vtable-probe-profile.txt";
    mTimelinePath = mMod.getDataDir() / "vtable-probe-timeline.txt";
    mModuleBuildId.clear();
    mModuleFileSize = 0;
    mModuleLoadBase = 0;
    mHeartbeatTarget = 0;
    mTableStart = 0;
    mSlotAddress = 0;
    mOriginalTarget = 0;
    mPageSize = 0;
    mOriginalProtection = 0;
    mOriginalPermissions.clear();
    mTargetValidated = false;
    mPatchAttempted = false;
    mPatchEverInstalled = false;
    mPatchRestoreAttempted = false;
    mPatchRestoreSucceeded = false;
    mRollbackAttempted = false;
    mRollbackSucceeded = false;
    mInstallWritableErrno = 0;
    mInstallRestoreErrno = 0;
    mRollbackWritableErrno = 0;
    mRollbackRestoreErrno = 0;
    mUninstallWritableErrno = 0;
    mUninstallRestoreErrno = 0;
    mMenuTrueObservedMilliseconds = 0;
    mMenuFalseObservedMilliseconds = 0;
    mMenuUnknownObservedMilliseconds = 0;
    mPreviousTotal = 0;
    mFailureReason.clear();
    mMenuOriginalStorage = nullptr;
    mMenuOriginalCallable.store(nullptr, std::memory_order_release);
    mMenuState.store(-1, std::memory_order_release);
    mMenuObserverCalls.store(0, std::memory_order_relaxed);
    mCallbacksInFlight.store(0, std::memory_order_relaxed);
    mStopRequested.store(false, std::memory_order_release);
    mPatchInstalled.store(false, std::memory_order_release);
    mCounter.total.store(0, std::memory_order_relaxed);
    mCounter.menuTrue.store(0, std::memory_order_relaxed);
    mCounter.menuFalse.store(0, std::memory_order_relaxed);
    mCounter.menuUnknown.store(0, std::memory_order_relaxed);
    mCounter.firstThreadId.store(0, std::memory_order_relaxed);
    mCounter.otherThreadCalls.store(0, std::memory_order_relaxed);
    mMenuTrueIntervals = IntervalStats{};
    mMenuFalseIntervals = IntervalStats{};

    createTimelineHeader();

    if (!validateTarget()) {
        writeProfile("validation_failed");
        mMod.getLogger().warn(
            "Single-slot vtable probe validation failed: {}",
            mFailureReason);
        return false;
    }

    aeronautics_slot160_original_target = mOriginalTarget;
    std::atomic_thread_fence(std::memory_order_release);

    VtableSlotProbe* expected = nullptr;
    if (!sActive.compare_exchange_strong(
            expected,
            this,
            std::memory_order_acq_rel,
            std::memory_order_acquire)) {
        mFailureReason = "another single-slot probe instance is already active";
        writeProfile("registration_failed");
        return false;
    }

    mMenuHook = pl::memory::HookHandle(
        reinterpret_cast<pl::memory::FuncPtr>(mHeartbeatTarget),
        reinterpret_cast<pl::memory::FuncPtr>(&VtableSlotProbe::menuDetour),
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

    mStartedAt = std::chrono::steady_clock::now();
    writeProfile("waiting_for_safe_patch");

    try {
        mWorker = std::thread([this] {
            workerLoop();
        });
    } catch (const std::system_error& error) {
        mFailureReason = std::string("slot probe worker thread failed: ") + error.what();
        mMenuHook.reset();
        mMenuOriginalCallable.store(nullptr, std::memory_order_release);
        clearActiveRegistration();
        writeProfile("worker_start_failed");
        return false;
    }

    mMod.getLogger().info(
        "Delayed single-slot vtable pointer probe armed; slot=160; activation_delay_ms={}; no Minecraft code bytes will be modified",
        activationDelayMilliseconds);
    return true;
}

void VtableSlotProbe::uninstall() noexcept {
    mStopRequested.store(true, std::memory_order_release);
    if (mWorker.joinable()) {
        mWorker.join();
    }

    if (mPatchInstalled.load(std::memory_order_acquire)) {
        restoreSlot();
    }

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

    if (mPatchInstalled.load(std::memory_order_acquire)) {
        if (mFailureReason.empty()) {
            mFailureReason =
                "vtable slot still points to module trampoline; refusing safe unload";
        }
        writeProfile("restore_failed_unsafe_to_unload");
        mMod.getLogger().error("{}", mFailureReason);
        return;
    }

    writeProfile("stopped");
    clearActiveRegistration();
    mMenuOriginalStorage = nullptr;
}

void VtableSlotProbe::recordActive() noexcept {
    if (VtableSlotProbe* const active =
            sActive.load(std::memory_order_acquire)) {
        active->record();
    }
}

bool VtableSlotProbe::menuDetour(void* instance) {
    VtableSlotProbe* const active = sActive.load(std::memory_order_acquire);
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

void VtableSlotProbe::record() noexcept {
    mCallbacksInFlight.fetch_add(1, std::memory_order_acq_rel);
    mCounter.total.fetch_add(1, std::memory_order_relaxed);

    const int menuState = mMenuState.load(std::memory_order_acquire);
    if (menuState == 1) {
        mCounter.menuTrue.fetch_add(1, std::memory_order_relaxed);
    } else if (menuState == 0) {
        mCounter.menuFalse.fetch_add(1, std::memory_order_relaxed);
    } else {
        mCounter.menuUnknown.fetch_add(1, std::memory_order_relaxed);
    }

    const std::uint32_t threadId = currentThreadId();
    if (threadId != 0) {
        std::uint32_t expected = 0;
        if (!mCounter.firstThreadId.compare_exchange_strong(
                expected,
                threadId,
                std::memory_order_acq_rel,
                std::memory_order_acquire) &&
            expected != threadId) {
            mCounter.otherThreadCalls.fetch_add(1, std::memory_order_relaxed);
        }
    }

    mCallbacksInFlight.fetch_sub(1, std::memory_order_acq_rel);
}

void VtableSlotProbe::workerLoop() {
    using namespace std::chrono_literals;

    std::uint64_t waitingSequence = 0;
    while (!mStopRequested.load(std::memory_order_acquire)) {
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - mStartedAt);
        const bool observerReady =
            mMenuObserverCalls.load(std::memory_order_relaxed) != 0 &&
            mMenuState.load(std::memory_order_acquire) != -1;

        if (observerReady &&
            static_cast<std::uint64_t>(elapsed.count()) >=
                activationDelayMilliseconds) {
            break;
        }

        if (static_cast<std::uint64_t>(elapsed.count()) >=
            activationTimeoutMilliseconds) {
            mFailureReason =
                "menu observer did not become ready before the delayed patch timeout";
            writeProfile("activation_timeout");
            return;
        }

        std::this_thread::sleep_for(100ms);
        ++waitingSequence;
        if ((waitingSequence % 20U) == 0U) {
            writeProfile("waiting_for_safe_patch");
        }
    }

    if (mStopRequested.load(std::memory_order_acquire)) {
        return;
    }

    mPatchAttempted = true;
    if (!patchSlot()) {
        writeProfile("patch_failed");
        return;
    }

    mPatchedAt = std::chrono::steady_clock::now();
    writeProfile("sampling");
    sampleLoop();
}

void VtableSlotProbe::sampleLoop() {
    using namespace std::chrono_literals;

    std::uint64_t sequence = 0;
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

        const std::uint64_t total =
            mCounter.total.load(std::memory_order_relaxed);
        const std::uint64_t delta = total - mPreviousTotal;
        mPreviousTotal = total;
        if (stableState && (intervalState == 0 || intervalState == 1)) {
            updateIntervalStats(intervalState, delta);
        }

        ++sequence;
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - mPatchedAt);
        appendTimeline(
            sequence,
            static_cast<std::uint64_t>(elapsed.count()),
            intervalState,
            stableState,
            delta);
        writeProfile("sampling");
    }
}

bool VtableSlotProbe::validateTarget() {
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

    mTableStart = module->loadBase + expectedVtableRunStartOffset;
    const std::size_t tableBytes =
        static_cast<std::size_t>(expectedVtableEntryCount) * sizeof(std::uintptr_t);
    const MemoryRegion* const tableRegion =
        findContainingRegion(*module, mTableStart, tableBytes);
    if (tableRegion == nullptr || tableRegion->executable) {
        mFailureReason =
            "accepted vtable run is not inside readable non-executable Minecraft memory";
        return false;
    }

    std::uintptr_t heartbeatEntry = 0;
    if (!readPointer(
            *module,
            mTableStart + heartbeatSlotIndex * sizeof(std::uintptr_t),
            heartbeatEntry) ||
        heartbeatEntry != mHeartbeatTarget) {
        mFailureReason =
            "vtable slot 152 does not point to the proven heartbeat function";
        return false;
    }

    mSlotAddress = mTableStart + probeSlotOffsetBytes;
    if ((mSlotAddress % alignof(std::uintptr_t)) != 0U ||
        !readPointer(*module, mSlotAddress, mOriginalTarget) ||
        mOriginalTarget != module->loadBase + probeTargetOffset ||
        !isExecutableAddress(*module, mOriginalTarget)) {
        mFailureReason =
            "slot 160 does not contain the exact accepted executable target";
        return false;
    }

    const long rawPageSize = ::sysconf(_SC_PAGESIZE);
    if (rawPageSize <= 0) {
        mFailureReason = "sysconf(_SC_PAGESIZE) failed";
        return false;
    }
    mPageSize = static_cast<std::size_t>(rawPageSize);

    if (!queryMemoryProtection(
            mSlotAddress,
            mOriginalProtection,
            mOriginalPermissions) ||
        (mOriginalProtection & PROT_READ) == 0 ||
        (mOriginalProtection & PROT_EXEC) != 0) {
        mFailureReason =
            "could not determine a safe readable non-executable protection for the vtable page";
        return false;
    }

    mTargetValidated = true;
    return true;
}

bool VtableSlotProbe::patchSlot() noexcept {
    const std::uintptr_t trampoline =
        reinterpret_cast<std::uintptr_t>(&aeronautics_slot160_trampoline);
    if (readSlotPointer() != mOriginalTarget) {
        mFailureReason =
            "slot 160 changed after validation; refusing to overwrite another writer";
        return false;
    }

    const bool writeSucceeded = writeSlotPointer(
        trampoline,
        mInstallWritableErrno,
        mInstallRestoreErrno);
    if (!writeSucceeded || readSlotPointer() != trampoline) {
        mRollbackAttempted = readSlotPointer() == trampoline;
        if (mRollbackAttempted) {
            mRollbackSucceeded = writeSlotPointer(
                mOriginalTarget,
                mRollbackWritableErrno,
                mRollbackRestoreErrno);
        }
        mFailureReason =
            "single vtable pointer replacement failed or could not restore page protection";
        return false;
    }

    mPatchEverInstalled = true;
    mPatchInstalled.store(true, std::memory_order_release);
    mMod.getLogger().info(
        "Single vtable pointer probe installed: slot=160, slot_offset=0x{:x}, target_offset=0x{:x}; Minecraft code bytes modified=0",
        probeSlotAddressOffset,
        probeTargetOffset);
    return true;
}

bool VtableSlotProbe::restoreSlot() noexcept {
    mPatchRestoreAttempted = true;
    const std::uintptr_t trampoline =
        reinterpret_cast<std::uintptr_t>(&aeronautics_slot160_trampoline);

    for (int attempt = 0; attempt < 3; ++attempt) {
        const std::uintptr_t current = readSlotPointer();
        if (current == mOriginalTarget) {
            mPatchInstalled.store(false, std::memory_order_release);
            mPatchRestoreSucceeded = true;
            return true;
        }
        if (current != trampoline) {
            mFailureReason =
                "slot 160 was changed by another writer; refusing destructive restoration";
            return false;
        }

        if (writeSlotPointer(
                mOriginalTarget,
                mUninstallWritableErrno,
                mUninstallRestoreErrno) &&
            readSlotPointer() == mOriginalTarget) {
            mPatchInstalled.store(false, std::memory_order_release);
            mPatchRestoreSucceeded = true;
            mMod.getLogger().info(
                "Single vtable pointer probe removed; original slot 160 target restored");
            return true;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    mFailureReason =
        "failed to restore original slot 160 pointer after three attempts";
    return false;
}

bool VtableSlotProbe::writeSlotPointer(
    std::uintptr_t value,
    int& writableErrno,
    int& restoreErrno) noexcept {
    writableErrno = 0;
    restoreErrno = 0;
    if (mSlotAddress == 0 || mPageSize == 0) {
        writableErrno = EINVAL;
        return false;
    }

    const std::uintptr_t pageSize = static_cast<std::uintptr_t>(mPageSize);
    const std::uintptr_t pageStart = mSlotAddress - (mSlotAddress % pageSize);
    const std::uintptr_t byteEnd = mSlotAddress + sizeof(std::uintptr_t);
    const std::uintptr_t pageEnd =
        byteEnd % pageSize == 0
        ? byteEnd
        : byteEnd + (pageSize - (byteEnd % pageSize));
    const std::size_t length = static_cast<std::size_t>(pageEnd - pageStart);

    const int writableProtection =
        (mOriginalProtection | PROT_READ | PROT_WRITE) & ~PROT_EXEC;
    if (::mprotect(
            reinterpret_cast<void*>(pageStart),
            length,
            writableProtection) != 0) {
        writableErrno = errno;
        return false;
    }

    __atomic_store_n(
        reinterpret_cast<std::uintptr_t*>(mSlotAddress),
        value,
        __ATOMIC_RELEASE);
    std::atomic_thread_fence(std::memory_order_seq_cst);

    if (::mprotect(
            reinterpret_cast<void*>(pageStart),
            length,
            mOriginalProtection) != 0) {
        restoreErrno = errno;
        return false;
    }

    return readSlotPointer() == value;
}

std::uintptr_t VtableSlotProbe::readSlotPointer() const noexcept {
    if (mSlotAddress == 0) {
        return 0;
    }
    std::uintptr_t value = 0;
    std::memcpy(
        &value,
        reinterpret_cast<const void*>(mSlotAddress),
        sizeof(value));
    return value;
}

void VtableSlotProbe::createTimelineHeader() noexcept {
    std::ofstream output(mTimelinePath, std::ios::trunc);
    if (!output) {
        return;
    }
    output
        << "schema=3\n"
        << "interval_ms=2000\n"
        << "probe_slot=160\n"
        << "patch_mode=vtable_data_pointer_swap\n"
        << "columns=sequence,elapsed_since_patch_ms,menu_state,stable_state,total_delta\n";
}

void VtableSlotProbe::appendTimeline(
    std::uint64_t sequence,
    std::uint64_t elapsedMilliseconds,
    int menuState,
    bool stableState,
    std::uint64_t delta) noexcept {
    std::ofstream output(mTimelinePath, std::ios::app);
    if (!output) {
        return;
    }
    output << sequence << ','
           << elapsedMilliseconds << ','
           << menuStateName(menuState) << ','
           << (stableState ? "true" : "false") << ','
           << delta << '\n';
}

void VtableSlotProbe::updateIntervalStats(
    int menuState,
    std::uint64_t delta) noexcept {
    IntervalStats& stats =
        menuState == 1 ? mMenuTrueIntervals : mMenuFalseIntervals;
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

void VtableSlotProbe::writeProfile(std::string_view state) noexcept {
    if (mProfilePath.empty()) {
        return;
    }

    std::ofstream output(mProfilePath, std::ios::trunc);
    if (!output) {
        return;
    }

    const std::uintptr_t trampoline =
        reinterpret_cast<std::uintptr_t>(&aeronautics_slot160_trampoline);
    const std::uintptr_t currentSlot = readSlotPointer();
    const std::uint64_t menuTrueCalls =
        mCounter.menuTrue.load(std::memory_order_relaxed);
    const std::uint64_t menuFalseCalls =
        mCounter.menuFalse.load(std::memory_order_relaxed);

    output << "schema=6\n";
    output << "state=" << state << '\n';
    output << "minecraft_version=" << CompatibilityProfile::minecraftVersion << '\n';
    output << "module_build_id="
           << (mModuleBuildId.empty() ? "unavailable" : mModuleBuildId) << '\n';
    output << "module_file_size=" << mModuleFileSize << '\n';
    output << "discovery_method=vtable_slot_pointer_trampoline\n";
    output << "patch_mode=data_pointer_swap_no_minecraft_code_patch\n";
    output << "minecraft_code_bytes_modified=0\n";
    output << "previous_failed_method=0.0.8_five_full_inline_hooks\n";
    output << "source_vtable_run_start_offset=0x"
           << std::hex << expectedVtableRunStartOffset << std::dec << '\n';
    output << "source_vtable_entry_count=" << expectedVtableEntryCount << '\n';
    output << "heartbeat_slot_index=" << heartbeatSlotIndex << '\n';
    output << "heartbeat_target_offset=0x"
           << std::hex << expectedHeartbeatOffset << std::dec << '\n';
    output << "probe_slot_index=" << probeSlotIndex << '\n';
    output << "probe_slot_address_offset=0x"
           << std::hex << probeSlotAddressOffset << std::dec << '\n';
    output << "probe_target_offset=0x"
           << std::hex << probeTargetOffset << std::dec << '\n';
    output << "activation_delay_ms=" << activationDelayMilliseconds << '\n';
    output << "activation_timeout_ms=" << activationTimeoutMilliseconds << '\n';
    output << "target_validated=" << (mTargetValidated ? "true" : "false") << '\n';
    output << "original_page_permissions="
           << (mOriginalPermissions.empty() ? "unavailable" : mOriginalPermissions)
           << '\n';
    output << "page_size=" << mPageSize << '\n';
    output << "patch_attempted=" << (mPatchAttempted ? "true" : "false") << '\n';
    output << "patch_ever_installed="
           << (mPatchEverInstalled ? "true" : "false") << '\n';
    output << "patch_currently_installed="
           << (mPatchInstalled.load(std::memory_order_acquire) ? "true" : "false")
           << '\n';
    output << "slot_current_state="
           << slotStateName(currentSlot, mOriginalTarget, trampoline) << '\n';
    output << "patch_restore_attempted="
           << (mPatchRestoreAttempted ? "true" : "false") << '\n';
    output << "patch_restore_succeeded="
           << (mPatchRestoreSucceeded ? "true" : "false") << '\n';
    output << "rollback_attempted="
           << (mRollbackAttempted ? "true" : "false") << '\n';
    output << "rollback_succeeded="
           << (mRollbackSucceeded ? "true" : "false") << '\n';
    output << "install_mprotect_writable_errno=" << mInstallWritableErrno << '\n';
    output << "install_mprotect_restore_errno=" << mInstallRestoreErrno << '\n';
    output << "rollback_mprotect_writable_errno=" << mRollbackWritableErrno << '\n';
    output << "rollback_mprotect_restore_errno=" << mRollbackRestoreErrno << '\n';
    output << "uninstall_mprotect_writable_errno=" << mUninstallWritableErrno << '\n';
    output << "uninstall_mprotect_restore_errno=" << mUninstallRestoreErrno << '\n';
    output << "menu_observer_calls="
           << mMenuObserverCalls.load(std::memory_order_relaxed) << '\n';
    output << "menu_state_current="
           << menuStateName(mMenuState.load(std::memory_order_acquire)) << '\n';
    output << "menu_true_observed_ms=" << mMenuTrueObservedMilliseconds << '\n';
    output << "menu_false_observed_ms=" << mMenuFalseObservedMilliseconds << '\n';
    output << "menu_unknown_observed_ms=" << mMenuUnknownObservedMilliseconds << '\n';
    output << "total_calls="
           << mCounter.total.load(std::memory_order_relaxed) << '\n';
    output << "menu_true_calls=" << menuTrueCalls << '\n';
    output << "menu_false_calls=" << menuFalseCalls << '\n';
    output << "menu_unknown_calls="
           << mCounter.menuUnknown.load(std::memory_order_relaxed) << '\n';
    output << "menu_true_rate_millihz="
           << rateMilliHertz(menuTrueCalls, mMenuTrueObservedMilliseconds) << '\n';
    output << "menu_false_rate_millihz="
           << rateMilliHertz(menuFalseCalls, mMenuFalseObservedMilliseconds) << '\n';
    output << "first_thread_id="
           << mCounter.firstThreadId.load(std::memory_order_relaxed) << '\n';
    output << "other_thread_calls="
           << mCounter.otherThreadCalls.load(std::memory_order_relaxed) << '\n';
    output << "callbacks_in_flight="
           << mCallbacksInFlight.load(std::memory_order_acquire) << '\n';
    output << "stable_menu_true_intervals=" << mMenuTrueIntervals.intervals << '\n';
    output << "stable_menu_true_delta_min="
           << (mMenuTrueIntervals.initialized ? mMenuTrueIntervals.minimum : 0)
           << '\n';
    output << "stable_menu_true_delta_max="
           << (mMenuTrueIntervals.initialized ? mMenuTrueIntervals.maximum : 0)
           << '\n';
    output << "stable_menu_false_intervals=" << mMenuFalseIntervals.intervals << '\n';
    output << "stable_menu_false_delta_min="
           << (mMenuFalseIntervals.initialized ? mMenuFalseIntervals.minimum : 0)
           << '\n';
    output << "stable_menu_false_delta_max="
           << (mMenuFalseIntervals.initialized ? mMenuFalseIntervals.maximum : 0)
           << '\n';
    output << "failure_reason="
           << (mFailureReason.empty() ? "none" : mFailureReason) << '\n';
    output << "timeline_file=vtable-probe-timeline.txt\n";
}

void VtableSlotProbe::clearActiveRegistration() noexcept {
    VtableSlotProbe* expected = this;
    sActive.compare_exchange_strong(
        expected,
        nullptr,
        std::memory_order_acq_rel,
        std::memory_order_acquire);
}

}  // namespace aeronautics::bedrock
