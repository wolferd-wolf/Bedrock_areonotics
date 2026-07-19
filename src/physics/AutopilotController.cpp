#include "physics/AutopilotController.hpp"

#include <algorithm>
#include <cmath>

namespace aeronautics::physics {
namespace {

constexpr double pi = 3.14159265358979323846;

[[nodiscard]] double clampUnit(double value) noexcept {
    return std::clamp(value, -1.0, 1.0);
}

[[nodiscard]] double wrapRadians(double value) noexcept {
    while (value > pi) value -= 2.0 * pi;
    while (value < -pi) value += 2.0 * pi;
    return value;
}

[[nodiscard]] PilotControl clampControl(const PilotControl& control) noexcept {
    return {
        clampUnit(control.forward),
        clampUnit(control.vertical),
        clampUnit(control.yaw),
    };
}

[[nodiscard]] double safeDelta(double deltaSeconds) noexcept {
    if (!std::isfinite(deltaSeconds) || deltaSeconds <= 0.0) return 0.05;
    return std::clamp(deltaSeconds, 0.001, 0.25);
}

}  // namespace

AutopilotController::AutopilotController(AutopilotConfig config) noexcept
    : mConfig(config) {}

void AutopilotController::reset() noexcept {
    mAuthority = FlightAuthority::inactive;
    mSetpoint = {};
    mLastManualControl = {};
    mHandoffElapsedSeconds = 0.0;
    resetIntegrators();
}

void AutopilotController::enterManualHelm() noexcept {
    mAuthority = FlightAuthority::manualHelm;
    mHandoffElapsedSeconds = 0.0;
    resetIntegrators();
}

void AutopilotController::leaveHelm(
    const ShipFlightState& state,
    const PilotControl& lastManualControl) noexcept {
    captureHoldSetpoint(state, state.forwardSpeedMetersPerSecond);
    mLastManualControl = clampControl(lastManualControl);
    mHandoffElapsedSeconds = 0.0;
    mAuthority = FlightAuthority::handoff;
    resetIntegrators();
}

void AutopilotController::engageHold(
    const ShipFlightState& state,
    double desiredForwardSpeedMetersPerSecond) noexcept {
    captureHoldSetpoint(state, desiredForwardSpeedMetersPerSecond);
    mAuthority = FlightAuthority::autopilotHold;
    mHandoffElapsedSeconds = mConfig.handoffSeconds;
    resetIntegrators();
}

void AutopilotController::engageEmergencyBrake(
    const ShipFlightState& state) noexcept {
    captureHoldSetpoint(state, 0.0);
    mAuthority = FlightAuthority::emergencyBrake;
    mHandoffElapsedSeconds = mConfig.handoffSeconds;
    resetIntegrators();
}

FlightControlOutput AutopilotController::update(
    const ShipFlightState& state,
    const PilotControl& manualControl,
    double deltaSeconds) noexcept {
    const double delta = safeDelta(deltaSeconds);

    if (mAuthority == FlightAuthority::manualHelm) {
        const PilotControl clamped = clampControl(manualControl);
        mLastManualControl = clamped;
        return {
            clamped.forward,
            clamped.vertical,
            clamped.yaw,
            0.0,
            mAuthority,
        };
    }

    if (mAuthority == FlightAuthority::inactive) {
        return {};
    }

    FlightControlOutput automatic = calculateAutopilot(state, delta);
    if (mAuthority != FlightAuthority::handoff) {
        automatic.handoffProgress = 1.0;
        automatic.authority = mAuthority;
        return automatic;
    }

    mHandoffElapsedSeconds += delta;
    const double duration = std::max(mConfig.handoffSeconds, 0.001);
    const double blend = std::clamp(mHandoffElapsedSeconds / duration, 0.0, 1.0);
    automatic.forward =
        mLastManualControl.forward * (1.0 - blend) + automatic.forward * blend;
    automatic.vertical =
        mLastManualControl.vertical * (1.0 - blend) + automatic.vertical * blend;
    automatic.yaw =
        mLastManualControl.yaw * (1.0 - blend) + automatic.yaw * blend;
    automatic.handoffProgress = blend;
    automatic.authority = FlightAuthority::handoff;

    if (blend >= 1.0) {
        mAuthority = FlightAuthority::autopilotHold;
        automatic.authority = mAuthority;
    }
    return automatic;
}

FlightAuthority AutopilotController::authority() const noexcept {
    return mAuthority;
}

AutopilotSetpoint AutopilotController::setpoint() const noexcept {
    return mSetpoint;
}

std::string_view AutopilotController::architectureMarker() noexcept {
    return "autopilot=cascaded_speed_altitude_heading_hold_bounded_anti_windup";
}

FlightControlOutput AutopilotController::calculateAutopilot(
    const ShipFlightState& state,
    double deltaSeconds) noexcept {
    const double speedError =
        mSetpoint.forwardSpeedMetersPerSecond -
        state.forwardSpeedMetersPerSecond;
    mSpeedIntegral = std::clamp(
        mSpeedIntegral + speedError * deltaSeconds,
        -mConfig.integralLimit,
        mConfig.integralLimit);
    const double forward = clampUnit(
        mConfig.speedProportionalGain * speedError +
        mConfig.speedIntegralGain * mSpeedIntegral);

    const double altitudeError =
        mSetpoint.altitudeMeters - state.altitudeMeters;
    const double desiredVerticalSpeed = std::clamp(
        mConfig.altitudeProportionalGain * altitudeError,
        -mConfig.maximumVerticalSpeedMetersPerSecond,
        mConfig.maximumVerticalSpeedMetersPerSecond);
    const double verticalSpeedError =
        desiredVerticalSpeed - state.verticalSpeedMetersPerSecond;
    mVerticalSpeedIntegral = std::clamp(
        mVerticalSpeedIntegral + verticalSpeedError * deltaSeconds,
        -mConfig.integralLimit,
        mConfig.integralLimit);
    const double vertical = clampUnit(
        mConfig.verticalSpeedProportionalGain * verticalSpeedError +
        mConfig.verticalSpeedIntegralGain * mVerticalSpeedIntegral);

    const double headingError = wrapRadians(
        mSetpoint.headingRadians - state.headingRadians);
    const double desiredYawRate = std::clamp(
        mConfig.headingProportionalGain * headingError,
        -mConfig.maximumYawRateRadiansPerSecond,
        mConfig.maximumYawRateRadiansPerSecond);
    const double yawRateError =
        desiredYawRate - state.yawRateRadiansPerSecond;
    mYawRateIntegral = std::clamp(
        mYawRateIntegral + yawRateError * deltaSeconds,
        -mConfig.integralLimit,
        mConfig.integralLimit);
    const double yaw = clampUnit(
        mConfig.yawRateProportionalGain * yawRateError +
        mConfig.yawRateIntegralGain * mYawRateIntegral);

    return {
        forward,
        vertical,
        yaw,
        1.0,
        mAuthority,
    };
}

void AutopilotController::captureHoldSetpoint(
    const ShipFlightState& state,
    double desiredForwardSpeedMetersPerSecond) noexcept {
    mSetpoint.altitudeMeters = state.altitudeMeters;
    mSetpoint.headingRadians = wrapRadians(state.headingRadians);
    mSetpoint.forwardSpeedMetersPerSecond = std::clamp(
        desiredForwardSpeedMetersPerSecond,
        0.0,
        mConfig.maximumForwardSpeedMetersPerSecond);
}

void AutopilotController::resetIntegrators() noexcept {
    mSpeedIntegral = 0.0;
    mVerticalSpeedIntegral = 0.0;
    mYawRateIntegral = 0.0;
}

}  // namespace aeronautics::physics
