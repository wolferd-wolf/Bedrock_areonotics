#include "bedrock/LevelRenderHook.hpp"

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
std::uintptr_t aeronautics_level_render_original_target = 0;
}

namespace aeronautics::bedrock {
namespace {

constexpr std::string_view expectedBuildId{"2e318db12824cadb2618754ab7c82fa96fb30659"};
constexpr std::uintmax_t expectedModuleFileSize = 349243744;
constexpr std::uintptr_t baseRendererTypeInfoOffset = 0x141d2620;
constexpr std::uintptr_t playerRendererVtableAddressPointOffset = 0x141d1b58;
constexpr std::uintptr_t playerRendererTypeInfoOffset = 0x141d26a8;
constexpr std::uintptr_t playerRendererTypeNameOffset = 0x02bad974;
constexpr std::string_view playerRendererTypeName{"19LevelRendererPlayer"};
constexpr std::uint32_t renderSlotIndex = 24;
constexpr std::uintptr_t renderSlotOffset = 0x141d1c18;
constexpr std::uintptr_t renderTargetOffset = 0x0bd6f97c;
constexpr std::uint64_t heartbeatStabilityMilliseconds = 8000;
constexpr std::uint64_t heartbeatMaximumStallMilliseconds = 2000;
constexpr std::uint64_t statusIntervalMilliseconds = 2000;
constexpr std::array<std::uint8_t, 16> expectedFunctionPrefix{
    0xec, 0x0f, 0x17, 0xfc, 0xeb, 0x2b, 0x01, 0x6d,
    0xe9, 0x23, 0x02, 0x6d, 0xfd, 0x7b, 0x03, 0xa9};

[[nodiscard]] const MemoryRegion* findRegion(
    const ModuleFingerprint& module,
    std::uintptr_t address,
    std::size_t size) noexcept {
    for (const auto& region : module.regions) {
        if (region.readable && address >= region.start && address < region.end &&
            size <= region.end - address) {
            return &region;
        }
    }
    return nullptr;
}

[[nodiscard]] bool readPointer(
    const ModuleFingerprint& module,
    std::uintptr_t address,
    std::uintptr_t& value) noexcept {
    if (findRegion(module, address, sizeof(value)) == nullptr) return false;
    std::memcpy(&value, reinterpret_cast<const void*>(address), sizeof(value));
    return true;
}

[[nodiscard]] bool readSignedPointer(
    const ModuleFingerprint& module,
    std::uintptr_t address,
    std::ptrdiff_t& value) noexcept {
    if (findRegion(module, address, sizeof(value)) == nullptr) return false;
    std::memcpy(&value, reinterpret_cast<const void*>(address), sizeof(value));
    return true;
}

[[nodiscard]] bool readBytes(
    const ModuleFingerprint& module,
    std::uintptr_t address,
    void* output,
    std::size_t size) noexcept {
    if (findRegion(module, address, size) == nullptr) return false;
    std::memcpy(output, reinterpret_cast<const void*>(address), size);
    return true;
}

[[nodiscard]] bool matchesCString(
    const ModuleFingerprint& module,
    std::uintptr_t address,
    std::string_view expected) noexcept {
    if (findRegion(module, address, expected.size() + 1U) == nullptr) return false;
    const auto* text = reinterpret_cast<const char*>(address);
    return std::memcmp(text, expected.data(), expected.size()) == 0 &&
        text[expected.size()] == '\0';
}

[[nodiscard]] std::string bytesToHex(const std::uint8_t* bytes, std::size_t count) {
    std::ostringstream stream;
    stream << std::hex << std::setfill('0');
    for (std::size_t i = 0; i < count; ++i) {
        if (i != 0) stream << ' ';
        stream << std::setw(2) << static_cast<unsigned int>(bytes[i]);
    }
    return stream.str();
}

[[nodiscard]] bool parseHex(std::string_view text, std::uintptr_t& value) noexcept {
    value = 0;
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value, 16);
    return result.ec == std::errc{} && result.ptr == text.data() + text.size();
}

[[nodiscard]] bool queryProtection(
    std::uintptr_t address,
    int& protection,
    std::string& permissions) {
    std::ifstream maps("/proc/self/maps");
    std::string line;
    while (std::getline(maps, line)) {
        std::istringstream stream{line};
        std::string range;
        std::string perms;
        if (!(stream >> range >> perms)) continue;
        const auto separator = range.find('-');
        if (separator == std::string::npos) continue;
        std::uintptr_t start = 0;
        std::uintptr_t end = 0;
        if (!parseHex(std::string_view(range).substr(0, separator), start) ||
            !parseHex(std::string_view(range).substr(separator + 1), end) ||
            address < start || address >= end) continue;
        protection = 0;
        if (!perms.empty() && perms[0] == 'r') protection |= PROT_READ;
        if (perms.size() > 1 && perms[1] == 'w') protection |= PROT_WRITE;
        if (perms.size() > 2 && perms[2] == 'x') protection |= PROT_EXEC;
        permissions = perms;
        return true;
    }
    return false;
}

[[nodiscard]] std::uint32_t currentThreadId() noexcept {
    const long raw = ::syscall(SYS_gettid);
    if (raw <= 0 || static_cast<unsigned long>(raw) >
            static_cast<unsigned long>(std::numeric_limits<std::uint32_t>::max())) return 0;
    return static_cast<std::uint32_t>(raw);
}

[[nodiscard]] const char* slotStateName(
    std::uintptr_t value,
    std::uintptr_t original,
    std::uintptr_t trampoline) noexcept {
    if (original == 0) return "uninitialized";
    if (value == original) return "original";
    if (value == trampoline) return "trampoline";
    return "other";
}

}  // namespace

std::atomic<LevelRenderHook*> LevelRenderHook::sActive{nullptr};

}  // namespace aeronautics::bedrock

extern "C" __attribute__((visibility("hidden"), used, noinline))
void aeronautics_level_render_record(
    void* renderer,
    void* context,
    const void* view,
    void* client) noexcept {
    aeronautics::bedrock::LevelRenderHook::recordActive(renderer, context, view, client);
}

extern "C" __attribute__((naked, visibility("hidden"), used))
void aeronautics_level_render_trampoline() noexcept {
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
        "ldp x0, x1, [sp, #0x00]\n"
        "ldp x2, x3, [sp, #0x10]\n"
        "bl aeronautics_level_render_record\n"
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
        "adrp x16, aeronautics_level_render_original_target\n"
        "ldr x16, [x16, :lo12:aeronautics_level_render_original_target]\n"
        "br x16\n");
}

namespace aeronautics::bedrock {

LevelRenderHook::LevelRenderHook(
    ll::mod::NativeMod& mod,
    HeartbeatHook& heartbeat,
    LevelRenderBus& eventBus) noexcept
    : mMod(mod), mHeartbeat(heartbeat), mEventBus(eventBus) {}

LevelRenderHook::~LevelRenderHook() { uninstall(); }

bool LevelRenderHook::install() {
    if (mWorker.joinable() || mPatchInstalled.load(std::memory_order_acquire)) return true;
    mStatusPath = mMod.getDataDir() / "level-render-source-status.txt";
    mFailureReason.clear();
    mStopRequested.store(false, std::memory_order_release);
    mPatchInstalled.store(false, std::memory_order_release);
    mCallbacksInFlight.store(0, std::memory_order_relaxed);
    mTotalCalls.store(0, std::memory_order_relaxed);
    mDeliveredCallbacks.store(0, std::memory_order_relaxed);
    mFirstThreadId.store(0, std::memory_order_relaxed);
    mOtherThreadCalls.store(0, std::memory_order_relaxed);
    mFirstRenderer.store(0, std::memory_order_relaxed);
    mLastRenderer.store(0, std::memory_order_relaxed);
    mRendererTransitions.store(0, std::memory_order_relaxed);
    mLastRenderContext.store(0, std::memory_order_relaxed);
    mLastViewRenderObject.store(0, std::memory_order_relaxed);
    mLastClientInstance.store(0, std::memory_order_relaxed);
    mTargetValidated = mRttiValidated = mFunctionPrefixValidated = false;
    mPatchAttempted = mPatchEverInstalled = false;
    mPatchRestoreAttempted = mPatchRestoreSucceeded = false;
    mRollbackAttempted = mRollbackSucceeded = false;
    mInstallWritableErrno = mInstallRestoreErrno = 0;
    mRollbackWritableErrno = mRollbackRestoreErrno = 0;
    mUninstallWritableErrno = mUninstallRestoreErrno = 0;
    mHeartbeatCallsAtArm = mHeartbeat.callCount();
    mHeartbeatCallsAtFirstActivity = mHeartbeatCallsAtPatch = 0;

    if (!validateTarget()) {
        writeStatus("validation_failed");
        mMod.getLogger().warn("LevelRendererPlayer render validation failed: {}", mFailureReason);
        return false;
    }
    aeronautics_level_render_original_target = mOriginalTarget;
    std::atomic_thread_fence(std::memory_order_release);
    LevelRenderHook* expected = nullptr;
    if (!sActive.compare_exchange_strong(expected, this, std::memory_order_acq_rel)) {
        mFailureReason = "another level render source is active";
        writeStatus("registration_failed");
        return false;
    }
    writeStatus("waiting_for_primary_heartbeat");
    try {
        mWorker = std::thread([this] { workerLoop(); });
    } catch (const std::system_error& error) {
        mFailureReason = std::string("level render worker failed: ") + error.what();
        clearActiveRegistration();
        writeStatus("worker_start_failed");
        return false;
    }
    mMod.getLogger().info(
        "Level render event source armed; class=LevelRendererPlayer; inherited render slot=24; waiting for stable primary heartbeat");
    return true;
}

void LevelRenderHook::uninstall() noexcept {
    mStopRequested.store(true, std::memory_order_release);
    if (mWorker.joinable()) mWorker.join();
    if (mPatchInstalled.load(std::memory_order_acquire)) (void)restoreSlot();
    using namespace std::chrono_literals;
    for (int i = 0; mCallbacksInFlight.load(std::memory_order_acquire) != 0 && i < 1000; ++i)
        std::this_thread::sleep_for(1ms);
    if (mPatchInstalled.load(std::memory_order_acquire)) {
        if (mFailureReason.empty()) mFailureReason = "derived renderer vtable still references trampoline";
        writeStatus("restore_failed_unsafe_to_unload");
        return;
    }
    writeStatus("stopped");
    clearActiveRegistration();
}

void LevelRenderHook::recordActive(
    void* renderer,
    void* context,
    const void* view,
    void* client) noexcept {
    if (auto* active = sActive.load(std::memory_order_acquire))
        active->record(renderer, context, view, client);
}

void LevelRenderHook::record(
    void* renderer,
    void* context,
    const void* view,
    void* client) noexcept {
    mCallbacksInFlight.fetch_add(1, std::memory_order_acq_rel);
    const auto sequence = mTotalCalls.fetch_add(1, std::memory_order_relaxed) + 1U;
    const auto threadId = currentThreadId();
    if (threadId != 0) {
        std::uint32_t expected = 0;
        if (!mFirstThreadId.compare_exchange_strong(expected, threadId, std::memory_order_acq_rel) &&
            expected != threadId) mOtherThreadCalls.fetch_add(1, std::memory_order_relaxed);
    }
    const auto rendererValue = reinterpret_cast<std::uintptr_t>(renderer);
    std::uintptr_t expectedRenderer = 0;
    mFirstRenderer.compare_exchange_strong(expectedRenderer, rendererValue, std::memory_order_acq_rel);
    const auto previous = mLastRenderer.exchange(rendererValue, std::memory_order_acq_rel);
    if (previous != 0 && rendererValue != 0 && previous != rendererValue)
        mRendererTransitions.fetch_add(1, std::memory_order_relaxed);
    mLastRenderContext.store(reinterpret_cast<std::uintptr_t>(context), std::memory_order_relaxed);
    mLastViewRenderObject.store(reinterpret_cast<std::uintptr_t>(view), std::memory_order_relaxed);
    mLastClientInstance.store(reinterpret_cast<std::uintptr_t>(client), std::memory_order_relaxed);
    const LevelRenderEvent event{renderer, context, view, client, sequence, threadId};
    mDeliveredCallbacks.fetch_add(mEventBus.publish(event), std::memory_order_relaxed);
    mCallbacksInFlight.fetch_sub(1, std::memory_order_acq_rel);
}

void LevelRenderHook::workerLoop() {
    using namespace std::chrono_literals;
    auto stabilityStart = std::chrono::steady_clock::time_point{};
    auto lastAdvance = std::chrono::steady_clock::time_point{};
    auto lastHeartbeat = mHeartbeatCallsAtArm;
    std::uint64_t loop = 0;
    while (!mStopRequested.load(std::memory_order_acquire)) {
        const auto now = std::chrono::steady_clock::now();
        const auto heartbeat = mHeartbeat.callCount();
        if (heartbeat > lastHeartbeat) {
            lastHeartbeat = heartbeat;
            lastAdvance = now;
            if (stabilityStart == std::chrono::steady_clock::time_point{}) {
                stabilityStart = now;
                mHeartbeatCallsAtFirstActivity = heartbeat;
            }
        }
        if (stabilityStart != std::chrono::steady_clock::time_point{}) {
            const auto stable = std::chrono::duration_cast<std::chrono::milliseconds>(now - stabilityStart).count();
            const auto stalled = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastAdvance).count();
            if (stalled > static_cast<std::int64_t>(heartbeatMaximumStallMilliseconds)) {
                stabilityStart = {};
                mHeartbeatCallsAtFirstActivity = 0;
            } else if (stable >= static_cast<std::int64_t>(heartbeatStabilityMilliseconds) &&
                       heartbeat > mHeartbeatCallsAtFirstActivity) break;
        }
        std::this_thread::sleep_for(100ms);
        if ((++loop % 20U) == 0U) writeStatus("waiting_for_primary_heartbeat");
    }
    if (mStopRequested.load(std::memory_order_acquire)) return;
    mHeartbeatCallsAtPatch = mHeartbeat.callCount();
    mPatchAttempted = true;
    if (!patchSlot()) {
        writeStatus("patch_failed");
        return;
    }
    while (!mStopRequested.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(statusIntervalMilliseconds));
        if (!mStopRequested.load(std::memory_order_acquire))
            writeStatus("publishing_level_render_events_from_derived_vtable");
    }
}

bool LevelRenderHook::validateTarget() {
    const auto module = inspectLoadedModule(CompatibilityProfile::moduleName);
    if (!module) { mFailureReason = "libminecraftpe.so is not loaded"; return false; }
    mModuleBuildId = module->buildId;
    mModuleFileSize = module->fileSize;
    mModuleLoadBase = module->loadBase;
    if (module->buildId != expectedBuildId || module->fileSize != expectedModuleFileSize) {
        mFailureReason = "Minecraft binary fingerprint mismatch";
        return false;
    }
    const auto addressPoint = module->loadBase + playerRendererVtableAddressPointOffset;
    const auto typeInfo = module->loadBase + playerRendererTypeInfoOffset;
    const auto typeName = module->loadBase + playerRendererTypeNameOffset;
    mSlotAddress = module->loadBase + renderSlotOffset;
    const auto* region = findRegion(*module, addressPoint - 16U, (renderSlotIndex + 3U) * sizeof(std::uintptr_t));
    if (region == nullptr || region->executable) {
        mFailureReason = "LevelRendererPlayer vtable is not readable non-executable memory";
        return false;
    }
    std::ptrdiff_t offsetToTop = 1;
    std::uintptr_t observedTypeInfo = 0;
    std::uintptr_t observedTypeName = 0;
    std::uintptr_t firstBaseType = 0;
    if (!readSignedPointer(*module, addressPoint - 16U, offsetToTop) || offsetToTop != 0 ||
        !readPointer(*module, addressPoint - 8U, observedTypeInfo) || observedTypeInfo != typeInfo ||
        !readPointer(*module, typeInfo + 8U, observedTypeName) || observedTypeName != typeName ||
        !matchesCString(*module, typeName, playerRendererTypeName) ||
        !readPointer(*module, typeInfo + 24U, firstBaseType) || firstBaseType != module->loadBase + baseRendererTypeInfoOffset) {
        mFailureReason = "LevelRendererPlayer RTTI/base validation failed";
        return false;
    }
    mRttiValidated = true;
    if (!readPointer(*module, mSlotAddress, mOriginalTarget) ||
        mOriginalTarget != module->loadBase + renderTargetOffset ||
        !isExecutableAddress(*module, mOriginalTarget)) {
        mFailureReason = "LevelRendererPlayer inherited slot 24 target mismatch";
        return false;
    }
    std::array<std::uint8_t, expectedFunctionPrefix.size()> observed{};
    if (!readBytes(*module, mOriginalTarget, observed.data(), observed.size())) {
        mFailureReason = "could not read render function prefix";
        return false;
    }
    mObservedFunctionPrefix = bytesToHex(observed.data(), observed.size());
    if (observed != expectedFunctionPrefix) {
        mFailureReason = "render instruction prefix mismatch";
        return false;
    }
    mFunctionPrefixValidated = true;
    const long pageSize = ::sysconf(_SC_PAGESIZE);
    if (pageSize <= 0) { mFailureReason = "invalid page size"; return false; }
    mPageSize = static_cast<std::size_t>(pageSize);
    if (!queryProtection(mSlotAddress, mOriginalProtection, mOriginalPermissions) ||
        (mOriginalProtection & PROT_READ) == 0 || (mOriginalProtection & PROT_EXEC) != 0) {
        mFailureReason = "unsafe derived vtable page permissions";
        return false;
    }
    mTargetValidated = true;
    return true;
}

bool LevelRenderHook::patchSlot() noexcept {
    const auto trampoline = reinterpret_cast<std::uintptr_t>(&aeronautics_level_render_trampoline);
    if (readSlotPointer() != mOriginalTarget) {
        mFailureReason = "derived render slot changed after validation";
        return false;
    }
    if (!writeSlotPointer(trampoline, mInstallWritableErrno, mInstallRestoreErrno) ||
        readSlotPointer() != trampoline) {
        mRollbackAttempted = readSlotPointer() == trampoline;
        if (mRollbackAttempted)
            mRollbackSucceeded = writeSlotPointer(mOriginalTarget, mRollbackWritableErrno, mRollbackRestoreErrno);
        mFailureReason = "derived render slot replacement failed";
        return false;
    }
    mPatchEverInstalled = true;
    mPatchInstalled.store(true, std::memory_order_release);
    mMod.getLogger().info(
        "Level render event source installed; target=LevelRendererPlayer inherited LevelRendererCamera::render; slot=24; Minecraft code bytes modified=0");
    return true;
}

bool LevelRenderHook::restoreSlot() noexcept {
    mPatchRestoreAttempted = true;
    const auto trampoline = reinterpret_cast<std::uintptr_t>(&aeronautics_level_render_trampoline);
    for (int i = 0; i < 3; ++i) {
        const auto current = readSlotPointer();
        if (current == mOriginalTarget) {
            mPatchInstalled.store(false, std::memory_order_release);
            mPatchRestoreSucceeded = true;
            return true;
        }
        if (current != trampoline) {
            mFailureReason = "derived render slot changed by another writer";
            return false;
        }
        if (writeSlotPointer(mOriginalTarget, mUninstallWritableErrno, mUninstallRestoreErrno) &&
            readSlotPointer() == mOriginalTarget) {
            mPatchInstalled.store(false, std::memory_order_release);
            mPatchRestoreSucceeded = true;
            return true;
        }
    }
    mFailureReason = "original derived render pointer could not be restored";
    return false;
}

bool LevelRenderHook::writeSlotPointer(
    std::uintptr_t value,
    int& writableErrno,
    int& restoreErrno) noexcept {
    writableErrno = restoreErrno = 0;
    const auto page = mSlotAddress & ~(static_cast<std::uintptr_t>(mPageSize) - 1U);
    if (::mprotect(reinterpret_cast<void*>(page), mPageSize, mOriginalProtection | PROT_WRITE) != 0) {
        writableErrno = errno;
        return false;
    }
    __atomic_store_n(reinterpret_cast<std::uintptr_t*>(mSlotAddress), value, __ATOMIC_RELEASE);
    if (::mprotect(reinterpret_cast<void*>(page), mPageSize, mOriginalProtection) != 0) {
        restoreErrno = errno;
        return false;
    }
    return true;
}

std::uintptr_t LevelRenderHook::readSlotPointer() const noexcept {
    std::uintptr_t value = 0;
    if (mSlotAddress != 0)
        std::memcpy(&value, reinterpret_cast<const void*>(mSlotAddress), sizeof(value));
    return value;
}

void LevelRenderHook::writeStatus(std::string_view state) noexcept {
    std::ofstream output(mStatusPath, std::ios::trunc);
    if (!output) return;
    const auto trampoline = reinterpret_cast<std::uintptr_t>(&aeronautics_level_render_trampoline);
    const auto current = readSlotPointer();
    output << "schema=2\n"
           << "state=" << state << '\n'
           << "minecraft_version=1.26.33.1\n"
           << "module_build_id=" << mModuleBuildId << '\n'
           << "module_file_size=" << mModuleFileSize << '\n'
           << "module_load_base=0x" << std::hex << mModuleLoadBase << std::dec << '\n'
           << "source=LevelRendererPlayer inherited LevelRendererCamera::render\n"
           << "source_mode=derived_primary_vtable_typed_level_render_event_dispatch\n"
           << "event_type=LevelRenderEvent\n"
           << "event_bus_capacity=" << LevelRenderBus::capacity << '\n'
           << "event_bus_listener_count=" << mEventBus.listenerCount() << '\n'
           << "event_bus_published_events=" << mEventBus.publishedEvents() << '\n'
           << "event_bus_delivered_callbacks=" << mEventBus.deliveredCallbacks() << '\n'
           << "discovery_method=itanium_rtti_derived_vtable_runtime_dispatch_correction\n"
           << "base_class=19LevelRendererCamera\n"
           << "runtime_candidate=19LevelRendererPlayer\n"
           << "shadow_candidate=25LevelRendererShadowCamera\n"
           << "previous_base_slot_test_total_calls=0\n"
           << "patch_mode=exact_derived_vtable_data_pointer_swap_no_code_patch\n"
           << "minecraft_code_bytes_modified=0\n"
           << "geometry_submission=disabled_in_derived_callback_validation_build\n"
           << "vtable_address_point_offset=0x" << std::hex << playerRendererVtableAddressPointOffset << std::dec << '\n'
           << "rtti_typeinfo_offset=0x" << std::hex << playerRendererTypeInfoOffset << std::dec << '\n'
           << "rtti_type_name_offset=0x" << std::hex << playerRendererTypeNameOffset << std::dec << '\n'
           << "rtti_type_name=" << playerRendererTypeName << '\n'
           << "vtable_slot_index=" << renderSlotIndex << '\n'
           << "vtable_slot_address_offset=0x" << std::hex << renderSlotOffset << std::dec << '\n'
           << "original_target_offset=0x" << std::hex << renderTargetOffset << std::dec << '\n'
           << "expected_function_prefix=" << bytesToHex(expectedFunctionPrefix.data(), expectedFunctionPrefix.size()) << '\n'
           << "observed_function_prefix=" << mObservedFunctionPrefix << '\n'
           << "rtti_validated=" << (mRttiValidated ? "true" : "false") << '\n'
           << "function_prefix_validated=" << (mFunctionPrefixValidated ? "true" : "false") << '\n'
           << "target_validated=" << (mTargetValidated ? "true" : "false") << '\n'
           << "original_page_permissions=" << mOriginalPermissions << '\n'
           << "patch_attempted=" << (mPatchAttempted ? "true" : "false") << '\n'
           << "patch_ever_installed=" << (mPatchEverInstalled ? "true" : "false") << '\n'
           << "patch_currently_installed=" << (mPatchInstalled.load(std::memory_order_acquire) ? "true" : "false") << '\n'
           << "slot_current_state=" << slotStateName(current, mOriginalTarget, trampoline) << '\n'
           << "patch_restore_attempted=" << (mPatchRestoreAttempted ? "true" : "false") << '\n'
           << "patch_restore_succeeded=" << (mPatchRestoreSucceeded ? "true" : "false") << '\n'
           << "heartbeat_calls_at_arm=" << mHeartbeatCallsAtArm << '\n'
           << "heartbeat_calls_at_first_activity=" << mHeartbeatCallsAtFirstActivity << '\n'
           << "heartbeat_calls_at_patch=" << mHeartbeatCallsAtPatch << '\n'
           << "heartbeat_calls_current=" << mHeartbeat.callCount() << '\n'
           << "total_calls=" << mTotalCalls.load(std::memory_order_relaxed) << '\n'
           << "delivered_callbacks=" << mDeliveredCallbacks.load(std::memory_order_relaxed) << '\n'
           << "first_thread_id=" << mFirstThreadId.load(std::memory_order_relaxed) << '\n'
           << "other_thread_calls=" << mOtherThreadCalls.load(std::memory_order_relaxed) << '\n'
           << "first_renderer=0x" << std::hex << mFirstRenderer.load(std::memory_order_relaxed) << '\n'
           << "last_renderer=0x" << mLastRenderer.load(std::memory_order_relaxed) << std::dec << '\n'
           << "renderer_transitions=" << mRendererTransitions.load(std::memory_order_relaxed) << '\n'
           << "last_render_context=0x" << std::hex << mLastRenderContext.load(std::memory_order_relaxed) << '\n'
           << "last_view_render_object=0x" << mLastViewRenderObject.load(std::memory_order_relaxed) << '\n'
           << "last_client_instance=0x" << mLastClientInstance.load(std::memory_order_relaxed) << std::dec << '\n'
           << "callbacks_in_flight=" << mCallbacksInFlight.load(std::memory_order_relaxed) << '\n'
           << "install_mprotect_writable_errno=" << mInstallWritableErrno << '\n'
           << "install_mprotect_restore_errno=" << mInstallRestoreErrno << '\n'
           << "rollback_attempted=" << (mRollbackAttempted ? "true" : "false") << '\n'
           << "rollback_succeeded=" << (mRollbackSucceeded ? "true" : "false") << '\n'
           << "failure_reason=" << (mFailureReason.empty() ? "none" : mFailureReason) << '\n';
}

void LevelRenderHook::clearActiveRegistration() noexcept {
    LevelRenderHook* expected = this;
    sActive.compare_exchange_strong(expected, nullptr, std::memory_order_acq_rel);
}

}  // namespace aeronautics::bedrock
