#pragma once

#include <cstdint>

namespace aeronautics::physics {

struct VerticalBodyState final {
    std::int64_t positionYMicrometers{10'000'000};
    std::int64_t velocityYMicrometersPerSecond{0};
    bool grounded{false};
};

enum class VerticalContactResult : std::uint8_t {
    none,
    bounced,
    enteredSleep,
};

class VerticalBodyIntegrator final {
public:
    static constexpr std::int64_t fixedStepMilliseconds = 50;
    static constexpr std::int64_t gravityMicrometersPerSecondSquared = -9'810'000;
    static constexpr std::int64_t initialHeightMicrometers = 10'000'000;
    static constexpr std::int64_t sleepImpactSpeedMicrometersPerSecond = 500'000;
    static constexpr std::int64_t restitutionNumerator = 35;
    static constexpr std::int64_t restitutionDenominator = 100;

    static void reset(VerticalBodyState& state) noexcept {
        state.positionYMicrometers = initialHeightMicrometers;
        state.velocityYMicrometersPerSecond = 0;
        state.grounded = false;
    }

    [[nodiscard]] static VerticalContactResult step(
        VerticalBodyState& state) noexcept {
        if (state.grounded) {
            state.positionYMicrometers = 0;
            state.velocityYMicrometersPerSecond = 0;
            return VerticalContactResult::none;
        }

        state.velocityYMicrometersPerSecond +=
            gravityMicrometersPerSecondSquared * fixedStepMilliseconds / 1000;
        state.positionYMicrometers +=
            state.velocityYMicrometersPerSecond * fixedStepMilliseconds / 1000;

        if (state.positionYMicrometers > 0) {
            return VerticalContactResult::none;
        }

        state.positionYMicrometers = 0;
        const std::int64_t inboundSpeed =
            state.velocityYMicrometersPerSecond < 0
                ? -state.velocityYMicrometersPerSecond
                : 0;

        if (inboundSpeed <= sleepImpactSpeedMicrometersPerSecond) {
            state.velocityYMicrometersPerSecond = 0;
            state.grounded = true;
            return VerticalContactResult::enteredSleep;
        }

        state.velocityYMicrometersPerSecond =
            inboundSpeed * restitutionNumerator / restitutionDenominator;
        return VerticalContactResult::bounced;
    }
};

}  // namespace aeronautics::physics
