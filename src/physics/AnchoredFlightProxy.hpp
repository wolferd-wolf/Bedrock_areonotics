#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace aeronautics::physics {

enum class AnchoredFlightPhase {
    idle,
    ascending,
    hovering,
    returning,
};

struct AnchoredFlightProxyState final {
    AnchoredFlightPhase phase{AnchoredFlightPhase::idle};
    double altitudeMeters{};
    double velocityMetersPerSecond{};
    std::uint64_t simulationStep{};
};

class AnchoredFlightProxy final {
public:
    static constexpr double fixedStepSeconds = 0.05;
    static constexpr double targetAltitudeMeters = 4.0;
    static constexpr double gravityMetersPerSecondSquared = 9.81;
    static constexpr double engineThrustNewtons = 25'000.0;

    void confirmAssembly(double massKilograms, std::size_t engineCount) noexcept;
    [[nodiscard]] bool hasLiftAuthority() const noexcept;
    [[nodiscard]] bool engage() noexcept;
    void requestReturn() noexcept;
    void step() noexcept;
    void reset() noexcept;

    [[nodiscard]] double massKilograms() const noexcept { return mMassKilograms; }
    [[nodiscard]] std::size_t engineCount() const noexcept { return mEngineCount; }
    [[nodiscard]] const AnchoredFlightProxyState& state() const noexcept {
        return mState;
    }

    [[nodiscard]] static constexpr std::string_view architectureMarker() noexcept {
        return "anchored_flight_proxy=fixed_20hz_pd_lift_hover_return_v1";
    }

private:
    double mMassKilograms{};
    std::size_t mEngineCount{};
    AnchoredFlightProxyState mState{};
};

[[nodiscard]] constexpr std::string_view flightPhaseName(
    AnchoredFlightPhase phase) noexcept {
    switch (phase) {
    case AnchoredFlightPhase::idle:
        return "idle";
    case AnchoredFlightPhase::ascending:
        return "ascending";
    case AnchoredFlightPhase::hovering:
        return "hovering";
    case AnchoredFlightPhase::returning:
        return "returning";
    }
    return "unknown";
}

}  // namespace aeronautics::physics
