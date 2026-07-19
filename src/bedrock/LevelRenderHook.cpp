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

// LevelRendererCameraAnon::framebuilderInsertTerrainCommandsForChunks::$_0::operator()
constexpr std::uintptr_t terrainTaskOffset = 0x0bdb87b8;
constexpr std::array<std::uint8_t, 16> terrainTaskPrefix{
    0xff, 0x03, 0x07, 0xd1, 0xfd, 0x7b, 0x18, 0xa9,
    0xfc, 0x5f, 0x19, 0xa9, 0xf6, 0x57, 0x1a, 0xa9};

// Terrain command construction helper called by the framebuilder terrain task.
constexpr std::uintptr_t terrainCommandHelperOffset = 0x1266ee9c;
constexpr std::array<std::uint8_t, 16> terrainCommandHelperPrefix{
    0xff, 0x43, 0x02, 0xd1, 0xe8, 0x1b, 0x00, 0xfd,
    0xfd, 0xfb, 0x03, 0xa9, 0xfb, 0x27, 0x00, 0xf9};

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
constexpr std::size_t descriptorSampleCapacity = 32;
constexpr std::size_t helperCountHistogramSize = 25;
constexpr std::size_t payloadQwordCount = 8;
constexpr std::size_t payloadSizeBytes = payloadQwordCount * sizeof(std::uint64_t);
constexpr float epsilon = 0.00001F;
constexpr float pi = 3.14159265358979323846F;

using MinecraftRenderFn = void (*)(void*, void*, const void*, void*);
using TerrainTaskFn = void (*)(void*, void*);
using TerrainCommandHelperFn = void (*)(
    void*, const void*, const void*, const void*, const void*, const void*,
    const void*, std::uint32_t, float);

MinecraftRenderFn gOriginalMinecraftRender = nullptr;
TerrainTaskFn gOriginalTerrainTask = nullptr;
TerrainCommandHelperFn gOriginalTerrainCommandHelper = nullptr;

std::atomic<std::uintptr_t> gModuleLoadBase{0};
std::atomic<std::uint64_t> gModuleFileSpan{0};

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
    std::uint64_t usedBytes{};
    std::uint64_t capacityBytes{};
    bool vectorValid{};
};

struct TerrainThreadState final {
    std::uint32_t depth{};
    std::uint32_t ordinal{};
    std::uint8_t flag{};
    std::uintptr_t vectorObject{};
    std::uintptr_t vectorBegin{};
    std::uintptr_t vectorEnd{};
    std::uintptr_t vectorCapacity{};
    std::uintptr_t commandContext{};
    std::uintptr_t renderObject{};
    std::uintptr_t view{};
    std::uint64_t usedBytes{};
    std::uint64_t capacityBytes{};
    bool vectorValid{};
};

thread_local TerrainThreadState gTerrainThreadState;

struct AtomicDescriptorSample final {
    std::atomic<std::uint64_t> key{0};
    std::atomic<std::uint64_t> calls{0};
    std::atomic<std::uintptr_t> descriptor{0};
    std::atomic<std::uint64_t> descriptorRelative{std::numeric_limits<std::uint64_t>::max()};
    std::atomic<std::uint32_t> mode{0};
    std::atomic<std::uint32_t> firstOrdinal{0};
    std::atomic<std::uint32_t> lastOrdinal{0};
    std::atomic<std::uint32_t> minimumOrdinal{std::numeric_limits<std::uint32_t>::max()};
    std::atomic<std::uint32_t> maximumOrdinal{0};
    std::atomic<std::uint64_t> minimumUsedBytes{std::numeric_limits<std::uint64_t>::max()};
    std::atomic<std::uint64_t> maximumUsedBytes{0};
    std::atomic<std::uint64_t> minimumCapacityBytes{std::numeric_limits<std::uint64_t>::max()};
    std::atomic<std::uint64_t> maximumCapacityBytes{0};
    std::atomic<std::uint64_t> validVectorCalls{0};
    std::atomic<std::uint64_t> invalidVectorCalls{0};
    std::atomic<std::uint64_t> viewMatches{0};
    std::atomic<std::uint64_t> viewMismatches{0};
    std::atomic<std::uint64_t> vectorObjectMatches{0};
    std::atomic<std::uint64_t> vectorObjectMismatches{0};
    std::atomic<std::uint8_t> flagOr{0};
    std::atomic<std::uint32_t> firstThreadId{0};
    std::atomic<std::uint64_t> otherThreadCalls{0};
    std::atomic<std::uint32_t> scaleBits{0};

    std::atomic<std::uint64_t> payloadCaptureCalls{0};
    std::atomic<std::uint64_t> payloadCaptureFailures{0};
    std::atomic<std::uint64_t> append64Calls{0};
    std::atomic<std::uint64_t> unexpectedGrowthCalls{0};
    std::atomic<std::uint64_t> beforeImageCalls{0};
    std::atomic<std::uint64_t> destinationInPreUsedCalls{0};
    std::atomic<std::uint64_t> destinationInPostUsedCalls{0};
    std::atomic<std::uint64_t> destinationMatchesPostElementCalls{0};
    std::atomic<std::uint8_t> changedQwordMask{0};
    std::atomic<std::uint8_t> nonzeroQwordMask{0};
    std::atomic<std::uint8_t> varyingQwordMask{0};
    std::atomic<std::uint8_t> modulePointerQwordMask{0};
    std::atomic<std::uint8_t> vectorPointerQwordMask{0};
    std::atomic<std::uint8_t> alignedPointerLikeQwordMask{0};
    std::atomic<std::uint32_t> firstPayloadState{0};
    std::array<std::atomic<std::uint64_t>, payloadQwordCount> firstPayloadQwords{};
    std::array<std::atomic<std::uint64_t>, payloadQwordCount> lastPayloadQwords{};
};

struct VectorWindow final {
    std::uintptr_t begin{};
    std::uintptr_t end{};
    std::uintptr_t capacity{};
    std::uint64_t usedBytes{};
    std::uint64_t capacityBytes{};
    bool valid{};
};

struct TerrainHelperPending final {
    AtomicDescriptorSample* sample{};
    const void* commandVector{};
    std::uintptr_t destination{};
    VectorWindow beforeWindow{};
    std::array<std::uint64_t, payloadQwordCount> beforePayload{};
    bool active{};
    bool beforePayloadValid{};
};

thread_local TerrainHelperPending gTerrainHelperPending;

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
std::atomic<std::uint64_t> gTerrainVectorValidTasks{0};
std::atomic<std::uint64_t> gTerrainVectorInvalidTasks{0};
std::atomic<std::uint64_t> gTerrainLatestUsedBytes{0};
std::atomic<std::uint64_t> gTerrainLatestCapacityBytes{0};
std::atomic<std::uint64_t> gTerrainMaximumUsedBytes{0};
std::atomic<std::uint64_t> gTerrainMaximumCapacityBytes{0};

std::array<AtomicDescriptorSample, descriptorSampleCapacity> gDescriptorSamples{};
std::atomic<std::uint64_t> gDescriptorSampleOverflow{0};
std::atomic<std::uint64_t> gDescriptorInModuleCalls{0};
std::atomic<std::uint64_t> gDescriptorOutsideModuleCalls{0};
std::atomic<std::uint64_t> gHelperVectorArgumentMatches{0};
std::atomic<std::uint64_t> gHelperVectorArgumentMismatches{0};
std::atomic<std::uint64_t> gHelperContextArgumentMatches{0};
std::atomic<std::uint64_t> gHelperContextArgumentMismatches{0};
std::atomic<std::uint64_t> gHelperRenderObjectArgumentMatches{0};
std::atomic<std::uint64_t> gHelperRenderObjectArgumentMismatches{0};
std::atomic<std::uint64_t> gHelperViewArgumentMatches{0};
std::atomic<std::uint64_t> gHelperViewArgumentMismatches{0};
std::array<std::atomic<std::uint64_t>, helperCountHistogramSize> gHelperCallsPerTask{};
std::atomic<std::uint64_t> gHelperCallsPerTaskOverflow{0};
std::atomic<std::uint32_t> gMinimumHelperCallsPerTask{std::numeric_limits<std::uint32_t>::max()};
std::atomic<std::uint32_t> gMaximumHelperCallsPerTask{0};

std::atomic<std::uint64_t> gPayloadCaptureCalls{0};
std::atomic<std::uint64_t> gPayloadCaptureFailures{0};
std::atomic<std::uint64_t> gPayloadAppend64Calls{0};
std::atomic<std::uint64_t> gPayloadUnexpectedGrowthCalls{0};
std::atomic<std::uint64_t> gPayloadBeforeImageCalls{0};
std::atomic<std::uint64_t> gPayloadDestinationInPreUsedCalls{0};
std::atomic<std::uint64_t> gPayloadDestinationInPostUsedCalls{0};
std::atomic<std::uint64_t> gPayloadDestinationMatchesPostElementCalls{0};
std::atomic<std::uint64_t> gPayloadPendingOverwriteCalls{0};

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

template <class T>
void updateMinimum(std::atomic<T>& target, T candidate) noexcept {
    T current = target.load(std::memory_order_relaxed);
    while (candidate < current && !target.compare_exchange_weak(
        current, candidate, std::memory_order_relaxed, std::memory_order_relaxed)) {}
}

template <class T>
void updateMaximum(std::atomic<T>& target, T candidate) noexcept {
    T current = target.load(std::memory_order_relaxed);
    while (candidate > current && !target.compare_exchange_weak(
        current, candidate, std::memory_order_relaxed, std::memory_order_relaxed)) {}
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
    for (std::size_t i = 0; i < source.relativeOrigin.size(); ++i) {
        gSnapshot.relativeOrigin[i].store(source.relativeOrigin[i], std::memory_order_relaxed);
    }
    for (std::size_t i = 0; i < source.frustum.size(); ++i) {
        gSnapshot.frustum[i].store(source.frustum[i], std::memory_order_relaxed);
    }
    for (std::size_t i = 0; i < source.view.size(); ++i) {
        gSnapshot.view[i].store(source.view[i], std::memory_order_relaxed);
    }
    gSnapshot.valid.store(source.valid, std::memory_order_relaxed);
    gSnapshot.sequence.fetch_add(1, std::memory_order_release);
}

[[nodiscard]] CameraSnapshot readSnapshot() noexcept {
    CameraSnapshot out{};
    for (unsigned attempt = 0; attempt < 8U; ++attempt) {
        const std::uint64_t before = gSnapshot.sequence.load(std::memory_order_acquire);
        if ((before & 1U) != 0U) continue;
        out.renderState = gSnapshot.renderState.load(std::memory_order_relaxed);
        out.cameraObject = gSnapshot.cameraObject.load(std::memory_order_relaxed);
        for (std::size_t i = 0; i < out.relativeOrigin.size(); ++i) {
            out.relativeOrigin[i] = gSnapshot.relativeOrigin[i].load(std::memory_order_relaxed);
        }
        for (std::size_t i = 0; i < out.frustum.size(); ++i) {
            out.frustum[i] = gSnapshot.frustum[i].load(std::memory_order_relaxed);
        }
        for (std::size_t i = 0; i < out.view.size(); ++i) {
            out.view[i] = gSnapshot.view[i].load(std::memory_order_relaxed);
        }
        out.valid = gSnapshot.valid.load(std::memory_order_relaxed);
        const std::uint64_t after = gSnapshot.sequence.load(std::memory_order_acquire);
        if (before == after && (after & 1U) == 0U) {
            out.derived = deriveFrustum(out.frustum);
            out.valid = out.valid && out.derived.valid;
            return out;
        }
    }
    return {};
}

void updateCameraChanges(
    const CameraSnapshot& previous,
    const CameraSnapshot& current) noexcept {
    if (!previous.valid || !current.valid) return;
    bool originChanged = false;
    for (std::size_t axis = 0; axis < 3; ++axis) {
        originChanged |= std::abs(
            current.relativeOrigin[axis] - previous.relativeOrigin[axis]) > epsilon;
    }
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
        if (candidateChanged) {
            gViewChanged[candidate].fetch_add(1, std::memory_order_relaxed);
        }
        updateMaximum(gViewMaxDelta[candidate], candidateMaximum);
    }
}

[[nodiscard]] TerrainFields captureTerrain(const void* closure) noexcept {
    TerrainFields out{};
    if (closure == nullptr) return out;
    for (std::size_t i = 0; i < out.qwords.size(); ++i) {
        out.qwords[i] = readObject<std::uintptr_t>(closure, 0x08 + i * 8);
    }
    out.flag = readObject<std::uint8_t>(closure, 0x48);

    const std::uintptr_t begin = out.qwords[5];
    const std::uintptr_t end = out.qwords[6];
    const std::uintptr_t capacity = out.qwords[7];
    constexpr std::uint64_t maximumObservedContainerBytes = 64ULL * 1024ULL * 1024ULL;
    if (begin <= end && end <= capacity) {
        const std::uint64_t usedBytes = static_cast<std::uint64_t>(end - begin);
        const std::uint64_t capacityBytes = static_cast<std::uint64_t>(capacity - begin);
        if (usedBytes <= maximumObservedContainerBytes &&
                capacityBytes <= maximumObservedContainerBytes) {
            out.usedBytes = usedBytes;
            out.capacityBytes = capacityBytes;
            out.vectorValid = true;
        }
    }
    return out;
}

[[nodiscard]] VectorWindow captureVectorWindow(const void* vectorObject) noexcept {
    VectorWindow out{};
    if (vectorObject == nullptr) return out;
    out.begin = readObject<std::uintptr_t>(vectorObject, 0);
    out.end = readObject<std::uintptr_t>(vectorObject, sizeof(std::uintptr_t));
    out.capacity = readObject<std::uintptr_t>(vectorObject, sizeof(std::uintptr_t) * 2);
    constexpr std::uint64_t maximumObservedContainerBytes = 64ULL * 1024ULL * 1024ULL;
    if (out.begin == 0 || out.begin > out.end || out.end > out.capacity) return out;
    out.usedBytes = static_cast<std::uint64_t>(out.end - out.begin);
    out.capacityBytes = static_cast<std::uint64_t>(out.capacity - out.begin);
    out.valid = out.usedBytes <= maximumObservedContainerBytes &&
        out.capacityBytes <= maximumObservedContainerBytes;
    return out;
}

[[nodiscard]] bool payloadInsideRange(
    std::uintptr_t payload,
    std::uintptr_t begin,
    std::uintptr_t end) noexcept {
    return payload >= begin && payload <= end &&
        static_cast<std::uint64_t>(end - payload) >= payloadSizeBytes;
}

[[nodiscard]] std::array<std::uint64_t, payloadQwordCount> readPayload(
    std::uintptr_t payload) noexcept {
    std::array<std::uint64_t, payloadQwordCount> out{};
    if (payload != 0) {
        std::memcpy(out.data(), reinterpret_cast<const void*>(payload), payloadSizeBytes);
    }
    return out;
}

[[nodiscard]] std::uint64_t descriptorKey(
    std::uintptr_t descriptor,
    std::uint32_t mode) noexcept {
    std::uint64_t hash = 1469598103934665603ULL;
    const auto mix = [&hash](std::uint64_t value) noexcept {
        hash ^= value;
        hash *= 1099511628211ULL;
    };
    mix(static_cast<std::uint64_t>(descriptor));
    mix(mode);
    hash &= ~std::uint64_t{1};
    return hash < 2 ? 2 : hash;
}

[[nodiscard]] std::uint64_t descriptorRelative(
    std::uintptr_t descriptor) noexcept {
    const std::uintptr_t base = gModuleLoadBase.load(std::memory_order_relaxed);
    const std::uint64_t span = gModuleFileSpan.load(std::memory_order_relaxed);
    if (base != 0 && descriptor >= base &&
            static_cast<std::uint64_t>(descriptor - base) < span) {
        return static_cast<std::uint64_t>(descriptor - base);
    }
    return std::numeric_limits<std::uint64_t>::max();
}

void updateDescriptorSample(
    AtomicDescriptorSample& sample,
    std::uint32_t ordinal,
    std::uint8_t flag,
    bool vectorValid,
    std::uint64_t usedBytes,
    std::uint64_t capacityBytes,
    bool viewMatches,
    bool vectorObjectMatches,
    std::uint32_t threadId) noexcept {
    sample.calls.fetch_add(1, std::memory_order_relaxed);
    sample.lastOrdinal.store(ordinal, std::memory_order_relaxed);
    updateMinimum(sample.minimumOrdinal, ordinal);
    updateMaximum(sample.maximumOrdinal, ordinal);
    if (vectorValid) {
        sample.validVectorCalls.fetch_add(1, std::memory_order_relaxed);
        updateMinimum(sample.minimumUsedBytes, usedBytes);
        updateMaximum(sample.maximumUsedBytes, usedBytes);
        updateMinimum(sample.minimumCapacityBytes, capacityBytes);
        updateMaximum(sample.maximumCapacityBytes, capacityBytes);
    } else {
        sample.invalidVectorCalls.fetch_add(1, std::memory_order_relaxed);
    }
    (viewMatches ? sample.viewMatches : sample.viewMismatches)
        .fetch_add(1, std::memory_order_relaxed);
    (vectorObjectMatches ? sample.vectorObjectMatches : sample.vectorObjectMismatches)
        .fetch_add(1, std::memory_order_relaxed);
    sample.flagOr.fetch_or(flag, std::memory_order_relaxed);
    std::uint32_t expected = 0;
    sample.firstThreadId.compare_exchange_strong(
        expected, threadId, std::memory_order_relaxed, std::memory_order_relaxed);
    if (expected != 0 && expected != threadId) {
        sample.otherThreadCalls.fetch_add(1, std::memory_order_relaxed);
    }
}

[[nodiscard]] AtomicDescriptorSample* recordDescriptor(
    const void* descriptor,
    std::uint32_t mode,
    float scale,
    std::uint32_t ordinal,
    const TerrainThreadState& state,
    bool viewMatches,
    bool vectorObjectMatches,
    std::uint32_t threadId) noexcept {
    const std::uintptr_t descriptorAddress = reinterpret_cast<std::uintptr_t>(descriptor);
    const std::uint64_t relative = descriptorRelative(descriptorAddress);
    if (relative == std::numeric_limits<std::uint64_t>::max()) {
        gDescriptorOutsideModuleCalls.fetch_add(1, std::memory_order_relaxed);
    } else {
        gDescriptorInModuleCalls.fetch_add(1, std::memory_order_relaxed);
    }

    const std::uint64_t key = descriptorKey(descriptorAddress, mode);
    for (auto& sample : gDescriptorSamples) {
        const std::uint64_t observed = sample.key.load(std::memory_order_acquire);
        if (observed == key) {
            updateDescriptorSample(
                sample, ordinal, state.flag, state.vectorValid,
                state.usedBytes, state.capacityBytes, viewMatches,
                vectorObjectMatches, threadId);
            return &sample;
        }
        if (observed != 0) continue;
        std::uint64_t expected = 0;
        if (!sample.key.compare_exchange_strong(
                expected, 1, std::memory_order_acq_rel,
                std::memory_order_acquire)) {
            continue;
        }

        sample.calls.store(0, std::memory_order_relaxed);
        sample.descriptor.store(descriptorAddress, std::memory_order_relaxed);
        sample.descriptorRelative.store(relative, std::memory_order_relaxed);
        sample.mode.store(mode, std::memory_order_relaxed);
        sample.firstOrdinal.store(ordinal, std::memory_order_relaxed);
        sample.lastOrdinal.store(ordinal, std::memory_order_relaxed);
        sample.minimumOrdinal.store(ordinal, std::memory_order_relaxed);
        sample.maximumOrdinal.store(ordinal, std::memory_order_relaxed);
        sample.minimumUsedBytes.store(
            state.vectorValid ? state.usedBytes : std::numeric_limits<std::uint64_t>::max(),
            std::memory_order_relaxed);
        sample.maximumUsedBytes.store(
            state.vectorValid ? state.usedBytes : 0, std::memory_order_relaxed);
        sample.minimumCapacityBytes.store(
            state.vectorValid ? state.capacityBytes : std::numeric_limits<std::uint64_t>::max(),
            std::memory_order_relaxed);
        sample.maximumCapacityBytes.store(
            state.vectorValid ? state.capacityBytes : 0, std::memory_order_relaxed);
        sample.validVectorCalls.store(0, std::memory_order_relaxed);
        sample.invalidVectorCalls.store(0, std::memory_order_relaxed);
        sample.viewMatches.store(0, std::memory_order_relaxed);
        sample.viewMismatches.store(0, std::memory_order_relaxed);
        sample.vectorObjectMatches.store(0, std::memory_order_relaxed);
        sample.vectorObjectMismatches.store(0, std::memory_order_relaxed);
        sample.flagOr.store(0, std::memory_order_relaxed);
        sample.firstThreadId.store(threadId, std::memory_order_relaxed);
        sample.otherThreadCalls.store(0, std::memory_order_relaxed);
        sample.scaleBits.store(std::bit_cast<std::uint32_t>(scale), std::memory_order_relaxed);
        updateDescriptorSample(
            sample, ordinal, state.flag, state.vectorValid,
            state.usedBytes, state.capacityBytes, viewMatches,
            vectorObjectMatches, threadId);
        sample.key.store(key, std::memory_order_release);
        return &sample;
    }
    gDescriptorSampleOverflow.fetch_add(1, std::memory_order_relaxed);
    return nullptr;
}

void recordPayload(
    AtomicDescriptorSample& sample,
    const std::array<std::uint64_t, payloadQwordCount>& payload,
    const std::array<std::uint64_t, payloadQwordCount>* beforePayload,
    const VectorWindow& afterWindow,
    bool appended64,
    bool destinationInPreUsed,
    bool destinationInPostUsed,
    bool destinationMatchesPostElement) noexcept {
    sample.payloadCaptureCalls.fetch_add(1, std::memory_order_relaxed);
    gPayloadCaptureCalls.fetch_add(1, std::memory_order_relaxed);

    if (appended64) {
        sample.append64Calls.fetch_add(1, std::memory_order_relaxed);
        gPayloadAppend64Calls.fetch_add(1, std::memory_order_relaxed);
    } else {
        sample.unexpectedGrowthCalls.fetch_add(1, std::memory_order_relaxed);
        gPayloadUnexpectedGrowthCalls.fetch_add(1, std::memory_order_relaxed);
    }
    if (destinationInPreUsed) {
        sample.destinationInPreUsedCalls.fetch_add(1, std::memory_order_relaxed);
        gPayloadDestinationInPreUsedCalls.fetch_add(1, std::memory_order_relaxed);
    }
    if (destinationInPostUsed) {
        sample.destinationInPostUsedCalls.fetch_add(1, std::memory_order_relaxed);
        gPayloadDestinationInPostUsedCalls.fetch_add(1, std::memory_order_relaxed);
    }
    if (destinationMatchesPostElement) {
        sample.destinationMatchesPostElementCalls.fetch_add(1, std::memory_order_relaxed);
        gPayloadDestinationMatchesPostElementCalls.fetch_add(1, std::memory_order_relaxed);
    }

    std::uint8_t changedMask = 0;
    std::uint8_t nonzeroMask = 0;
    std::uint8_t modulePointerMask = 0;
    std::uint8_t vectorPointerMask = 0;
    std::uint8_t alignedPointerLikeMask = 0;
    const std::uintptr_t moduleBase = gModuleLoadBase.load(std::memory_order_relaxed);
    const std::uint64_t moduleSpan = gModuleFileSpan.load(std::memory_order_relaxed);

    for (std::size_t qword = 0; qword < payload.size(); ++qword) {
        const std::uint64_t value = payload[qword];
        const std::uint8_t bit = static_cast<std::uint8_t>(1U << qword);
        if (value != 0) nonzeroMask = static_cast<std::uint8_t>(nonzeroMask | bit);
        if (beforePayload != nullptr && (*beforePayload)[qword] != value) {
            changedMask = static_cast<std::uint8_t>(changedMask | bit);
        }
        if (moduleBase != 0 && value >= moduleBase &&
                value - moduleBase < moduleSpan) {
            modulePointerMask = static_cast<std::uint8_t>(modulePointerMask | bit);
        }
        if (afterWindow.valid && value >= afterWindow.begin &&
                value < afterWindow.capacity) {
            vectorPointerMask = static_cast<std::uint8_t>(vectorPointerMask | bit);
        }
        if (value >= 0x10000ULL && (value & (alignof(void*) - 1U)) == 0U) {
            alignedPointerLikeMask =
                static_cast<std::uint8_t>(alignedPointerLikeMask | bit);
        }
        sample.lastPayloadQwords[qword].store(value, std::memory_order_relaxed);
    }

    if (beforePayload != nullptr) {
        sample.beforeImageCalls.fetch_add(1, std::memory_order_relaxed);
        sample.changedQwordMask.fetch_or(changedMask, std::memory_order_relaxed);
        gPayloadBeforeImageCalls.fetch_add(1, std::memory_order_relaxed);
    }
    sample.nonzeroQwordMask.fetch_or(nonzeroMask, std::memory_order_relaxed);
    sample.modulePointerQwordMask.fetch_or(modulePointerMask, std::memory_order_relaxed);
    sample.vectorPointerQwordMask.fetch_or(vectorPointerMask, std::memory_order_relaxed);
    sample.alignedPointerLikeQwordMask.fetch_or(
        alignedPointerLikeMask, std::memory_order_relaxed);

    std::uint32_t firstState = sample.firstPayloadState.load(std::memory_order_acquire);
    if (firstState == 0) {
        std::uint32_t expected = 0;
        if (sample.firstPayloadState.compare_exchange_strong(
                expected, 1, std::memory_order_acq_rel, std::memory_order_acquire)) {
            for (std::size_t qword = 0; qword < payload.size(); ++qword) {
                sample.firstPayloadQwords[qword].store(
                    payload[qword], std::memory_order_relaxed);
            }
            sample.firstPayloadState.store(2, std::memory_order_release);
            return;
        }
        firstState = expected;
    }
    if (firstState != 2) return;

    std::uint8_t varyingMask = 0;
    for (std::size_t qword = 0; qword < payload.size(); ++qword) {
        if (sample.firstPayloadQwords[qword].load(std::memory_order_relaxed) !=
                payload[qword]) {
            varyingMask = static_cast<std::uint8_t>(
                varyingMask | static_cast<std::uint8_t>(1U << qword));
        }
    }
    sample.varyingQwordMask.fetch_or(varyingMask, std::memory_order_relaxed);
}

void resetDiscovery() noexcept {
    gSnapshot.sequence.store(0, std::memory_order_relaxed);
    gSnapshot.renderState.store(0, std::memory_order_relaxed);
    gSnapshot.cameraObject.store(0, std::memory_order_relaxed);
    gSnapshot.valid.store(false, std::memory_order_relaxed);
    for (auto& item : gSnapshot.relativeOrigin) item.store(0.0F, std::memory_order_relaxed);
    for (auto& item : gSnapshot.frustum) item.store(0.0F, std::memory_order_relaxed);
    for (auto& item : gSnapshot.view) item.store(0.0F, std::memory_order_relaxed);

    gPreValid.store(0, std::memory_order_relaxed);
    gPreInvalid.store(0, std::memory_order_relaxed);
    gPostValid.store(0, std::memory_order_relaxed);
    gPostInvalid.store(0, std::memory_order_relaxed);
    gOriginChanged.store(0, std::memory_order_relaxed);
    gFrustumChanged.store(0, std::memory_order_relaxed);
    gFrustumChangedElements.store(0, std::memory_order_relaxed);
    gFrustumMaxDelta.store(0.0F, std::memory_order_relaxed);
    for (std::size_t i = 0; i < viewCandidateCount; ++i) {
        gViewChanged[i].store(0, std::memory_order_relaxed);
        gViewMaxDelta[i].store(0.0F, std::memory_order_relaxed);
    }

    for (auto& item : gTerrainQwords) item.store(0, std::memory_order_relaxed);
    gTerrainFlag.store(0, std::memory_order_relaxed);
    for (auto& item : gTerrainFlagCounts) item.store(0, std::memory_order_relaxed);
    gTerrainOtherFlagCount.store(0, std::memory_order_relaxed);
    gTerrainVectorValidTasks.store(0, std::memory_order_relaxed);
    gTerrainVectorInvalidTasks.store(0, std::memory_order_relaxed);
    gTerrainLatestUsedBytes.store(0, std::memory_order_relaxed);
    gTerrainLatestCapacityBytes.store(0, std::memory_order_relaxed);
    gTerrainMaximumUsedBytes.store(0, std::memory_order_relaxed);
    gTerrainMaximumCapacityBytes.store(0, std::memory_order_relaxed);

    for (auto& sample : gDescriptorSamples) {
        sample.key.store(0, std::memory_order_relaxed);
        sample.calls.store(0, std::memory_order_relaxed);
        sample.descriptor.store(0, std::memory_order_relaxed);
        sample.descriptorRelative.store(
            std::numeric_limits<std::uint64_t>::max(), std::memory_order_relaxed);
        sample.mode.store(0, std::memory_order_relaxed);
        sample.firstOrdinal.store(0, std::memory_order_relaxed);
        sample.lastOrdinal.store(0, std::memory_order_relaxed);
        sample.minimumOrdinal.store(
            std::numeric_limits<std::uint32_t>::max(), std::memory_order_relaxed);
        sample.maximumOrdinal.store(0, std::memory_order_relaxed);
        sample.minimumUsedBytes.store(
            std::numeric_limits<std::uint64_t>::max(), std::memory_order_relaxed);
        sample.maximumUsedBytes.store(0, std::memory_order_relaxed);
        sample.minimumCapacityBytes.store(
            std::numeric_limits<std::uint64_t>::max(), std::memory_order_relaxed);
        sample.maximumCapacityBytes.store(0, std::memory_order_relaxed);
        sample.validVectorCalls.store(0, std::memory_order_relaxed);
        sample.invalidVectorCalls.store(0, std::memory_order_relaxed);
        sample.viewMatches.store(0, std::memory_order_relaxed);
        sample.viewMismatches.store(0, std::memory_order_relaxed);
        sample.vectorObjectMatches.store(0, std::memory_order_relaxed);
        sample.vectorObjectMismatches.store(0, std::memory_order_relaxed);
        sample.flagOr.store(0, std::memory_order_relaxed);
        sample.firstThreadId.store(0, std::memory_order_relaxed);
        sample.otherThreadCalls.store(0, std::memory_order_relaxed);
        sample.scaleBits.store(0, std::memory_order_relaxed);
        sample.payloadCaptureCalls.store(0, std::memory_order_relaxed);
        sample.payloadCaptureFailures.store(0, std::memory_order_relaxed);
        sample.append64Calls.store(0, std::memory_order_relaxed);
        sample.unexpectedGrowthCalls.store(0, std::memory_order_relaxed);
        sample.beforeImageCalls.store(0, std::memory_order_relaxed);
        sample.destinationInPreUsedCalls.store(0, std::memory_order_relaxed);
        sample.destinationInPostUsedCalls.store(0, std::memory_order_relaxed);
        sample.destinationMatchesPostElementCalls.store(0, std::memory_order_relaxed);
        sample.changedQwordMask.store(0, std::memory_order_relaxed);
        sample.nonzeroQwordMask.store(0, std::memory_order_relaxed);
        sample.varyingQwordMask.store(0, std::memory_order_relaxed);
        sample.modulePointerQwordMask.store(0, std::memory_order_relaxed);
        sample.vectorPointerQwordMask.store(0, std::memory_order_relaxed);
        sample.alignedPointerLikeQwordMask.store(0, std::memory_order_relaxed);
        sample.firstPayloadState.store(0, std::memory_order_relaxed);
        for (auto& qword : sample.firstPayloadQwords) {
            qword.store(0, std::memory_order_relaxed);
        }
        for (auto& qword : sample.lastPayloadQwords) {
            qword.store(0, std::memory_order_relaxed);
        }
    }
    gDescriptorSampleOverflow.store(0, std::memory_order_relaxed);
    gDescriptorInModuleCalls.store(0, std::memory_order_relaxed);
    gDescriptorOutsideModuleCalls.store(0, std::memory_order_relaxed);
    gHelperVectorArgumentMatches.store(0, std::memory_order_relaxed);
    gHelperVectorArgumentMismatches.store(0, std::memory_order_relaxed);
    gHelperContextArgumentMatches.store(0, std::memory_order_relaxed);
    gHelperContextArgumentMismatches.store(0, std::memory_order_relaxed);
    gHelperRenderObjectArgumentMatches.store(0, std::memory_order_relaxed);
    gHelperRenderObjectArgumentMismatches.store(0, std::memory_order_relaxed);
    gHelperViewArgumentMatches.store(0, std::memory_order_relaxed);
    gHelperViewArgumentMismatches.store(0, std::memory_order_relaxed);
    for (auto& item : gHelperCallsPerTask) item.store(0, std::memory_order_relaxed);
    gHelperCallsPerTaskOverflow.store(0, std::memory_order_relaxed);
    gMinimumHelperCallsPerTask.store(
        std::numeric_limits<std::uint32_t>::max(), std::memory_order_relaxed);
    gMaximumHelperCallsPerTask.store(0, std::memory_order_relaxed);
    gPayloadCaptureCalls.store(0, std::memory_order_relaxed);
    gPayloadCaptureFailures.store(0, std::memory_order_relaxed);
    gPayloadAppend64Calls.store(0, std::memory_order_relaxed);
    gPayloadUnexpectedGrowthCalls.store(0, std::memory_order_relaxed);
    gPayloadBeforeImageCalls.store(0, std::memory_order_relaxed);
    gPayloadDestinationInPreUsedCalls.store(0, std::memory_order_relaxed);
    gPayloadDestinationInPostUsedCalls.store(0, std::memory_order_relaxed);
    gPayloadDestinationMatchesPostElementCalls.store(0, std::memory_order_relaxed);
    gPayloadPendingOverwriteCalls.store(0, std::memory_order_relaxed);
    gTerrainHelperPending = {};
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
    if (gOriginalMinecraftRender != nullptr) {
        gOriginalMinecraftRender(renderer, context, view, client);
    }
    LevelRenderHook::recordMinecraftRenderPost(renderer, context, view, client);
}

void terrainTaskDetour(void* closure, void* taskContext) {
    LevelRenderHook::recordTerrainTaskBegin(closure, taskContext);
    if (gOriginalTerrainTask != nullptr) {
        gOriginalTerrainTask(closure, taskContext);
    }
    LevelRenderHook::recordTerrainTaskEnd();
}

void terrainCommandHelperDetour(
    void* destination,
    const void* commandVector,
    const void* commandContext,
    const void* renderObject,
    const void* view,
    const void* sharedOwner,
    const void* descriptor,
    std::uint32_t mode,
    float scale) {
    LevelRenderHook::recordTerrainCommandHelper(
        destination, commandVector, commandContext, renderObject, view,
        sharedOwner, descriptor, mode, scale);
    if (gOriginalTerrainCommandHelper != nullptr) {
        gOriginalTerrainCommandHelper(
            destination, commandVector, commandContext, renderObject, view,
            sharedOwner, descriptor, mode, scale);
    }
    LevelRenderHook::recordTerrainCommandHelperEnd();
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
    if (mWorker.joinable()) return true;
    mStatusPath = mMod.getDataDir() / "terrain-command-payload-status.txt";
    mTimelinePath = mMod.getDataDir() / "terrain-command-payload-timeline.csv";

    mStopRequested.store(false, std::memory_order_relaxed);
    mRestoreSucceeded.store(false, std::memory_order_relaxed);
    mCallbacksInFlight.store(0, std::memory_order_relaxed);
    mMinecraftPreCalls.store(0, std::memory_order_relaxed);
    mMinecraftPostCalls.store(0, std::memory_order_relaxed);
    mTerrainTaskCalls.store(0, std::memory_order_relaxed);
    mTerrainHelperCalls.store(0, std::memory_order_relaxed);
    mTerrainHelperInsideTaskCalls.store(0, std::memory_order_relaxed);
    mTerrainHelperOutsideTaskCalls.store(0, std::memory_order_relaxed);
    mMinecraftThreadId.store(0, std::memory_order_relaxed);
    mTerrainThreadId.store(0, std::memory_order_relaxed);
    mTerrainHelperThreadId.store(0, std::memory_order_relaxed);
    mOtherMinecraftThreadCalls.store(0, std::memory_order_relaxed);
    mOtherTerrainThreadCalls.store(0, std::memory_order_relaxed);
    mOtherTerrainHelperThreadCalls.store(0, std::memory_order_relaxed);
    mFirstRenderer.store(0, std::memory_order_relaxed);
    mLastContext.store(0, std::memory_order_relaxed);
    mLastView.store(0, std::memory_order_relaxed);
    mLastClient.store(0, std::memory_order_relaxed);
    mFirstTerrainClosure.store(0, std::memory_order_relaxed);
    mLastTerrainClosure.store(0, std::memory_order_relaxed);
    mLastTerrainTaskContext.store(0, std::memory_order_relaxed);
    mFingerprintValidated.store(false, std::memory_order_relaxed);
    mMinecraftPrefixValidated.store(false, std::memory_order_relaxed);
    mTerrainPrefixValidated.store(false, std::memory_order_relaxed);
    mTerrainHelperPrefixValidated.store(false, std::memory_order_relaxed);
    mFailureReason.clear();
    gModuleLoadBase.store(0, std::memory_order_relaxed);
    gModuleFileSpan.store(0, std::memory_order_relaxed);
    resetDiscovery();
    createTimeline();

    LevelRenderHook* expected = nullptr;
    if (!sActive.compare_exchange_strong(expected, this)) {
        mFailureReason = "another terrain command payload hook is active";
        writeStatus("registration_failed");
        return false;
    }

    writeStatus("waiting_for_stable_heartbeat");
    try {
        mWorker = std::thread(&LevelRenderHook::workerLoop, this);
    } catch (...) {
        sActive.store(nullptr, std::memory_order_release);
        mFailureReason = "failed to start terrain command payload worker";
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
        writeStatus("running_terrain_command_payload_decode");
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
    mFingerprintValidated.store(true, std::memory_order_relaxed);
    gModuleLoadBase.store(module->loadBase, std::memory_order_relaxed);
    gModuleFileSpan.store(
        static_cast<std::uint64_t>(module->fileSize), std::memory_order_relaxed);

    const std::uintptr_t minecraftAddress = module->loadBase + minecraftRenderOffset;
    if (readInstructionPrefix(*module, minecraftAddress, minecraftRenderPrefix.size()) !=
            formatPrefix(minecraftRenderPrefix)) {
        mFailureReason = "LevelRendererCamera::render prefix mismatch";
        return false;
    }
    mMinecraftPrefixValidated.store(true, std::memory_order_relaxed);

    const std::uintptr_t terrainAddress = module->loadBase + terrainTaskOffset;
    if (readInstructionPrefix(*module, terrainAddress, terrainTaskPrefix.size()) !=
            formatPrefix(terrainTaskPrefix)) {
        mFailureReason = "framebuilder terrain task operator prefix mismatch";
        return false;
    }
    mTerrainPrefixValidated.store(true, std::memory_order_relaxed);

    const std::uintptr_t helperAddress = module->loadBase + terrainCommandHelperOffset;
    if (readInstructionPrefix(
            *module, helperAddress, terrainCommandHelperPrefix.size()) !=
            formatPrefix(terrainCommandHelperPrefix)) {
        mFailureReason = "terrain command helper prefix mismatch";
        return false;
    }
    mTerrainHelperPrefixValidated.store(true, std::memory_order_relaxed);

    mTerrainHelperHook = pl::memory::HookHandle(
        reinterpret_cast<pl::memory::FuncPtr>(helperAddress),
        reinterpret_cast<pl::memory::FuncPtr>(&terrainCommandHelperDetour),
        &mTerrainHelperOriginalStorage,
        pl::memory::HookPriority::Low);
    if (!mTerrainHelperHook.installed() || mTerrainHelperOriginalStorage == nullptr) {
        mTerrainHelperHook.reset();
        mFailureReason = "terrain command helper hook failed";
        return false;
    }
    gOriginalTerrainCommandHelper = reinterpret_cast<TerrainCommandHelperFn>(
        mTerrainHelperOriginalStorage);

    mTerrainHook = pl::memory::HookHandle(
        reinterpret_cast<pl::memory::FuncPtr>(terrainAddress),
        reinterpret_cast<pl::memory::FuncPtr>(&terrainTaskDetour),
        &mTerrainOriginalStorage,
        pl::memory::HookPriority::Low);
    if (!mTerrainHook.installed() || mTerrainOriginalStorage == nullptr) {
        mFailureReason = "active terrain task hook failed";
        removeHooks();
        return false;
    }
    gOriginalTerrainTask = reinterpret_cast<TerrainTaskFn>(mTerrainOriginalStorage);

    mMinecraftHook = pl::memory::HookHandle(
        reinterpret_cast<pl::memory::FuncPtr>(minecraftAddress),
        reinterpret_cast<pl::memory::FuncPtr>(&minecraftRenderDetour),
        &mMinecraftOriginalStorage,
        pl::memory::HookPriority::Low);
    if (!mMinecraftHook.installed() || mMinecraftOriginalStorage == nullptr) {
        mFailureReason = "LevelRendererCamera::render hook failed";
        removeHooks();
        return false;
    }
    gOriginalMinecraftRender = reinterpret_cast<MinecraftRenderFn>(
        mMinecraftOriginalStorage);
    return true;
}

void LevelRenderHook::recordMinecraftRenderPre(
    void* renderer,
    void* context,
    const void* view,
    void* client) noexcept {
    LevelRenderHook* self = sActive.load(std::memory_order_acquire);
    if (self == nullptr) return;
    self->mCallbacksInFlight.fetch_add(1, std::memory_order_acq_rel);
    self->mMinecraftPreCalls.fetch_add(1, std::memory_order_relaxed);

    const std::uint32_t thread = currentThreadId();
    std::uint32_t expected = 0;
    self->mMinecraftThreadId.compare_exchange_strong(
        expected, thread, std::memory_order_relaxed, std::memory_order_relaxed);
    if (expected != 0 && expected != thread) {
        self->mOtherMinecraftThreadCalls.fetch_add(1, std::memory_order_relaxed);
    }

    std::uintptr_t empty = 0;
    self->mFirstRenderer.compare_exchange_strong(
        empty, reinterpret_cast<std::uintptr_t>(renderer),
        std::memory_order_relaxed, std::memory_order_relaxed);
    self->mLastContext.store(
        reinterpret_cast<std::uintptr_t>(context), std::memory_order_relaxed);
    self->mLastView.store(
        reinterpret_cast<std::uintptr_t>(view), std::memory_order_relaxed);
    self->mLastClient.store(
        reinterpret_cast<std::uintptr_t>(client), std::memory_order_relaxed);

    const CameraSnapshot snapshot = captureCamera(context, view);
    (snapshot.valid ? gPreValid : gPreInvalid).fetch_add(1, std::memory_order_relaxed);
}

void LevelRenderHook::recordMinecraftRenderPost(
    void* renderer,
    void* context,
    const void* view,
    void* client) noexcept {
    LevelRenderHook* self = sActive.load(std::memory_order_acquire);
    if (self == nullptr) return;

    const CameraSnapshot previous = readSnapshot();
    const CameraSnapshot current = captureCamera(context, view);
    publishSnapshot(current);
    if (current.valid) {
        gPostValid.fetch_add(1, std::memory_order_relaxed);
        updateCameraChanges(previous, current);
    } else {
        gPostInvalid.fetch_add(1, std::memory_order_relaxed);
    }

    const std::uint64_t sequence =
        self->mMinecraftPostCalls.fetch_add(1, std::memory_order_relaxed) + 1U;
    const LevelRenderEvent event{
        renderer, context, view, client, sequence, currentThreadId()};
    (void)self->mEventBus.publish(event);
    self->mCallbacksInFlight.fetch_sub(1, std::memory_order_acq_rel);
}

void LevelRenderHook::recordTerrainTaskBegin(
    const void* closure,
    const void* taskContext) noexcept {
    LevelRenderHook* self = sActive.load(std::memory_order_acquire);
    if (self == nullptr) return;
    self->mCallbacksInFlight.fetch_add(1, std::memory_order_acq_rel);
    self->mTerrainTaskCalls.fetch_add(1, std::memory_order_relaxed);

    const std::uint32_t thread = currentThreadId();
    std::uint32_t expected = 0;
    self->mTerrainThreadId.compare_exchange_strong(
        expected, thread, std::memory_order_relaxed, std::memory_order_relaxed);
    if (expected != 0 && expected != thread) {
        self->mOtherTerrainThreadCalls.fetch_add(1, std::memory_order_relaxed);
    }

    std::uintptr_t empty = 0;
    self->mFirstTerrainClosure.compare_exchange_strong(
        empty, reinterpret_cast<std::uintptr_t>(closure),
        std::memory_order_relaxed, std::memory_order_relaxed);
    self->mLastTerrainClosure.store(
        reinterpret_cast<std::uintptr_t>(closure), std::memory_order_relaxed);
    self->mLastTerrainTaskContext.store(
        reinterpret_cast<std::uintptr_t>(taskContext), std::memory_order_relaxed);

    const TerrainFields fields = captureTerrain(closure);
    for (std::size_t i = 0; i < fields.qwords.size(); ++i) {
        gTerrainQwords[i].store(fields.qwords[i], std::memory_order_relaxed);
    }
    gTerrainFlag.store(fields.flag, std::memory_order_relaxed);
    if (fields.flag < gTerrainFlagCounts.size()) {
        gTerrainFlagCounts[fields.flag].fetch_add(1, std::memory_order_relaxed);
    } else {
        gTerrainOtherFlagCount.fetch_add(1, std::memory_order_relaxed);
    }
    if (fields.vectorValid) {
        gTerrainVectorValidTasks.fetch_add(1, std::memory_order_relaxed);
        gTerrainLatestUsedBytes.store(fields.usedBytes, std::memory_order_relaxed);
        gTerrainLatestCapacityBytes.store(fields.capacityBytes, std::memory_order_relaxed);
        updateMaximum(gTerrainMaximumUsedBytes, fields.usedBytes);
        updateMaximum(gTerrainMaximumCapacityBytes, fields.capacityBytes);
    } else {
        gTerrainVectorInvalidTasks.fetch_add(1, std::memory_order_relaxed);
    }

    if (gTerrainThreadState.depth == 0) {
        gTerrainThreadState.ordinal = 0;
        gTerrainThreadState.flag = fields.flag;
        gTerrainThreadState.vectorObject = reinterpret_cast<std::uintptr_t>(closure) + 0x30;
        gTerrainThreadState.vectorBegin = fields.qwords[5];
        gTerrainThreadState.vectorEnd = fields.qwords[6];
        gTerrainThreadState.vectorCapacity = fields.qwords[7];
        gTerrainThreadState.commandContext = reinterpret_cast<std::uintptr_t>(closure) + 0x10;
        gTerrainThreadState.renderObject = fields.qwords[0];
        gTerrainThreadState.view = fields.qwords[2];
        gTerrainThreadState.usedBytes = fields.usedBytes;
        gTerrainThreadState.capacityBytes = fields.capacityBytes;
        gTerrainThreadState.vectorValid = fields.vectorValid;
    }
    ++gTerrainThreadState.depth;
}

void LevelRenderHook::recordTerrainTaskEnd() noexcept {
    LevelRenderHook* self = sActive.load(std::memory_order_acquire);
    if (self == nullptr) return;

    if (gTerrainThreadState.depth > 0) {
        --gTerrainThreadState.depth;
        if (gTerrainThreadState.depth == 0) {
            const std::uint32_t count = gTerrainThreadState.ordinal;
            updateMinimum(gMinimumHelperCallsPerTask, count);
            updateMaximum(gMaximumHelperCallsPerTask, count);
            if (count < gHelperCallsPerTask.size()) {
                gHelperCallsPerTask[count].fetch_add(1, std::memory_order_relaxed);
            } else {
                gHelperCallsPerTaskOverflow.fetch_add(1, std::memory_order_relaxed);
            }
            gTerrainThreadState = {};
        }
    }
    self->mCallbacksInFlight.fetch_sub(1, std::memory_order_acq_rel);
}

void LevelRenderHook::recordTerrainCommandHelper(
    const void* destination,
    const void* commandVector,
    const void* commandContext,
    const void* renderObject,
    const void* view,
    const void* sharedOwner,
    const void* descriptor,
    std::uint32_t mode,
    float scale) noexcept {
    (void)destination;
    (void)sharedOwner;

    LevelRenderHook* self = sActive.load(std::memory_order_acquire);
    if (self == nullptr) return;
    self->mCallbacksInFlight.fetch_add(1, std::memory_order_acq_rel);
    self->mTerrainHelperCalls.fetch_add(1, std::memory_order_relaxed);

    const std::uint32_t thread = currentThreadId();
    std::uint32_t expected = 0;
    self->mTerrainHelperThreadId.compare_exchange_strong(
        expected, thread, std::memory_order_relaxed, std::memory_order_relaxed);
    if (expected != 0 && expected != thread) {
        self->mOtherTerrainHelperThreadCalls.fetch_add(1, std::memory_order_relaxed);
    }

    if (gTerrainThreadState.depth == 0) {
        self->mTerrainHelperOutsideTaskCalls.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    self->mTerrainHelperInsideTaskCalls.fetch_add(1, std::memory_order_relaxed);

    const bool vectorMatches = reinterpret_cast<std::uintptr_t>(commandVector) ==
        gTerrainThreadState.vectorObject;
    const bool contextMatches = reinterpret_cast<std::uintptr_t>(commandContext) ==
        gTerrainThreadState.commandContext;
    const bool renderObjectMatches = reinterpret_cast<std::uintptr_t>(renderObject) ==
        gTerrainThreadState.renderObject;
    const bool viewMatches = reinterpret_cast<std::uintptr_t>(view) ==
        gTerrainThreadState.view;

    (vectorMatches ? gHelperVectorArgumentMatches : gHelperVectorArgumentMismatches)
        .fetch_add(1, std::memory_order_relaxed);
    (contextMatches ? gHelperContextArgumentMatches : gHelperContextArgumentMismatches)
        .fetch_add(1, std::memory_order_relaxed);
    (renderObjectMatches ? gHelperRenderObjectArgumentMatches :
        gHelperRenderObjectArgumentMismatches).fetch_add(1, std::memory_order_relaxed);
    (viewMatches ? gHelperViewArgumentMatches : gHelperViewArgumentMismatches)
        .fetch_add(1, std::memory_order_relaxed);

    if (gTerrainHelperPending.active) {
        gPayloadPendingOverwriteCalls.fetch_add(1, std::memory_order_relaxed);
    }
    gTerrainHelperPending = {};

    const std::uint32_t ordinal = gTerrainThreadState.ordinal++;
    AtomicDescriptorSample* sample = recordDescriptor(
        descriptor, mode, scale, ordinal, gTerrainThreadState,
        viewMatches, vectorMatches, thread);
    if (sample == nullptr) return;

    gTerrainHelperPending.sample = sample;
    gTerrainHelperPending.commandVector = commandVector;
    gTerrainHelperPending.destination = reinterpret_cast<std::uintptr_t>(destination);
    gTerrainHelperPending.beforeWindow = captureVectorWindow(commandVector);
    if (gTerrainHelperPending.beforeWindow.valid &&
            payloadInsideRange(
                gTerrainHelperPending.destination,
                gTerrainHelperPending.beforeWindow.begin,
                gTerrainHelperPending.beforeWindow.end)) {
        gTerrainHelperPending.beforePayload =
            readPayload(gTerrainHelperPending.destination);
        gTerrainHelperPending.beforePayloadValid = true;
    }
    gTerrainHelperPending.active = true;
}

void LevelRenderHook::recordTerrainCommandHelperEnd() noexcept {
    LevelRenderHook* self = sActive.load(std::memory_order_acquire);
    if (self == nullptr) return;

    TerrainHelperPending pending = gTerrainHelperPending;
    gTerrainHelperPending = {};
    if (pending.active && pending.sample != nullptr) {
        const VectorWindow afterWindow =
            captureVectorWindow(pending.commandVector);
        const bool destinationInPreUsed =
            pending.beforeWindow.valid &&
            payloadInsideRange(
                pending.destination,
                pending.beforeWindow.begin,
                pending.beforeWindow.end);
        const bool destinationInPostUsed =
            afterWindow.valid &&
            payloadInsideRange(
                pending.destination,
                afterWindow.begin,
                afterWindow.end);
        const bool appended64 =
            pending.beforeWindow.valid && afterWindow.valid &&
            afterWindow.usedBytes == pending.beforeWindow.usedBytes + payloadSizeBytes;
        const bool destinationMatchesPostElement =
            afterWindow.valid && afterWindow.usedBytes >= payloadSizeBytes &&
            pending.destination == afterWindow.end - payloadSizeBytes;

        std::uintptr_t payloadAddress = 0;
        if (destinationInPostUsed) {
            payloadAddress = pending.destination;
        } else if (appended64) {
            payloadAddress = afterWindow.end - payloadSizeBytes;
        }

        if (payloadAddress != 0 &&
                payloadInsideRange(payloadAddress, afterWindow.begin, afterWindow.end)) {
            const auto payload = readPayload(payloadAddress);
            const auto* beforePayload = pending.beforePayloadValid ?
                &pending.beforePayload : nullptr;
            recordPayload(
                *pending.sample, payload, beforePayload, afterWindow,
                appended64, destinationInPreUsed, destinationInPostUsed,
                destinationMatchesPostElement);
        } else {
            pending.sample->payloadCaptureFailures.fetch_add(
                1, std::memory_order_relaxed);
            gPayloadCaptureFailures.fetch_add(1, std::memory_order_relaxed);
            if (pending.beforeWindow.valid && afterWindow.valid &&
                    afterWindow.usedBytes !=
                        pending.beforeWindow.usedBytes + payloadSizeBytes) {
                pending.sample->unexpectedGrowthCalls.fetch_add(
                    1, std::memory_order_relaxed);
                gPayloadUnexpectedGrowthCalls.fetch_add(
                    1, std::memory_order_relaxed);
            }
        }
    }
    self->mCallbacksInFlight.fetch_sub(1, std::memory_order_acq_rel);
}

void LevelRenderHook::createTimeline() noexcept {
    std::ofstream out(mTimelinePath, std::ios::trunc);
    if (!out) return;
    out << "timestamp_unix_ms,state,minecraft_pre_calls,minecraft_post_calls,"
           "terrain_task_calls,terrain_helper_calls,helper_inside_task_calls,"
           "helper_outside_task_calls,minecraft_thread_id,terrain_thread_id,"
           "terrain_helper_thread_id,camera_valid,forward_x,forward_y,forward_z,"
           "near,far,horizontal_fov,vertical_fov,aspect,vector_used_bytes,"
           "vector_capacity_bytes,terrain_flag,descriptor_sample_count,"
           "minimum_helpers_per_task,maximum_helpers_per_task,"
           "payload_capture_calls,payload_capture_failures,payload_append_64_calls,"
           "payload_unexpected_growth_calls\n";
}

void LevelRenderHook::appendTimeline(const char* state) noexcept {
    const CameraSnapshot camera = readSnapshot();
    std::size_t descriptorCount = 0;
    for (const auto& sample : gDescriptorSamples) {
        if (sample.key.load(std::memory_order_acquire) >= 2) ++descriptorCount;
    }
    const std::uint32_t minimum = gMinimumHelperCallsPerTask.load(std::memory_order_relaxed);

    std::ofstream out(mTimelinePath, std::ios::app);
    if (!out) return;
    out << std::fixed << std::setprecision(7)
        << unixMillisecondsNow() << ',' << state
        << ',' << mMinecraftPreCalls.load(std::memory_order_relaxed)
        << ',' << mMinecraftPostCalls.load(std::memory_order_relaxed)
        << ',' << mTerrainTaskCalls.load(std::memory_order_relaxed)
        << ',' << mTerrainHelperCalls.load(std::memory_order_relaxed)
        << ',' << mTerrainHelperInsideTaskCalls.load(std::memory_order_relaxed)
        << ',' << mTerrainHelperOutsideTaskCalls.load(std::memory_order_relaxed)
        << ',' << mMinecraftThreadId.load(std::memory_order_relaxed)
        << ',' << mTerrainThreadId.load(std::memory_order_relaxed)
        << ',' << mTerrainHelperThreadId.load(std::memory_order_relaxed)
        << ',' << (camera.valid ? 1 : 0)
        << ',' << camera.derived.forward[0]
        << ',' << camera.derived.forward[1]
        << ',' << camera.derived.forward[2]
        << ',' << camera.derived.nearDistance
        << ',' << camera.derived.farDistance
        << ',' << camera.derived.horizontalFov
        << ',' << camera.derived.verticalFov
        << ',' << camera.derived.aspect
        << ',' << gTerrainLatestUsedBytes.load(std::memory_order_relaxed)
        << ',' << gTerrainLatestCapacityBytes.load(std::memory_order_relaxed)
        << ',' << static_cast<unsigned>(gTerrainFlag.load(std::memory_order_relaxed))
        << ',' << descriptorCount
        << ',' << (minimum == std::numeric_limits<std::uint32_t>::max() ? 0 : minimum)
        << ',' << gMaximumHelperCallsPerTask.load(std::memory_order_relaxed)
        << ',' << gPayloadCaptureCalls.load(std::memory_order_relaxed)
        << ',' << gPayloadCaptureFailures.load(std::memory_order_relaxed)
        << ',' << gPayloadAppend64Calls.load(std::memory_order_relaxed)
        << ',' << gPayloadUnexpectedGrowthCalls.load(std::memory_order_relaxed)
        << '\n';
}

void LevelRenderHook::writeStatus(const char* state) noexcept {
    const CameraSnapshot camera = readSnapshot();
    const auto physics = mPhysicsScheduler.renderSnapshot();
    std::size_t descriptorCount = 0;
    for (const auto& sample : gDescriptorSamples) {
        if (sample.key.load(std::memory_order_acquire) >= 2) ++descriptorCount;
    }
    const std::uint32_t minimumHelpers =
        gMinimumHelperCallsPerTask.load(std::memory_order_relaxed);

    std::ofstream out(mStatusPath, std::ios::trunc);
    if (!out) return;
    out << std::boolalpha << std::fixed << std::setprecision(7)
        << "schema=11\nstate=" << state
        << "\nsource=terrain_command_payload_decoding"
        << "\nread_only=true"
        << "\nvisible_geometry_expected=false"
        << "\ngeometry_submission=none_discovery_only"
        << "\nminecraft_owned_submission=not_attempted"
        << "\nminecraft_target=LevelRendererCamera::render+0xbd6f97c"
        << "\nterrain_target=LevelRendererCameraAnon::framebuilderInsertTerrainCommandsForChunks_task_operator+0xbdb87b8"
        << "\nterrain_helper_target=terrain_command_construction_helper+0x1266ee9c"
        << "\nhelper_signature=8_integer_pointer_arguments_plus_float_s0"
        << "\nhelper_scope=thread_local_outer_terrain_task_gate"
        << "\ndescriptor_census=fixed_atomic_slots_no_heap_no_locks"
        << "\npayload_decode=post_call_vector_bounded_64_byte_qwords"
        << "\npayload_size_bytes=64"
        << "\npayload_storage=fixed_atomic_descriptor_slots_no_heap_no_locks"
        << "\npayload_before_capture=gated_destination_inside_pre_call_vector_size"
        << "\npayload_after_capture=gated_destination_or_new_last_element_inside_post_call_vector_size"
        << "\ncontainer_interpretation=closure_0x30_begin_0x38_end_0x40_capacity"
        << "\ncontainer_element_type=64_byte_terrain_command_payload_unknown_layout"
        << "\nclosure_flag_interpretation=bitmask_bits_0_1_2_gate_optional_command_groups"
        << "\nfingerprint_validated=" << mFingerprintValidated.load(std::memory_order_relaxed)
        << "\nminecraft_prefix_validated=" << mMinecraftPrefixValidated.load(std::memory_order_relaxed)
        << "\nterrain_prefix_validated=" << mTerrainPrefixValidated.load(std::memory_order_relaxed)
        << "\nterrain_helper_prefix_validated=" << mTerrainHelperPrefixValidated.load(std::memory_order_relaxed)
        << "\nminecraft_hook_installed=" << mMinecraftHook.installed()
        << "\nterrain_hook_installed=" << mTerrainHook.installed()
        << "\nterrain_helper_hook_installed=" << mTerrainHelperHook.installed()
        << "\nminecraft_pre_calls=" << mMinecraftPreCalls.load(std::memory_order_relaxed)
        << "\nminecraft_post_calls=" << mMinecraftPostCalls.load(std::memory_order_relaxed)
        << "\nterrain_task_calls=" << mTerrainTaskCalls.load(std::memory_order_relaxed)
        << "\nterrain_helper_calls=" << mTerrainHelperCalls.load(std::memory_order_relaxed)
        << "\nterrain_helper_inside_task_calls=" << mTerrainHelperInsideTaskCalls.load(std::memory_order_relaxed)
        << "\nterrain_helper_outside_task_calls=" << mTerrainHelperOutsideTaskCalls.load(std::memory_order_relaxed)
        << "\npre_camera_valid_samples=" << gPreValid.load(std::memory_order_relaxed)
        << "\npre_camera_invalid_samples=" << gPreInvalid.load(std::memory_order_relaxed)
        << "\npost_camera_valid_samples=" << gPostValid.load(std::memory_order_relaxed)
        << "\npost_camera_invalid_samples=" << gPostInvalid.load(std::memory_order_relaxed)
        << "\nrelative_origin_changed_frames=" << gOriginChanged.load(std::memory_order_relaxed)
        << "\nfrustum_changed_frames=" << gFrustumChanged.load(std::memory_order_relaxed)
        << "\nfrustum_changed_elements=" << gFrustumChangedElements.load(std::memory_order_relaxed)
        << "\nfrustum_maximum_delta=" << gFrustumMaxDelta.load(std::memory_order_relaxed)
        << "\ncamera_snapshot_valid=" << camera.valid
        << "\ncamera_relative_origin=" << camera.relativeOrigin[0] << ','
        << camera.relativeOrigin[1] << ',' << camera.relativeOrigin[2]
        << "\nforward=" << camera.derived.forward[0] << ','
        << camera.derived.forward[1] << ',' << camera.derived.forward[2]
        << "\nnear_distance=" << camera.derived.nearDistance
        << "\nfar_distance=" << camera.derived.farDistance
        << "\nhorizontal_fov_degrees=" << camera.derived.horizontalFov
        << "\nvertical_fov_degrees=" << camera.derived.verticalFov
        << "\naspect_ratio=" << camera.derived.aspect
        << "\nminimum_plane_normal_length=" << camera.derived.minPlaneLength
        << "\nmaximum_plane_normal_length=" << camera.derived.maxPlaneLength
        << "\nterrain_vector_valid_tasks=" << gTerrainVectorValidTasks.load(std::memory_order_relaxed)
        << "\nterrain_vector_invalid_tasks=" << gTerrainVectorInvalidTasks.load(std::memory_order_relaxed)
        << "\nterrain_vector_latest_used_bytes=" << gTerrainLatestUsedBytes.load(std::memory_order_relaxed)
        << "\nterrain_vector_latest_capacity_bytes=" << gTerrainLatestCapacityBytes.load(std::memory_order_relaxed)
        << "\nterrain_vector_maximum_used_bytes=" << gTerrainMaximumUsedBytes.load(std::memory_order_relaxed)
        << "\nterrain_vector_maximum_capacity_bytes=" << gTerrainMaximumCapacityBytes.load(std::memory_order_relaxed)
        << "\nhelper_vector_argument_matches=" << gHelperVectorArgumentMatches.load(std::memory_order_relaxed)
        << "\nhelper_vector_argument_mismatches=" << gHelperVectorArgumentMismatches.load(std::memory_order_relaxed)
        << "\nhelper_context_argument_matches=" << gHelperContextArgumentMatches.load(std::memory_order_relaxed)
        << "\nhelper_context_argument_mismatches=" << gHelperContextArgumentMismatches.load(std::memory_order_relaxed)
        << "\nhelper_render_object_argument_matches=" << gHelperRenderObjectArgumentMatches.load(std::memory_order_relaxed)
        << "\nhelper_render_object_argument_mismatches=" << gHelperRenderObjectArgumentMismatches.load(std::memory_order_relaxed)
        << "\nhelper_view_argument_matches=" << gHelperViewArgumentMatches.load(std::memory_order_relaxed)
        << "\nhelper_view_argument_mismatches=" << gHelperViewArgumentMismatches.load(std::memory_order_relaxed)
        << "\ndescriptor_sample_count=" << descriptorCount
        << "\ndescriptor_sample_capacity=" << descriptorSampleCapacity
        << "\ndescriptor_sample_overflow=" << gDescriptorSampleOverflow.load(std::memory_order_relaxed)
        << "\ndescriptor_in_module_calls=" << gDescriptorInModuleCalls.load(std::memory_order_relaxed)
        << "\ndescriptor_outside_module_calls=" << gDescriptorOutsideModuleCalls.load(std::memory_order_relaxed)
        << "\nminimum_helpers_per_task="
        << (minimumHelpers == std::numeric_limits<std::uint32_t>::max() ? 0 : minimumHelpers)
        << "\nmaximum_helpers_per_task=" << gMaximumHelperCallsPerTask.load(std::memory_order_relaxed)
        << "\npayload_capture_calls=" << gPayloadCaptureCalls.load(std::memory_order_relaxed)
        << "\npayload_capture_failures=" << gPayloadCaptureFailures.load(std::memory_order_relaxed)
        << "\npayload_append_64_calls=" << gPayloadAppend64Calls.load(std::memory_order_relaxed)
        << "\npayload_unexpected_growth_calls=" << gPayloadUnexpectedGrowthCalls.load(std::memory_order_relaxed)
        << "\npayload_before_image_calls=" << gPayloadBeforeImageCalls.load(std::memory_order_relaxed)
        << "\npayload_destination_in_pre_used_calls=" << gPayloadDestinationInPreUsedCalls.load(std::memory_order_relaxed)
        << "\npayload_destination_in_post_used_calls=" << gPayloadDestinationInPostUsedCalls.load(std::memory_order_relaxed)
        << "\npayload_destination_matches_post_element_calls=" << gPayloadDestinationMatchesPostElementCalls.load(std::memory_order_relaxed)
        << "\npayload_pending_overwrite_calls=" << gPayloadPendingOverwriteCalls.load(std::memory_order_relaxed)
        << '\n';

    for (std::size_t i = 0; i < viewCandidateCount; ++i) {
        out << "view_candidate_" << i << '=' << camera.view[i * 3] << ','
            << camera.view[i * 3 + 1] << ',' << camera.view[i * 3 + 2] << '\n'
            << "view_candidate_" << i << "_changed_frames="
            << gViewChanged[i].load(std::memory_order_relaxed) << '\n'
            << "view_candidate_" << i << "_maximum_delta="
            << gViewMaxDelta[i].load(std::memory_order_relaxed) << '\n';
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
    for (std::size_t i = 0; i < gTerrainQwords.size(); ++i) {
        out << "terrain_qword_0x" << std::setw(2) << std::setfill('0')
            << (8 + i * 8) << "=0x"
            << gTerrainQwords[i].load(std::memory_order_relaxed) << '\n';
    }
    out << std::dec << std::setfill(' ')
        << "terrain_flag_0x48="
        << static_cast<unsigned>(gTerrainFlag.load(std::memory_order_relaxed)) << '\n';
    for (std::size_t i = 0; i < gTerrainFlagCounts.size(); ++i) {
        out << "terrain_flag_" << i << "_calls="
            << gTerrainFlagCounts[i].load(std::memory_order_relaxed) << '\n';
    }
    out << "terrain_other_flag_calls="
        << gTerrainOtherFlagCount.load(std::memory_order_relaxed) << '\n';

    for (std::size_t count = 0; count < gHelperCallsPerTask.size(); ++count) {
        out << "helper_calls_per_task_" << count << '='
            << gHelperCallsPerTask[count].load(std::memory_order_relaxed) << '\n';
    }
    out << "helper_calls_per_task_overflow="
        << gHelperCallsPerTaskOverflow.load(std::memory_order_relaxed) << '\n';

    std::size_t outputIndex = 0;
    for (const auto& sample : gDescriptorSamples) {
        const std::uint64_t key = sample.key.load(std::memory_order_acquire);
        if (key < 2) continue;
        const std::uint64_t relative =
            sample.descriptorRelative.load(std::memory_order_relaxed);
        const std::uint64_t minimumUsed =
            sample.minimumUsedBytes.load(std::memory_order_relaxed);
        const std::uint64_t minimumCapacity =
            sample.minimumCapacityBytes.load(std::memory_order_relaxed);
        out << "descriptor_" << outputIndex << "_key=0x" << std::hex << key << std::dec << '\n'
            << "descriptor_" << outputIndex << "_relative=";
        if (relative == std::numeric_limits<std::uint64_t>::max()) {
            out << "outside_module\n";
        } else {
            out << "0x" << std::hex << relative << std::dec << '\n';
        }
        out << "descriptor_" << outputIndex << "_mode="
            << sample.mode.load(std::memory_order_relaxed) << '\n'
            << "descriptor_" << outputIndex << "_scale="
            << std::bit_cast<float>(sample.scaleBits.load(std::memory_order_relaxed)) << '\n'
            << "descriptor_" << outputIndex << "_calls="
            << sample.calls.load(std::memory_order_relaxed) << '\n'
            << "descriptor_" << outputIndex << "_first_ordinal="
            << sample.firstOrdinal.load(std::memory_order_relaxed) << '\n'
            << "descriptor_" << outputIndex << "_last_ordinal="
            << sample.lastOrdinal.load(std::memory_order_relaxed) << '\n'
            << "descriptor_" << outputIndex << "_minimum_ordinal="
            << sample.minimumOrdinal.load(std::memory_order_relaxed) << '\n'
            << "descriptor_" << outputIndex << "_maximum_ordinal="
            << sample.maximumOrdinal.load(std::memory_order_relaxed) << '\n'
            << "descriptor_" << outputIndex << "_minimum_used_bytes="
            << (minimumUsed == std::numeric_limits<std::uint64_t>::max() ? 0 : minimumUsed) << '\n'
            << "descriptor_" << outputIndex << "_maximum_used_bytes="
            << sample.maximumUsedBytes.load(std::memory_order_relaxed) << '\n'
            << "descriptor_" << outputIndex << "_minimum_capacity_bytes="
            << (minimumCapacity == std::numeric_limits<std::uint64_t>::max() ? 0 : minimumCapacity) << '\n'
            << "descriptor_" << outputIndex << "_maximum_capacity_bytes="
            << sample.maximumCapacityBytes.load(std::memory_order_relaxed) << '\n'
            << "descriptor_" << outputIndex << "_valid_vector_calls="
            << sample.validVectorCalls.load(std::memory_order_relaxed) << '\n'
            << "descriptor_" << outputIndex << "_invalid_vector_calls="
            << sample.invalidVectorCalls.load(std::memory_order_relaxed) << '\n'
            << "descriptor_" << outputIndex << "_view_matches="
            << sample.viewMatches.load(std::memory_order_relaxed) << '\n'
            << "descriptor_" << outputIndex << "_view_mismatches="
            << sample.viewMismatches.load(std::memory_order_relaxed) << '\n'
            << "descriptor_" << outputIndex << "_vector_object_matches="
            << sample.vectorObjectMatches.load(std::memory_order_relaxed) << '\n'
            << "descriptor_" << outputIndex << "_vector_object_mismatches="
            << sample.vectorObjectMismatches.load(std::memory_order_relaxed) << '\n'
            << "descriptor_" << outputIndex << "_flag_or="
            << static_cast<unsigned>(sample.flagOr.load(std::memory_order_relaxed)) << '\n'
            << "descriptor_" << outputIndex << "_first_thread_id="
            << sample.firstThreadId.load(std::memory_order_relaxed) << '\n'
            << "descriptor_" << outputIndex << "_other_thread_calls="
            << sample.otherThreadCalls.load(std::memory_order_relaxed) << '\n'
            << "descriptor_" << outputIndex << "_payload_capture_calls="
            << sample.payloadCaptureCalls.load(std::memory_order_relaxed) << '\n'
            << "descriptor_" << outputIndex << "_payload_capture_failures="
            << sample.payloadCaptureFailures.load(std::memory_order_relaxed) << '\n'
            << "descriptor_" << outputIndex << "_append_64_calls="
            << sample.append64Calls.load(std::memory_order_relaxed) << '\n'
            << "descriptor_" << outputIndex << "_unexpected_growth_calls="
            << sample.unexpectedGrowthCalls.load(std::memory_order_relaxed) << '\n'
            << "descriptor_" << outputIndex << "_before_image_calls="
            << sample.beforeImageCalls.load(std::memory_order_relaxed) << '\n'
            << "descriptor_" << outputIndex << "_destination_in_pre_used_calls="
            << sample.destinationInPreUsedCalls.load(std::memory_order_relaxed) << '\n'
            << "descriptor_" << outputIndex << "_destination_in_post_used_calls="
            << sample.destinationInPostUsedCalls.load(std::memory_order_relaxed) << '\n'
            << "descriptor_" << outputIndex << "_destination_matches_post_element_calls="
            << sample.destinationMatchesPostElementCalls.load(std::memory_order_relaxed) << '\n'
            << "descriptor_" << outputIndex << "_changed_qword_mask=0x"
            << std::hex
            << static_cast<unsigned>(sample.changedQwordMask.load(std::memory_order_relaxed))
            << std::dec << '\n'
            << "descriptor_" << outputIndex << "_nonzero_qword_mask=0x"
            << std::hex
            << static_cast<unsigned>(sample.nonzeroQwordMask.load(std::memory_order_relaxed))
            << std::dec << '\n'
            << "descriptor_" << outputIndex << "_varying_qword_mask=0x"
            << std::hex
            << static_cast<unsigned>(sample.varyingQwordMask.load(std::memory_order_relaxed))
            << std::dec << '\n'
            << "descriptor_" << outputIndex << "_module_pointer_qword_mask=0x"
            << std::hex
            << static_cast<unsigned>(sample.modulePointerQwordMask.load(std::memory_order_relaxed))
            << std::dec << '\n'
            << "descriptor_" << outputIndex << "_vector_pointer_qword_mask=0x"
            << std::hex
            << static_cast<unsigned>(sample.vectorPointerQwordMask.load(std::memory_order_relaxed))
            << std::dec << '\n'
            << "descriptor_" << outputIndex << "_aligned_pointer_like_qword_mask=0x"
            << std::hex
            << static_cast<unsigned>(
                sample.alignedPointerLikeQwordMask.load(std::memory_order_relaxed))
            << std::dec << '\n'
            << "descriptor_" << outputIndex << "_first_payload_complete="
            << (sample.firstPayloadState.load(std::memory_order_acquire) == 2) << '\n';
        for (std::size_t qword = 0; qword < payloadQwordCount; ++qword) {
            out << "descriptor_" << outputIndex << "_first_qword_" << qword
                << "=0x" << std::hex
                << sample.firstPayloadQwords[qword].load(std::memory_order_relaxed)
                << std::dec << '\n'
                << "descriptor_" << outputIndex << "_last_qword_" << qword
                << "=0x" << std::hex
                << sample.lastPayloadQwords[qword].load(std::memory_order_relaxed)
                << std::dec << '\n';
        }
        ++outputIndex;
    }

    out << "minecraft_render_thread_id=" << mMinecraftThreadId.load(std::memory_order_relaxed)
        << "\nterrain_task_thread_id=" << mTerrainThreadId.load(std::memory_order_relaxed)
        << "\nterrain_helper_thread_id=" << mTerrainHelperThreadId.load(std::memory_order_relaxed)
        << "\nrender_and_terrain_threads_match="
        << (mMinecraftThreadId.load(std::memory_order_relaxed) != 0 &&
            mMinecraftThreadId.load(std::memory_order_relaxed) ==
                mTerrainThreadId.load(std::memory_order_relaxed))
        << "\nterrain_and_helper_first_threads_match="
        << (mTerrainThreadId.load(std::memory_order_relaxed) != 0 &&
            mTerrainThreadId.load(std::memory_order_relaxed) ==
                mTerrainHelperThreadId.load(std::memory_order_relaxed))
        << "\nother_minecraft_thread_calls="
        << mOtherMinecraftThreadCalls.load(std::memory_order_relaxed)
        << "\nother_terrain_thread_calls="
        << mOtherTerrainThreadCalls.load(std::memory_order_relaxed)
        << "\nother_terrain_helper_thread_calls="
        << mOtherTerrainHelperThreadCalls.load(std::memory_order_relaxed)
        << "\nfirst_renderer=0x" << std::hex
        << mFirstRenderer.load(std::memory_order_relaxed)
        << "\nlast_render_context=0x" << mLastContext.load(std::memory_order_relaxed)
        << "\nlast_view=0x" << mLastView.load(std::memory_order_relaxed)
        << "\nlast_client=0x" << mLastClient.load(std::memory_order_relaxed)
        << "\nfirst_terrain_closure=0x"
        << mFirstTerrainClosure.load(std::memory_order_relaxed)
        << "\nlast_terrain_closure=0x"
        << mLastTerrainClosure.load(std::memory_order_relaxed)
        << "\nlast_terrain_task_context=0x"
        << mLastTerrainTaskContext.load(std::memory_order_relaxed)
        << std::dec
        << "\nevent_bus_published_events=" << mEventBus.publishedEvents()
        << "\nevent_bus_delivered_callbacks=" << mEventBus.deliveredCallbacks()
        << "\nphysics_snapshot_coherent=" << physics.coherent
        << "\nphysics_world_generation=" << physics.worldGeneration
        << "\nphysics_simulation_step=" << physics.simulationStep
        << "\ncallbacks_in_flight=" << mCallbacksInFlight.load(std::memory_order_relaxed)
        << "\nhook_restore_succeeded=" << mRestoreSucceeded.load(std::memory_order_relaxed)
        << "\nsafe_to_unload=" << safeToUnload()
        << "\nstatus_file=terrain-command-payload-status.txt"
        << "\ntimeline_file=terrain-command-payload-timeline.csv"
        << "\nfailure_reason="
        << (mFailureReason.empty() ? "none" : mFailureReason) << '\n';
}

void LevelRenderHook::removeHooks() noexcept {
    mMinecraftHook.reset();
    mTerrainHook.reset();
    mTerrainHelperHook.reset();

    using namespace std::chrono_literals;
    for (unsigned attempt = 0;
         attempt < 400U && mCallbacksInFlight.load(std::memory_order_acquire) != 0;
         ++attempt) {
        std::this_thread::sleep_for(5ms);
    }

    gOriginalMinecraftRender = nullptr;
    gOriginalTerrainTask = nullptr;
    gOriginalTerrainCommandHelper = nullptr;
    mRestoreSucceeded.store(
        !mMinecraftHook.installed() && !mTerrainHook.installed() &&
            !mTerrainHelperHook.installed() &&
            mCallbacksInFlight.load(std::memory_order_acquire) == 0,
        std::memory_order_release);
}

void LevelRenderHook::uninstall() noexcept {
    mStopRequested.store(true, std::memory_order_release);
    if (mWorker.joinable()) mWorker.join();
    removeHooks();
    LevelRenderHook* expected = this;
    (void)sActive.compare_exchange_strong(
        expected, nullptr, std::memory_order_acq_rel, std::memory_order_acquire);
    appendTimeline("stopped");
    writeStatus("stopped");
}

bool LevelRenderHook::safeToUnload() const noexcept {
    return mRestoreSucceeded.load(std::memory_order_acquire) &&
        !mMinecraftHook.installed() && !mTerrainHook.installed() &&
        !mTerrainHelperHook.installed() &&
        mCallbacksInFlight.load(std::memory_order_acquire) == 0;
}
}  // namespace aeronautics::bedrock
