#include "bedrock/VtableSlotProbeV3.hpp"

#include "bedrock/CompatibilityProfile.hpp"
#include "bedrock/HeartbeatHook.hpp"

#include <atomic>
#include <cerrno>
#include <charconv>
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

extern "C" {
__attribute__((visibility("hidden"), used, aligned(8)))
std::uintptr_t aeronautics_slot160_v3_original_target = 0;
}

namespace aeronautics::bedrock {
namespace {

constexpr std::string_view expectedBuildId{
    "2e318db12824cadb2618754ab7c82fa96fb30659"};
constexpr std::uintmax_t expectedModuleFileSize = 349243744;
constexpr std::uintptr_t expectedVtableRunStartOffset = 0x140545a0;
constexpr std::uint32_t expectedVtableEntryCount = 438;
constexpr std::uint32_t probeSlotIndex = 160;
constexpr std::uintptr_t probeTargetOffset = 0x9d82094;
constexpr std::uintptr_t probeSlotAddressOffset =
    expectedVtableRunStartOffset +
    static_cast<std::uintptr_t>(probeSlotIndex) * sizeof(std::uintptr_t);
constexpr std::uint64_t activationDelayMilliseconds = 8000;
constexpr std::uint64_t activationTimeoutMilliseconds = 30000;
constexpr std::uint64_t sampleIntervalMilliseconds = 2000;

[[nodiscard]] const MemoryRegion* findContainingRegion(
    const ModuleFingerprint& module,
    std::uintptr_t address,
    std::size_t byteCount) noexcept {
    for (const MemoryRegion& region : module.regions) {
        if (region.readable && address >= region.start && address < region.end &&
            byteCount <= region.end - address) {
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

[[nodiscard]] bool parseHex(std::string_view text, std::uintptr_t& value) noexcept {
    value = 0;
    const auto result = std::from_chars(
        text.data(), text.data() + text.size(), value, 16);
    return result.ec == std::errc{} && result.ptr == text.data() + text.size();
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
        std::istringstream stream{line};
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
        if (!parseHex(std::string_view(range).substr(0, separator), start) ||
            !parseHex(std::string_view(range).substr(separator + 1), end) ||
            address < start || address >= end) {
            continue;
        }
        protection = 0;
        protection |= !perms.empty() && perms[0] == 'r' ? PROT_READ : 0;
        protection |= perms.size() >= 2 && perms[1] == 'w' ? PROT_WRITE : 0;
        protection |= perms.size() >= 3 && perms[2] == 'x' ? PROT_EXEC : 0;
        permissions = perms;
        return true;
    }
    return false;
}

[[nodiscard]] std::uint32_t currentThreadId() noexcept {
    const long raw = ::syscall(SYS_gettid);
    if (raw <= 0 || static_cast<unsigned long>(raw) >
            static_cast<unsigned long>(std::numeric_limits<std::uint32_t>::max())) {
        return 0;
    }
    return static_cast<std::uint32_t>(raw);
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

std::atomic<VtableSlotProbeV3*> VtableSlotProbeV3::sActive{nullptr};

}  // namespace aeronautics::bedrock

extern "C" __attribute__((visibility("hidden"), used, noinline))
void aeronautics_slot160_v3_record() noexcept {
    aeronautics::bedrock::VtableSlotProbeV3::recordActive();
}

extern "C" __attribute__((naked, visibility("hidden"), used))
void aeronautics_slot160_v3_trampoline() noexcept {
    asm volatile(
        ".inst 0xd503245f\n"
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
        "bl aeronautics_slot160_v3_record\n"
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
        "adrp x16, aeronautics_slot160_v3_original_target\n"
        "ldr x16, [x16, :lo12:aeronautics_slot160_v3_original_target]\n"
        "br x16\n");
}

namespace aeronautics::bedrock {

VtableSlotProbeV3::VtableSlotProbeV3(
    ll::mod::NativeMod& mod,
    HeartbeatHook& heartbeat) noexcept
    : mMod(mod), mHeartbeat(heartbeat) {}

VtableSlotProbeV3::~VtableSlotProbeV3() {
    uninstall();
}

bool VtableSlotProbeV3::install() {
    if (mWorker.joinable() || mPatchInstalled.load(std::memory_order_acquire)) {
        return true;
    }
    mProfilePath = mMod.getDataDir() / "vtable-probe-profile.txt";
    mTimelinePath = mMod.getDataDir() / "vtable-probe-timeline.txt";
    mFailureReason.clear();
    mTotalCalls.store(0, std::memory_order_relaxed);
    mFirstThreadId.store(0, std::memory_order_relaxed);
    mOtherThreadCalls.store(0, std::memory_order_relaxed);
    mCallbacksInFlight.store(0, std::memory_order_relaxed);
    mStopRequested.store(false, std::memory_order_release);
    mPatchInstalled.store(false, std::memory_order_release);
    mPatchAttempted = false;
    mPatchEverInstalled = false;
    mPatchRestoreAttempted = false;
    mPatchRestoreSucceeded = false;
    mRollbackAttempted = false;
    mRollbackSucceeded = false;
    mPreviousTotal = 0;
    mHeartbeatCallsAtArm = mHeartbeat.callCount();
    createTimelineHeader();

    if (!validateTarget()) {
        writeProfile("validation_failed");
        return false;
    }
    aeronautics_slot160_v3_original_target = mOriginalTarget;
    std::atomic_thread_fence(std::memory_order_release);
    VtableSlotProbeV3* expected = nullptr;
    if (!sActive.compare_exchange_strong(
            expected, this, std::memory_order_acq_rel, std::memory_order_acquire)) {
        mFailureReason = "another slot probe instance is active";
        writeProfile("registration_failed");
        return false;
    }
    mArmedAt = std::chrono::steady_clock::now();
    writeProfile("waiting_for_primary_heartbeat");
    try {
        mWorker = std::thread([this] { workerLoop(); });
    } catch (const std::system_error& error) {
        mFailureReason = std::string("slot probe worker failed: ") + error.what();
        clearActiveRegistration();
        writeProfile("worker_start_failed");
        return false;
    }
    mMod.getLogger().info(
        "Heartbeat-gated slot pointer probe armed; no duplicate menu hook; activation_delay_ms={}",
        activationDelayMilliseconds);
    return true;
}

void VtableSlotProbeV3::uninstall() noexcept {
    mStopRequested.store(true, std::memory_order_release);
    if (mWorker.joinable()) {
        mWorker.join();
    }
    if (mPatchInstalled.load(std::memory_order_acquire)) {
        (void)restoreSlot();
    }
    using namespace std::chrono_literals;
    for (int attempt = 0;
         mCallbacksInFlight.load(std::memory_order_acquire) != 0 && attempt < 1000;
         ++attempt) {
        std::this_thread::sleep_for(1ms);
    }
    writeProfile(mPatchInstalled.load(std::memory_order_acquire)
        ? "restore_failed_unsafe_to_unload" : "stopped");
    if (!mPatchInstalled.load(std::memory_order_acquire)) {
        clearActiveRegistration();
    }
}

void VtableSlotProbeV3::recordActive() noexcept {
    if (VtableSlotProbeV3* active = sActive.load(std::memory_order_acquire)) {
        active->record();
    }
}

void VtableSlotProbeV3::record() noexcept {
    mCallbacksInFlight.fetch_add(1, std::memory_order_acq_rel);
    mTotalCalls.fetch_add(1, std::memory_order_relaxed);
    const std::uint32_t tid = currentThreadId();
    if (tid != 0) {
        std::uint32_t expected = 0;
        if (!mFirstThreadId.compare_exchange_strong(
                expected, tid, std::memory_order_acq_rel, std::memory_order_acquire) &&
            expected != tid) {
            mOtherThreadCalls.fetch_add(1, std::memory_order_relaxed);
        }
    }
    mCallbacksInFlight.fetch_sub(1, std::memory_order_acq_rel);
}

void VtableSlotProbeV3::workerLoop() {
    using namespace std::chrono_literals;
    while (!mStopRequested.load(std::memory_order_acquire)) {
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - mArmedAt);
        const std::uint64_t heartbeatCalls = mHeartbeat.callCount();
        const bool heartbeatAdvanced = heartbeatCalls > mHeartbeatCallsAtArm;
        if (heartbeatAdvanced && elapsed.count() >=
                static_cast<std::int64_t>(activationDelayMilliseconds)) {
            break;
        }
        if (elapsed.count() >= static_cast<std::int64_t>(activationTimeoutMilliseconds)) {
            mFailureReason = "primary proven heartbeat did not advance before activation timeout";
            writeProfile("activation_timeout");
            return;
        }
        std::this_thread::sleep_for(100ms);
    }
    if (mStopRequested.load(std::memory_order_acquire)) {
        return;
    }
    mHeartbeatCallsAtPatch = mHeartbeat.callCount();
    mPatchAttempted = true;
    if (!patchSlot()) {
        writeProfile("patch_failed");
        return;
    }
    mPatchedAt = std::chrono::steady_clock::now();
    writeProfile("sampling");
    std::uint64_t sequence = 0;
    while (!mStopRequested.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(
            std::chrono::milliseconds(sampleIntervalMilliseconds));
        if (mStopRequested.load(std::memory_order_acquire)) {
            break;
        }
        const std::uint64_t total = mTotalCalls.load(std::memory_order_relaxed);
        const std::uint64_t delta = total - mPreviousTotal;
        mPreviousTotal = total;
        ++sequence;
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - mPatchedAt);
        appendTimeline(
            sequence,
            static_cast<std::uint64_t>(elapsed.count()),
            mHeartbeat.callCount(),
            delta);
        writeProfile("sampling");
    }
}

bool VtableSlotProbeV3::validateTarget() {
    const auto module = inspectLoadedModule(CompatibilityProfile::moduleName);
    if (!module) {
        mFailureReason = "libminecraftpe.so is not loaded";
        return false;
    }
    mModuleBuildId = module->buildId;
    mModuleFileSize = module->fileSize;
    mModuleLoadBase = module->loadBase;
    if (module->buildId != expectedBuildId || module->fileSize != expectedModuleFileSize) {
        mFailureReason = "Minecraft fingerprint mismatch";
        return false;
    }
    mTableStart = module->loadBase + expectedVtableRunStartOffset;
    const std::size_t tableBytes =
        static_cast<std::size_t>(expectedVtableEntryCount) * sizeof(std::uintptr_t);
    const MemoryRegion* tableRegion = findContainingRegion(*module, mTableStart, tableBytes);
    if (tableRegion == nullptr || tableRegion->executable) {
        mFailureReason = "accepted vtable run is not readable non-executable memory";
        return false;
    }
    mSlotAddress = module->loadBase + probeSlotAddressOffset;
    if (!readPointer(*module, mSlotAddress, mOriginalTarget) ||
        mOriginalTarget != module->loadBase + probeTargetOffset ||
        !isExecutableAddress(*module, mOriginalTarget)) {
        mFailureReason = "slot 160 target mismatch";
        return false;
    }
    const long rawPageSize = ::sysconf(_SC_PAGESIZE);
    if (rawPageSize <= 0) {
        mFailureReason = "sysconf page size failed";
        return false;
    }
    mPageSize = static_cast<std::size_t>(rawPageSize);
    if (!queryMemoryProtection(
            mSlotAddress, mOriginalProtection, mOriginalPermissions) ||
        (mOriginalProtection & PROT_READ) == 0 ||
        (mOriginalProtection & PROT_EXEC) != 0) {
        mFailureReason = "unsafe vtable page protection";
        return false;
    }
    mTargetValidated = true;
    return true;
}

bool VtableSlotProbeV3::patchSlot() noexcept {
    const auto trampoline =
        reinterpret_cast<std::uintptr_t>(&aeronautics_slot160_v3_trampoline);
    if (readSlotPointer() != mOriginalTarget) {
        mFailureReason = "slot changed after validation";
        return false;
    }
    const bool written = writeSlotPointer(
        trampoline, mInstallWritableErrno, mInstallRestoreErrno);
    if (!written || readSlotPointer() != trampoline) {
        mRollbackAttempted = readSlotPointer() == trampoline;
        if (mRollbackAttempted) {
            mRollbackSucceeded = writeSlotPointer(
                mOriginalTarget, mRollbackWritableErrno, mRollbackRestoreErrno);
        }
        mFailureReason = "vtable pointer replacement failed";
        return false;
    }
    mPatchEverInstalled = true;
    mPatchInstalled.store(true, std::memory_order_release);
    return true;
}

bool VtableSlotProbeV3::restoreSlot() noexcept {
    mPatchRestoreAttempted = true;
    const auto trampoline =
        reinterpret_cast<std::uintptr_t>(&aeronautics_slot160_v3_trampoline);
    for (int attempt = 0; attempt < 3; ++attempt) {
        const std::uintptr_t current = readSlotPointer();
        if (current == mOriginalTarget) {
            mPatchInstalled.store(false, std::memory_order_release);
            mPatchRestoreSucceeded = true;
            return true;
        }
        if (current != trampoline) {
            mFailureReason = "slot changed by another writer";
            return false;
        }
        if (writeSlotPointer(
                mOriginalTarget, mUninstallWritableErrno, mUninstallRestoreErrno) &&
            readSlotPointer() == mOriginalTarget) {
            mPatchInstalled.store(false, std::memory_order_release);
            mPatchRestoreSucceeded = true;
            return true;
        }
    }
    mFailureReason = "original slot pointer could not be restored";
    return false;
}

bool VtableSlotProbeV3::writeSlotPointer(
    std::uintptr_t value,
    int& writableErrno,
    int& restoreErrno) noexcept {
    writableErrno = 0;
    restoreErrno = 0;
    const std::uintptr_t page =
        mSlotAddress & ~(static_cast<std::uintptr_t>(mPageSize) - 1U);
    if (::mprotect(
            reinterpret_cast<void*>(page),
            mPageSize,
            mOriginalProtection | PROT_WRITE) != 0) {
        writableErrno = errno;
        return false;
    }
    std::atomic_ref<std::uintptr_t> slot(
        *reinterpret_cast<std::uintptr_t*>(mSlotAddress));
    slot.store(value, std::memory_order_release);
    if (::mprotect(
            reinterpret_cast<void*>(page),
            mPageSize,
            mOriginalProtection) != 0) {
        restoreErrno = errno;
        return false;
    }
    return true;
}

std::uintptr_t VtableSlotProbeV3::readSlotPointer() const noexcept {
    std::uintptr_t value = 0;
    std::memcpy(&value, reinterpret_cast<const void*>(mSlotAddress), sizeof(value));
    return value;
}

void VtableSlotProbeV3::createTimelineHeader() noexcept {
    std::ofstream output(mTimelinePath, std::ios::trunc);
    if (output) {
        output << "schema=4\n"
               << "interval_ms=" << sampleIntervalMilliseconds << "\n"
               << "probe_slot=160\n"
               << "heartbeat_source=primary_proven_hook_call_count\n"
               << "columns=sequence,elapsed_since_patch_ms,heartbeat_calls,total_delta\n";
    }
}

void VtableSlotProbeV3::appendTimeline(
    std::uint64_t sequence,
    std::uint64_t elapsedMilliseconds,
    std::uint64_t heartbeatCalls,
    std::uint64_t totalDelta) noexcept {
    std::ofstream output(mTimelinePath, std::ios::app);
    if (output) {
        output << sequence << ',' << elapsedMilliseconds << ','
               << heartbeatCalls << ',' << totalDelta << '\n';
    }
}

void VtableSlotProbeV3::writeProfile(std::string_view state) noexcept {
    std::ofstream output(mProfilePath, std::ios::trunc);
    if (!output) {
        return;
    }
    const auto trampoline =
        reinterpret_cast<std::uintptr_t>(&aeronautics_slot160_v3_trampoline);
    const std::uintptr_t current = mSlotAddress == 0 ? 0 : readSlotPointer();
    output << "schema=7\n"
           << "state=" << state << '\n'
           << "minecraft_version=1.26.33.1\n"
           << "module_build_id=" << mModuleBuildId << '\n'
           << "module_file_size=" << mModuleFileSize << '\n'
           << "discovery_method=vtable_slot_pointer_trampoline_primary_heartbeat_gate\n"
           << "patch_mode=data_pointer_swap_no_minecraft_code_patch\n"
           << "minecraft_code_bytes_modified=0\n"
           << "heartbeat_state_source=primary_proven_hook_call_count\n"
           << "duplicate_menu_hook_installed=false\n"
           << "probe_slot_index=160\n"
           << "probe_slot_address_offset=0x" << std::hex << probeSlotAddressOffset << std::dec << '\n'
           << "probe_target_offset=0x" << std::hex << probeTargetOffset << std::dec << '\n'
           << "activation_delay_ms=" << activationDelayMilliseconds << '\n'
           << "activation_timeout_ms=" << activationTimeoutMilliseconds << '\n'
           << "target_validated=" << (mTargetValidated ? "true" : "false") << '\n'
           << "original_page_permissions=" << mOriginalPermissions << '\n'
           << "patch_attempted=" << (mPatchAttempted ? "true" : "false") << '\n'
           << "patch_ever_installed=" << (mPatchEverInstalled ? "true" : "false") << '\n'
           << "patch_currently_installed="
           << (mPatchInstalled.load(std::memory_order_acquire) ? "true" : "false") << '\n'
           << "slot_current_state="
           << slotStateName(current, mOriginalTarget, trampoline) << '\n'
           << "patch_restore_attempted=" << (mPatchRestoreAttempted ? "true" : "false") << '\n'
           << "patch_restore_succeeded=" << (mPatchRestoreSucceeded ? "true" : "false") << '\n'
           << "heartbeat_calls_at_arm=" << mHeartbeatCallsAtArm << '\n'
           << "heartbeat_calls_at_patch=" << mHeartbeatCallsAtPatch << '\n'
           << "heartbeat_calls_current=" << mHeartbeat.callCount() << '\n'
           << "total_calls=" << mTotalCalls.load(std::memory_order_relaxed) << '\n'
           << "first_thread_id=" << mFirstThreadId.load(std::memory_order_relaxed) << '\n'
           << "other_thread_calls=" << mOtherThreadCalls.load(std::memory_order_relaxed) << '\n'
           << "callbacks_in_flight=" << mCallbacksInFlight.load(std::memory_order_relaxed) << '\n'
           << "install_mprotect_writable_errno=" << mInstallWritableErrno << '\n'
           << "install_mprotect_restore_errno=" << mInstallRestoreErrno << '\n'
           << "rollback_attempted=" << (mRollbackAttempted ? "true" : "false") << '\n'
           << "rollback_succeeded=" << (mRollbackSucceeded ? "true" : "false") << '\n'
           << "failure_reason=" << (mFailureReason.empty() ? "none" : mFailureReason) << '\n'
           << "timeline_file=vtable-probe-timeline.txt\n"
           << "class_discovery_file=level-class-discovery.txt\n";
}

void VtableSlotProbeV3::clearActiveRegistration() noexcept {
    VtableSlotProbeV3* expected = this;
    sActive.compare_exchange_strong(
        expected, nullptr, std::memory_order_acq_rel, std::memory_order_acquire);
}

}  // namespace aeronautics::bedrock
