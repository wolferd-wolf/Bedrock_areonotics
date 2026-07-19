#include "physics/AutopilotController.hpp"

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <string_view>

namespace {

[[noreturn]] void fail(const char* message) {
    std::cerr << message << '\n';
    std::exit(EXIT_FAILURE);
}

void require(bool condition, const char* message) {
    if (!condition) fail(message);
}

}  // namespace

int main() {
    using namespace aeronautics::physics;

    AutopilotController controller;
    require(
        AutopilotController::architectureMarker() ==
            std::string_view{
                "autopilot=cascaded_speed_altitude_heading_hold_bounded_anti_windup"},
        "autopilot architecture marker changed unexpectedly");

    const ShipFlightState releaseState{
        80.0,
        0.0,
        3.13,
        0.0,
        6.0,
    };
    const PilotControl manual{0.7, 0.1, 0.2};

    controller.enterManualHelm();
    FlightControlOutput output =
        controller.update(releaseState, manual, 0.05);
    require(output.authority == FlightAuthority::manualHelm,
        "manual helm authority was not retained");
    require(std::abs(output.forward - manual.forward) < 1.0e-9,
        "manual forward input changed");

    controller.leaveHelm(releaseState, manual);
    require(controller.authority() == FlightAuthority::handoff,
        "leaving the helm did not begin handoff");
    const AutopilotSetpoint captured = controller.setpoint();
    require(std::abs(captured.altitudeMeters - 80.0) < 1.0e-9,
        "handoff did not capture altitude");
    require(std::abs(captured.forwardSpeedMetersPerSecond - 6.0) < 1.0e-9,
        "handoff did not capture forward speed");

    double previousProgress = 0.0;
    for (int tick = 0; tick < 20; ++tick) {
        output = controller.update(releaseState, {}, 0.05);
        require(output.handoffProgress >= previousProgress,
            "handoff progress moved backwards");
        previousProgress = output.handoffProgress;
    }
    require(controller.authority() == FlightAuthority::autopilotHold,
        "handoff did not complete into autopilot hold");
    require(output.authority == FlightAuthority::autopilotHold,
        "completed handoff output has the wrong authority");

    ShipFlightState lowState = releaseState;
    lowState.altitudeMeters = 70.0;
    output = controller.update(lowState, {}, 0.05);
    require(output.vertical > 0.0,
        "autopilot did not command climb below target altitude");

    ShipFlightState fastState = releaseState;
    fastState.forwardSpeedMetersPerSecond = 12.0;
    output = controller.update(fastState, {}, 0.05);
    require(output.forward < 0.0,
        "autopilot did not reduce excessive forward speed");

    ShipFlightState wrappedHeadingState = releaseState;
    wrappedHeadingState.headingRadians = -3.13;
    output = controller.update(wrappedHeadingState, {}, 0.05);
    require(std::abs(output.yaw) < 0.2,
        "heading controller did not choose the short wraparound path");

    controller.engageEmergencyBrake(releaseState);
    output = controller.update(releaseState, {}, 0.05);
    require(output.authority == FlightAuthority::emergencyBrake,
        "emergency brake authority was not retained");
    require(output.forward < 0.0,
        "emergency brake did not oppose forward motion");

    ShipFlightState extreme{
        -1000.0,
        -1000.0,
        1000.0,
        -1000.0,
        1000.0,
    };
    for (int tick = 0; tick < 500; ++tick) {
        output = controller.update(extreme, {}, 0.05);
        require(std::abs(output.forward) <= 1.0,
            "forward output exceeded bounds");
        require(std::abs(output.vertical) <= 1.0,
            "vertical output exceeded bounds");
        require(std::abs(output.yaw) <= 1.0,
            "yaw output exceeded bounds");
    }

    controller.enterManualHelm();
    output = controller.update(releaseState, {-2.0, 3.0, -4.0}, 0.05);
    require(
        output.forward == -1.0 &&
        output.vertical == 1.0 &&
        output.yaw == -1.0,
        "manual inputs were not bounded");

    std::cout
        << "autopilot handoff passed; authority=manual; bounded_outputs=true\n";
    return EXIT_SUCCESS;
}
