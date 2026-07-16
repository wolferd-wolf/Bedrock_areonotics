#include "bedrock/LevelRenderHook.hpp"

#include "bedrock/CompatibilityProfile.hpp"
#include "bedrock/HeartbeatHook.hpp"
#include "physics/PhysicsScheduler.hpp"
#include "render/EglDiagnosticCubeOverlay.hpp"
#include "render/RenderInterpolation.hpp"

#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <dlfcn.h>
#include <fstream>
#include <iomanip>
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

using MinecraftRenderFn = void (*)(void*, void*, const void*, void*);
using VulkanPresentFn = std::int32_t (*)(void*, const void*);
using EglSwapFn = std::uint32_t (*)(void*, void*);

MinecraftRenderFn gOriginalMinecraftRender = nullptr;
VulkanPresentFn gOriginalVulkanPresent = nullptr;
EglSwapFn gOriginalEglSwap = nullptr;
aeronautics::render::EglDiagnosticCubeOverlay gCubeOverlay;

struct RawViewCamera final {
    float position[3]{};
    float target[3]{};
};

[[nodiscard]] std::uint32_t currentThreadId() noexcept {
    const long value = ::syscall(SYS_gettid);
    return value > 0 ? static_cast<std::uint32_t>(value) : 0U;
}

[[nodiscard]] std::int64_t steadyNanosecondsNow() noexcept {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

void minecraftRenderDetour(void* renderer, void* context, const void* view, void* client) {
    LevelRenderHook::recordMinecraftRender(renderer, context, view, client);
    if (gOriginalMinecraftRender != nullptr) {
        gOriginalMinecraftRender(renderer, context, view, client);
    }
}

std::int32_t vulkanPresentDetour(void* queue, const void* presentInfo) {
    LevelRenderHook::recordPresent(true);
    return gOriginalVulkanPresent != nullptr ? gOriginalVulkanPresent(queue, presentInfo) : -3;
}

std::uint32_t eglSwapDetour(void* display, void* surface) {
    LevelRenderHook::recordPresent(false);
    LevelRenderHook::drawWorldSpaceCube();
    return gOriginalEglSwap != nullptr ? gOriginalEglSwap(display, surface) : 0U;
}

[[nodiscard]] void* resolveSymbol(const char* library, const char* symbol) noexcept {
    if (void* direct = ::dlsym(RTLD_DEFAULT, symbol); direct != nullptr) {
        return direct;
    }
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

LevelRenderHook::~LevelRenderHook() {
    uninstall();
}

bool LevelRenderHook::install() {
    if (mWorker.joinable()) {
        return true;
    }

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
    if (mStopRequested.load(std::memory_order_acquire)) {
        return;
    }
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
        if (index != 0) {
            expected << ' ';
        }
        expected << std::setw(2) << static_cast<unsigned>(expectedPrefix[index]);
    }
    if (observed != expected.str()) {
        mFailureReason = "Minecraft render function prefix mismatch";
        return false;
    }
    mPrefixValidated.store(true, std::memory_order_release);

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
    if (self == nullptr) {
        return;
    }

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
        empty,
        reinterpret_cast<std::uintptr_t>(renderer));
    self->mLastContext.store(
        reinterpret_cast<std::uintptr_t>(context),
        std::memory_order_relaxed);
    self->mLastView.store(
        reinterpret_cast<std::uintptr_t>(view),
        std::memory_order_relaxed);
    self->mLastClient.store(
        reinterpret_cast<std::uintptr_t>(client),
        std::memory_order_relaxed);

    if (view != nullptr) {
        RawViewCamera raw{};
        std::memcpy(&raw, view, sizeof(raw));
        (void)gCubeOverlay.publishCamera(
            {raw.position[0], raw.position[1], raw.position[2]},
            {raw.target[0], raw.target[1], raw.target[2]},
            now);
    } else {
        (void)gCubeOverlay.publishCamera({}, {}, now);
    }

    const LevelRenderEvent event{renderer, context, view, client, sequence, threadId};
    (void)self->mEventBus.publish(event);
    self->mCallbacksInFlight.fetch_sub(1, std::memory_order_acq_rel);
}

void LevelRenderHook::recordPresent(bool vulkan) noexcept {
    LevelRenderHook* self = sActive.load(std::memory_order_acquire);
    if (self == nullptr) {
        return;
    }

    self->mCallbacksInFlight.fetch_add(1, std::memory_order_acq_rel);
    self->mPresentCalls.fetch_add(1, std::memory_order_relaxed);
    if (vulkan) {
        self->mVulkanPresentCalls.fetch_add(1, std::memory_order_relaxed);
    } else {
        self->mEglPresentCalls.fetch_add(1, std::memory_order_relaxed);
    }

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
    if (self == nullptr) {
        return;
    }

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
        physicsSnapshot.worldGeneration,
        std::memory_order_relaxed);
    if (gCubeOverlay.draw(
            attempt,
            physicsSnapshot.worldGeneration,
            normalizedPhysicsOffset)) {
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
    if (hadMinecraft || hadVulkan || hadEgl) {
        mRestoreSucceeded.store(true, std::memory_order_release);
    }
}

void LevelRenderHook::uninstall() noexcept {
    mStopRequested.store(true, std::memory_order_release);
    if (mWorker.joinable()) {
        mWorker.join();
    }
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
    if (!out) {
        return;
    }

    const auto minecraftThread = mMinecraftThreadId.load(std::memory_order_relaxed);
    const auto presentThread = mPresentThreadId.load(std::memory_order_relaxed);
    const render::CameraSnapshot camera = gCubeOverlay.cameraSnapshot();
    const render::Vec3f anchor = gCubeOverlay.lastAnchor();

    out << "schema=6\n"
        << "state=" << state << '\n'
        << "source=world_space_cube\n"
        << "minecraft_target=LevelRendererCamera::render+0xbd6f97c\n"
        << "graphics_backend=egl_opengles\n"
        << "geometry_submission=egl_opengles_camera_derived_world_anchor\n"
        << "visible_cube_expected=true\n"
        << "world_space_geometry=true\n"
        << "minecraft_owned_submission=false\n"
        << "camera_layout=ViewRenderObject.mViewData_cameraPos_at_0_cameraTarget_at_12\n"
        << "projection=derived_perspective_70deg_near_0.05_far_2048\n"
        << "depth_mode=reuse_current_egl_depth_buffer_when_available\n"
        << "physics_gate=active_20hz_world_only_pause_suppressed\n"
        << "next_geometry_mode=minecraft_owned_tessellator_material_submission\n"
        << "hook_engine=preloader_android_hook_handle\n"
        << "fingerprint_validated="
        << (mFingerprintValidated.load(std::memory_order_relaxed) ? "true" : "false")
        << '\n'
        << "function_prefix_validated="
        << (mPrefixValidated.load(std::memory_order_relaxed) ? "true" : "false")
        << '\n'
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
        << "camera_valid_samples=" << gCubeOverlay.validCameraSamples() << '\n'
        << "camera_invalid_samples=" << gCubeOverlay.invalidCameraSamples() << '\n'
        << "camera_incoherent_reads=" << gCubeOverlay.incoherentCameraReads() << '\n'
        << "camera_snapshot_coherent=" << (camera.coherent ? "true" : "false") << '\n'
        << std::fixed << std::setprecision(6)
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
