#pragma once

#include <cstdint>

namespace aeronautics::render {

class EglDiagnosticCubeOverlay final {
public:
    EglDiagnosticCubeOverlay() = default;
    ~EglDiagnosticCubeOverlay();

    EglDiagnosticCubeOverlay(const EglDiagnosticCubeOverlay&) = delete;
    EglDiagnosticCubeOverlay& operator=(const EglDiagnosticCubeOverlay&) = delete;

    [[nodiscard]] bool draw(std::uint64_t frameIndex) noexcept;
    void reset() noexcept;

    [[nodiscard]] bool initialized() const noexcept { return mInitialized; }
    [[nodiscard]] std::uint64_t successfulFrames() const noexcept { return mSuccessfulFrames; }
    [[nodiscard]] std::uint64_t failedFrames() const noexcept { return mFailedFrames; }

private:
    [[nodiscard]] bool initialize() noexcept;

    unsigned int mProgram{};
    unsigned int mVertexArray{};
    unsigned int mVertexBuffer{};
    unsigned int mIndexBuffer{};
    int mPositionLocation{-1};
    bool mInitialized{false};
    std::uint64_t mSuccessfulFrames{};
    std::uint64_t mFailedFrames{};
};

}  // namespace aeronautics::render
