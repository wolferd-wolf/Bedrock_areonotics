#pragma once

#include <string_view>

namespace aeronautics::physics {

enum class FlightAuthority {
    inactive,
    manualHelm,
    handoff,
    autopilotHold,
    emergencyBrake,
};

struct ShipFlightState final {
    double altitudeMeters{};
    double verticalSpeedMetersPerSecond{};
    double headingRadians{};
    double yawRateRadiansPerSecond{};
    double forwardSpeedMetersPerSecond{};
};

struct PilotControl final {
    double forward{};
    double vertical{};
    double yaw{};
};

struct AutopilotSetpoint final {
    double altitudeMeters{};
    double headingRadians{};
    double forwardSpeedMetersPerSecond{};
};

struct FlightControlOutput final {
    double forward{};
    double vertical{};
    double yaw{};
    double handoffProgress{};
    FlightAuthority authority{FlightAuthority::inactive};
};

struct AutopilotConfig final {
    double handoffSeconds{1.0};
    double maximumForwardSpeedMetersPerSecond{20.0};
    double maximumVerticalSpeedMetersPerSecond{5.0};
    double maximumYawRateRadiansPerSecond{0.8};

    double speedProportionalGain{0.22};
    double speedIntegralGain{0.04};
    double altitudeProportionalGain{0.45};
    double verticalSpeedProportionalGain{0.30};
    double verticalSpeedIntegralGain{0.05};
    double headingProportionalGain{1.4};
    double yawRateProportionalGain{0.75};
    double yawRateIntegralGain{0.05};
    double integralLimit{4.0};
};

class AutopilotController final {
public:
    AutopilotController() noexcept = default;
    explicit AutopilotController(AutopilotConfig config) noexcept;

    void reset() noexcept;
    void enterManualHelm() noexcept;
    void leaveHelm(
        const ShipFlightState& state,
        const PilotControl& lastManualControl) noexcept;
    void engageHold(
        const ShipFlightState& state,
        double desiredForwardSpeedMetersPerSecond) noexcept;
    void engageEmergencyBrake(const ShipFlightState& state) noexcept;

    [[nodiscard]] FlightControlOutput update(
        const ShipFlightState& state,
        const PilotControl& manualControl,
        double deltaSeconds) noexcept;

    [[nodiscard]] FlightAuthority authority() const noexcept;
    [[nodiscard]] AutopilotSetpoint setpoint() const noexcept;
    [[nodiscard]] static std::string_view architectureMarker() noexcept;

private:
    [[nodiscard]] FlightControlOutput calculateAutopilot(
        const ShipFlightState& state,
        double deltaSeconds) noexcept;
    void captureHoldSetpoint(
        const ShipFlightState& state,
        double desiredForwardSpeedMetersPerSecond) noexcept;
    void resetIntegrators() noexcept;

    AutopilotConfig mConfig{};
    FlightAuthority mAuthority{FlightAuthority::inactive};
    AutopilotSetpoint mSetpoint{};
    PilotControl mLastManualControl{};
    double mHandoffElapsedSeconds{};
    double mSpeedIntegral{};
    double mVerticalSpeedIntegral{};
    double mYawRateIntegral{};
};

}  // namespace aeronautics::physics
