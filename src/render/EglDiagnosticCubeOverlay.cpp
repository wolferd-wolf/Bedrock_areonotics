#include "render/EglDiagnosticCubeOverlay.hpp"

#include <array>
#include <cmath>

#include <GLES3/gl3.h>

namespace aeronautics::render {
namespace {

constexpr const char* vertexShaderSource = R"glsl(#version 300 es
layout(location = 0) in vec3 aPosition;
void main() {
    gl_Position = vec4(aPosition, 1.0);
}
)glsl";

constexpr const char* fragmentShaderSource = R"glsl(#version 300 es
precision mediump float;
out vec4 fragColor;
void main() {
    fragColor = vec4(1.0, 0.55, 0.08, 0.95);
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

}  // namespace

EglDiagnosticCubeOverlay::~EglDiagnosticCubeOverlay() {
    reset();
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

    glGenVertexArrays(1, &mVertexArray);
    glGenBuffers(1, &mVertexBuffer);
    glGenBuffers(1, &mIndexBuffer);
    if (mVertexArray == 0U || mVertexBuffer == 0U || mIndexBuffer == 0U) {
        reset();
        return false;
    }

    constexpr std::array<GLushort, 24> indices{
        0,1, 1,2, 2,3, 3,0,
        4,5, 5,6, 6,7, 7,4,
        0,4, 1,5, 2,6, 3,7
    };

    glBindVertexArray(mVertexArray);
    glBindBuffer(GL_ARRAY_BUFFER, mVertexBuffer);
    glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(8U * 3U * sizeof(float)), nullptr, GL_DYNAMIC_DRAW);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, mIndexBuffer);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, static_cast<GLsizeiptr>(indices.size() * sizeof(GLushort)), indices.data(), GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3 * static_cast<GLsizei>(sizeof(float)), nullptr);
    glBindVertexArray(0);

    mPositionLocation = 0;
    mInitialized = true;
    return true;
}

bool EglDiagnosticCubeOverlay::draw(std::uint64_t frameIndex) noexcept {
    if (!mInitialized && !initialize()) {
        ++mFailedFrames;
        return false;
    }

    GLint viewport[4]{};
    GLint previousProgram = 0;
    GLint previousVertexArray = 0;
    GLint previousArrayBuffer = 0;
    GLint previousElementBuffer = 0;
    GLfloat previousLineWidth = 1.0F;
    glGetIntegerv(GL_VIEWPORT, viewport);
    glGetIntegerv(GL_CURRENT_PROGRAM, &previousProgram);
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &previousVertexArray);
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &previousArrayBuffer);
    glGetIntegerv(GL_ELEMENT_ARRAY_BUFFER_BINDING, &previousElementBuffer);
    glGetFloatv(GL_LINE_WIDTH, &previousLineWidth);

    const GLboolean blendWasEnabled = glIsEnabled(GL_BLEND);
    const GLboolean depthWasEnabled = glIsEnabled(GL_DEPTH_TEST);
    const GLboolean cullWasEnabled = glIsEnabled(GL_CULL_FACE);
    const GLboolean scissorWasEnabled = glIsEnabled(GL_SCISSOR_TEST);

    const float time = static_cast<float>(frameIndex % 3600U) / 60.0F;
    const float angleY = time * 0.9F;
    const float angleX = time * 0.55F;
    const float cy = std::cos(angleY);
    const float sy = std::sin(angleY);
    const float cx = std::cos(angleX);
    const float sx = std::sin(angleX);
    const float bounce = -0.22F + std::abs(std::sin(time * 1.35F)) * 0.42F;
    const float aspect = viewport[3] > 0 ? static_cast<float>(viewport[2]) / static_cast<float>(viewport[3]) : 1.0F;
    const float scale = 0.34F;

    constexpr std::array<std::array<float, 3>, 8> corners{{
        {{-1.0F,-1.0F,-1.0F}}, {{1.0F,-1.0F,-1.0F}}, {{1.0F,1.0F,-1.0F}}, {{-1.0F,1.0F,-1.0F}},
        {{-1.0F,-1.0F, 1.0F}}, {{1.0F,-1.0F, 1.0F}}, {{1.0F,1.0F, 1.0F}}, {{-1.0F,1.0F, 1.0F}}
    }};
    std::array<float, 24> vertices{};
    for (std::size_t i = 0; i < corners.size(); ++i) {
        const float x0 = corners[i][0] * scale;
        const float y0 = corners[i][1] * scale;
        const float z0 = corners[i][2] * scale;
        const float x1 = x0 * cy + z0 * sy;
        const float z1 = -x0 * sy + z0 * cy;
        const float y2 = y0 * cx - z1 * sx;
        const float z2 = y0 * sx + z1 * cx + 2.8F;
        const float perspective = 1.55F / z2;
        vertices[i * 3U + 0U] = (x1 * perspective) / aspect;
        vertices[i * 3U + 1U] = y2 * perspective + bounce;
        vertices[i * 3U + 2U] = 0.0F;
    }

    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_SCISSOR_TEST);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glUseProgram(mProgram);
    glBindVertexArray(mVertexArray);
    glBindBuffer(GL_ARRAY_BUFFER, mVertexBuffer);
    glBufferSubData(GL_ARRAY_BUFFER, 0, static_cast<GLsizeiptr>(vertices.size() * sizeof(float)), vertices.data());
    glLineWidth(3.0F);
    glDrawElements(GL_LINES, 24, GL_UNSIGNED_SHORT, nullptr);

    glBindVertexArray(static_cast<GLuint>(previousVertexArray));
    glBindBuffer(GL_ARRAY_BUFFER, static_cast<GLuint>(previousArrayBuffer));
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, static_cast<GLuint>(previousElementBuffer));
    glUseProgram(static_cast<GLuint>(previousProgram));
    glLineWidth(previousLineWidth);
    if (blendWasEnabled == GL_FALSE) glDisable(GL_BLEND);
    if (depthWasEnabled == GL_TRUE) glEnable(GL_DEPTH_TEST);
    if (cullWasEnabled == GL_TRUE) glEnable(GL_CULL_FACE);
    if (scissorWasEnabled == GL_TRUE) glEnable(GL_SCISSOR_TEST);

    const GLenum error = glGetError();
    if (error != GL_NO_ERROR) {
        ++mFailedFrames;
        return false;
    }
    ++mSuccessfulFrames;
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
    mPositionLocation = -1;
    mInitialized = false;
}

}  // namespace aeronautics::render
