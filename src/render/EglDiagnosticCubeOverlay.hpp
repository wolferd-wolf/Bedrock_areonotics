#pragma once

#include "render/WorldSpaceProjection.hpp"

#include <atomic>
#include <cstdint>

namespace aeronautics::render {

struct CameraSnapshot final {
    Vec3f position{};
    Vec3f target{};
    std::int64_t timestampNanoseconds{};
    bool coherent{};
};

class EglDiagnosticCubeOverlay final {
public:
    EglDiagnosticCubeOverlay() = default;
    ~EglDiagnosticCubeOverlay();

    EglDiagnosticCubeOverlay(const EglDiagnosticCubeOverlay&) = delete;
    EglDiagnosticCubeOverlay& operator=(const EglDiagnosticCubeOverlay&) = delete;

    void beginSession() noexcept;
    [[nodiscard]] bool publishCamera(
        Vec3f position,
        Vec3f target,
        std::int64_t timestampNanoseconds) noexcept;
    [[nodiscard]] CameraSnapshot cameraSnapshot() const noexcept;
    [[nodiscard]] bool draw(
        std::uint64_t frameIndex,
        std::uint64_t worldGeneration,
        float verticalPhysicsOffsetMeters) noexcept;
    void reset() noexcept;

    [[nodiscard]] bool initialized() const noexcept {
        return mInitialized.load(std::memory_order_acquire);
    }
    [[nodiscard]] std::uint64_t successfulFrames() const noexcept {
        return mSuccessfulFrames.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint64_t failedFrames() const noexcept {
        return mFailedFrames.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint64_t validCameraSamples() const noexcept {
        return mValidCameraSamples.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint64_t invalidCameraSamples() const noexcept {
        return mInvalidCameraSamples.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint64_t incoherentCameraReads() const noexcept {
        return mIncoherentCameraReads.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint64_t anchorResets() const noexcept {
        return mAnchorResets.load(std::memory_order_relaxed);
    }
    [[nodiscard]] bool anchorLocked() const noexcept {
        return mAnchorLocked.load(std::memory_order_acquire);
    }
    [[nodiscard]] Vec3f lastAnchor() const noexcept {
        return {
            mLastAnchorX.load(std::memory_order_relaxed),
            mLastAnchorY.load(std::memory_order_relaxed),
            mLastAnchorZ.load(std::memory_order_relaxed),
        };
    }
    [[nodiscard]] int depthBits() const noexcept {
        return mDepthBits.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint64_t depthTestFrames() const noexcept {
        return mDepthTestFrames.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint64_t framesWithoutCamera() const noexcept {
        return mFramesWithoutCamera.load(std::memory_order_relaxed);
    }

private:
    [[nodiscard]] bool initialize() noexcept;

    unsigned int mProgram{};
    unsigned int mVertexArray{};
    unsigned int mVertexBuffer{};
    unsigned int mIndexBuffer{};
    int mMvpLocation{-1};

    std::atomic_bool mInitialized{false};
    std::atomic<std::uint64_t> mSuccessfulFrames{0};
    std::atomic<std::uint64_t> mFailedFrames{0};
    std::atomic<std::uint64_t> mValidCameraSamples{0};
    std::atomic<std::uint64_t> mInvalidCameraSamples{0};
    mutable std::atomic<std::uint64_t> mIncoherentCameraReads{0};
    std::atomic<std::uint64_t> mFramesWithoutCamera{0};
    std::atomic<std::uint64_t> mAnchorResets{0};
    std::atomic<std::uint64_t> mDepthTestFrames{0};
    std::atomic<int> mDepthBits{0};
    std::atomic_bool mAnchorLocked{false};
    std::atomic<float> mLastAnchorX{0.0F};
    std::atomic<float> mLastAnchorY{0.0F};
    std::atomic<float> mLastAnchorZ{0.0F};

    std::atomic<std::uint64_t> mCameraSequence{0};
    std::atomic<float> mCameraPositionX{0.0F};
    std::atomic<float> mCameraPositionY{0.0F};
    std::atomic<float> mCameraPositionZ{0.0F};
    std::atomic<float> mCameraTargetX{0.0F};
    std::atomic<float> mCameraTargetY{0.0F};
    std::atomic<float> mCameraTargetZ{0.0F};
    std::atomic<std::int64_t> mCameraTimestampNanoseconds{0};

    Vec3f mWorldAnchor{};
    std::uint64_t mAnchorWorldGeneration{};
    bool mHasWorldAnchor{false};
};

}  // namespace aeronautics::render
