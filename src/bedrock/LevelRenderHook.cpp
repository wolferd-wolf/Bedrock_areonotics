#include "bedrock/LevelRenderHook.hpp"

#include "bedrock/CompatibilityProfile.hpp"
#include "bedrock/HeartbeatHook.hpp"
#include "render/EglDiagnosticCubeOverlay.hpp"

#include <array>
#include <chrono>
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
constexpr std::int64_t activeWorldWindowNanoseconds = 250'000'000;

using MinecraftRenderFn = void (*)(void*, void*, const void*, void*);
using VulkanPresentFn = std::int32_t (*)(void*, const void*);
using EglSwapFn = std::uint32_t (*)(void*, void*);

MinecraftRenderFn gOriginalMinecraftRender = nullptr;
VulkanPresentFn gOriginalVulkanPresent = nullptr;
EglSwapFn gOriginalEglSwap = nullptr;
aeronautics::render::EglDiagnosticCubeOverlay gCubeOverlay;

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
    LevelRenderHook::drawVisibleCubeOverlay();
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
    LevelRenderBus& eventBus) noexcept
    : mMod(mod), mHeartbeat(heartbeat), mEventBus(eventBus) {}

LevelRenderHook::~LevelRenderHook() { uninstall(); }

bool LevelRenderHook::install() {
    if (mWorker.joinable()) return true;
    mStatusPath = mMod.getDataDir() / "visible-cube-render-status.txt";
    mStopRequested.store(false, std::memory_order_release);
    mRestoreSucceeded.store(false, std::memory_order_release);
    mOverlayDrawAttempts.store(0, std::memory_order_relaxed);
    mOverlayDrawSuccesses.store(0, std::memory_order_relaxed);
    mOverlayDrawFailures.store(0, std::memory_order_relaxed);
    mLastMinecraftRenderNanoseconds.store(0, std::memory_order_relaxed);
    mFailureReason.clear();
    LevelRenderHook* expected = nullptr;
    if (!sActive.compare_exchange_strong(expected, this, std::memory_order_acq_rel)) {
        mFailureReason = "another visible cube renderer is active";
        writeStatus("registration_failed");
        return false;
    }
    writeStatus("waiting_for_stable_heartbeat");
    try {
        mWorker = std::thread(&LevelRenderHook::workerLoop, this);
    } catch (...) {
        sActive.store(nullptr, std::memory_order_release);
        mFailureReason = "failed to start visible cube worker";
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
    writeStatus("running_visible_cube");
    while (!mStopRequested.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(2s);
        writeStatus("running_visible_cube");
    }
}

bool LevelRenderHook::installHooks() noexcept {
    const auto module = inspectLoadedModule(CompatibilityProfile::moduleName);
    if (!module || module->buildId != expectedBuildId || module->fileSize != expectedModuleFileSize) {
        mFailureReason = "Minecraft binary fingerprint mismatch";
        return false;
    }
    mFingerprintValidated.store(true, std::memory_order_release);
    const auto targetAddress = module->loadBase + minecraftRenderOffset;
    const std::string observed = readInstructionPrefix(*module, targetAddress, expectedPrefix.size());
    std::ostringstream expected;
    expected << std::hex << std::setfill('0');
    for (std::size_t i = 0; i < expectedPrefix.size(); ++i) {
        if (i != 0) expected << ' ';
        expected << std::setw(2) << static_cast<unsigned>(expectedPrefix[i]);
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
    gOriginalMinecraftRender = reinterpret_cast<MinecraftRenderFn>(mMinecraftOriginalStorage);

    if (void* symbol = resolveSymbol("libvulkan.so", "vkQueuePresentKHR"); symbol != nullptr) {
        mVulkanHook = pl::memory::HookHandle(
            reinterpret_cast<pl::memory::FuncPtr>(symbol),
            reinterpret_cast<pl::memory::FuncPtr>(&vulkanPresentDetour),
            &mVulkanOriginalStorage,
            pl::memory::HookPriority::Low);
        if (mVulkanHook.installed() && mVulkanOriginalStorage != nullptr) {
            gOriginalVulkanPresent = reinterpret_cast<VulkanPresentFn>(mVulkanOriginalStorage);
        } else {
            mVulkanHook.reset();
        }
    }

    if (void* symbol = resolveSymbol("libEGL.so", "eglSwapBuffers"); symbol != nullptr) {
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
        return false;
    }
    return true;
}

void LevelRenderHook::recordMinecraftRender(void* renderer, void* context, const void* view, void* client) noexcept {
    LevelRenderHook* self = sActive.load(std::memory_order_acquire);
    if (self == nullptr) return;
    self->mCallbacksInFlight.fetch_add(1, std::memory_order_acq_rel);
    const auto sequence = self->mMinecraftCalls.fetch_add(1, std::memory_order_relaxed) + 1U;
    self->mLastMinecraftRenderNanoseconds.store(steadyNanosecondsNow(), std::memory_order_release);
    const auto threadId = currentThreadId();
    std::uint32_t expected = 0;
    self->mMinecraftThreadId.compare_exchange_strong(expected, threadId);
    if (expected != 0 && expected != threadId) self->mOtherMinecraftThreadCalls.fetch_add(1);
    std::uintptr_t empty = 0;
    self->mFirstRenderer.compare_exchange_strong(empty, reinterpret_cast<std::uintptr_t>(renderer));
    self->mLastContext.store(reinterpret_cast<std::uintptr_t>(context));
    self->mLastView.store(reinterpret_cast<std::uintptr_t>(view));
    self->mLastClient.store(reinterpret_cast<std::uintptr_t>(client));
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
    std::uint32_t expected = 0;
    self->mPresentThreadId.compare_exchange_strong(expected, threadId);
    if (expected != 0 && expected != threadId) self->mOtherPresentThreadCalls.fetch_add(1);
    self->mCallbacksInFlight.fetch_sub(1, std::memory_order_acq_rel);
}

void LevelRenderHook::drawVisibleCubeOverlay() noexcept {
    LevelRenderHook* self = sActive.load(std::memory_order_acquire);
    if (self == nullptr) return;
    const std::int64_t lastRender = self->mLastMinecraftRenderNanoseconds.load(std::memory_order_acquire);
    const std::int64_t now = steadyNanosecondsNow();
    if (lastRender == 0 || now - lastRender > activeWorldWindowNanoseconds) return;
    self->mCallbacksInFlight.fetch_add(1, std::memory_order_acq_rel);
    const std::uint64_t attempt = self->mOverlayDrawAttempts.fetch_add(1, std::memory_order_relaxed) + 1U;
    if (gCubeOverlay.draw(attempt)) {
        self->mOverlayDrawSuccesses.fetch_add(1, std::memory_order_relaxed);
    } else {
        self->mOverlayDrawFailures.fetch_add(1, std::memory_order_relaxed);
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
    if (mWorker.joinable()) mWorker.join();
    removeHooks();
    sActive.store(nullptr, std::memory_order_release);
    for (unsigned i = 0; i < 200U && mCallbacksInFlight.load(std::memory_order_acquire) != 0; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    writeStatus("stopped");
}

bool LevelRenderHook::safeToUnload() const noexcept {
    return !mMinecraftHook.installed() && !mVulkanHook.installed() && !mEglHook.installed() &&
        mCallbacksInFlight.load(std::memory_order_acquire) == 0;
}

void LevelRenderHook::writeStatus(const char* state) noexcept {
    std::ofstream out(mStatusPath, std::ios::trunc);
    if (!out) return;
    const auto mcThread = mMinecraftThreadId.load();
    const auto presentThread = mPresentThreadId.load();
    out << "schema=5\n"
        << "state=" << state << '\n'
        << "source=first_visible_cube\n"
        << "minecraft_target=LevelRendererCamera::render+0xbd6f97c\n"
        << "graphics_backend=egl_opengles\n"
        << "geometry_submission=egl_clip_space_wireframe_cube\n"
        << "visible_cube_expected=true\n"
        << "world_space_geometry=false\n"
        << "next_geometry_mode=minecraft_owned_world_space_submission\n"
        << "hook_engine=preloader_android_hook_handle\n"
        << "fingerprint_validated=" << (mFingerprintValidated.load() ? "true" : "false") << '\n'
        << "function_prefix_validated=" << (mPrefixValidated.load() ? "true" : "false") << '\n'
        << "minecraft_hook_installed=" << (mMinecraftHook.installed() ? "true" : "false") << '\n'
        << "vulkan_hook_installed=" << (mVulkanHook.installed() ? "true" : "false") << '\n'
        << "egl_hook_installed=" << (mEglHook.installed() ? "true" : "false") << '\n'
        << "minecraft_render_calls=" << mMinecraftCalls.load() << '\n'
        << "graphics_present_calls=" << mPresentCalls.load() << '\n'
        << "vulkan_present_calls=" << mVulkanPresentCalls.load() << '\n'
        << "egl_present_calls=" << mEglPresentCalls.load() << '\n'
        << "overlay_draw_attempts=" << mOverlayDrawAttempts.load() << '\n'
        << "overlay_draw_successes=" << mOverlayDrawSuccesses.load() << '\n'
        << "overlay_draw_failures=" << mOverlayDrawFailures.load() << '\n'
        << "minecraft_render_thread_id=" << mcThread << '\n'
        << "graphics_present_thread_id=" << presentThread << '\n'
        << "threads_match=" << (mcThread != 0 && mcThread == presentThread ? "true" : "false") << '\n'
        << "other_minecraft_thread_calls=" << mOtherMinecraftThreadCalls.load() << '\n'
        << "other_present_thread_calls=" << mOtherPresentThreadCalls.load() << '\n'
        << "first_renderer=0x" << std::hex << mFirstRenderer.load() << '\n'
        << "last_render_context=0x" << mLastContext.load() << '\n'
        << "last_view=0x" << mLastView.load() << '\n'
        << "last_client=0x" << mLastClient.load() << std::dec << '\n'
        << "event_bus_published_events=" << mEventBus.publishedEvents() << '\n'
        << "event_bus_delivered_callbacks=" << mEventBus.deliveredCallbacks() << '\n'
        << "callbacks_in_flight=" << mCallbacksInFlight.load() << '\n'
        << "hook_restore_succeeded=" << (mRestoreSucceeded.load() ? "true" : "false") << '\n'
        << "safe_to_unload=" << (safeToUnload() ? "true" : "false") << '\n'
        << "failure_reason=" << (mFailureReason.empty() ? "none" : mFailureReason) << '\n';
}

}  // namespace aeronautics::bedrock
