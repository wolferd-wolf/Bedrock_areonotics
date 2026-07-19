#include "physics/AnchoredFlightProxy.hpp"

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

    require(
        AnchoredFlightProxy::architectureMarker() ==
            std::string_view{
                "anchored_flight_proxy=fixed_20hz_pd_lift_hover_return_v1"},
        "anchored flight proxy architecture marker changed");

    AnchoredFlightProxy proxy;
    require(!proxy.engage(), "unconfirmed proxy must not engage");
    proxy.confirmAssembly(1'000.0, 1);
    require(proxy.hasLiftAuthority(), "one engine must lift the test assembly");
    require(proxy.engage(), "confirmed proxy must engage");

    for (
        int step = 0;
        step < 800 && proxy.state().phase != AnchoredFlightPhase::hovering;
        ++step) {
        proxy.step();
    }
    require(
        proxy.state().phase == AnchoredFlightPhase::hovering,
        "proxy must settle into hover");
    require(
        std::abs(
            proxy.state().altitudeMeters -
            AnchoredFlightProxy::targetAltitudeMeters) <= 0.05,
        "hover altitude must settle at four meters");
    require(
        std::abs(proxy.state().velocityMetersPerSecond) <= 0.08,
        "hover velocity must settle");

    proxy.requestReturn();
    for (
        int step = 0;
        step < 800 && proxy.state().phase != AnchoredFlightPhase::idle;
        ++step) {
        proxy.step();
    }
    require(
        proxy.state().phase == AnchoredFlightPhase::idle,
        "proxy must finish its return");
    require(proxy.state().altitudeMeters == 0.0, "landed altitude must be zero");
    require(
        proxy.state().velocityMetersPerSecond == 0.0,
        "landed velocity must be zero");

    AnchoredFlightProxy overloaded;
    overloaded.confirmAssembly(10'000.0, 1);
    require(
        !overloaded.hasLiftAuthority(),
        "overloaded assembly must report insufficient thrust");
    require(!overloaded.engage(), "overloaded assembly must not engage");

    std::cout
        << "anchored flight proxy passed; fixed_step_hz=20; hover_m=4; return=landed\n";
    return EXIT_SUCCESS;
}
