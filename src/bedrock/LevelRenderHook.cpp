#include "bedrock/LevelRenderHook.hpp"

#include "bedrock/CompatibilityProfile.hpp"
#include "bedrock/HeartbeatHook.hpp"
#include "physics/PhysicsScheduler.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <limits>
#include <sstream>
#include <string_view>

#include <sys/syscall.h>
#include <unistd.h>

namespace aeronautics::bedrock {
namespace {

constexpr std::string_view expectedBuildId{"2e318db12824cadb2618754ab7c82fa96fb30659"};
constexpr std::uintmax_t expectedModuleFileSize = 349243744;
constexpr std::uintptr_t minecraftRenderOffset = 0x0bd6f97c;
constexpr std::array<std::uint8_t, 16> minecraftRenderPrefix{
    0xec, 0x0f, 0x17, 0xfc, 0xeb, 0x2b, 0x01, 0x6d,
    0xe9, 0x23, 0x02, 0x6d, 0xfd, 0x7b, 0x03, 0xa9};
constexpr std::uintptr_t chunkLayerTaskOffset = 0x126d8cc0;
constexpr std::array<std::uint8_t, 16> chunkLayerTaskPrefix{
    0xff, 0x03, 0x03, 0xd1, 0xec, 0x1b, 0x00, 0xfd,
    0xeb, 0x2b, 0x04, 0x6d, 0xe9, 0x23, 0x05, 0x6d};

constexpr std::size_t contextRenderStateOffset = 0x28;
constexpr std::size_t renderStateCameraObjectOffset = 0x18;
constexpr std::size_t cameraOriginOffset = 0x13c;
constexpr std::size_t cameraMatrixBlockOffset = 0x158;
constexpr std::size_t cameraMatrixCount = 3;
constexpr std::size_t matrixFloatCount = 16;
constexpr std::size_t allMatrixFloatCount = cameraMatrixCount * matrixFloatCount;
constexpr std::size_t closureSampleCapacity = 8;
constexpr float changedEpsilon = 0.00001F;

using MinecraftRenderFn = void (*)(void*, void*, const void*, void*);
using ChunkLayerTaskFn = void (*)(void*, void*);
MinecraftRenderFn gOriginalMinecraftRender = nullptr;
ChunkLayerTaskFn gOriginalChunkLayerTask = nullptr;

struct RawCameraSnapshot final {
    std::uintptr_t renderState{};
    std::uintptr_t cameraObject{};
    std::array<float, 3> origin{};
    std::array<float, allMatrixFloatCount> matrices{};
    bool valid{};
};

struct AtomicCameraSnapshot final {
    std::atomic<std::uint64_t> sequence{0};
    std::atomic<std::uintptr_t> renderState{0};
    std::atomic<std::uintptr_t> cameraObject{0};
    std::array<std::atomic<float>, 3> origin{};
    std::array<std::atomic<float>, allMatrixFloatCount> matrices{};
    std::atomic_bool valid{false};
};

struct ClosureFields final {
    std::uintptr_t q220{};
    std::uintptr_t q228{};
    std::uintptr_t q230{};
    std::uintptr_t q238{};
    std::uint32_t d240{};
    std::uint64_t q248{};
    std::uint64_t q250{};
    std::uintptr_t q258{};
    float f260{};
};

struct AtomicClosureSample final {
    std::atomic<std::uint64_t> key{0};
    std::atomic<std::uint64_t> count{0};
    std::atomic<std::uint32_t> d240{0};
    std::atomic<std::uint64_t> q248{0};
    std::atomic<std::uint64_t> q250{0};
    std::atomic<float> f260{0.0F};
};

AtomicCameraSnapshot gPreSnapshot;
AtomicCameraSnapshot gPostSnapshot;
std::atomic<std::uint64_t> gPreValid{0};
std::atomic<std::uint64_t> gPreInvalid{0};
std::atomic<std::uint64_t> gPostValid{0};
std::atomic<std::uint64_t> gPostInvalid{0};
std::atomic<std::uint64_t> gOriginChangedFrames{0};
std::array<std::atomic<std::uint64_t>, cameraMatrixCount> gMatrixChangedFrames{};
std::array<std::atomic<std::uint64_t>, cameraMatrixCount> gMatrixChangedElements{};
std::array<std::atomic<float>, cameraMatrixCount> gMatrixMaximumDelta{};
std::array<AtomicClosureSample, closureSampleCapacity> gClosureSamples{};
std::atomic<std::uint64_t> gClosureSampleOverflow{0};

[[nodiscard]] std::uint32_t currentThreadId() noexcept {
    const long raw = ::syscall(SYS_gettid);
    if (raw <= 0 || static_cast<unsigned long>(raw) >
            static_cast<unsigned long>(std::numeric_limits<std::uint32_t>::max())) {
        return 0;
    }
    return static_cast<std::uint32_t>(raw);
}

[[nodiscard]] std::int64_t unixMillisecondsNow() noexcept {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

template <class T>
[[nodiscard]] T readObject(const void* base, std::size_t offset) noexcept {
    T value{};
    if (base != nullptr) {
        std::memcpy(&value, static_cast<const std::byte*>(base) + offset, sizeof(T));
    }
    return value;
}

[[nodiscard]] RawCameraSnapshot captureCamera(const void* context) noexcept {
    RawCameraSnapshot out{};
    if (context == nullptr) return out;
    const void* renderState = readObject<const void*>(context, contextRenderStateOffset);
    if (renderState == nullptr) return out;
    const void* cameraObject = readObject<const void*>(
        renderState, renderStateCameraObjectOffset);
    if (cameraObject == nullptr) return out;

    out.renderState = reinterpret_cast<std::uintptr_t>(renderState);
    out.cameraObject = reinterpret_cast<std::uintptr_t>(cameraObject);
    out.origin = readObject<std::array<float, 3>>(cameraObject, cameraOriginOffset);
    out.matrices = readObject<std::array<float, allMatrixFloatCount>>(
        cameraObject, cameraMatrixBlockOffset);
    bool matrixNonZero = false;
    out.valid = true;
    for (const float value : out.origin) {
        out.valid = out.valid && std::isfinite(value) && std::abs(value) < 1.0e12F;
    }
    for (const float value : out.matrices) {
        out.valid = out.valid && std::isfinite(value) && std::abs(value) < 1.0e12F;
        matrixNonZero = matrixNonZero || std::abs(value) > 0.000001F;
    }
    out.valid = out.valid && matrixNonZero;
    return out;
}

void publishCamera(
    AtomicCameraSnapshot& destination,
    const RawCameraSnapshot& source) noexcept {
    destination.sequence.fetch_add(1, std::memory_order_acq_rel);
    destination.renderState.store(source.renderState, std::memory_order_relaxed);
    destination.cameraObject.store(source.cameraObject, std::memory_order_relaxed);
    for (std::size_t i = 0; i < source.origin.size(); ++i) {
        destination.origin[i].store(source.origin[i], std::memory_order_relaxed);
    }
    for (std::size_t i = 0; i < source.matrices.size(); ++i) {
        destination.matrices[i].store(source.matrices[i], std::memory_order_relaxed);
    }
    destination.valid.store(source.valid, std::memory_order_relaxed);
    destination.sequence.fetch_add(1, std::memory_order_release);
}

[[nodiscard]] RawCameraSnapshot readCamera(
    const AtomicCameraSnapshot& source) noexcept {
    RawCameraSnapshot out{};
    for (unsigned attempt = 0; attempt < 8U; ++attempt) {
        const std::uint64_t before = source.sequence.load(std::memory_order_acquire);
        if ((before & 1U) != 0U) continue;
        out.renderState = source.renderState.load(std::memory_order_relaxed);
        out.cameraObject = source.cameraObject.load(std::memory_order_relaxed);
        for (std::size_t i = 0; i < out.origin.size(); ++i) {
            out.origin[i] = source.origin[i].load(std::memory_order_relaxed);
        }
        for (std::size_t i = 0; i < out.matrices.size(); ++i) {
            out.matrices[i] = source.matrices[i].load(std::memory_order_relaxed);
        }
        out.valid = source.valid.load(std::memory_order_relaxed);
        const std::uint64_t after = source.sequence.load(std::memory_order_acquire);
        if (before == after && (after & 1U) == 0U) return out;
    }
    out.valid = false;
    return out;
}

void updateMaximum(std::atomic<float>& target, float candidate) noexcept {
    float current = target.load(std::memory_order_relaxed);
    while (candidate > current && !target.compare_exchange_weak(
            current, candidate, std::memory_order_relaxed,
            std::memory_order_relaxed)) {}
}

void updateMatrixChanges(
    const RawCameraSnapshot& previous,
    const RawCameraSnapshot& current) noexcept {
    if (!previous.valid || !current.valid) return;
    if (std::abs(current.origin[0] - previous.origin[0]) > changedEpsilon ||
        std::abs(current.origin[1] - previous.origin[1]) > changedEpsilon ||
        std::abs(current.origin[2] - previous.origin[2]) > changedEpsilon) {
        gOriginChangedFrames.fetch_add(1, std::memory_order_relaxed);
    }
    for (std::size_t matrix = 0; matrix < cameraMatrixCount; ++matrix) {
        std::uint64_t changed = 0;
        float maximum = 0.0F;
        for (std::size_t element = 0; element < matrixFloatCount; ++element) {
            const std::size_t index = matrix * matrixFloatCount + element;
            const float delta = std::abs(
                current.matrices[index] - previous.matrices[index]);
            maximum = std::max(maximum, delta);
            if (delta > changedEpsilon) ++changed;
        }
        if (changed != 0) {
            gMatrixChangedFrames[matrix].fetch_add(1, std::memory_order_relaxed);
            gMatrixChangedElements[matrix].fetch_add(changed, std::memory_order_relaxed);
        }
        updateMaximum(gMatrixMaximumDelta[matrix], maximum);
    }
}

[[nodiscard]] std::uint64_t closureKey(const ClosureFields& fields) noexcept {
    std::uint64_t hash = 1469598103934665603ULL;
    const auto mix = [&hash](std::uint64_t value) noexcept {
        hash ^= value;
        hash *= 1099511628211ULL;
    };
    mix(fields.d240);
    mix(fields.q248);
    mix(fields.q250);
    mix(std::bit_cast<std::uint32_t>(fields.f260));
    hash &= ~std::uint64_t{1};
    return hash < 2 ? 2 : hash;
}

void recordClosureSample(const ClosureFields& fields) noexcept {
    const std::uint64_t key = closureKey(fields);
    for (auto& sample : gClosureSamples) {
        const std::uint64_t observed = sample.key.load(std::memory_order_acquire);
        if (observed == key) {
            sample.count.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        if (observed != 0) continue;
        std::uint64_t expected = 0;
        if (!sample.key.compare_exchange_strong(
                expected, 1, std::memory_order_acq_rel,
                std::memory_order_acquire)) continue;
        sample.d240.store(fields.d240, std::memory_order_relaxed);
        sample.q248.store(fields.q248, std::memory_order_relaxed);
        sample.q250.store(fields.q250, std::memory_order_relaxed);
        sample.f260.store(fields.f260, std::memory_order_relaxed);
        sample.count.store(1, std::memory_order_relaxed);
        sample.key.store(key, std::memory_order_release);
        return;
    }
    gClosureSampleOverflow.fetch_add(1, std::memory_order_relaxed);
}

void resetDiscovery() noexcept {
    for (AtomicCameraSnapshot* snapshot : {&gPreSnapshot, &gPostSnapshot}) {
        snapshot->sequence.store(0, std::memory_order_relaxed);
        snapshot->renderState.store(0, std::memory_order_relaxed);
        snapshot->cameraObject.store(0, std::memory_order_relaxed);
        snapshot->valid.store(false, std::memory_order_relaxed);
        for (auto& value : snapshot->origin) value.store(0.0F, std::memory_order_relaxed);
        for (auto& value : snapshot->matrices) value.store(0.0F, std::memory_order_relaxed);
    }
    gPreValid.store(0, std::memory_order_relaxed);
    gPreInvalid.store(0, std::memory_order_relaxed);
    gPostValid.store(0, std::memory_order_relaxed);
    gPostInvalid.store(0, std::memory_order_relaxed);
    gOriginChangedFrames.store(0, std::memory_order_relaxed);
    for (std::size_t matrix = 0; matrix < cameraMatrixCount; ++matrix) {
        gMatrixChangedFrames[matrix].store(0, std::memory_order_relaxed);
        gMatrixChangedElements[matrix].store(0, std::memory_order_relaxed);
        gMatrixMaximumDelta[matrix].store(0.0F, std::memory_order_relaxed);
    }
    for (auto& sample : gClosureSamples) {
        sample.key.store(0, std::memory_order_relaxed);
        sample.count.store(0, std::memory_order_relaxed);
        sample.d240.store(0, std::memory_order_relaxed);
        sample.q248.store(0, std::memory_order_relaxed);
        sample.q250.store(0, std::memory_order_relaxed);
        sample.f260.store(0.0F, std::memory_order_relaxed);
    }
    gClosureSampleOverflow.store(0, std::memory_order_relaxed);
}

[[nodiscard]] std::string formatPrefix(
    const std::array<std::uint8_t, 16>& prefix) {
    std::ostringstream stream;
    stream << std::hex << std::setfill('0');
    for (std::size_t i = 0; i < prefix.size(); ++i) {
        if (i != 0) stream << ' ';
        stream << std::setw(2) << static_cast<unsigned>(prefix[i]);
    }
    return stream.str();
}

void minecraftRenderDetour(
    void* renderer, void* context, const void* view, void* client) {
    LevelRenderHook::recordMinecraftRenderPre(renderer, context, view, client);
    if (gOriginalMinecraftRender != nullptr) {
        gOriginalMinecraftRender(renderer, context, view, client);
    }
    LevelRenderHook::recordMinecraftRenderPost(renderer, context, view, client);
}

void chunkLayerTaskDetour(void* closure, void* taskContext) {
    LevelRenderHook::recordChunkLayerTaskBegin(closure, taskContext);
    if (gOriginalChunkLayerTask != nullptr) {
        gOriginalChunkLayerTask(closure, taskContext);
    }
    LevelRenderHook::recordChunkLayerTaskEnd();
}

void appendCameraCsv(std::ostream& out, const RawCameraSnapshot& snapshot) {
    out << ',' << snapshot.renderState << ',' << snapshot.cameraObject
        << ',' << (snapshot.valid ? 1 : 0)
        << ',' << snapshot.origin[0] << ',' << snapshot.origin[1]
        << ',' << snapshot.origin[2];
    for (const float value : snapshot.matrices) out << ',' << value;
}

void writeMatrixLines(
    std::ostream& out,
    std::string_view phase,
    const RawCameraSnapshot& snapshot) {
    for (std::size_t matrix = 0; matrix < cameraMatrixCount; ++matrix) {
        out << phase << "_matrix" << matrix << '=';
        for (std::size_t element = 0; element < matrixFloatCount; ++element) {
            if (element != 0) out << ',';
            out << snapshot.matrices[matrix * matrixFloatCount + element];
        }
        out << '\n';
    }
}

} // namespace

std::atomic<LevelRenderHook*> LevelRenderHook::sActive{nullptr};

LevelRenderHook::LevelRenderHook(
    ll::mod::NativeMod& mod,
    HeartbeatHook& heartbeat,
    LevelRenderBus& eventBus,
    physics::PhysicsScheduler& physicsScheduler) noexcept
    : mMod(mod), mHeartbeat(heartbeat), mEventBus(eventBus),
      mPhysicsScheduler(physicsScheduler) {}

LevelRenderHook::~LevelRenderHook() { uninstall(); }

bool LevelRenderHook::install() {
    if (mWorker.joinable()) return true;
    mStatusPath = mMod.getDataDir() / "renderdragon-discovery-status.txt";
    mTimelinePath = mMod.getDataDir() / "renderdragon-discovery-timeline.csv";
    mStopRequested.store(false, std::memory_order_release);
    mRestoreSucceeded.store(false, std::memory_order_release);
    mCallbacksInFlight.store(0, std::memory_order_relaxed);
    mMinecraftPreCalls.store(0, std::memory_order_relaxed);
    mMinecraftPostCalls.store(0, std::memory_order_relaxed);
    mChunkLayerTaskCalls.store(0, std::memory_order_relaxed);
    mMinecraftThreadId.store(0, std::memory_order_relaxed);
    mChunkLayerThreadId.store(0, std::memory_order_relaxed);
    mOtherMinecraftThreadCalls.store(0, std::memory_order_relaxed);
    mOtherChunkLayerThreadCalls.store(0, std::memory_order_relaxed);
    mFirstRenderer.store(0, std::memory_order_relaxed);
    mLastContext.store(0, std::memory_order_relaxed);
    mLastView.store(0, std::memory_order_relaxed);
    mLastClient.store(0, std::memory_order_relaxed);
    mFirstChunkLayerClosure.store(0, std::memory_order_relaxed);
    mLastChunkLayerClosure.store(0, std::memory_order_relaxed);
    mLastChunkLayerTaskContext.store(0, std::memory_order_relaxed);
    mClosureQword220.store(0, std::memory_order_relaxed);
    mClosureQword228.store(0, std::memory_order_relaxed);
    mClosureQword230.store(0, std::memory_order_relaxed);
    mClosureQword238.store(0, std::memory_order_relaxed);
    mClosureDword240.store(0, std::memory_order_relaxed);
    mClosureQword248.store(0, std::memory_order_relaxed);
    mClosureQword250.store(0, std::memory_order_relaxed);
    mClosureQword258.store(0, std::memory_order_relaxed);
    mClosureFloat260.store(0.0F, std::memory_order_relaxed);
    mFingerprintValidated.store(false, std::memory_order_relaxed);
    mMinecraftPrefixValidated.store(false, std::memory_order_relaxed);
    mChunkLayerPrefixValidated.store(false, std::memory_order_relaxed);
    mFailureReason.clear();
    resetDiscovery();
    createTimeline();

    LevelRenderHook* expected = nullptr;
    if (!sActive.compare_exchange_strong(expected, this, std::memory_order_acq_rel)) {
        mFailureReason = "another RenderDragon discovery hook is active";
        writeStatus("registration_failed");
        return false;
    }
    writeStatus("waiting_for_stable_heartbeat");
    try {
        mWorker = std::thread(&LevelRenderHook::workerLoop, this);
    } catch (...) {
        sActive.store(nullptr, std::memory_order_release);
        mFailureReason = "failed to start RenderDragon discovery worker";
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
        const std::uint64_t current = mHeartbeat.callCount();
        stableSeconds = current > previous ? stableSeconds + 1U : 0U;
        previous = current;
    }
    if (mStopRequested.load(std::memory_order_acquire)) return;
    if (!installHooks()) {
        writeStatus("hook_install_failed");
        appendTimeline("hook_install_failed");
        return;
    }
    while (!mStopRequested.load(std::memory_order_acquire)) {
        writeStatus("running_renderdragon_discovery");
        appendTimeline("running");
        std::this_thread::sleep_for(1s);
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
    const std::uintptr_t minecraftAddress = module->loadBase + minecraftRenderOffset;
    if (readInstructionPrefix(*module, minecraftAddress, minecraftRenderPrefix.size()) !=
        formatPrefix(minecraftRenderPrefix)) {
        mFailureReason = "LevelRendererCamera::render prefix mismatch";
        return false;
    }
    mMinecraftPrefixValidated.store(true, std::memory_order_release);
    const std::uintptr_t chunkAddress = module->loadBase + chunkLayerTaskOffset;
    if (readInstructionPrefix(*module, chunkAddress, chunkLayerTaskPrefix.size()) !=
        formatPrefix(chunkLayerTaskPrefix)) {
        mFailureReason = "Renderers::_renderChunkLayer task closure prefix mismatch";
        return false;
    }
    mChunkLayerPrefixValidated.store(true, std::memory_order_release);
    mMinecraftOriginalStorage = nullptr;
    mChunkLayerOriginalStorage = nullptr;

    mChunkLayerHook = pl::memory::HookHandle(
        reinterpret_cast<pl::memory::FuncPtr>(chunkAddress),
        reinterpret_cast<pl::memory::FuncPtr>(&chunkLayerTaskDetour),
        &mChunkLayerOriginalStorage, pl::memory::HookPriority::Low);
    if (!mChunkLayerHook.installed() || mChunkLayerOriginalStorage == nullptr) {
        mChunkLayerHook.reset();
        mFailureReason = "preloader hook failed for RenderDragon chunk-layer task";
        return false;
    }
    gOriginalChunkLayerTask = reinterpret_cast<ChunkLayerTaskFn>(
        mChunkLayerOriginalStorage);

    mMinecraftHook = pl::memory::HookHandle(
        reinterpret_cast<pl::memory::FuncPtr>(minecraftAddress),
        reinterpret_cast<pl::memory::FuncPtr>(&minecraftRenderDetour),
        &mMinecraftOriginalStorage, pl::memory::HookPriority::Low);
    if (!mMinecraftHook.installed() || mMinecraftOriginalStorage == nullptr) {
        mFailureReason = "preloader hook failed for LevelRendererCamera::render";
        removeHooks();
        return false;
    }
    gOriginalMinecraftRender = reinterpret_cast<MinecraftRenderFn>(
        mMinecraftOriginalStorage);
    return true;
}

void LevelRenderHook::recordMinecraftRenderPre(
    void* renderer, void* context, const void* view, void* client) noexcept {
    LevelRenderHook* self = sActive.load(std::memory_order_acquire);
    if (self == nullptr) return;
    self->mCallbacksInFlight.fetch_add(1, std::memory_order_acq_rel);
    self->mMinecraftPreCalls.fetch_add(1, std::memory_order_relaxed);
    const std::uint32_t thread = currentThreadId();
    std::uint32_t expected = 0;
    self->mMinecraftThreadId.compare_exchange_strong(expected, thread);
    if (expected != 0 && expected != thread) {
        self->mOtherMinecraftThreadCalls.fetch_add(1, std::memory_order_relaxed);
    }
    std::uintptr_t empty = 0;
    self->mFirstRenderer.compare_exchange_strong(
        empty, reinterpret_cast<std::uintptr_t>(renderer));
    self->mLastContext.store(reinterpret_cast<std::uintptr_t>(context),
                             std::memory_order_relaxed);
    self->mLastView.store(reinterpret_cast<std::uintptr_t>(view),
                          std::memory_order_relaxed);
    self->mLastClient.store(reinterpret_cast<std::uintptr_t>(client),
                            std::memory_order_relaxed);
    const RawCameraSnapshot snapshot = captureCamera(context);
    publishCamera(gPreSnapshot, snapshot);
    (snapshot.valid ? gPreValid : gPreInvalid).fetch_add(
        1, std::memory_order_relaxed);
}

void LevelRenderHook::recordMinecraftRenderPost(
    void* renderer, void* context, const void* view, void* client) noexcept {
    LevelRenderHook* self = sActive.load(std::memory_order_acquire);
    if (self == nullptr) return;
    const RawCameraSnapshot previous = readCamera(gPostSnapshot);
    const RawCameraSnapshot snapshot = captureCamera(context);
    publishCamera(gPostSnapshot, snapshot);
    if (snapshot.valid) {
        gPostValid.fetch_add(1, std::memory_order_relaxed);
        updateMatrixChanges(previous, snapshot);
    } else {
        gPostInvalid.fetch_add(1, std::memory_order_relaxed);
    }
    const std::uint64_t sequence = self->mMinecraftPostCalls.fetch_add(
        1, std::memory_order_relaxed) + 1U;
    const LevelRenderEvent event{
        renderer, context, view, client, sequence, currentThreadId()};
    (void)self->mEventBus.publish(event);
    self->mCallbacksInFlight.fetch_sub(1, std::memory_order_acq_rel);
}

void LevelRenderHook::recordChunkLayerTaskBegin(
    const void* closure, const void* taskContext) noexcept {
    LevelRenderHook* self = sActive.load(std::memory_order_acquire);
    if (self == nullptr) return;
    self->mCallbacksInFlight.fetch_add(1, std::memory_order_acq_rel);
    self->mChunkLayerTaskCalls.fetch_add(1, std::memory_order_relaxed);
    const std::uint32_t thread = currentThreadId();
    std::uint32_t expected = 0;
    self->mChunkLayerThreadId.compare_exchange_strong(expected, thread);
    if (expected != 0 && expected != thread) {
        self->mOtherChunkLayerThreadCalls.fetch_add(1, std::memory_order_relaxed);
    }
    std::uintptr_t empty = 0;
    self->mFirstChunkLayerClosure.compare_exchange_strong(
        empty, reinterpret_cast<std::uintptr_t>(closure));
    self->mLastChunkLayerClosure.store(reinterpret_cast<std::uintptr_t>(closure),
                                       std::memory_order_relaxed);
    self->mLastChunkLayerTaskContext.store(
        reinterpret_cast<std::uintptr_t>(taskContext), std::memory_order_relaxed);
    const ClosureFields fields{
        readObject<std::uintptr_t>(closure, 0x220),
        readObject<std::uintptr_t>(closure, 0x228),
        readObject<std::uintptr_t>(closure, 0x230),
        readObject<std::uintptr_t>(closure, 0x238),
        readObject<std::uint32_t>(closure, 0x240),
        readObject<std::uint64_t>(closure, 0x248),
        readObject<std::uint64_t>(closure, 0x250),
        readObject<std::uintptr_t>(closure, 0x258),
        readObject<float>(closure, 0x260)};
    self->mClosureQword220.store(fields.q220, std::memory_order_relaxed);
    self->mClosureQword228.store(fields.q228, std::memory_order_relaxed);
    self->mClosureQword230.store(fields.q230, std::memory_order_relaxed);
    self->mClosureQword238.store(fields.q238, std::memory_order_relaxed);
    self->mClosureDword240.store(fields.d240, std::memory_order_relaxed);
    self->mClosureQword248.store(fields.q248, std::memory_order_relaxed);
    self->mClosureQword250.store(fields.q250, std::memory_order_relaxed);
    self->mClosureQword258.store(fields.q258, std::memory_order_relaxed);
    self->mClosureFloat260.store(fields.f260, std::memory_order_relaxed);
    recordClosureSample(fields);
}

void LevelRenderHook::recordChunkLayerTaskEnd() noexcept {
    LevelRenderHook* self = sActive.load(std::memory_order_acquire);
    if (self != nullptr) {
        self->mCallbacksInFlight.fetch_sub(1, std::memory_order_acq_rel);
    }
}

void LevelRenderHook::createTimeline() noexcept {
    std::ofstream out(mTimelinePath, std::ios::trunc);
    if (!out) return;
    out << "timestamp_unix_ms,state,minecraft_pre_calls,minecraft_post_calls,"
           "chunk_layer_task_calls,minecraft_thread_id,chunk_layer_thread_id";
    for (const char* phase : {"pre", "post"}) {
        out << ',' << phase << "_render_state," << phase << "_camera_object,"
            << phase << "_valid," << phase << "_origin_x,"
            << phase << "_origin_y," << phase << "_origin_z";
        for (std::size_t i = 0; i < allMatrixFloatCount; ++i) {
            out << ',' << phase << "_matrix_value_" << i;
        }
    }
    out << ",closure_0x220,closure_0x228,closure_0x230,closure_0x238,"
           "closure_0x240,closure_0x248,closure_0x250,closure_0x258,"
           "closure_float_0x260\n";
}

void LevelRenderHook::appendTimeline(const char* state) noexcept {
    const RawCameraSnapshot pre = readCamera(gPreSnapshot);
    const RawCameraSnapshot post = readCamera(gPostSnapshot);
    std::ofstream out(mTimelinePath, std::ios::app);
    if (!out) return;
    out << std::fixed << std::setprecision(7)
        << unixMillisecondsNow() << ',' << state
        << ',' << mMinecraftPreCalls.load(std::memory_order_relaxed)
        << ',' << mMinecraftPostCalls.load(std::memory_order_relaxed)
        << ',' << mChunkLayerTaskCalls.load(std::memory_order_relaxed)
        << ',' << mMinecraftThreadId.load(std::memory_order_relaxed)
        << ',' << mChunkLayerThreadId.load(std::memory_order_relaxed);
    appendCameraCsv(out, pre);
    appendCameraCsv(out, post);
    out << ',' << mClosureQword220.load(std::memory_order_relaxed)
        << ',' << mClosureQword228.load(std::memory_order_relaxed)
        << ',' << mClosureQword230.load(std::memory_order_relaxed)
        << ',' << mClosureQword238.load(std::memory_order_relaxed)
        << ',' << mClosureDword240.load(std::memory_order_relaxed)
        << ',' << mClosureQword248.load(std::memory_order_relaxed)
        << ',' << mClosureQword250.load(std::memory_order_relaxed)
        << ',' << mClosureQword258.load(std::memory_order_relaxed)
        << ',' << mClosureFloat260.load(std::memory_order_relaxed) << '\n';
}

void LevelRenderHook::writeStatus(const char* state) noexcept {
    const RawCameraSnapshot pre = readCamera(gPreSnapshot);
    const RawCameraSnapshot post = readCamera(gPostSnapshot);
    const auto physics = mPhysicsScheduler.renderSnapshot();
    std::ofstream out(mStatusPath, std::ios::trunc);
    if (!out) return;
    out << std::boolalpha << std::fixed << std::setprecision(7)
        << "schema=8\nstate=" << state
        << "\nsource=renderdragon_chunk_layer_discovery"
        << "\nread_only=true"
        << "\nvisible_geometry_expected=false"
        << "\ngeometry_submission=none_discovery_only"
        << "\nminecraft_owned_submission=not_attempted"
        << "\nminecraft_target=LevelRendererCamera::render+0xbd6f97c"
        << "\nchunk_layer_target=Renderers::_renderChunkLayer_task_closure_operator+0x126d8cc0"
        << "\ncamera_pointer_chain=BaseActorRenderContext+0x28_to_render_state_plus_0x18"
        << "\ncamera_origin_offset=0x13c"
        << "\ncamera_matrix_block_offset=0x158"
        << "\ncamera_matrix_count=3"
        << "\ncamera_matrix_stride_bytes=64"
        << "\ncamera_matrix_layout=unknown_raw_float_order"
        << "\nclosure_fields=verified_load_offsets_semantics_pending\n";
    out << "fingerprint_validated=" << mFingerprintValidated.load() << '\n'
        << "minecraft_prefix_validated=" << mMinecraftPrefixValidated.load() << '\n'
        << "chunk_layer_prefix_validated=" << mChunkLayerPrefixValidated.load() << '\n'
        << "minecraft_hook_installed=" << mMinecraftHook.installed() << '\n'
        << "chunk_layer_hook_installed=" << mChunkLayerHook.installed() << '\n'
        << "minecraft_pre_calls=" << mMinecraftPreCalls.load() << '\n'
        << "minecraft_post_calls=" << mMinecraftPostCalls.load() << '\n'
        << "chunk_layer_task_calls=" << mChunkLayerTaskCalls.load() << '\n'
        << "pre_camera_valid_samples=" << gPreValid.load() << '\n'
        << "pre_camera_invalid_samples=" << gPreInvalid.load() << '\n'
        << "post_camera_valid_samples=" << gPostValid.load() << '\n'
        << "post_camera_invalid_samples=" << gPostInvalid.load() << '\n'
        << "origin_changed_frames=" << gOriginChangedFrames.load() << '\n';
    for (std::size_t matrix = 0; matrix < cameraMatrixCount; ++matrix) {
        out << "matrix" << matrix << "_changed_frames="
            << gMatrixChangedFrames[matrix].load() << '\n'
            << "matrix" << matrix << "_changed_elements="
            << gMatrixChangedElements[matrix].load() << '\n'
            << "matrix" << matrix << "_maximum_delta="
            << gMatrixMaximumDelta[matrix].load() << '\n';
    }
    out << "pre_render_state=0x" << std::hex << pre.renderState
        << "\npre_camera_object=0x" << pre.cameraObject << std::dec
        << "\npre_snapshot_valid=" << pre.valid
        << "\npre_origin=" << pre.origin[0] << ',' << pre.origin[1] << ',' << pre.origin[2]
        << "\npost_render_state=0x" << std::hex << post.renderState
        << "\npost_camera_object=0x" << post.cameraObject << std::dec
        << "\npost_snapshot_valid=" << post.valid
        << "\npost_origin=" << post.origin[0] << ',' << post.origin[1] << ',' << post.origin[2]
        << '\n';
    writeMatrixLines(out, "pre", pre);
    writeMatrixLines(out, "post", post);
    out << "closure_qword_0x220=0x" << std::hex << mClosureQword220.load()
        << "\nclosure_qword_0x228=0x" << mClosureQword228.load()
        << "\nclosure_qword_0x230=0x" << mClosureQword230.load()
        << "\nclosure_qword_0x238=0x" << mClosureQword238.load()
        << std::dec << "\nclosure_dword_0x240=" << mClosureDword240.load()
        << "\nclosure_qword_0x248=0x" << std::hex << mClosureQword248.load()
        << "\nclosure_qword_0x250=0x" << mClosureQword250.load()
        << "\nclosure_qword_0x258=0x" << mClosureQword258.load()
        << std::dec << "\nclosure_float_0x260=" << mClosureFloat260.load() << '\n';
    std::size_t unique = 0;
    for (const auto& sample : gClosureSamples) {
        if (sample.key.load(std::memory_order_acquire) >= 2) ++unique;
    }
    out << "closure_unique_samples=" << unique << '\n'
        << "closure_sample_overflow=" << gClosureSampleOverflow.load() << '\n';
    for (std::size_t i = 0; i < gClosureSamples.size(); ++i) {
        const auto& sample = gClosureSamples[i];
        const std::uint64_t key = sample.key.load(std::memory_order_acquire);
        if (key < 2) continue;
        out << "closure_sample_" << i << "=" << std::hex << key << std::dec
            << ',' << sample.count.load() << ',' << sample.d240.load()
            << ",0x" << std::hex << sample.q248.load()
            << ",0x" << sample.q250.load() << std::dec
            << ',' << sample.f260.load() << '\n';
    }
    out << "minecraft_render_thread_id=" << mMinecraftThreadId.load() << '\n'
        << "chunk_layer_thread_id=" << mChunkLayerThreadId.load() << '\n'
        << "threads_match=" << (mMinecraftThreadId.load() != 0 &&
             mMinecraftThreadId.load() == mChunkLayerThreadId.load()) << '\n'
        << "other_minecraft_thread_calls=" << mOtherMinecraftThreadCalls.load() << '\n'
        << "other_chunk_layer_thread_calls=" << mOtherChunkLayerThreadCalls.load() << '\n'
        << "first_renderer=0x" << std::hex << mFirstRenderer.load()
        << "\nlast_render_context=0x" << mLastContext.load()
        << "\nlast_view=0x" << mLastView.load()
        << "\nlast_client=0x" << mLastClient.load()
        << "\nfirst_chunk_layer_closure=0x" << mFirstChunkLayerClosure.load()
        << "\nlast_chunk_layer_closure=0x" << mLastChunkLayerClosure.load()
        << "\nlast_chunk_layer_task_context=0x" << mLastChunkLayerTaskContext.load()
        << std::dec << "\nevent_bus_published_events=" << mEventBus.publishedEvents()
        << "\nevent_bus_delivered_callbacks=" << mEventBus.deliveredCallbacks()
        << "\nphysics_snapshot_coherent=" << physics.coherent
        << "\nphysics_world_generation=" << physics.worldGeneration
        << "\nphysics_simulation_step=" << physics.simulationStep
        << "\ncallbacks_in_flight=" << mCallbacksInFlight.load()
        << "\nhook_restore_succeeded=" << mRestoreSucceeded.load()
        << "\nsafe_to_unload=" << safeToUnload()
        << "\nstatus_file=renderdragon-discovery-status.txt"
        << "\ntimeline_file=renderdragon-discovery-timeline.csv"
        << "\nfailure_reason=" << (mFailureReason.empty() ? "none" : mFailureReason)
        << '\n';
}

void LevelRenderHook::removeHooks() noexcept {
    mMinecraftHook.reset();
    mChunkLayerHook.reset();
    using namespace std::chrono_literals;
    for (unsigned attempt = 0; attempt < 200U &&
         mCallbacksInFlight.load(std::memory_order_acquire) != 0; ++attempt) {
        std::this_thread::sleep_for(5ms);
    }
    gOriginalMinecraftRender = nullptr;
    gOriginalChunkLayerTask = nullptr;
    mRestoreSucceeded.store(
        !mMinecraftHook.installed() && !mChunkLayerHook.installed() &&
        mCallbacksInFlight.load(std::memory_order_acquire) == 0,
        std::memory_order_release);
}

void LevelRenderHook::uninstall() noexcept {
    mStopRequested.store(true, std::memory_order_release);
    if (mWorker.joinable()) mWorker.join();
    removeHooks();
    LevelRenderHook* expected = this;
    (void)sActive.compare_exchange_strong(
        expected, nullptr, std::memory_order_acq_rel);
    appendTimeline("stopped");
    writeStatus("stopped");
}

bool LevelRenderHook::safeToUnload() const noexcept {
    return mRestoreSucceeded.load(std::memory_order_acquire) &&
           !mMinecraftHook.installed() && !mChunkLayerHook.installed() &&
           mCallbacksInFlight.load(std::memory_order_acquire) == 0;
}

} // namespace aeronautics::bedrock
