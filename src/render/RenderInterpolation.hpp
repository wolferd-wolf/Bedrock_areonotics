#pragma once

#include <algorithm>
#include <cstdint>

namespace aeronautics::render {

struct InterpolatedVerticalTransform final {
    std::int64_t positionYMicrometers{};
    std::uint32_t alphaPartsPerMillion{};
};

class RenderInterpolation final {
public:
    static constexpr std::int64_t physicsStepNanoseconds = 50'000'000;
    static constexpr std::uint32_t alphaOne = 1'000'000;

    [[nodiscard]] static constexpr std::uint32_t alphaPartsPerMillion(
        std::int64_t renderTimeNanoseconds,
        std::int64_t lastPhysicsTickNanoseconds) noexcept {
        if (lastPhysicsTickNanoseconds <= 0 ||
            renderTimeNanoseconds <= lastPhysicsTickNanoseconds) {
            return 0;
        }

        const std::int64_t elapsed =
            renderTimeNanoseconds - lastPhysicsTickNanoseconds;
        if (elapsed >= physicsStepNanoseconds) {
            return alphaOne;
        }

        return static_cast<std::uint32_t>(
            elapsed * static_cast<std::int64_t>(alphaOne) /
            physicsStepNanoseconds);
    }

    [[nodiscard]] static constexpr std::int64_t interpolateMicrometers(
        std::int64_t previous,
        std::int64_t current,
        std::uint32_t alphaPartsPerMillionValue) noexcept {
        const std::uint32_t clamped =
            std::min(alphaPartsPerMillionValue, alphaOne);
        const std::int64_t delta = current - previous;
        return previous +
            delta * static_cast<std::int64_t>(clamped) /
                static_cast<std::int64_t>(alphaOne);
    }

    [[nodiscard]] static constexpr InterpolatedVerticalTransform sample(
        std::int64_t previous,
        std::int64_t current,
        std::int64_t renderTimeNanoseconds,
        std::int64_t lastPhysicsTickNanoseconds) noexcept {
        const std::uint32_t alpha = alphaPartsPerMillion(
            renderTimeNanoseconds,
            lastPhysicsTickNanoseconds);
        return {interpolateMicrometers(previous, current, alpha), alpha};
    }
};

}  // namespace aeronautics::render
