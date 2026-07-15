#include "bedrock/ClientLevelTickHook.hpp"

#include "bedrock/CompatibilityProfile.hpp"
#include "bedrock/HeartbeatHook.hpp"

#include <array>
#include <atomic>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <limits>
#include <sstream>
#include <string>
#include <system_error>

#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

extern "C" {
__attribute__((visibility("hidden"), used, aligned(8)))
std::uintptr_t aeronautics_clientlevel_tick_original_target = 0;
}

namespace aeronautics::bedrock {
namespace {

constexpr std::string_view expectedBuildId{
    "2e318db12824cadb2618754ab7c82fa96fb30659"};
constexpr std::uintmax_t expectedModuleFileSize = 349243744;
constexpr std::uintptr_t clientLevelVtableAddressPointOffset = 0x14199700;
constexpr std::uintptr_t clientLevelTypeInfoOffset = 0x1419b880;
constexpr std::uintptr_t clientLevelTypeNameOffset = 0x02b4145b;
constexpr std::string_view clientLevelTypeName{"11ClientLevel"};
constexpr std::uint32_t clientLevelSubTickSlotIndex = 408;
constexpr std::uintptr_t clientLevelSubTickSlotOffset = 0x1419a3c0;
constexpr std::uintptr_t clientLevelSubTickTargetOffset = 0x0b72f984;
constexpr std::uint64_t heartbeatStabilityMilliseconds = 8000;
constexpr std::uint64_t heartbeatMaximumStallMilliseconds = 2000;
constexpr std::uint64_t statusIntervalMilliseconds = 2000;
constexpr std::array<std::uint8_t, 16> expectedFunctionPrefix{
    0xff, 0xc3, 0x01, 0xd1, 0xfd, 0x7b, 0x03, 0xa9,
    0xf7, 0x23, 0x00, 0xf9, 0xf6, 0x57, 0x05, 0xa9};

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

[[nodiscard]] bool readSignedPointer(
    const ModuleFingerprint& module,
    std::uintptr_t address,
    std::ptrdiff_t& value) noexcept {
    if (findContainingRegion(module, address, sizeof(value)) == nullptr) {
        return false;
    }
    std::memcpy(&value, reinterpret_cast<const void*>(address), sizeof(value));
    return true;
}

[[nodiscard]] bool readBytes(
    const ModuleFingerprint& module,
    std::uintptr_t address,
    void* output,
    std::size_t byteCount) noexcept {
    if (findContainingRegion(module, address, byteCount) == nullptr) {
        return false;
    }
    std::memcpy(output, reinterpret_cast<const void*>(address), byteCount);
    return true;
}

[[nodiscard]] bool matchesCString(
    const ModuleFingerprint& module,
    std::uintptr_t address,
    std::string_view expected) noexcept {
    if (findContainingRegion(module, address, expected.size() + 1U) == nullptr) {
        return false;
    }
    const auto* text = reinterpret_cast<const char*>(address);
    return std::memcmp(text, expected.data(), expected.size()) == 0 &&
        text[expected.size()] == '\0';
}

[[nodiscard]] std::string bytesToHex(const std::uint8_t* bytes, std::size_t count) {
    std::ostringstream stream;
    stream << std::hex << std::setfill('0');
    for (std::size_t index = 0; index < count; ++index) {
        if (index != 0) {
            stream << ' ';
        }
        stream << std::setw(2) << static_cast<unsigned int>(bytes[index]);
    }
    return stream.str();
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
    if (original == 0) {
        return "uninitialized";
    }
    if (value == original) {
        return "original";
    }
    if (value == trampoline) {
        return "trampoline";
    }
    return "other";
}

}  // namespace

std::atomic<ClientLevelTickHook*> ClientLevelTickHook::sActive{nullptr};

}  // namespace aeronautics::bedrock

extern "C" __attribute__((visibility("hidden"), used, noinline))
void aeronautics_clientlevel_tick_record(void* instance) noexcept {
    aeronautics::bedrock::ClientLevelTickHook::recordActive(instance);
}

extern "C" __attribute__((naked, visibility("hidden"), used))
void aeronautics_clientlevel_tick_trampoline() noexcept {
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
        "ldr x0, [sp, #0x00]\n"
        "bl aeronautics_clientlevel_tick_record\n"
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
        "adrp x16, aeronautics_clientlevel_tick_original_target\n"
        "ldr x16, [x16, :lo12:aeronautics_clientlevel_tick_original_target]\n"
        "br x16\n");
}

namespace aeronautics::bedrock {

ClientLevelTickHook::ClientLevelTickHook(
    ll::mod::NativeMod& mod,
    HeartbeatHook& heartbeat,
    ClientLevelTickBus& eventBus) noexcept
    : mMod(mod), mHeartbeat(heartbeat), mEventBus(eventBus) {}

ClientLevelTickHook::~ClientLevelTickHook() {
    uninstall();
}

bool ClientLevelTickHook::install() {
    if (mWorker.joinable() || mPatchInstalled.load(std::memory_order_acquire)) {
        return true;
    }

    mStatusPath = mMod.getDataDir() / "clientlevel-tick-source-status.txt";
    mFailureReason.clear();
    mStopRequested.store(false, std::memory_order_release);
    mPatchInstalled.store(false, std::memory_order_release);
    mCallbacksInFlight.store(0, std::memory_order_relaxed);
    mTotalCalls.store(0, std::memory_order_relaxed);
    mDeliveredCallbacks.store(0, std::memory_order_relaxed);
    mFirstThreadId.store(0, std::memory_order_relaxed);
    mOtherThreadCalls.store(0, std::memory_order_relaxed);
    mFirstInstance.store(0, std::memory_order_relaxed);
    mLastInstance.store(0, std::memory_order_relaxed);
    mInstanceTransitions.store(0, std::memory_order_relaxed);
    mTargetValidated = false;
    mRttiValidated = false;
    mFunctionPrefixValidated = false;
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
    mHeartbeatCallsAtArm = mHeartbeat.callCount();
    mHeartbeatCallsAtFirstActivity = 0;
    mHeartbeatCallsAtPatch = 0;

    if (!validateTarget()) {
        writeStatus("validation_failed");
        mMod.getLogger().warn(
            "ClientLevel tick source validation failed: {}",
            mFailureReason);
        return false;
    }

    aeronautics_clientlevel_tick_original_target = mOriginalTarget;
    std::atomic_thread_fence(std::memory_order_release);

    ClientLevelTickHook* expected = nullptr;
    if (!sActive.compare_exchange_strong(
            expected,
            this,
            std::memory_order_acq_rel,
            std::memory_order_acquire)) {
        mFailureReason = "another ClientLevel tick source is active";
        writeStatus("registration_failed");
        return false;
    }

    writeStatus("waiting_for_primary_heartbeat");
    try {
        mWorker = std::thread([this] { workerLoop(); });
    } catch (const std::system_error& error) {
        mFailureReason = std::string("ClientLevel tick source worker failed: ") +
            error.what();
        clearActiveRegistration();
        writeStatus("worker_start_failed");
        return false;
    }

    mMod.getLogger().info(
        "ClientLevel tick event source armed; exact slot=408; waiting for stable primary heartbeat");
    return true;
}

void ClientLevelTickHook::uninstall() noexcept {
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

    if (mPatchInstalled.load(std::memory_order_acquire)) {
        if (mFailureReason.empty()) {
            mFailureReason = "ClientLevel vtable still references the tick trampoline";
        }
        writeStatus("restore_failed_unsafe_to_unload");
        return;
    }

    writeStatus("stopped");
    clearActiveRegistration();
}

void ClientLevelTickHook::recordActive(void* instance) noexcept {
    if (ClientLevelTickHook* active = sActive.load(std::memory_order_acquire)) {
        active->record(instance);
    }
}

void ClientLevelTickHook::record(void* instance) noexcept {
    mCallbacksInFlight.fetch_add(1, std::memory_order_acq_rel);
    const std::uint64_t sequence =
        mTotalCalls.fetch_add(1, std::memory_order_relaxed) + 1U;
    const std::uint32_t threadId = currentThreadId();

    if (threadId != 0) {
        std::uint32_t expectedThread = 0;
        if (!mFirstThreadId.compare_exchange_strong(
                expectedThread,
                threadId,
                std::memory_order_acq_rel,
                std::memory_order_acquire) &&
            expectedThread != threadId) {
            mOtherThreadCalls.fetch_add(1, std::memory_order_relaxed);
        }
    }

    const std::uintptr_t instanceValue = reinterpret_cast<std::uintptr_t>(instance);
    std::uintptr_t expectedInstance = 0;
    mFirstInstance.compare_exchange_strong(
        expectedInstance,
        instanceValue,
        std::memory_order_acq_rel,
        std::memory_order_acquire);
    const std::uintptr_t previousInstance =
        mLastInstance.exchange(instanceValue, std::memory_order_acq_rel);
    if (previousInstance != 0 && previousInstance != instanceValue) {
        mInstanceTransitions.fetch_add(1, std::memory_order_relaxed);
    }

    const ClientLevelTickEvent event{instance, sequence, threadId};
    const std::size_t delivered = mEventBus.publish(event);
    mDeliveredCallbacks.fetch_add(
        static_cast<std::uint64_t>(delivered),
        std::memory_order_relaxed);
    mCallbacksInFlight.fetch_sub(1, std::memory_order_acq_rel);
}

void ClientLevelTickHook::workerLoop() {
    using namespace std::chrono_literals;

    std::uint64_t lastHeartbeatCount = mHeartbeatCallsAtArm;
    auto lastHeartbeatAdvance = std::chrono::steady_clock::time_point{};
    auto stabilityStart = std::chrono::steady_clock::time_point{};
    std::uint64_t loopSequence = 0;

    while (!mStopRequested.load(std::memory_order_acquire)) {
        const auto now = std::chrono::steady_clock::now();
        const std::uint64_t heartbeatCount = mHeartbeat.callCount();

        if (heartbeatCount > lastHeartbeatCount) {
            lastHeartbeatCount = heartbeatCount;
            lastHeartbeatAdvance = now;
            if (stabilityStart == std::chrono::steady_clock::time_point{}) {
                stabilityStart = now;
                mHeartbeatCallsAtFirstActivity = heartbeatCount;
                writeStatus("validating_primary_heartbeat_stability");
            }
        }

        if (stabilityStart != std::chrono::steady_clock::time_point{}) {
            const auto stableFor = std::chrono::duration_cast<std::chrono::milliseconds>(
                now - stabilityStart);
            const auto stalledFor = std::chrono::duration_cast<std::chrono::milliseconds>(
                now - lastHeartbeatAdvance);

            if (stalledFor.count() >
                static_cast<std::int64_t>(heartbeatMaximumStallMilliseconds)) {
                stabilityStart = std::chrono::steady_clock::time_point{};
                mHeartbeatCallsAtFirstActivity = 0;
                writeStatus("waiting_for_primary_heartbeat");
            } else if (
                stableFor.count() >=
                    static_cast<std::int64_t>(heartbeatStabilityMilliseconds) &&
                heartbeatCount > mHeartbeatCallsAtFirstActivity) {
                break;
            }
        }

        std::this_thread::sleep_for(100ms);
        if ((++loopSequence % 20U) == 0U) {
            writeStatus(
                stabilityStart == std::chrono::steady_clock::time_point{}
                    ? "waiting_for_primary_heartbeat"
                    : "validating_primary_heartbeat_stability");
        }
    }

    if (mStopRequested.load(std::memory_order_acquire)) {
        return;
    }

    mHeartbeatCallsAtPatch = mHeartbeat.callCount();
    mPatchAttempted = true;
    if (!patchSlot()) {
        writeStatus("patch_failed");
        return;
    }

    writeStatus("publishing_clientlevel_tick_events");
    while (!mStopRequested.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(
            std::chrono::milliseconds(statusIntervalMilliseconds));
        if (!mStopRequested.load(std::memory_order_acquire)) {
            writeStatus("publishing_clientlevel_tick_events");
        }
    }
}

bool ClientLevelTickHook::validateTarget() {
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
        mFailureReason = "Minecraft binary fingerprint mismatch";
        return false;
    }

    const std::uintptr_t vtableAddressPoint =
        module->loadBase + clientLevelVtableAddressPointOffset;
    const std::uintptr_t typeInfoAddress =
        module->loadBase + clientLevelTypeInfoOffset;
    const std::uintptr_t typeNameAddress =
        module->loadBase + clientLevelTypeNameOffset;
    mSlotAddress = module->loadBase + clientLevelSubTickSlotOffset;

    const MemoryRegion* vtableRegion = findContainingRegion(
        *module,
        vtableAddressPoint - 2U * sizeof(std::uintptr_t),
        (static_cast<std::size_t>(clientLevelSubTickSlotIndex) + 3U) *
            sizeof(std::uintptr_t));
    if (vtableRegion == nullptr || vtableRegion->executable) {
        mFailureReason = "ClientLevel vtable is not readable non-executable memory";
        return false;
    }

    std::ptrdiff_t offsetToTop = 1;
    std::uintptr_t observedTypeInfo = 0;
    std::uintptr_t observedTypeName = 0;
    if (!readSignedPointer(
            *module,
            vtableAddressPoint - 2U * sizeof(std::uintptr_t),
            offsetToTop) ||
        offsetToTop != 0 ||
        !readPointer(
            *module,
            vtableAddressPoint - sizeof(std::uintptr_t),
            observedTypeInfo) ||
        observedTypeInfo != typeInfoAddress ||
        !readPointer(
            *module,
            typeInfoAddress + sizeof(std::uintptr_t),
            observedTypeName) ||
        observedTypeName != typeNameAddress ||
        !matchesCString(*module, typeNameAddress, clientLevelTypeName)) {
        mFailureReason = "ClientLevel Itanium RTTI validation failed";
        return false;
    }
    mRttiValidated = true;

    if (!readPointer(*module, mSlotAddress, mOriginalTarget) ||
        mOriginalTarget != module->loadBase + clientLevelSubTickTargetOffset ||
        !isExecutableAddress(*module, mOriginalTarget)) {
        mFailureReason = "ClientLevel slot 408 target mismatch";
        return false;
    }

    std::array<std::uint8_t, expectedFunctionPrefix.size()> observedPrefix{};
    if (!readBytes(
            *module,
            mOriginalTarget,
            observedPrefix.data(),
            observedPrefix.size())) {
        mFailureReason = "could not read ClientLevel::_subTick prefix";
        return false;
    }
    mObservedFunctionPrefix = bytesToHex(observedPrefix.data(), observedPrefix.size());
    if (observedPrefix != expectedFunctionPrefix) {
        mFailureReason = "ClientLevel::_subTick instruction prefix mismatch";
        return false;
    }
    mFunctionPrefixValidated = true;

    const long rawPageSize = ::sysconf(_SC_PAGESIZE);
    if (rawPageSize <= 0) {
        mFailureReason = "sysconf(_SC_PAGESIZE) failed";
        return false;
    }
    mPageSize = static_cast<std::size_t>(rawPageSize);
    if ((mPageSize & (mPageSize - 1U)) != 0U) {
        mFailureReason = "runtime page size is not a power of two";
        return false;
    }

    if (!queryMemoryProtection(
            mSlotAddress,
            mOriginalProtection,
            mOriginalPermissions) ||
        (mOriginalProtection & PROT_READ) == 0 ||
        (mOriginalProtection & PROT_EXEC) != 0) {
        mFailureReason = "unsafe ClientLevel vtable page permissions";
        return false;
    }

    mTargetValidated = true;
    return true;
}

bool ClientLevelTickHook::patchSlot() noexcept {
    const auto trampoline =
        reinterpret_cast<std::uintptr_t>(&aeronautics_clientlevel_tick_trampoline);
    if (readSlotPointer() != mOriginalTarget) {
        mFailureReason = "ClientLevel slot changed after validation";
        return false;
    }

    const bool written = writeSlotPointer(
        trampoline,
        mInstallWritableErrno,
        mInstallRestoreErrno);
    if (!written || readSlotPointer() != trampoline) {
        mRollbackAttempted = readSlotPointer() == trampoline;
        if (mRollbackAttempted) {
            mRollbackSucceeded = writeSlotPointer(
                mOriginalTarget,
                mRollbackWritableErrno,
                mRollbackRestoreErrno);
        }
        mFailureReason = "ClientLevel tick pointer replacement failed";
        return false;
    }

    mPatchEverInstalled = true;
    mPatchInstalled.store(true, std::memory_order_release);
    mMod.getLogger().info(
        "ClientLevel tick event source installed; slot=408; listeners={}; Minecraft code bytes modified=0",
        mEventBus.listenerCount());
    return true;
}

bool ClientLevelTickHook::restoreSlot() noexcept {
    mPatchRestoreAttempted = true;
    const auto trampoline =
        reinterpret_cast<std::uintptr_t>(&aeronautics_clientlevel_tick_trampoline);

    for (int attempt = 0; attempt < 3; ++attempt) {
        const std::uintptr_t current = readSlotPointer();
        if (current == mOriginalTarget) {
            mPatchInstalled.store(false, std::memory_order_release);
            mPatchRestoreSucceeded = true;
            return true;
        }
        if (current != trampoline) {
            mFailureReason = "ClientLevel slot changed by another writer";
            return false;
        }
        if (writeSlotPointer(
                mOriginalTarget,
                mUninstallWritableErrno,
                mUninstallRestoreErrno) &&
            readSlotPointer() == mOriginalTarget) {
            mPatchInstalled.store(false, std::memory_order_release);
            mPatchRestoreSucceeded = true;
            return true;
        }
    }

    mFailureReason = "original ClientLevel tick pointer could not be restored";
    return false;
}

bool ClientLevelTickHook::writeSlotPointer(
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

    __atomic_store_n(
        reinterpret_cast<std::uintptr_t*>(mSlotAddress),
        value,
        __ATOMIC_RELEASE);

    if (::mprotect(
            reinterpret_cast<void*>(page),
            mPageSize,
            mOriginalProtection) != 0) {
        restoreErrno = errno;
        return false;
    }
    return true;
}

std::uintptr_t ClientLevelTickHook::readSlotPointer() const noexcept {
    std::uintptr_t value = 0;
    if (mSlotAddress != 0) {
        std::memcpy(&value, reinterpret_cast<const void*>(mSlotAddress), sizeof(value));
    }
    return value;
}

void ClientLevelTickHook::writeStatus(std::string_view state) noexcept {
    std::ofstream output(mStatusPath, std::ios::trunc);
    if (!output) {
        return;
    }

    const auto trampoline =
        reinterpret_cast<std::uintptr_t>(&aeronautics_clientlevel_tick_trampoline);
    const std::uintptr_t current = readSlotPointer();

    output << "schema=1\n"
           << "state=" << state << '\n'
           << "minecraft_version=1.26.33.1\n"
           << "module_build_id=" << mModuleBuildId << '\n'
           << "module_file_size=" << mModuleFileSize << '\n'
           << "source=ClientLevel::_subTick\n"
           << "source_mode=permanent_typed_event_dispatch\n"
           << "event_type=ClientLevelTickEvent\n"
           << "event_bus_capacity=" << ClientLevelTickBus::capacity << '\n'
           << "event_bus_listener_count=" << mEventBus.listenerCount() << '\n'
           << "event_bus_published_events=" << mEventBus.publishedEvents() << '\n'
           << "event_bus_delivered_callbacks=" << mEventBus.deliveredCallbacks() << '\n'
           << "patch_mode=exact_vtable_data_pointer_swap_no_code_patch\n"
           << "minecraft_code_bytes_modified=0\n"
           << "heartbeat_gate=wait_indefinitely_then_require_8_seconds_continuous_activity\n"
           << "vtable_slot_index=" << clientLevelSubTickSlotIndex << '\n'
           << "vtable_slot_address_offset=0x" << std::hex
           << clientLevelSubTickSlotOffset << std::dec << '\n'
           << "original_target_offset=0x" << std::hex
           << clientLevelSubTickTargetOffset << std::dec << '\n'
           << "expected_function_prefix="
           << bytesToHex(expectedFunctionPrefix.data(), expectedFunctionPrefix.size()) << '\n'
           << "observed_function_prefix=" << mObservedFunctionPrefix << '\n'
           << "rtti_validated=" << (mRttiValidated ? "true" : "false") << '\n'
           << "function_prefix_validated="
           << (mFunctionPrefixValidated ? "true" : "false") << '\n'
           << "target_validated=" << (mTargetValidated ? "true" : "false") << '\n'
           << "original_page_permissions=" << mOriginalPermissions << '\n'
           << "patch_attempted=" << (mPatchAttempted ? "true" : "false") << '\n'
           << "patch_ever_installed=" << (mPatchEverInstalled ? "true" : "false") << '\n'
           << "patch_currently_installed="
           << (mPatchInstalled.load(std::memory_order_acquire) ? "true" : "false") << '\n'
           << "slot_current_state="
           << slotStateName(current, mOriginalTarget, trampoline) << '\n'
           << "patch_restore_attempted="
           << (mPatchRestoreAttempted ? "true" : "false") << '\n'
           << "patch_restore_succeeded="
           << (mPatchRestoreSucceeded ? "true" : "false") << '\n'
           << "heartbeat_calls_at_arm=" << mHeartbeatCallsAtArm << '\n'
           << "heartbeat_calls_at_first_activity="
           << mHeartbeatCallsAtFirstActivity << '\n'
           << "heartbeat_calls_at_patch=" << mHeartbeatCallsAtPatch << '\n'
           << "heartbeat_calls_current=" << mHeartbeat.callCount() << '\n'
           << "total_calls=" << mTotalCalls.load(std::memory_order_relaxed) << '\n'
           << "delivered_callbacks="
           << mDeliveredCallbacks.load(std::memory_order_relaxed) << '\n'
           << "first_thread_id="
           << mFirstThreadId.load(std::memory_order_relaxed) << '\n'
           << "other_thread_calls="
           << mOtherThreadCalls.load(std::memory_order_relaxed) << '\n'
           << "first_instance=0x" << std::hex
           << mFirstInstance.load(std::memory_order_relaxed) << std::dec << '\n'
           << "last_instance=0x" << std::hex
           << mLastInstance.load(std::memory_order_relaxed) << std::dec << '\n'
           << "instance_transitions="
           << mInstanceTransitions.load(std::memory_order_relaxed) << '\n'
           << "callbacks_in_flight="
           << mCallbacksInFlight.load(std::memory_order_relaxed) << '\n'
           << "install_mprotect_writable_errno=" << mInstallWritableErrno << '\n'
           << "install_mprotect_restore_errno=" << mInstallRestoreErrno << '\n'
           << "rollback_attempted=" << (mRollbackAttempted ? "true" : "false") << '\n'
           << "rollback_succeeded=" << (mRollbackSucceeded ? "true" : "false") << '\n'
           << "failure_reason="
           << (mFailureReason.empty() ? "none" : mFailureReason) << '\n';
}

void ClientLevelTickHook::clearActiveRegistration() noexcept {
    ClientLevelTickHook* expected = this;
    sActive.compare_exchange_strong(
        expected,
        nullptr,
        std::memory_order_acq_rel,
        std::memory_order_acquire);
}

}  // namespace aeronautics::bedrock
