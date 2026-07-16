#include "bedrock/LevelRenderHook.hpp"

#include "bedrock/CompatibilityProfile.hpp"
#include "bedrock/HeartbeatHook.hpp"

#include <array>
#include <chrono>
#include <dlfcn.h>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <string>

#include <dobby.h>
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

using MinecraftRenderFn = void (*)(void*, void*, const void*, void*);
using VulkanPresentFn = std::int32_t (*)(void*, const void*);
using EglSwapFn = std::uint32_t (*)(void*, void*);

MinecraftRenderFn gOriginalMinecraftRender = nullptr;
VulkanPresentFn gOriginalVulkanPresent = nullptr;
EglSwapFn gOriginalEglSwap = nullptr;
void* gMinecraftTarget = nullptr;
void* gVulkanTarget = nullptr;
void* gEglTarget = nullptr;

[[nodiscard]] std::uint32_t currentThreadId() noexcept {
    const long value = ::syscall(SYS_gettid);
    return value > 0 ? static_cast<std::uint32_t>(value) : 0U;
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
    mStatusPath = mMod.getDataDir() / "dual-render-proof-status.txt";
    mStopRequested.store(false, std::memory_order_release);
    mRestoreSucceeded.store(false, std::memory_order_release);
    mFailureReason.clear();
    LevelRenderHook* expected = nullptr;
    if (!sActive.compare_exchange_strong(expected, this, std::memory_order_acq_rel)) {
        mFailureReason = "another dual render proof is active";
        writeStatus("registration_failed");
        return false;
    }
    writeStatus("waiting_for_stable_heartbeat");
    try {
        mWorker = std::thread(&LevelRenderHook::workerLoop, this);
    } catch (...) {
        sActive.store(nullptr, std::memory_order_release);
        mFailureReason = "failed to start dual render proof worker";
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
    writeStatus("running_dual_render_proof");
    while (!mStopRequested.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(2s);
        writeStatus("running_dual_render_proof");
    }
}

bool LevelRenderHook::installHooks() noexcept {
    const auto module = inspectLoadedModule(CompatibilityProfile::moduleName);
    if (!module || module->buildId != expectedBuildId || module->fileSize != expectedModuleFileSize) {
        mFailureReason = "Minecraft binary fingerprint mismatch";
        return false;
    }
    mFingerprintValidated.store(true, std::memory_order_release);
    gMinecraftTarget = reinterpret_cast<void*>(module->loadBase + minecraftRenderOffset);
    const std::string observed = readInstructionPrefix(*module, reinterpret_cast<std::uintptr_t>(gMinecraftTarget), expectedPrefix.size());
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

    if (DobbyHook(gMinecraftTarget, reinterpret_cast<void*>(minecraftRenderDetour), reinterpret_cast<void**>(&gOriginalMinecraftRender)) != 0) {
        mFailureReason = "DobbyHook failed for Minecraft render target";
        return false;
    }
    mMinecraftHookInstalled.store(true, std::memory_order_release);

    gVulkanTarget = resolveSymbol("libvulkan.so", "vkQueuePresentKHR");
    if (gVulkanTarget != nullptr && DobbyHook(gVulkanTarget, reinterpret_cast<void*>(vulkanPresentDetour), reinterpret_cast<void**>(&gOriginalVulkanPresent)) == 0) {
        mVulkanHookInstalled.store(true, std::memory_order_release);
    }
    gEglTarget = resolveSymbol("libEGL.so", "eglSwapBuffers");
    if (gEglTarget != nullptr && DobbyHook(gEglTarget, reinterpret_cast<void*>(eglSwapDetour), reinterpret_cast<void**>(&gOriginalEglSwap)) == 0) {
        mEglHookInstalled.store(true, std::memory_order_release);
    }
    if (!mVulkanHookInstalled.load() && !mEglHookInstalled.load()) {
        mFailureReason = "no graphics presentation symbol could be hooked";
    }
    mHooksInstalled.store(true, std::memory_order_release);
    return true;
}

void LevelRenderHook::recordMinecraftRender(void* renderer, void* context, const void* view, void* client) noexcept {
    LevelRenderHook* self = sActive.load(std::memory_order_acquire);
    if (self == nullptr) return;
    self->mCallbacksInFlight.fetch_add(1, std::memory_order_acq_rel);
    const auto sequence = self->mMinecraftCalls.fetch_add(1, std::memory_order_relaxed) + 1U;
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
    (vulkan ? self->mVulkanPresentCalls : self->mEglPresentCalls).fetch_add(1, std::memory_order_relaxed);
    const auto threadId = currentThreadId();
    std::uint32_t expected = 0;
    self->mPresentThreadId.compare_exchange_strong(expected, threadId);
    if (expected != 0 && expected != threadId) self->mOtherPresentThreadCalls.fetch_add(1);
    self->mCallbacksInFlight.fetch_sub(1, std::memory_order_acq_rel);
}

void LevelRenderHook::removeHooks() noexcept {
    bool ok = true;
    if (mEglHookInstalled.exchange(false) && gEglTarget != nullptr) ok = DobbyDestroy(gEglTarget) == 0 && ok;
    if (mVulkanHookInstalled.exchange(false) && gVulkanTarget != nullptr) ok = DobbyDestroy(gVulkanTarget) == 0 && ok;
    if (mMinecraftHookInstalled.exchange(false) && gMinecraftTarget != nullptr) ok = DobbyDestroy(gMinecraftTarget) == 0 && ok;
    mHooksInstalled.store(false, std::memory_order_release);
    mRestoreSucceeded.store(ok, std::memory_order_release);
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
    return !mHooksInstalled.load(std::memory_order_acquire) &&
        !mMinecraftHookInstalled.load(std::memory_order_acquire) &&
        !mVulkanHookInstalled.load(std::memory_order_acquire) &&
        !mEglHookInstalled.load(std::memory_order_acquire) &&
        mCallbacksInFlight.load(std::memory_order_acquire) == 0;
}

void LevelRenderHook::writeStatus(const char* state) noexcept {
    std::ofstream out(mStatusPath, std::ios::trunc);
    if (!out) return;
    const auto mcThread = mMinecraftThreadId.load();
    const auto presentThread = mPresentThreadId.load();
    out << "schema=4\n"
        << "state=" << state << '\n'
        << "source=dual_render_proof\n"
        << "minecraft_target=LevelRendererCamera::render+0xbd6f97c\n"
        << "graphics_probe=vkQueuePresentKHR_then_eglSwapBuffers\n"
        << "geometry_submission=disabled\n"
        << "hook_engine=Dobby_inline_arm64\n"
        << "fingerprint_validated=" << (mFingerprintValidated.load() ? "true" : "false") << '\n'
        << "function_prefix_validated=" << (mPrefixValidated.load() ? "true" : "false") << '\n'
        << "minecraft_hook_installed=" << (mMinecraftHookInstalled.load() ? "true" : "false") << '\n'
        << "vulkan_hook_installed=" << (mVulkanHookInstalled.load() ? "true" : "false") << '\n'
        << "egl_hook_installed=" << (mEglHookInstalled.load() ? "true" : "false") << '\n'
        << "minecraft_render_calls=" << mMinecraftCalls.load() << '\n'
        << "graphics_present_calls=" << mPresentCalls.load() << '\n'
        << "vulkan_present_calls=" << mVulkanPresentCalls.load() << '\n'
        << "egl_present_calls=" << mEglPresentCalls.load() << '\n'
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
