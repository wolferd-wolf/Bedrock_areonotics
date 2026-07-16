#include "bedrock/LevelRenderHook.hpp"

#include "bedrock/CompatibilityProfile.hpp"
#include "bedrock/HeartbeatHook.hpp"
#include "physics/PhysicsScheduler.hpp"
#include "render/EglDiagnosticCubeOverlay.hpp"
#include "render/RenderInterpolation.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <dlfcn.h>
#include <fstream>
#include <iomanip>
#include <limits>
#include <sstream>
#include <string>

#include <sys/syscall.h>
#include <unistd.h>

namespace aeronautics::bedrock {
namespace {

constexpr std::string_view expectedBuildId{"2e318db12824cadb2618754ab7c82fa96fb30659"};
constexpr std::uintmax_t expectedModuleFileSize = 349243744;
constexpr std::uintptr_t minecraftRenderOffset = 0x0bd6f97c;
constexpr std::array<std::uint8_t, 16> expectedPrefix{
    0xec, 0x0f, 0x17, 0xfc, 0xeb, 0x2b, 0x01, 0x6d,
    0xe9, 0x23, 0x02, 0x6d, 0xfd, 0x7b, 0x03, 0xa9};
constexpr std::int64_t activePhysicsWindowNanoseconds = 250'000'000;
constexpr std::int64_t activeCameraWindowNanoseconds = 250'000'000;

// Layout generated for current client headers. The scan fallback below prevents this
// fixed layout from being the only camera source.
constexpr std::size_t contextCameraTargetOffset = 216;
constexpr std::size_t contextCameraPositionOffset = 228;
constexpr std::size_t viewCameraPositionOffset = 0;
constexpr std::size_t viewCameraTargetOffset = 12;
constexpr std::size_t contextScanBegin = 128;
constexpr std::size_t contextScanEnd = 384;

using MinecraftRenderFn = void (*)(void*, void*, const void*, void*);
using VulkanPresentFn = std::int32_t (*)(void*, const void*);
using EglSwapFn = std::uint32_t (*)(void*, void*);
using GetVec3ReferenceFn = const float* (*)(const void*);

MinecraftRenderFn gOriginalMinecraftRender = nullptr;
VulkanPresentFn gOriginalVulkanPresent = nullptr;
EglSwapFn gOriginalEglSwap = nullptr;
GetVec3ReferenceFn gGetCameraPosition = nullptr;
GetVec3ReferenceFn gGetCameraTargetPosition = nullptr;
aeronautics::render::EglDiagnosticCubeOverlay gCubeOverlay;

// 0=none, 1=context fields, 2=context getters, 3=view fields,
// 4=context scan target-then-position, 5=context scan position-then-target.
std::atomic<int> gSelectedCameraSource{0};
std::atomic<std::uint32_t> gSelectedCameraOffset{0};
std::atomic<std::uint64_t> gContextFieldValid{0};
std::atomic<std::uint64_t> gContextFieldInvalid{0};
std::atomic<std::uint64_t> gContextGetterValid{0};
std::atomic<std::uint64_t> gContextGetterInvalid{0};
std::atomic<std::uint64_t> gViewFieldValid{0};
std::atomic<std::uint64_t> gViewFieldInvalid{0};
std::atomic<std::uint64_t> gScanCandidatesAccepted{0};
std::atomic<std::uint64_t> gScanCandidatesRejected{0};
std::array<std::atomic<float>, 3> gRawContextPosition{};
std::array<std::atomic<float>, 3> gRawContextTarget{};
std::array<std::atomic<float>, 3> gRawViewPosition{};
std::array<std::atomic<float>, 3> gRawViewTarget{};

int gPendingScanSource = 0;
std::uint32_t gPendingScanOffset = 0;
unsigned gPendingScanFrames = 0;

struct CameraCandidate final {
    render::Vec3f position{};
    render::Vec3f target{};
    int source{};
    std::uint32_t offset{};
    bool valid{};
};

[[nodiscard]] std::uint32_t currentThreadId() noexcept {
    const long value = ::syscall(SYS_gettid);
    return value > 0 ? static_cast<std::uint32_t>(value) : 0U;
}

[[nodiscard]] std::int64_t steadyNanosecondsNow() noexcept {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

[[nodiscard]] render::Vec3f readVec3(const void* base, std::size_t offset) noexcept {
    render::Vec3f value{};
    if (base != nullptr) {
        const auto* bytes = static_cast<const std::byte*>(base);
        std::memcpy(&value, bytes + offset, sizeof(value));
    }
    return value;
}

void storeRaw(
    std::array<std::atomic<float>, 3>& destination,
    render::Vec3f value) noexcept {
    destination[0].store(value.x, std::memory_order_relaxed);
    destination[1].store(value.y, std::memory_order_relaxed);
    destination[2].store(value.z, std::memory_order_relaxed);
}

[[nodiscard]] bool plausibleCamera(
    render::Vec3f position,
    render::Vec3f target) noexcept {
    if (!render::validCameraSample(position, target)) {
        return false;
    }
    const float distanceSquared = render::lengthSquared(target - position);
    if (distanceSquared < 0.04F || distanceSquared > 4096.0F) {
        return false;
    }
    if (std::abs(position.y) > 4096.0F || std::abs(target.y) > 4096.0F) {
        return false;
    }
    // Reject color-like or zero-filled pairs. Normal overworld cameras almost
    // always have at least one coordinate outside this tiny interval.
    const float magnitude = std::max({
        std::abs(position.x), std::abs(position.y), std::abs(position.z),
        std::abs(target.x), std::abs(target.y), std::abs(target.z)});
    return magnitude > 2.0F;
}

[[nodiscard]] CameraCandidate contextFieldCandidate(const void* context) noexcept {
    CameraCandidate result{};
    result.source = 1;
    result.offset = static_cast<std::uint32_t>(contextCameraTargetOffset);
    if (context == nullptr) {
        return result;
    }
    result.target = readVec3(context, contextCameraTargetOffset);
    result.position = readVec3(context, contextCameraPositionOffset);
    storeRaw(gRawContextPosition, result.position);
    storeRaw(gRawContextTarget, result.target);
    result.valid = plausibleCamera(result.position, result.target);
    return result;
}

[[nodiscard]] CameraCandidate contextGetterCandidate(const void* context) noexcept {
    CameraCandidate result{};
    result.source = 2;
    if (context == nullptr || gGetCameraPosition == nullptr) {
        return result;
    }
    const float* position = gGetCameraPosition(context);
    const float* target = gGetCameraTargetPosition != nullptr
        ? gGetCameraTargetPosition(context)
        : nullptr;
    if (position == nullptr) {
        return result;
    }
    std::memcpy(&result.position, position, sizeof(result.position));
    result.target = target != nullptr
        ? render::Vec3f{target[0], target[1], target[2]}
        : readVec3(context, contextCameraTargetOffset);
    result.valid = plausibleCamera(result.position, result.target);
    return result;
}

[[nodiscard]] CameraCandidate viewFieldCandidate(const void* view) noexcept {
    CameraCandidate result{};
    result.source = 3;
    result.offset = static_cast<std::uint32_t>(viewCameraPositionOffset);
    if (view == nullptr) {
        return result;
    }
    result.position = readVec3(view, viewCameraPositionOffset);
    result.target = readVec3(view, viewCameraTargetOffset);
    storeRaw(gRawViewPosition, result.position);
    storeRaw(gRawViewTarget, result.target);
    result.valid = plausibleCamera(result.position, result.target);
    return result;
}

[[nodiscard]] CameraCandidate scanContextCandidate(const void* context) noexcept {
    CameraCandidate best{};
    float bestScore = -std::numeric_limits<float>::infinity();
    if (context == nullptr) {
        return best;
    }

    for (std::size_t offset = contextScanBegin;
         offset + (2U * sizeof(render::Vec3f)) <= contextScanEnd;
         offset += alignof(float)) {
        const render::Vec3f first = readVec3(context, offset);
        const render::Vec3f second = readVec3(context, offset + sizeof(render::Vec3f));
        const std::array<CameraCandidate, 2> candidates{{
            {second, first, 4, static_cast<std::uint32_t>(offset),
             plausibleCamera(second, first)},
            {first, second, 5, static_cast<std::uint32_t>(offset),
             plausibleCamera(first, second)},
        }};

        for (const CameraCandidate& candidate : candidates) {
            if (!candidate.valid) {
                gScanCandidatesRejected.fetch_add(1, std::memory_order_relaxed);
                continue;
            }
            const float distance = std::sqrt(
                render::lengthSquared(candidate.target - candidate.position));
            const float coordinateMagnitude = std::max({
                std::abs(candidate.position.x), std::abs(candidate.position.y),
                std::abs(candidate.position.z)});
            const float score = coordinateMagnitude - std::abs(distance - 1.0F) * 0.25F;
            if (score > bestScore) {
                best = candidate;
                bestScore = score;
            }
        }
    }
    if (best.valid) {
        gScanCandidatesAccepted.fetch_add(1, std::memory_order_relaxed);
    }
    return best;
}

[[nodiscard]] CameraCandidate candidateForSelectedSource(
    int source,
    std::uint32_t offset,
    const void* context,
    const void* view) noexcept {
    if (source == 1) return contextFieldCandidate(context);
    if (source == 2) return contextGetterCandidate(context);
    if (source == 3) return viewFieldCandidate(view);
    if ((source == 4 || source == 5) && context != nullptr) {
        const render::Vec3f first = readVec3(context, offset);
        const render::Vec3f second = readVec3(context, offset + sizeof(render::Vec3f));
        CameraCandidate result{};
        result.source = source;
        result.offset = offset;
        if (source == 4) {
            result.target = first;
            result.position = second;
        } else {
            result.position = first;
            result.target = second;
        }
        result.valid = plausibleCamera(result.position, result.target);
        return result;
    }
    return {};
}

[[nodiscard]] bool publishBestCamera(
    const void* context,
    const void* view,
    std::int64_t timestamp) noexcept {
    const int selectedSource = gSelectedCameraSource.load(std::memory_order_acquire);
    const std::uint32_t selectedOffset =
        gSelectedCameraOffset.load(std::memory_order_relaxed);
    if (selectedSource != 0) {
        const CameraCandidate selected = candidateForSelectedSource(
            selectedSource, selectedOffset, context, view);
        if (selected.valid &&
            gCubeOverlay.publishCamera(selected.position, selected.target, timestamp)) {
            return true;
        }
        gSelectedCameraSource.store(0, std::memory_order_release);
        gSelectedCameraOffset.store(0, std::memory_order_relaxed);
    }

    const CameraCandidate contextFields = contextFieldCandidate(context);
    if (contextFields.valid) {
        gContextFieldValid.fetch_add(1, std::memory_order_relaxed);
        gSelectedCameraSource.store(1, std::memory_order_release);
        return gCubeOverlay.publishCamera(
            contextFields.position, contextFields.target, timestamp);
    }
    gContextFieldInvalid.fetch_add(1, std::memory_order_relaxed);

    const CameraCandidate contextGetter = contextGetterCandidate(context);
    if (contextGetter.valid) {
        gContextGetterValid.fetch_add(1, std::memory_order_relaxed);
        gSelectedCameraSource.store(2, std::memory_order_release);
        return gCubeOverlay.publishCamera(
            contextGetter.position, contextGetter.target, timestamp);
    }
    gContextGetterInvalid.fetch_add(1, std::memory_order_relaxed);

    const CameraCandidate viewFields = viewFieldCandidate(view);
    if (viewFields.valid) {
        gViewFieldValid.fetch_add(1, std::memory_order_relaxed);
        gSelectedCameraSource.store(3, std::memory_order_release);
        return gCubeOverlay.publishCamera(viewFields.position, viewFields.target, timestamp);
    }
    gViewFieldInvalid.fetch_add(1, std::memory_order_relaxed);

    const CameraCandidate scanned = scanContextCandidate(context);
    if (scanned.valid) {
        if (gPendingScanSource == scanned.source &&
            gPendingScanOffset == scanned.offset) {
            ++gPendingScanFrames;
        } else {
            gPendingScanSource = scanned.source;
            gPendingScanOffset = scanned.offset;
            gPendingScanFrames = 1;
        }
        if (gPendingScanFrames >= 8U) {
            gSelectedCameraOffset.store(scanned.offset, std::memory_order_relaxed);
            gSelectedCameraSource.store(scanned.source, std::memory_order_release);
            return gCubeOverlay.publishCamera(scanned.position, scanned.target, timestamp);
        }
    } else {
        gPendingScanSource = 0;
        gPendingScanOffset = 0;
        gPendingScanFrames = 0;
    }

    (void)gCubeOverlay.publishCamera({}, {}, timestamp);
    return false;
}

[[nodiscard]] const char* cameraSourceName(int source) noexcept {
    switch (source) {
    case 1: return "base_actor_render_context_fields";
    case 2: return "base_actor_render_context_getters";
    case 3: return "view_render_object_fields";
    case 4: return "context_scan_target_then_position";
    case 5: return "context_scan_position_then_target";
    default: return "none";
    }
}

void resetCameraDiscovery() noexcept {
    gSelectedCameraSource.store(0, std::memory_order_relaxed);
    gSelectedCameraOffset.store(0, std::memory_order_relaxed);
    gContextFieldValid.store(0, std::memory_order_relaxed);
    gContextFieldInvalid.store(0, std::memory_order_relaxed);
    gContextGetterValid.store(0, std::memory_order_relaxed);
    gContextGetterInvalid.store(0, std::memory_order_relaxed);
    gViewFieldValid.store(0, std::memory_order_relaxed);
    gViewFieldInvalid.store(0, std::memory_order_relaxed);
    gScanCandidatesAccepted.store(0, std::memory_order_relaxed);
    gScanCandidatesRejected.store(0, std::memory_order_relaxed);
    for (auto* values : {&gRawContextPosition, &gRawContextTarget,
                         &gRawViewPosition, &gRawViewTarget}) {
        for (auto& value : *values) value.store(0.0F, std::memory_order_relaxed);
    }
    gPendingScanSource = 0;
    gPendingScanOffset = 0;
    gPendingScanFrames = 0;
}

void minecraftRenderDetour(void* renderer, void* context, const void* view, void* client) {
    LevelRenderHook::recordMinecraftRender(renderer, context, view, client);
    if (gOriginalMinecraftRender != nullptr) {
        gOriginalMinecraftRender(renderer, context, view, client);
    }
}

std::int32_t vulkanPresentDetour(void* queue, const void* presentInfo) {
    LevelRenderHook::recordPresent(true);
    return gOriginalVulkanPresent != nullptr
        ? gOriginalVulkanPresent(queue, presentInfo) : -3;
}

std::uint32_t eglSwapDetour(void* display, void* surface) {
    LevelRenderHook::recordPresent(false);
    LevelRenderHook::drawWorldSpaceCube();
    return gOriginalEglSwap != nullptr ? gOriginalEglSwap(display, surface) : 0U;
}

[[nodiscard]] void* resolveSymbol(const char* library, const char* symbol) noexcept {
    if (void* direct = ::dlsym(RTLD_DEFAULT, symbol); direct != nullptr) return direct;
    void* handle = ::dlopen(library, RTLD_NOW | RTLD_LOCAL);
    return handle != nullptr ? ::dlsym(handle, symbol) : nullptr;
}

}  // namespace

std::atomic<LevelRenderHook*> LevelRenderHook::sActive{nullptr};

LevelRenderHook::LevelRenderHook(
    ll::mod::NativeMod& mod,
    HeartbeatHook& heartbeat,
    LevelRenderBus& eventBus,
    physics::PhysicsScheduler& physicsScheduler) noexcept
    : mMod(mod),
      mHeartbeat(heartbeat),
      mEventBus(eventBus),
      mPhysicsScheduler(physicsScheduler) {}

LevelRenderHook::~LevelRenderHook() { uninstall(); }

bool LevelRenderHook::install() {
    if (mWorker.joinable()) return true;

    mStatusPath = mMod.getDataDir() / "world-space-cube-render-status.txt";
    mStopRequested.store(false, std::memory_order_release);
    mRestoreSucceeded.store(false, std::memory_order_release);
    mCallbacksInFlight.store(0, std::memory_order_relaxed);
    mMinecraftCalls.store(0, std::memory_order_relaxed);
    mPresentCalls.store(0, std::memory_order_relaxed);
    mVulkanPresentCalls.store(0, std::memory_order_relaxed);
    mEglPresentCalls.store(0, std::memory_order_relaxed);
    mWorldDrawAttempts.store(0, std::memory_order_relaxed);
    mWorldDrawSuccesses.store(0, std::memory_order_relaxed);
    mWorldDrawFailures.store(0, std::memory_order_relaxed);
    mSuppressedNoActiveWorld.store(0, std::memory_order_relaxed);
    mSuppressedStalePhysics.store(0, std::memory_order_relaxed);
    mSuppressedNoCamera.store(0, std::memory_order_relaxed);
    mLastMinecraftRenderNanoseconds.store(0, std::memory_order_relaxed);
    mLastWorldGeneration.store(0, std::memory_order_relaxed);
    mMinecraftThreadId.store(0, std::memory_order_relaxed);
    mPresentThreadId.store(0, std::memory_order_relaxed);
    mOtherMinecraftThreadCalls.store(0, std::memory_order_relaxed);
    mOtherPresentThreadCalls.store(0, std::memory_order_relaxed);
    mFirstRenderer.store(0, std::memory_order_relaxed);
    mLastContext.store(0, std::memory_order_relaxed);
    mLastView.store(0, std::memory_order_relaxed);
    mLastClient.store(0, std::memory_order_relaxed);
    mFingerprintValidated.store(false, std::memory_order_relaxed);
    mPrefixValidated.store(false, std::memory_order_relaxed);
    mFailureReason.clear();
    gGetCameraPosition = nullptr;
    gGetCameraTargetPosition = nullptr;
    resetCameraDiscovery();
    gCubeOverlay.beginSession();

    LevelRenderHook* expected = nullptr;
    if (!sActive.compare_exchange_strong(expected, this, std::memory_order_acq_rel)) {
        mFailureReason = "another world-space cube renderer is active";
        writeStatus("registration_failed");
        return false;
    }

    writeStatus("waiting_for_stable_heartbeat");
    try {
        mWorker = std::thread(&LevelRenderHook::workerLoop, this);
    } catch (...) {
        sActive.store(nullptr, std::memory_order_release);
        mFailureReason = "failed to start world-space cube worker";
        writeStatus("worker_start_failed");
        return false;
    }
    return true;
}

void LevelRenderHook::workerLoop() noexcept {
    using namespace std::chrono_literals;
    std::uint64_t previous = mHeartbeat.callCount();
    unsigned stableSeconds = 0;
    while (!mStopRequested.load(std::memory_order_acquire) && stableSeconds < 8U) {
        std::this_thread::sleep_for(1s);
        const auto current = mHeartbeat.callCount();
        stableSeconds = current > previous ? stableSeconds + 1U : 0U;
        previous = current;
    }
    if (mStopRequested.load(std::memory_order_acquire)) return;
    if (!installHooks()) {
        writeStatus("hook_install_failed");
        return;
    }
    writeStatus("running_world_space_cube");
    while (!mStopRequested.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(2s);
        writeStatus("running_world_space_cube");
    }
}

bool LevelRenderHook::installHooks() noexcept {
    const auto module = inspectLoadedModule(CompatibilityProfile::moduleName);
    if (!module || module->buildId != expectedBuildId ||
        module->fileSize != expectedModuleFileSize) {
        mFailureReason = "Minecraft binary fingerprint mismatch";
        return false;
    }
    mFingerprintValidated.store(true, std::memory_order_release);

    const auto targetAddress = module->loadBase + minecraftRenderOffset;
    const std::string observed =
        readInstructionPrefix(*module, targetAddress, expectedPrefix.size());
    std::ostringstream expected;
    expected << std::hex << std::setfill('0');
    for (std::size_t index = 0; index < expectedPrefix.size(); ++index) {
        if (index != 0) expected << ' ';
        expected << std::setw(2) << static_cast<unsigned>(expectedPrefix[index]);
    }
    if (observed != expected.str()) {
        mFailureReason = "Minecraft render function prefix mismatch";
        return false;
    }
    mPrefixValidated.store(true, std::memory_order_release);

    gGetCameraPosition = reinterpret_cast<GetVec3ReferenceFn>(resolveSymbol(
        "libminecraftpe.so",
        "_ZNK22BaseActorRenderContext17getCameraPositionEv"));
    gGetCameraTargetPosition = reinterpret_cast<GetVec3ReferenceFn>(resolveSymbol(
        "libminecraftpe.so",
        "_ZNK22BaseActorRenderContext23getCameraTargetPositionEv"));

    mMinecraftHook = pl::memory::HookHandle(
        reinterpret_cast<pl::memory::FuncPtr>(targetAddress),
        reinterpret_cast<pl::memory::FuncPtr>(&minecraftRenderDetour),
        &mMinecraftOriginalStorage,
        pl::memory::HookPriority::Low);
    if (!mMinecraftHook.installed() || mMinecraftOriginalStorage == nullptr) {
        mMinecraftHook.reset();
        mFailureReason = "preloader hook failed for Minecraft render target";
        return false;
    }
    gOriginalMinecraftRender =
        reinterpret_cast<MinecraftRenderFn>(mMinecraftOriginalStorage);

    if (void* symbol = resolveSymbol("libvulkan.so", "vkQueuePresentKHR");
        symbol != nullptr) {
        mVulkanHook = pl::memory::HookHandle(
            reinterpret_cast<pl::memory::FuncPtr>(symbol),
            reinterpret_cast<pl::memory::FuncPtr>(&vulkanPresentDetour),
            &mVulkanOriginalStorage,
            pl::memory::HookPriority::Low);
        if (mVulkanHook.installed() && mVulkanOriginalStorage != nullptr) {
            gOriginalVulkanPresent =
                reinterpret_cast<VulkanPresentFn>(mVulkanOriginalStorage);
        } else {
            mVulkanHook.reset();
        }
    }

    if (void* symbol = resolveSymbol("libEGL.so", "eglSwapBuffers");
        symbol != nullptr) {
        mEglHook = pl::memory::HookHandle(
            reinterpret_cast<pl::memory::FuncPtr>(symbol),
            reinterpret_cast<pl::memory::FuncPtr>(&eglSwapDetour),
            &mEglOriginalStorage,
            pl::memory::HookPriority::Low);
        if (mEglHook.installed() && mEglOriginalStorage != nullptr) {
            gOriginalEglSwap = reinterpret_cast<EglSwapFn>(mEglOriginalStorage);
        } else {
            mEglHook.reset();
        }
    }

    if (!mEglHook.installed()) {
        mFailureReason = "active EGL presentation path could not be hooked";
        removeHooks();
        return false;
    }
    return true;
}

void LevelRenderHook::recordMinecraftRender(
    void* renderer,
    void* context,
    const void* view,
    void* client) noexcept {
    LevelRenderHook* self = sActive.load(std::memory_order_acquire);
    if (self == nullptr) return;

    self->mCallbacksInFlight.fetch_add(1, std::memory_order_acq_rel);
    const auto sequence =
        self->mMinecraftCalls.fetch_add(1, std::memory_order_relaxed) + 1U;
    const std::int64_t now = steadyNanosecondsNow();
    self->mLastMinecraftRenderNanoseconds.store(now, std::memory_order_release);

    const auto threadId = currentThreadId();
    std::uint32_t expectedThread = 0;
    self->mMinecraftThreadId.compare_exchange_strong(expectedThread, threadId);
    if (expectedThread != 0 && expectedThread != threadId) {
        self->mOtherMinecraftThreadCalls.fetch_add(1, std::memory_order_relaxed);
    }

    std::uintptr_t empty = 0;
    self->mFirstRenderer.compare_exchange_strong(
        empty, reinterpret_cast<std::uintptr_t>(renderer));
    self->mLastContext.store(
        reinterpret_cast<std::uintptr_t>(context), std::memory_order_relaxed);
    self->mLastView.store(
        reinterpret_cast<std::uintptr_t>(view), std::memory_order_relaxed);
    self->mLastClient.store(
        reinterpret_cast<std::uintptr_t>(client), std::memory_order_relaxed);

    (void)publishBestCamera(context, view, now);

    const LevelRenderEvent event{renderer, context, view, client, sequence, threadId};
    (void)self->mEventBus.publish(event);
    self->mCallbacksInFlight.fetch_sub(1, std::memory_order_acq_rel);
}

void LevelRenderHook::recordPresent(bool vulkan) noexcept {
    LevelRenderHook* self = sActive.load(std::memory_order_acquire);
    if (self == nullptr) return;
    self->mCallbacksInFlight.fetch_add(1, std::memory_order_acq_rel);
    self->mPresentCalls.fetch_add(1, std::memory_order_relaxed);
    if (vulkan) self->mVulkanPresentCalls.fetch_add(1, std::memory_order_relaxed);
    else self->mEglPresentCalls.fetch_add(1, std::memory_order_relaxed);

    const auto threadId = currentThreadId();
    std::uint32_t expectedThread = 0;
    self->mPresentThreadId.compare_exchange_strong(expectedThread, threadId);
    if (expectedThread != 0 && expectedThread != threadId) {
        self->mOtherPresentThreadCalls.fetch_add(1, std::memory_order_relaxed);
    }
    self->mCallbacksInFlight.fetch_sub(1, std::memory_order_acq_rel);
}

void LevelRenderHook::drawWorldSpaceCube() noexcept {
    LevelRenderHook* self = sActive.load(std::memory_order_acquire);
    if (self == nullptr) return;

    self->mCallbacksInFlight.fetch_add(1, std::memory_order_acq_rel);
    const std::int64_t now = steadyNanosecondsNow();
    const physics::PhysicsRenderSnapshot physicsSnapshot =
        self->mPhysicsScheduler.renderSnapshot();

    if (!physicsSnapshot.coherent || physicsSnapshot.activeClientLevel == 0 ||
        physicsSnapshot.worldGeneration == 0 || physicsSnapshot.simulationStep == 0) {
        self->mSuppressedNoActiveWorld.fetch_add(1, std::memory_order_relaxed);
        self->mCallbacksInFlight.fetch_sub(1, std::memory_order_acq_rel);
        return;
    }
    if (physicsSnapshot.lastPhysicsTickNanoseconds <= 0 ||
        now - physicsSnapshot.lastPhysicsTickNanoseconds > activePhysicsWindowNanoseconds) {
        self->mSuppressedStalePhysics.fetch_add(1, std::memory_order_relaxed);
        self->mCallbacksInFlight.fetch_sub(1, std::memory_order_acq_rel);
        return;
    }

    const render::CameraSnapshot camera = gCubeOverlay.cameraSnapshot();
    if (!camera.coherent || camera.timestampNanoseconds <= 0 ||
        now - camera.timestampNanoseconds > activeCameraWindowNanoseconds) {
        self->mSuppressedNoCamera.fetch_add(1, std::memory_order_relaxed);
        self->mCallbacksInFlight.fetch_sub(1, std::memory_order_acq_rel);
        return;
    }

    const render::InterpolatedVerticalTransform interpolated =
        render::RenderInterpolation::sample(
            physicsSnapshot.previousPositionYMicrometers,
            physicsSnapshot.currentPositionYMicrometers,
            now,
            physicsSnapshot.lastPhysicsTickNanoseconds);
    const float normalizedPhysicsOffset = static_cast<float>(
        static_cast<double>(interpolated.positionYMicrometers) / 10'000'000.0);

    const std::uint64_t attempt =
        self->mWorldDrawAttempts.fetch_add(1, std::memory_order_relaxed) + 1U;
    self->mLastWorldGeneration.store(
        physicsSnapshot.worldGeneration, std::memory_order_relaxed);
    if (gCubeOverlay.draw(
            attempt, physicsSnapshot.worldGeneration, normalizedPhysicsOffset)) {
        self->mWorldDrawSuccesses.fetch_add(1, std::memory_order_relaxed);
    } else {
        self->mWorldDrawFailures.fetch_add(1, std::memory_order_relaxed);
    }
    self->mCallbacksInFlight.fetch_sub(1, std::memory_order_acq_rel);
}

void LevelRenderHook::removeHooks() noexcept {
    const bool hadMinecraft = mMinecraftHook.installed();
    const bool hadVulkan = mVulkanHook.installed();
    const bool hadEgl = mEglHook.installed();
    mEglHook.reset();
    mVulkanHook.reset();
    mMinecraftHook.reset();
    gOriginalEglSwap = nullptr;
    gOriginalVulkanPresent = nullptr;
    gOriginalMinecraftRender = nullptr;
    gGetCameraPosition = nullptr;
    gGetCameraTargetPosition = nullptr;
    if (hadMinecraft || hadVulkan || hadEgl) {
        mRestoreSucceeded.store(true, std::memory_order_release);
    }
}

void LevelRenderHook::uninstall() noexcept {
    mStopRequested.store(true, std::memory_order_release);
    if (mWorker.joinable()) mWorker.join();
    removeHooks();
    sActive.store(nullptr, std::memory_order_release);
    for (unsigned index = 0;
         index < 200U && mCallbacksInFlight.load(std::memory_order_acquire) != 0;
         ++index) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    writeStatus("stopped");
}

bool LevelRenderHook::safeToUnload() const noexcept {
    return !mMinecraftHook.installed() && !mVulkanHook.installed() &&
           !mEglHook.installed() &&
           mCallbacksInFlight.load(std::memory_order_acquire) == 0;
}

void LevelRenderHook::writeStatus(const char* state) noexcept {
    std::ofstream out(mStatusPath, std::ios::trunc);
    if (!out) return;

    const auto minecraftThread = mMinecraftThreadId.load(std::memory_order_relaxed);
    const auto presentThread = mPresentThreadId.load(std::memory_order_relaxed);
    const render::CameraSnapshot camera = gCubeOverlay.cameraSnapshot();
    const render::Vec3f anchor = gCubeOverlay.lastAnchor();
    const int cameraSource = gSelectedCameraSource.load(std::memory_order_acquire);

    out << "schema=7\n"
        << "state=" << state << '\n'
        << "source=world_space_cube_context_camera_fix\n"
        << "minecraft_target=LevelRendererCamera::render+0xbd6f97c\n"
        << "graphics_backend=egl_opengles\n"
        << "geometry_submission=egl_opengles_camera_derived_world_anchor\n"
        << "visible_cube_expected=true\n"
        << "world_space_geometry=true\n"
        << "minecraft_owned_submission=false\n"
        << "camera_strategy=context_fields_then_getters_then_view_then_bounded_scan\n"
        << "context_camera_target_offset=" << contextCameraTargetOffset << '\n'
        << "context_camera_position_offset=" << contextCameraPositionOffset << '\n'
        << "selected_camera_source=" << cameraSourceName(cameraSource) << '\n'
        << "selected_camera_offset=" << gSelectedCameraOffset.load() << '\n'
        << "camera_position_getter_resolved="
        << (gGetCameraPosition != nullptr ? "true" : "false") << '\n'
        << "camera_target_getter_resolved="
        << (gGetCameraTargetPosition != nullptr ? "true" : "false") << '\n'
        << "projection=derived_perspective_70deg_near_0.05_far_2048\n"
        << "depth_mode=reuse_current_egl_depth_buffer_when_available\n"
        << "physics_gate=active_20hz_world_only_pause_suppressed\n"
        << "hook_engine=preloader_android_hook_handle\n"
        << "fingerprint_validated="
        << (mFingerprintValidated.load() ? "true" : "false") << '\n'
        << "function_prefix_validated="
        << (mPrefixValidated.load() ? "true" : "false") << '\n'
        << "minecraft_hook_installed="
        << (mMinecraftHook.installed() ? "true" : "false") << '\n'
        << "vulkan_hook_installed="
        << (mVulkanHook.installed() ? "true" : "false") << '\n'
        << "egl_hook_installed="
        << (mEglHook.installed() ? "true" : "false") << '\n'
        << "minecraft_render_calls=" << mMinecraftCalls.load() << '\n'
        << "graphics_present_calls=" << mPresentCalls.load() << '\n'
        << "vulkan_present_calls=" << mVulkanPresentCalls.load() << '\n'
        << "egl_present_calls=" << mEglPresentCalls.load() << '\n'
        << "world_draw_attempts=" << mWorldDrawAttempts.load() << '\n'
        << "world_draw_successes=" << mWorldDrawSuccesses.load() << '\n'
        << "world_draw_failures=" << mWorldDrawFailures.load() << '\n'
        << "suppressed_no_active_world=" << mSuppressedNoActiveWorld.load() << '\n'
        << "suppressed_stale_physics=" << mSuppressedStalePhysics.load() << '\n'
        << "suppressed_no_camera=" << mSuppressedNoCamera.load() << '\n'
        << "context_field_valid_samples=" << gContextFieldValid.load() << '\n'
        << "context_field_invalid_samples=" << gContextFieldInvalid.load() << '\n'
        << "context_getter_valid_samples=" << gContextGetterValid.load() << '\n'
        << "context_getter_invalid_samples=" << gContextGetterInvalid.load() << '\n'
        << "view_field_valid_samples=" << gViewFieldValid.load() << '\n'
        << "view_field_invalid_samples=" << gViewFieldInvalid.load() << '\n'
        << "scan_candidates_accepted=" << gScanCandidatesAccepted.load() << '\n'
        << "scan_candidates_rejected=" << gScanCandidatesRejected.load() << '\n'
        << "camera_valid_samples=" << gCubeOverlay.validCameraSamples() << '\n'
        << "camera_invalid_samples=" << gCubeOverlay.invalidCameraSamples() << '\n'
        << "camera_incoherent_reads=" << gCubeOverlay.incoherentCameraReads() << '\n'
        << "camera_snapshot_coherent=" << (camera.coherent ? "true" : "false") << '\n'
        << std::fixed << std::setprecision(6)
        << "raw_context_position_x=" << gRawContextPosition[0].load() << '\n'
        << "raw_context_position_y=" << gRawContextPosition[1].load() << '\n'
        << "raw_context_position_z=" << gRawContextPosition[2].load() << '\n'
        << "raw_context_target_x=" << gRawContextTarget[0].load() << '\n'
        << "raw_context_target_y=" << gRawContextTarget[1].load() << '\n'
        << "raw_context_target_z=" << gRawContextTarget[2].load() << '\n'
        << "raw_view_position_x=" << gRawViewPosition[0].load() << '\n'
        << "raw_view_position_y=" << gRawViewPosition[1].load() << '\n'
        << "raw_view_position_z=" << gRawViewPosition[2].load() << '\n'
        << "raw_view_target_x=" << gRawViewTarget[0].load() << '\n'
        << "raw_view_target_y=" << gRawViewTarget[1].load() << '\n'
        << "raw_view_target_z=" << gRawViewTarget[2].load() << '\n'
        << "camera_position_x=" << camera.position.x << '\n'
        << "camera_position_y=" << camera.position.y << '\n'
        << "camera_position_z=" << camera.position.z << '\n'
        << "camera_target_x=" << camera.target.x << '\n'
        << "camera_target_y=" << camera.target.y << '\n'
        << "camera_target_z=" << camera.target.z << '\n'
        << "anchor_locked=" << (gCubeOverlay.anchorLocked() ? "true" : "false") << '\n'
        << "anchor_resets=" << gCubeOverlay.anchorResets() << '\n'
        << "anchor_world_x=" << anchor.x << '\n'
        << "anchor_world_y=" << anchor.y << '\n'
        << "anchor_world_z=" << anchor.z << '\n'
        << "last_world_generation=" << mLastWorldGeneration.load() << '\n'
        << "depth_bits=" << gCubeOverlay.depthBits() << '\n'
        << "depth_test_frames=" << gCubeOverlay.depthTestFrames() << '\n'
        << "renderer_successful_frames=" << gCubeOverlay.successfulFrames() << '\n'
        << "renderer_failed_frames=" << gCubeOverlay.failedFrames() << '\n'
        << "renderer_frames_without_camera=" << gCubeOverlay.framesWithoutCamera() << '\n'
        << "minecraft_render_thread_id=" << minecraftThread << '\n'
        << "graphics_present_thread_id=" << presentThread << '\n'
        << "threads_match="
        << (minecraftThread != 0 && minecraftThread == presentThread ? "true" : "false")
        << '\n'
        << "other_minecraft_thread_calls=" << mOtherMinecraftThreadCalls.load() << '\n'
        << "other_present_thread_calls=" << mOtherPresentThreadCalls.load() << '\n'
        << "first_renderer=0x" << std::hex << mFirstRenderer.load() << '\n'
        << "last_render_context=0x" << mLastContext.load() << '\n'
        << "last_view=0x" << mLastView.load() << '\n'
        << "last_client=0x" << mLastClient.load() << std::dec << '\n'
        << "event_bus_published_events=" << mEventBus.publishedEvents() << '\n'
        << "event_bus_delivered_callbacks=" << mEventBus.deliveredCallbacks() << '\n'
        << "callbacks_in_flight=" << mCallbacksInFlight.load() << '\n'
        << "hook_restore_succeeded="
        << (mRestoreSucceeded.load() ? "true" : "false") << '\n'
        << "safe_to_unload=" << (safeToUnload() ? "true" : "false") << '\n'
        << "failure_reason="
        << (mFailureReason.empty() ? "none" : mFailureReason) << '\n';
}

}  // namespace aeronautics::bedrock
