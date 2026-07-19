#include "physics/AnchoredFlightProxy.hpp"

#include <algorithm>
#include <cmath>

namespace aeronautics::physics {
namespace {

constexpr double positionGain = 4.5;
constexpr double velocityGain = 4.0;
constexpr double maximumUpwardAcceleration = 6.0;
constexpr double hoverPositionTolerance = 0.05;
constexpr double hoverVelocityTolerance = 0.08;

}  // namespace

void AnchoredFlightProxy::confirmAssembly(
    double massKilograms,
    std::size_t engineCount) noexcept {
    mMassKilograms =
        std::isfinite(massKilograms) && massKilograms > 0.0
        ? massKilograms
        : 0.0;
    mEngineCount = engineCount;
    mState = {};
}

bool AnchoredFlightProxy::hasLiftAuthority() const noexcept {
    if (mMassKilograms <= 0.0 || mEngineCount == 0U) {
        return false;
    }
    const double availableThrust =
        static_cast<double>(mEngineCount) * engineThrustNewtons;
    const double hoverThrust =
        mMassKilograms * gravityMetersPerSecondSquared;
    return availableThrust > hoverThrust * 1.05;
}

bool AnchoredFlightProxy::engage() noexcept {
    if (mState.phase != AnchoredFlightPhase::idle || !hasLiftAuthority()) {
        return false;
    }
    mState.phase = AnchoredFlightPhase::ascending;
    return true;
}

void AnchoredFlightProxy::requestReturn() noexcept {
    if (mState.phase != AnchoredFlightPhase::idle) {
        mState.phase = AnchoredFlightPhase::returning;
    }
}

void AnchoredFlightProxy::step() noexcept {
    if (mState.phase == AnchoredFlightPhase::idle) {
        return;
    }

    const double targetAltitude =
        mState.phase == AnchoredFlightPhase::returning
        ? 0.0
        : targetAltitudeMeters;
    const double positionError = targetAltitude - mState.altitudeMeters;
    const double commandedThrust =
        mMassKilograms *
        (gravityMetersPerSecondSquared +
         positionGain * positionError -
         velocityGain * mState.velocityMetersPerSecond);
    const double maximumThrust =
        static_cast<double>(mEngineCount) * engineThrustNewtons;
    const double thrust = std::clamp(commandedThrust, 0.0, maximumThrust);
    const double acceleration = std::clamp(
        thrust / mMassKilograms - gravityMetersPerSecondSquared,
        -gravityMetersPerSecondSquared,
        maximumUpwardAcceleration);

    mState.velocityMetersPerSecond += acceleration * fixedStepSeconds;
    mState.altitudeMeters +=
        mState.velocityMetersPerSecond * fixedStepSeconds;
    ++mState.simulationStep;

    if (
        mState.phase == AnchoredFlightPhase::returning &&
        mState.altitudeMeters <= 0.0) {
        mState.altitudeMeters = 0.0;
        mState.velocityMetersPerSecond = 0.0;
        mState.phase = AnchoredFlightPhase::idle;
        return;
    }

    if (
        mState.phase != AnchoredFlightPhase::returning &&
        std::abs(targetAltitudeMeters - mState.altitudeMeters) <=
            hoverPositionTolerance &&
        std::abs(mState.velocityMetersPerSecond) <=
            hoverVelocityTolerance) {
        mState.phase = AnchoredFlightPhase::hovering;
    }
}

void AnchoredFlightProxy::reset() noexcept {
    mState = {};
}

}  // namespace aeronautics::physics
