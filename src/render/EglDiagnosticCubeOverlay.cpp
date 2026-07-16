#include "render/EglDiagnosticCubeOverlay.hpp"

#include <array>
#include <cmath>

#include <GLES3/gl3.h>

namespace aeronautics::render {
namespace {

constexpr const char* vertexShaderSource = R"glsl(#version 300 es
layout(location = 0) in vec3 aPosition;
uniform mat4 uMvp;
void main() {
    gl_Position = uMvp * vec4(aPosition, 1.0);
}
)glsl";

constexpr const char* fragmentShaderSource = R"glsl(#version 300 es
precision mediump float;
out vec4 fragColor;
void main() {
    fragColor = vec4(1.0, 0.55, 0.08, 0.96);
}
)glsl";

[[nodiscard]] GLuint compileShader(GLenum type, const char* source) noexcept {
    const GLuint shader = glCreateShader(type);
    if (shader == 0U) {
        return 0U;
    }
    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);
    GLint compiled = GL_FALSE;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
    if (compiled != GL_TRUE) {
        glDeleteShader(shader);
        return 0U;
    }
    return shader;
}

void drainExistingGlErrors() noexcept {
    for (unsigned index = 0; index < 8U; ++index) {
        if (glGetError() == GL_NO_ERROR) {
            break;
        }
    }
}

}  // namespace

EglDiagnosticCubeOverlay::~EglDiagnosticCubeOverlay() {
    reset();
}

void EglDiagnosticCubeOverlay::beginSession() noexcept {
    mSuccessfulFrames.store(0, std::memory_order_relaxed);
    mFailedFrames.store(0, std::memory_order_relaxed);
    mValidCameraSamples.store(0, std::memory_order_relaxed);
    mInvalidCameraSamples.store(0, std::memory_order_relaxed);
    mIncoherentCameraReads.store(0, std::memory_order_relaxed);
    mFramesWithoutCamera.store(0, std::memory_order_relaxed);
    mAnchorResets.store(0, std::memory_order_relaxed);
    mDepthTestFrames.store(0, std::memory_order_relaxed);
    mDepthBits.store(0, std::memory_order_relaxed);
    mAnchorLocked.store(false, std::memory_order_release);
    mLastAnchorX.store(0.0F, std::memory_order_relaxed);
    mLastAnchorY.store(0.0F, std::memory_order_relaxed);
    mLastAnchorZ.store(0.0F, std::memory_order_relaxed);
    mCameraSequence.store(0, std::memory_order_release);
    mCameraTimestampNanoseconds.store(0, std::memory_order_release);
    mWorldAnchor = {};
    mAnchorWorldGeneration = 0;
    mHasWorldAnchor = false;
}

bool EglDiagnosticCubeOverlay::publishCamera(
    Vec3f position,
    Vec3f target,
    std::int64_t timestampNanoseconds) noexcept {
    if (!validCameraSample(position, target) || timestampNanoseconds <= 0) {
        mInvalidCameraSamples.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    mCameraSequence.fetch_add(1, std::memory_order_acq_rel);
    mCameraPositionX.store(position.x, std::memory_order_relaxed);
    mCameraPositionY.store(position.y, std::memory_order_relaxed);
    mCameraPositionZ.store(position.z, std::memory_order_relaxed);
    mCameraTargetX.store(target.x, std::memory_order_relaxed);
    mCameraTargetY.store(target.y, std::memory_order_relaxed);
    mCameraTargetZ.store(target.z, std::memory_order_relaxed);
    mCameraTimestampNanoseconds.store(timestampNanoseconds, std::memory_order_relaxed);
    mCameraSequence.fetch_add(1, std::memory_order_release);
    mValidCameraSamples.fetch_add(1, std::memory_order_relaxed);
    return true;
}

CameraSnapshot EglDiagnosticCubeOverlay::cameraSnapshot() const noexcept {
    for (unsigned attempt = 0; attempt < 8U; ++attempt) {
        const std::uint64_t before = mCameraSequence.load(std::memory_order_acquire);
        if ((before & 1U) != 0U) {
            continue;
        }

        CameraSnapshot snapshot{};
        snapshot.position = {
            mCameraPositionX.load(std::memory_order_relaxed),
            mCameraPositionY.load(std::memory_order_relaxed),
            mCameraPositionZ.load(std::memory_order_relaxed),
        };
        snapshot.target = {
            mCameraTargetX.load(std::memory_order_relaxed),
            mCameraTargetY.load(std::memory_order_relaxed),
            mCameraTargetZ.load(std::memory_order_relaxed),
        };
        snapshot.timestampNanoseconds =
            mCameraTimestampNanoseconds.load(std::memory_order_relaxed);

        const std::uint64_t after = mCameraSequence.load(std::memory_order_acquire);
        if (before == after && (after & 1U) == 0U && snapshot.timestampNanoseconds > 0 &&
            validCameraSample(snapshot.position, snapshot.target)) {
            snapshot.coherent = true;
            return snapshot;
        }
    }

    mIncoherentCameraReads.fetch_add(1, std::memory_order_relaxed);
    return {};
}

bool EglDiagnosticCubeOverlay::initialize() noexcept {
    const GLuint vertexShader = compileShader(GL_VERTEX_SHADER, vertexShaderSource);
    const GLuint fragmentShader = compileShader(GL_FRAGMENT_SHADER, fragmentShaderSource);
    if (vertexShader == 0U || fragmentShader == 0U) {
        if (vertexShader != 0U) glDeleteShader(vertexShader);
        if (fragmentShader != 0U) glDeleteShader(fragmentShader);
        return false;
    }

    mProgram = glCreateProgram();
    if (mProgram == 0U) {
        glDeleteShader(vertexShader);
        glDeleteShader(fragmentShader);
        return false;
    }
    glAttachShader(mProgram, vertexShader);
    glAttachShader(mProgram, fragmentShader);
    glLinkProgram(mProgram);
    glDeleteShader(vertexShader);
    glDeleteShader(fragmentShader);

    GLint linked = GL_FALSE;
    glGetProgramiv(mProgram, GL_LINK_STATUS, &linked);
    if (linked != GL_TRUE) {
        reset();
        return false;
    }

    mMvpLocation = glGetUniformLocation(mProgram, "uMvp");
    if (mMvpLocation < 0) {
        reset();
        return false;
    }

    glGenVertexArrays(1, &mVertexArray);
    glGenBuffers(1, &mVertexBuffer);
    glGenBuffers(1, &mIndexBuffer);
    if (mVertexArray == 0U || mVertexBuffer == 0U || mIndexBuffer == 0U) {
        reset();
        return false;
    }

    constexpr std::array<float, 24> vertices{
        -0.5F,-0.5F,-0.5F,  0.5F,-0.5F,-0.5F,
         0.5F, 0.5F,-0.5F, -0.5F, 0.5F,-0.5F,
        -0.5F,-0.5F, 0.5F,  0.5F,-0.5F, 0.5F,
         0.5F, 0.5F, 0.5F, -0.5F, 0.5F, 0.5F,
    };
    constexpr std::array<GLushort, 24> indices{
        0,1, 1,2, 2,3, 3,0,
        4,5, 5,6, 6,7, 7,4,
        0,4, 1,5, 2,6, 3,7,
    };

    glBindVertexArray(mVertexArray);
    glBindBuffer(GL_ARRAY_BUFFER, mVertexBuffer);
    glBufferData(
        GL_ARRAY_BUFFER,
        static_cast<GLsizeiptr>(vertices.size() * sizeof(float)),
        vertices.data(),
        GL_STATIC_DRAW);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, mIndexBuffer);
    glBufferData(
        GL_ELEMENT_ARRAY_BUFFER,
        static_cast<GLsizeiptr>(indices.size() * sizeof(GLushort)),
        indices.data(),
        GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(
        0,
        3,
        GL_FLOAT,
        GL_FALSE,
        3 * static_cast<GLsizei>(sizeof(float)),
        nullptr);
    glBindVertexArray(0);

    mInitialized.store(true, std::memory_order_release);
    return glGetError() == GL_NO_ERROR;
}

bool EglDiagnosticCubeOverlay::draw(
    std::uint64_t frameIndex,
    std::uint64_t worldGeneration,
    float verticalPhysicsOffsetMeters) noexcept {
    const CameraSnapshot camera = cameraSnapshot();
    if (!camera.coherent || worldGeneration == 0U) {
        mFramesWithoutCamera.fetch_add(1, std::memory_order_relaxed);
        mFailedFrames.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    if (!mHasWorldAnchor || mAnchorWorldGeneration != worldGeneration) {
        const Vec3f forward = normalize(camera.target - camera.position);
        mWorldAnchor = camera.position + forward * 5.0F;
        mWorldAnchor.y += 0.35F;
        mAnchorWorldGeneration = worldGeneration;
        mHasWorldAnchor = true;
        mAnchorLocked.store(true, std::memory_order_release);
        mLastAnchorX.store(mWorldAnchor.x, std::memory_order_relaxed);
        mLastAnchorY.store(mWorldAnchor.y, std::memory_order_relaxed);
        mLastAnchorZ.store(mWorldAnchor.z, std::memory_order_relaxed);
        mAnchorResets.fetch_add(1, std::memory_order_relaxed);
    }

    if (!mInitialized.load(std::memory_order_acquire) && !initialize()) {
        mFailedFrames.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    GLint viewport[4]{};
    GLint previousProgram = 0;
    GLint previousVertexArray = 0;
    GLint previousArrayBuffer = 0;
    GLint previousElementBuffer = 0;
    GLint previousDepthFunction = GL_LESS;
    GLint previousBlendSourceRgb = GL_ONE;
    GLint previousBlendDestinationRgb = GL_ZERO;
    GLint previousBlendSourceAlpha = GL_ONE;
    GLint previousBlendDestinationAlpha = GL_ZERO;
    GLint depthBits = 0;
    GLfloat previousLineWidth = 1.0F;
    GLboolean previousDepthWriteMask = GL_TRUE;

    glGetIntegerv(GL_VIEWPORT, viewport);
    glGetIntegerv(GL_CURRENT_PROGRAM, &previousProgram);
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &previousVertexArray);
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &previousArrayBuffer);
    glGetIntegerv(GL_ELEMENT_ARRAY_BUFFER_BINDING, &previousElementBuffer);
    glGetIntegerv(GL_DEPTH_FUNC, &previousDepthFunction);
    glGetIntegerv(GL_BLEND_SRC_RGB, &previousBlendSourceRgb);
    glGetIntegerv(GL_BLEND_DST_RGB, &previousBlendDestinationRgb);
    glGetIntegerv(GL_BLEND_SRC_ALPHA, &previousBlendSourceAlpha);
    glGetIntegerv(GL_BLEND_DST_ALPHA, &previousBlendDestinationAlpha);
    glGetIntegerv(GL_DEPTH_BITS, &depthBits);
    glGetFloatv(GL_LINE_WIDTH, &previousLineWidth);
    glGetBooleanv(GL_DEPTH_WRITEMASK, &previousDepthWriteMask);

    const GLboolean blendWasEnabled = glIsEnabled(GL_BLEND);
    const GLboolean depthWasEnabled = glIsEnabled(GL_DEPTH_TEST);
    const GLboolean cullWasEnabled = glIsEnabled(GL_CULL_FACE);
    const GLboolean scissorWasEnabled = glIsEnabled(GL_SCISSOR_TEST);

    if (viewport[2] <= 0 || viewport[3] <= 0) {
        mFailedFrames.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    const float time = static_cast<float>(frameIndex % 3600U) / 60.0F;
    const float angleY = time * 0.9F;
    const float angleX = time * 0.55F;
    const float clampedPhysicsOffset =
        std::fmax(0.0F, std::fmin(verticalPhysicsOffsetMeters, 1.0F));
    Vec3f animatedAnchor = mWorldAnchor;
    animatedAnchor.y += clampedPhysicsOffset;

    Mat4f mvp{};
    const float aspect = static_cast<float>(viewport[2]) / static_cast<float>(viewport[3]);
    if (!buildWorldSpaceMvp(
            camera.position,
            camera.target,
            animatedAnchor,
            aspect,
            angleX,
            angleY,
            mvp)) {
        mFailedFrames.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    drainExistingGlErrors();
    glDisable(GL_CULL_FACE);
    glDisable(GL_SCISSOR_TEST);
    glEnable(GL_BLEND);
    glBlendFuncSeparate(
        GL_SRC_ALPHA,
        GL_ONE_MINUS_SRC_ALPHA,
        GL_ONE,
        GL_ONE_MINUS_SRC_ALPHA);

    if (depthBits > 0) {
        glEnable(GL_DEPTH_TEST);
        glDepthFunc(GL_LEQUAL);
        glDepthMask(GL_FALSE);
        mDepthTestFrames.fetch_add(1, std::memory_order_relaxed);
    } else {
        glDisable(GL_DEPTH_TEST);
    }
    mDepthBits.store(depthBits, std::memory_order_relaxed);

    glUseProgram(mProgram);
    glUniformMatrix4fv(mMvpLocation, 1, GL_FALSE, mvp.values.data());
    glBindVertexArray(mVertexArray);
    glLineWidth(3.0F);
    glDrawElements(GL_LINES, 24, GL_UNSIGNED_SHORT, nullptr);

    glBindVertexArray(static_cast<GLuint>(previousVertexArray));
    glBindBuffer(GL_ARRAY_BUFFER, static_cast<GLuint>(previousArrayBuffer));
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, static_cast<GLuint>(previousElementBuffer));
    glUseProgram(static_cast<GLuint>(previousProgram));
    glLineWidth(previousLineWidth);
    glDepthFunc(static_cast<GLenum>(previousDepthFunction));
    glDepthMask(previousDepthWriteMask);
    glBlendFuncSeparate(
        static_cast<GLenum>(previousBlendSourceRgb),
        static_cast<GLenum>(previousBlendDestinationRgb),
        static_cast<GLenum>(previousBlendSourceAlpha),
        static_cast<GLenum>(previousBlendDestinationAlpha));

    if (blendWasEnabled == GL_FALSE) glDisable(GL_BLEND);
    if (depthWasEnabled == GL_TRUE) glEnable(GL_DEPTH_TEST);
    else glDisable(GL_DEPTH_TEST);
    if (cullWasEnabled == GL_TRUE) glEnable(GL_CULL_FACE);
    else glDisable(GL_CULL_FACE);
    if (scissorWasEnabled == GL_TRUE) glEnable(GL_SCISSOR_TEST);
    else glDisable(GL_SCISSOR_TEST);

    const GLenum error = glGetError();
    if (error != GL_NO_ERROR) {
        mFailedFrames.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    mSuccessfulFrames.fetch_add(1, std::memory_order_relaxed);
    return true;
}

void EglDiagnosticCubeOverlay::reset() noexcept {
    if (mIndexBuffer != 0U) glDeleteBuffers(1, &mIndexBuffer);
    if (mVertexBuffer != 0U) glDeleteBuffers(1, &mVertexBuffer);
    if (mVertexArray != 0U) glDeleteVertexArrays(1, &mVertexArray);
    if (mProgram != 0U) glDeleteProgram(mProgram);
    mProgram = 0U;
    mVertexArray = 0U;
    mVertexBuffer = 0U;
    mIndexBuffer = 0U;
    mMvpLocation = -1;
    mInitialized.store(false, std::memory_order_release);
    mHasWorldAnchor = false;
    mAnchorWorldGeneration = 0;
    mAnchorLocked.store(false, std::memory_order_release);
}

}  // namespace aeronautics::render
