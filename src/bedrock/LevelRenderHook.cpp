#include "bedrock/LevelRenderHook.hpp"

#include "bedrock/CompatibilityProfile.hpp"
#include "bedrock/HeartbeatHook.hpp"
#include "physics/PhysicsScheduler.hpp"

#include <algorithm>
#include <array>
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
// LevelRendererCameraAnon::framebuilderInsertTerrainCommandsForChunks::$_0::operator()
constexpr std::uintptr_t terrainTaskOffset = 0x0bdb87b8;
constexpr std::array<std::uint8_t, 16> terrainTaskPrefix{
    0xff, 0x03, 0x07, 0xd1, 0xfd, 0x7b, 0x18, 0xa9,
    0xfc, 0x5f, 0x19, 0xa9, 0xf6, 0x57, 0x1a, 0xa9};
constexpr std::size_t contextRenderStateOffset = 0x28;
constexpr std::size_t renderStateCameraObjectOffset = 0x18;
constexpr std::size_t cameraRelativeOriginOffset = 0x13c;
constexpr std::size_t cameraFrustumOffset = 0x158;
constexpr std::size_t planeCount = 6;
constexpr std::size_t planeFloats = 4;
constexpr std::size_t cornerCount = 8;
constexpr std::size_t cornerFloats = 3;
constexpr std::size_t frustumFloats = planeCount * planeFloats + cornerCount * cornerFloats;
constexpr std::size_t viewCandidateCount = 6;
constexpr std::size_t viewFloats = viewCandidateCount * 3;
constexpr float epsilon = 0.00001F;
constexpr float pi = 3.14159265358979323846F;

using MinecraftRenderFn = void (*)(void*, void*, const void*, void*);
using TerrainTaskFn = void (*)(void*, void*);
MinecraftRenderFn gOriginalMinecraftRender = nullptr;
TerrainTaskFn gOriginalTerrainTask = nullptr;

struct DerivedFrustum final {
    std::array<float, 3> forward{};
    float nearDistance{};
    float farDistance{};
    float horizontalFov{};
    float verticalFov{};
    float aspect{};
    float minPlaneLength{};
    float maxPlaneLength{};
    bool valid{};
};

struct CameraSnapshot final {
    std::uintptr_t renderState{};
    std::uintptr_t cameraObject{};
    std::array<float, 3> relativeOrigin{};
    std::array<float, frustumFloats> frustum{};
    std::array<float, viewFloats> view{};
    DerivedFrustum derived{};
    bool valid{};
};

struct AtomicSnapshot final {
    std::atomic<std::uint64_t> sequence{0};
    std::atomic<std::uintptr_t> renderState{0};
    std::atomic<std::uintptr_t> cameraObject{0};
    std::array<std::atomic<float>, 3> relativeOrigin{};
    std::array<std::atomic<float>, frustumFloats> frustum{};
    std::array<std::atomic<float>, viewFloats> view{};
    std::atomic_bool valid{false};
};

struct TerrainFields final {
    std::array<std::uintptr_t, 8> qwords{};
    std::uint8_t flag{};
};

AtomicSnapshot gSnapshot;
std::atomic<std::uint64_t> gPreValid{0};
std::atomic<std::uint64_t> gPreInvalid{0};
std::atomic<std::uint64_t> gPostValid{0};
std::atomic<std::uint64_t> gPostInvalid{0};
std::atomic<std::uint64_t> gOriginChanged{0};
std::atomic<std::uint64_t> gFrustumChanged{0};
std::atomic<std::uint64_t> gFrustumChangedElements{0};
std::atomic<float> gFrustumMaxDelta{0.0F};
std::array<std::atomic<std::uint64_t>, viewCandidateCount> gViewChanged{};
std::array<std::atomic<float>, viewCandidateCount> gViewMaxDelta{};
std::array<std::atomic<std::uintptr_t>, 8> gTerrainQwords{};
std::atomic<std::uint8_t> gTerrainFlag{0};
std::array<std::atomic<std::uint64_t>, 8> gTerrainFlagCounts{};
std::atomic<std::uint64_t> gTerrainOtherFlagCount{0};

[[nodiscard]] std::uint32_t currentThreadId() noexcept {
    const long raw = ::syscall(SYS_gettid);
    if (raw <= 0 || static_cast<unsigned long>(raw) >
            static_cast<unsigned long>(std::numeric_limits<std::uint32_t>::max())) return 0;
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

[[nodiscard]] float length3(const float* value) noexcept {
    const double squared = static_cast<double>(value[0]) * value[0] +
        static_cast<double>(value[1]) * value[1] +
        static_cast<double>(value[2]) * value[2];
    return squared > 0.0 ? static_cast<float>(std::sqrt(squared)) : 0.0F;
}

[[nodiscard]] float distance3(const float* lhs, const float* rhs) noexcept {
    const double x = static_cast<double>(lhs[0]) - rhs[0];
    const double y = static_cast<double>(lhs[1]) - rhs[1];
    const double z = static_cast<double>(lhs[2]) - rhs[2];
    return static_cast<float>(std::sqrt(x * x + y * y + z * z));
}

[[nodiscard]] DerivedFrustum deriveFrustum(
    const std::array<float, frustumFloats>& value) noexcept {
    DerivedFrustum out{};
    out.minPlaneLength = std::numeric_limits<float>::max();
    for (std::size_t plane = 0; plane < planeCount; ++plane) {
        const float length = length3(value.data() + plane * planeFloats);
        if (!std::isfinite(length)) return {};
        out.minPlaneLength = std::min(out.minPlaneLength, length);
        out.maxPlaneLength = std::max(out.maxPlaneLength, length);
    }
    constexpr std::size_t cornerStart = planeCount * planeFloats;
    std::array<float, 3> nearCenter{};
    std::array<float, 3> farCenter{};
    for (std::size_t corner = 0; corner < cornerCount; ++corner) {
        const float* source = value.data() + cornerStart + corner * cornerFloats;
        for (std::size_t axis = 0; axis < 3; ++axis) {
            if (!std::isfinite(source[axis])) return {};
            (corner < 4 ? nearCenter : farCenter)[axis] += source[axis] * 0.25F;
        }
    }
    out.nearDistance = length3(nearCenter.data());
    out.farDistance = length3(farCenter.data());
    if (!(out.nearDistance > 0.0001F && out.farDistance > out.nearDistance)) return {};
    for (std::size_t axis = 0; axis < 3; ++axis) {
        out.forward[axis] = nearCenter[axis] / out.nearDistance;
    }
    std::array<float, 6> pairDistances{};
    std::size_t pair = 0;
    for (std::size_t first = 0; first < 4; ++first) {
        for (std::size_t second = first + 1; second < 4; ++second) {
            pairDistances[pair++] = distance3(
                value.data() + cornerStart + first * cornerFloats,
                value.data() + cornerStart + second * cornerFloats);
        }
    }
    std::sort(pairDistances.begin(), pairDistances.end());
    const float height = pairDistances[1];
    const float width = pairDistances[3];
    if (!(width > 0.0F && height > 0.0F)) return {};
    out.horizontalFov = 2.0F * std::atan(width / (2.0F * out.nearDistance)) * 180.0F / pi;
    out.verticalFov = 2.0F * std::atan(height / (2.0F * out.nearDistance)) * 180.0F / pi;
    out.aspect = width / height;
    out.valid = out.minPlaneLength > 0.95F && out.maxPlaneLength < 1.05F &&
        out.horizontalFov > 20.0F && out.horizontalFov < 170.0F &&
        out.verticalFov > 10.0F && out.verticalFov < 150.0F &&
        out.aspect > 0.2F && out.aspect < 8.0F;
    return out;
}

[[nodiscard]] CameraSnapshot captureCamera(const void* context, const void* view) noexcept {
    CameraSnapshot out{};
    if (context == nullptr || view == nullptr) return out;
    const void* renderState = readObject<const void*>(context, contextRenderStateOffset);
    if (renderState == nullptr) return out;
    const void* cameraObject = readObject<const void*>(renderState, renderStateCameraObjectOffset);
    if (cameraObject == nullptr) return out;
    out.renderState = reinterpret_cast<std::uintptr_t>(renderState);
    out.cameraObject = reinterpret_cast<std::uintptr_t>(cameraObject);
    out.relativeOrigin = readObject<std::array<float, 3>>(
        cameraObject, cameraRelativeOriginOffset);
    out.frustum = readObject<std::array<float, frustumFloats>>(
        cameraObject, cameraFrustumOffset);
    out.view = readObject<std::array<float, viewFloats>>(view, 0);
    bool finite = true;
    for (float item : out.relativeOrigin) finite = finite && std::isfinite(item);
    for (float item : out.frustum) finite = finite && std::isfinite(item);
    for (float item : out.view) finite = finite && std::isfinite(item);
    out.derived = deriveFrustum(out.frustum);
    out.valid = finite && out.derived.valid;
    return out;
}

void publishSnapshot(const CameraSnapshot& source) noexcept {
    gSnapshot.sequence.fetch_add(1, std::memory_order_acq_rel);
    gSnapshot.renderState.store(source.renderState, std::memory_order_relaxed);
    gSnapshot.cameraObject.store(source.cameraObject, std::memory_order_relaxed);
    for (std::size_t i = 0; i < source.relativeOrigin.size(); ++i)
        gSnapshot.relativeOrigin[i].store(source.relativeOrigin[i], std::memory_order_relaxed);
    for (std::size_t i = 0; i < source.frustum.size(); ++i)
        gSnapshot.frustum[i].store(source.frustum[i], std::memory_order_relaxed);
    for (std::size_t i = 0; i < source.view.size(); ++i)
        gSnapshot.view[i].store(source.view[i], std::memory_order_relaxed);
    gSnapshot.valid.store(source.valid, std::memory_order_relaxed);
    gSnapshot.sequence.fetch_add(1, std::memory_order_release);
}

[[nodiscard]] CameraSnapshot readSnapshot() noexcept {
    CameraSnapshot out{};
    for (unsigned attempt = 0; attempt < 8U; ++attempt) {
        const auto before = gSnapshot.sequence.load(std::memory_order_acquire);
        if ((before & 1U) != 0U) continue;
        out.renderState = gSnapshot.renderState.load(std::memory_order_relaxed);
        out.cameraObject = gSnapshot.cameraObject.load(std::memory_order_relaxed);
        for (std::size_t i = 0; i < out.relativeOrigin.size(); ++i)
            out.relativeOrigin[i] = gSnapshot.relativeOrigin[i].load(std::memory_order_relaxed);
        for (std::size_t i = 0; i < out.frustum.size(); ++i)
            out.frustum[i] = gSnapshot.frustum[i].load(std::memory_order_relaxed);
        for (std::size_t i = 0; i < out.view.size(); ++i)
            out.view[i] = gSnapshot.view[i].load(std::memory_order_relaxed);
        out.valid = gSnapshot.valid.load(std::memory_order_relaxed);
        const auto after = gSnapshot.sequence.load(std::memory_order_acquire);
        if (before == after && (after & 1U) == 0U) {
            out.derived = deriveFrustum(out.frustum);
            out.valid = out.valid && out.derived.valid;
            return out;
        }
    }
    return {};
}

void updateMaximum(std::atomic<float>& target, float candidate) noexcept {
    float current = target.load(std::memory_order_relaxed);
    while (candidate > current && !target.compare_exchange_weak(
        current, candidate, std::memory_order_relaxed, std::memory_order_relaxed)) {}
}

void updateChanges(const CameraSnapshot& previous, const CameraSnapshot& current) noexcept {
    if (!previous.valid || !current.valid) return;
    bool originChanged = false;
    for (std::size_t axis = 0; axis < 3; ++axis)
        originChanged |= std::abs(current.relativeOrigin[axis] - previous.relativeOrigin[axis]) > epsilon;
    if (originChanged) gOriginChanged.fetch_add(1, std::memory_order_relaxed);
    std::uint64_t changed = 0;
    float maximum = 0.0F;
    for (std::size_t i = 0; i < current.frustum.size(); ++i) {
        const float delta = std::abs(current.frustum[i] - previous.frustum[i]);
        maximum = std::max(maximum, delta);
        if (delta > epsilon) ++changed;
    }
    if (changed != 0) {
        gFrustumChanged.fetch_add(1, std::memory_order_relaxed);
        gFrustumChangedElements.fetch_add(changed, std::memory_order_relaxed);
    }
    updateMaximum(gFrustumMaxDelta, maximum);
    for (std::size_t candidate = 0; candidate < viewCandidateCount; ++candidate) {
        bool candidateChanged = false;
        float candidateMaximum = 0.0F;
        for (std::size_t axis = 0; axis < 3; ++axis) {
            const std::size_t index = candidate * 3 + axis;
            const float delta = std::abs(current.view[index] - previous.view[index]);
            candidateChanged |= delta > epsilon;
            candidateMaximum = std::max(candidateMaximum, delta);
        }
        if (candidateChanged) gViewChanged[candidate].fetch_add(1, std::memory_order_relaxed);
        updateMaximum(gViewMaxDelta[candidate], candidateMaximum);
    }
}

[[nodiscard]] TerrainFields captureTerrain(const void* closure) noexcept {
    TerrainFields out{};
    if (closure == nullptr) return out;
    for (std::size_t i = 0; i < out.qwords.size(); ++i)
        out.qwords[i] = readObject<std::uintptr_t>(closure, 0x08 + i * 8);
    out.flag = readObject<std::uint8_t>(closure, 0x48);
    return out;
}

void resetDiscovery() noexcept {
    gSnapshot.sequence.store(0);
    gSnapshot.renderState.store(0);
    gSnapshot.cameraObject.store(0);
    gSnapshot.valid.store(false);
    for (auto& item : gSnapshot.relativeOrigin) item.store(0.0F);
    for (auto& item : gSnapshot.frustum) item.store(0.0F);
    for (auto& item : gSnapshot.view) item.store(0.0F);
    gPreValid.store(0); gPreInvalid.store(0); gPostValid.store(0); gPostInvalid.store(0);
    gOriginChanged.store(0); gFrustumChanged.store(0); gFrustumChangedElements.store(0);
    gFrustumMaxDelta.store(0.0F);
    for (std::size_t i = 0; i < viewCandidateCount; ++i) {
        gViewChanged[i].store(0); gViewMaxDelta[i].store(0.0F);
    }
    for (auto& item : gTerrainQwords) item.store(0);
    gTerrainFlag.store(0);
    for (auto& item : gTerrainFlagCounts) item.store(0);
    gTerrainOtherFlagCount.store(0);
}

[[nodiscard]] std::string formatPrefix(const std::array<std::uint8_t, 16>& prefix) {
    std::ostringstream out;
    out << std::hex << std::setfill('0');
    for (std::size_t i = 0; i < prefix.size(); ++i) {
        if (i != 0) out << ' ';
        out << std::setw(2) << static_cast<unsigned>(prefix[i]);
    }
    return out.str();
}

void minecraftRenderDetour(void* renderer, void* context, const void* view, void* client) {
    LevelRenderHook::recordMinecraftRenderPre(renderer, context, view, client);
    if (gOriginalMinecraftRender != nullptr) gOriginalMinecraftRender(renderer, context, view, client);
    LevelRenderHook::recordMinecraftRenderPost(renderer, context, view, client);
}

void terrainTaskDetour(void* closure, void* taskContext) {
    LevelRenderHook::recordTerrainTaskBegin(closure, taskContext);
    if (gOriginalTerrainTask != nullptr) gOriginalTerrainTask(closure, taskContext);
    LevelRenderHook::recordTerrainTaskEnd();
}
}  // namespace

std::atomic<LevelRenderHook*> LevelRenderHook::sActive{nullptr};

LevelRenderHook::LevelRenderHook(
    ll::mod::NativeMod& mod, HeartbeatHook& heartbeat, LevelRenderBus& eventBus,
    physics::PhysicsScheduler& physicsScheduler) noexcept
    : mMod(mod), mHeartbeat(heartbeat), mEventBus(eventBus), mPhysicsScheduler(physicsScheduler) {}
LevelRenderHook::~LevelRenderHook() { uninstall(); }

bool LevelRenderHook::install() {
    if (mWorker.joinable()) return true;
    mStatusPath = mMod.getDataDir() / "frustum-terrain-discovery-status.txt";
    mTimelinePath = mMod.getDataDir() / "frustum-terrain-discovery-timeline.csv";
    mStopRequested.store(false); mRestoreSucceeded.store(false); mCallbacksInFlight.store(0);
    mMinecraftPreCalls.store(0); mMinecraftPostCalls.store(0); mTerrainTaskCalls.store(0);
    mMinecraftThreadId.store(0); mTerrainThreadId.store(0);
    mOtherMinecraftThreadCalls.store(0); mOtherTerrainThreadCalls.store(0);
    mFirstRenderer.store(0); mLastContext.store(0); mLastView.store(0); mLastClient.store(0);
    mFirstTerrainClosure.store(0); mLastTerrainClosure.store(0); mLastTerrainTaskContext.store(0);
    mFingerprintValidated.store(false); mMinecraftPrefixValidated.store(false);
    mTerrainPrefixValidated.store(false); mFailureReason.clear();
    resetDiscovery(); createTimeline();
    LevelRenderHook* expected = nullptr;
    if (!sActive.compare_exchange_strong(expected, this)) {
        mFailureReason = "another frustum/terrain discovery hook is active";
        writeStatus("registration_failed"); return false;
    }
    writeStatus("waiting_for_stable_heartbeat");
    try { mWorker = std::thread(&LevelRenderHook::workerLoop, this); }
    catch (...) {
        sActive.store(nullptr); mFailureReason = "failed to start discovery worker";
        writeStatus("worker_start_failed"); return false;
    }
    return true;
}

void LevelRenderHook::workerLoop() noexcept {
    using namespace std::chrono_literals;
    std::uint64_t previous = mHeartbeat.callCount();
    unsigned stableSeconds = 0;
    while (!mStopRequested.load() && stableSeconds < 8U) {
        std::this_thread::sleep_for(1s);
        const auto current = mHeartbeat.callCount();
        stableSeconds = current > previous ? stableSeconds + 1U : 0U;
        previous = current;
    }
    if (mStopRequested.load()) return;
    if (!installHooks()) {
        writeStatus("hook_install_failed"); appendTimeline("hook_install_failed"); return;
    }
    while (!mStopRequested.load()) {
        writeStatus("running_frustum_terrain_discovery"); appendTimeline("running");
        std::this_thread::sleep_for(1s);
    }
}

bool LevelRenderHook::installHooks() noexcept {
    const auto module = inspectLoadedModule(CompatibilityProfile::moduleName);
    if (!module || module->buildId != expectedBuildId || module->fileSize != expectedModuleFileSize) {
        mFailureReason = "Minecraft binary fingerprint mismatch"; return false;
    }
    mFingerprintValidated.store(true);
    const auto minecraftAddress = module->loadBase + minecraftRenderOffset;
    if (readInstructionPrefix(*module, minecraftAddress, minecraftRenderPrefix.size()) !=
            formatPrefix(minecraftRenderPrefix)) {
        mFailureReason = "LevelRendererCamera::render prefix mismatch"; return false;
    }
    mMinecraftPrefixValidated.store(true);
    const auto terrainAddress = module->loadBase + terrainTaskOffset;
    if (readInstructionPrefix(*module, terrainAddress, terrainTaskPrefix.size()) !=
            formatPrefix(terrainTaskPrefix)) {
        mFailureReason = "framebuilder terrain task operator prefix mismatch"; return false;
    }
    mTerrainPrefixValidated.store(true);
    mTerrainHook = pl::memory::HookHandle(
        reinterpret_cast<pl::memory::FuncPtr>(terrainAddress),
        reinterpret_cast<pl::memory::FuncPtr>(&terrainTaskDetour),
        &mTerrainOriginalStorage, pl::memory::HookPriority::Low);
    if (!mTerrainHook.installed() || mTerrainOriginalStorage == nullptr) {
        mTerrainHook.reset(); mFailureReason = "active terrain task hook failed"; return false;
    }
    gOriginalTerrainTask = reinterpret_cast<TerrainTaskFn>(mTerrainOriginalStorage);
    mMinecraftHook = pl::memory::HookHandle(
        reinterpret_cast<pl::memory::FuncPtr>(minecraftAddress),
        reinterpret_cast<pl::memory::FuncPtr>(&minecraftRenderDetour),
        &mMinecraftOriginalStorage, pl::memory::HookPriority::Low);
    if (!mMinecraftHook.installed() || mMinecraftOriginalStorage == nullptr) {
        mFailureReason = "LevelRendererCamera::render hook failed"; removeHooks(); return false;
    }
    gOriginalMinecraftRender = reinterpret_cast<MinecraftRenderFn>(mMinecraftOriginalStorage);
    return true;
}

void LevelRenderHook::recordMinecraftRenderPre(
    void* renderer, void* context, const void* view, void* client) noexcept {
    auto* self = sActive.load(); if (self == nullptr) return;
    self->mCallbacksInFlight.fetch_add(1); self->mMinecraftPreCalls.fetch_add(1);
    const auto thread = currentThreadId(); std::uint32_t expected = 0;
    self->mMinecraftThreadId.compare_exchange_strong(expected, thread);
    if (expected != 0 && expected != thread) self->mOtherMinecraftThreadCalls.fetch_add(1);
    std::uintptr_t empty = 0;
    self->mFirstRenderer.compare_exchange_strong(empty, reinterpret_cast<std::uintptr_t>(renderer));
    self->mLastContext.store(reinterpret_cast<std::uintptr_t>(context));
    self->mLastView.store(reinterpret_cast<std::uintptr_t>(view));
    self->mLastClient.store(reinterpret_cast<std::uintptr_t>(client));
    const CameraSnapshot snapshot = captureCamera(context, view);
    (snapshot.valid ? gPreValid : gPreInvalid).fetch_add(1);
}

void LevelRenderHook::recordMinecraftRenderPost(
    void* renderer, void* context, const void* view, void* client) noexcept {
    auto* self = sActive.load(); if (self == nullptr) return;
    const CameraSnapshot previous = readSnapshot();
    const CameraSnapshot current = captureCamera(context, view);
    publishSnapshot(current);
    if (current.valid) { gPostValid.fetch_add(1); updateChanges(previous, current); }
    else gPostInvalid.fetch_add(1);
    const auto sequence = self->mMinecraftPostCalls.fetch_add(1) + 1U;
    const LevelRenderEvent event{renderer, context, view, client, sequence, currentThreadId()};
    (void)self->mEventBus.publish(event);
    self->mCallbacksInFlight.fetch_sub(1);
}

void LevelRenderHook::recordTerrainTaskBegin(
    const void* closure, const void* taskContext) noexcept {
    auto* self = sActive.load(); if (self == nullptr) return;
    self->mCallbacksInFlight.fetch_add(1); self->mTerrainTaskCalls.fetch_add(1);
    const auto thread = currentThreadId(); std::uint32_t expected = 0;
    self->mTerrainThreadId.compare_exchange_strong(expected, thread);
    if (expected != 0 && expected != thread) self->mOtherTerrainThreadCalls.fetch_add(1);
    std::uintptr_t empty = 0;
    self->mFirstTerrainClosure.compare_exchange_strong(
        empty, reinterpret_cast<std::uintptr_t>(closure));
    self->mLastTerrainClosure.store(reinterpret_cast<std::uintptr_t>(closure));
    self->mLastTerrainTaskContext.store(reinterpret_cast<std::uintptr_t>(taskContext));
    const TerrainFields fields = captureTerrain(closure);
    for (std::size_t i = 0; i < fields.qwords.size(); ++i) gTerrainQwords[i].store(fields.qwords[i]);
    gTerrainFlag.store(fields.flag);
    if (fields.flag < gTerrainFlagCounts.size()) gTerrainFlagCounts[fields.flag].fetch_add(1);
    else gTerrainOtherFlagCount.fetch_add(1);
}

void LevelRenderHook::recordTerrainTaskEnd() noexcept {
    auto* self = sActive.load(); if (self != nullptr) self->mCallbacksInFlight.fetch_sub(1);
}

void LevelRenderHook::createTimeline() noexcept {
    std::ofstream out(mTimelinePath, std::ios::trunc); if (!out) return;
    out << "timestamp_unix_ms,state,minecraft_pre_calls,minecraft_post_calls,terrain_task_calls,"
           "minecraft_thread_id,terrain_thread_id,valid,forward_x,forward_y,forward_z,near,far,"
           "horizontal_fov,vertical_fov,aspect";
    for (std::size_t i = 0; i < viewFloats; ++i) out << ",view_value_" << i;
    out << ",terrain_flag";
    for (std::size_t i = 0; i < 8; ++i) out << ",terrain_q" << i;
    out << '\n';
}

void LevelRenderHook::appendTimeline(const char* state) noexcept {
    const CameraSnapshot camera = readSnapshot();
    std::ofstream out(mTimelinePath, std::ios::app); if (!out) return;
    out << std::fixed << std::setprecision(7) << unixMillisecondsNow() << ',' << state
        << ',' << mMinecraftPreCalls.load() << ',' << mMinecraftPostCalls.load()
        << ',' << mTerrainTaskCalls.load() << ',' << mMinecraftThreadId.load()
        << ',' << mTerrainThreadId.load() << ',' << (camera.valid ? 1 : 0)
        << ',' << camera.derived.forward[0] << ',' << camera.derived.forward[1]
        << ',' << camera.derived.forward[2] << ',' << camera.derived.nearDistance
        << ',' << camera.derived.farDistance << ',' << camera.derived.horizontalFov
        << ',' << camera.derived.verticalFov << ',' << camera.derived.aspect;
    for (float value : camera.view) out << ',' << value;
    out << ',' << static_cast<unsigned>(gTerrainFlag.load());
    for (const auto& value : gTerrainQwords) out << ',' << value.load();
    out << '\n';
}

void LevelRenderHook::writeStatus(const char* state) noexcept {
    const CameraSnapshot camera = readSnapshot();
    const auto physics = mPhysicsScheduler.renderSnapshot();
    std::ofstream out(mStatusPath, std::ios::trunc); if (!out) return;
    out << std::boolalpha << std::fixed << std::setprecision(7)
        << "schema=9\nstate=" << state
        << "\nsource=frustum_active_terrain_discovery"
        << "\nread_only=true\nvisible_geometry_expected=false"
        << "\ngeometry_submission=none_discovery_only"
        << "\nminecraft_owned_submission=not_attempted"
        << "\nminecraft_target=LevelRendererCamera::render+0xbd6f97c"
        << "\nterrain_target=LevelRendererCameraAnon::framebuilderInsertTerrainCommandsForChunks_task_operator+0xbdb87b8"
        << "\nrejected_inactive_target=Renderers::_insertChunkLayer_task_operator+0x126d8cc0"
        << "\ncamera_pointer_chain=BaseActorRenderContext+0x28_to_render_state_plus_0x18"
        << "\ncamera_relative_origin_offset=0x13c\ncamera_frustum_offset=0x158"
        << "\ncamera_structure=6_plane_equations_plus_8_corner_vectors"
        << "\nprevious_three_matrix_assumption_rejected=true"
        << "\nview_candidate_layout=6_consecutive_vec3_from_ViewRenderObject_offset_0"
        << "\nterrain_closure_layout=qwords_0x08_through_0x40_plus_flag_0x48"
        << "\nfingerprint_validated=" << mFingerprintValidated.load()
        << "\nminecraft_prefix_validated=" << mMinecraftPrefixValidated.load()
        << "\nterrain_prefix_validated=" << mTerrainPrefixValidated.load()
        << "\nminecraft_hook_installed=" << mMinecraftHook.installed()
        << "\nterrain_hook_installed=" << mTerrainHook.installed()
        << "\nminecraft_pre_calls=" << mMinecraftPreCalls.load()
        << "\nminecraft_post_calls=" << mMinecraftPostCalls.load()
        << "\nterrain_task_calls=" << mTerrainTaskCalls.load()
        << "\npre_camera_valid_samples=" << gPreValid.load()
        << "\npre_camera_invalid_samples=" << gPreInvalid.load()
        << "\npost_camera_valid_samples=" << gPostValid.load()
        << "\npost_camera_invalid_samples=" << gPostInvalid.load()
        << "\nrelative_origin_changed_frames=" << gOriginChanged.load()
        << "\nfrustum_changed_frames=" << gFrustumChanged.load()
        << "\nfrustum_changed_elements=" << gFrustumChangedElements.load()
        << "\nfrustum_maximum_delta=" << gFrustumMaxDelta.load()
        << "\ncamera_snapshot_valid=" << camera.valid
        << "\ncamera_relative_origin=" << camera.relativeOrigin[0] << ','
        << camera.relativeOrigin[1] << ',' << camera.relativeOrigin[2]
        << "\nforward=" << camera.derived.forward[0] << ',' << camera.derived.forward[1]
        << ',' << camera.derived.forward[2]
        << "\nnear_distance=" << camera.derived.nearDistance
        << "\nfar_distance=" << camera.derived.farDistance
        << "\nhorizontal_fov_degrees=" << camera.derived.horizontalFov
        << "\nvertical_fov_degrees=" << camera.derived.verticalFov
        << "\naspect_ratio=" << camera.derived.aspect
        << "\nminimum_plane_normal_length=" << camera.derived.minPlaneLength
        << "\nmaximum_plane_normal_length=" << camera.derived.maxPlaneLength << '\n';
    for (std::size_t i = 0; i < viewCandidateCount; ++i) {
        out << "view_candidate_" << i << '=' << camera.view[i * 3] << ','
            << camera.view[i * 3 + 1] << ',' << camera.view[i * 3 + 2] << '\n'
            << "view_candidate_" << i << "_changed_frames=" << gViewChanged[i].load() << '\n'
            << "view_candidate_" << i << "_maximum_delta=" << gViewMaxDelta[i].load() << '\n';
    }
    for (std::size_t plane = 0; plane < planeCount; ++plane) {
        out << "frustum_plane_" << plane << '=';
        for (std::size_t component = 0; component < planeFloats; ++component) {
            if (component != 0) out << ',';
            out << camera.frustum[plane * planeFloats + component];
        }
        out << '\n';
    }
    constexpr std::size_t cornerStart = planeCount * planeFloats;
    for (std::size_t corner = 0; corner < cornerCount; ++corner) {
        out << "frustum_corner_" << corner << '=';
        for (std::size_t component = 0; component < cornerFloats; ++component) {
            if (component != 0) out << ',';
            out << camera.frustum[cornerStart + corner * cornerFloats + component];
        }
        out << '\n';
    }
    out << std::hex;
    for (std::size_t i = 0; i < gTerrainQwords.size(); ++i)
        out << "terrain_qword_0x" << std::setw(2) << std::setfill('0') << (8 + i * 8)
            << "=0x" << gTerrainQwords[i].load() << '\n';
    out << std::dec << std::setfill(' ')
        << "terrain_flag_0x48=" << static_cast<unsigned>(gTerrainFlag.load()) << '\n';
    for (std::size_t i = 0; i < gTerrainFlagCounts.size(); ++i)
        out << "terrain_flag_" << i << "_calls=" << gTerrainFlagCounts[i].load() << '\n';
    out << "terrain_other_flag_calls=" << gTerrainOtherFlagCount.load()
        << "\nminecraft_render_thread_id=" << mMinecraftThreadId.load()
        << "\nterrain_task_thread_id=" << mTerrainThreadId.load()
        << "\nthreads_match=" << (mMinecraftThreadId.load() != 0 &&
            mMinecraftThreadId.load() == mTerrainThreadId.load())
        << "\nother_minecraft_thread_calls=" << mOtherMinecraftThreadCalls.load()
        << "\nother_terrain_thread_calls=" << mOtherTerrainThreadCalls.load()
        << "\nfirst_renderer=0x" << std::hex << mFirstRenderer.load()
        << "\nlast_render_context=0x" << mLastContext.load()
        << "\nlast_view=0x" << mLastView.load()
        << "\nlast_client=0x" << mLastClient.load()
        << "\nfirst_terrain_closure=0x" << mFirstTerrainClosure.load()
        << "\nlast_terrain_closure=0x" << mLastTerrainClosure.load()
        << "\nlast_terrain_task_context=0x" << mLastTerrainTaskContext.load()
        << std::dec
        << "\nevent_bus_published_events=" << mEventBus.publishedEvents()
        << "\nevent_bus_delivered_callbacks=" << mEventBus.deliveredCallbacks()
        << "\nphysics_snapshot_coherent=" << physics.coherent
        << "\nphysics_world_generation=" << physics.worldGeneration
        << "\nphysics_simulation_step=" << physics.simulationStep
        << "\ncallbacks_in_flight=" << mCallbacksInFlight.load()
        << "\nhook_restore_succeeded=" << mRestoreSucceeded.load()
        << "\nsafe_to_unload=" << safeToUnload()
        << "\nstatus_file=frustum-terrain-discovery-status.txt"
        << "\ntimeline_file=frustum-terrain-discovery-timeline.csv"
        << "\nfailure_reason=" << (mFailureReason.empty() ? "none" : mFailureReason) << '\n';
}

void LevelRenderHook::removeHooks() noexcept {
    mMinecraftHook.reset(); mTerrainHook.reset();
    using namespace std::chrono_literals;
    for (unsigned i = 0; i < 200U && mCallbacksInFlight.load() != 0; ++i)
        std::this_thread::sleep_for(5ms);
    gOriginalMinecraftRender = nullptr; gOriginalTerrainTask = nullptr;
    mRestoreSucceeded.store(!mMinecraftHook.installed() && !mTerrainHook.installed() &&
        mCallbacksInFlight.load() == 0);
}

void LevelRenderHook::uninstall() noexcept {
    mStopRequested.store(true); if (mWorker.joinable()) mWorker.join();
    removeHooks(); LevelRenderHook* expected = this;
    (void)sActive.compare_exchange_strong(expected, nullptr);
    appendTimeline("stopped"); writeStatus("stopped");
}

bool LevelRenderHook::safeToUnload() const noexcept {
    return mRestoreSucceeded.load() && !mMinecraftHook.installed() &&
        !mTerrainHook.installed() && mCallbacksInFlight.load() == 0;
}
}  // namespace aeronautics::bedrock
