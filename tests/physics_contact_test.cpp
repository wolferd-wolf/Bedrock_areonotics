#include "physics/VerticalBodyIntegrator.hpp"

#include <cstdint>
#include <cstdlib>
#include <iostream>

namespace {

[[noreturn]] void fail(const char* message) {
    std::cerr << message << '\n';
    std::exit(EXIT_FAILURE);
}

}  // namespace

int main() {
    using aeronautics::physics::VerticalBodyIntegrator;
    using aeronautics::physics::VerticalBodyState;
    using aeronautics::physics::VerticalContactResult;

    VerticalBodyState state{};
    VerticalBodyIntegrator::reset(state);

    std::uint64_t impacts = 0;
    std::uint64_t sleeps = 0;
    std::uint64_t steps = 0;

    for (; steps < 400; ++steps) {
        const VerticalContactResult result = VerticalBodyIntegrator::step(state);
        if (result == VerticalContactResult::bounced ||
            result == VerticalContactResult::enteredSleep) {
            ++impacts;
        }
        if (result == VerticalContactResult::enteredSleep) {
            ++sleeps;
            break;
        }
    }

    if (!state.grounded) {
        fail("body never entered grounded sleep");
    }
    if (state.positionYMicrometers != 0 ||
        state.velocityYMicrometersPerSecond != 0) {
        fail("grounded body did not settle to an exact zero state");
    }
    if (sleeps != 1) {
        fail("body entered sleep an unexpected number of times");
    }
    if (impacts < 2 || impacts > 8) {
        fail("impact count indicates either no bounce or repeated jitter");
    }

    const std::uint64_t impactsBeforeRest = impacts;
    for (std::uint64_t restTick = 0; restTick < 200; ++restTick) {
        const VerticalContactResult result = VerticalBodyIntegrator::step(state);
        if (result != VerticalContactResult::none) {
            fail("grounded body generated another contact event");
        }
        if (!state.grounded || state.positionYMicrometers != 0 ||
            state.velocityYMicrometersPerSecond != 0) {
            fail("grounded body drifted while sleeping");
        }
    }

    if (impacts != impactsBeforeRest) {
        fail("impact count changed while the body was grounded");
    }

    VerticalBodyIntegrator::reset(state);
    if (state.grounded ||
        state.positionYMicrometers != VerticalBodyIntegrator::initialHeightMicrometers ||
        state.velocityYMicrometersPerSecond != 0) {
        fail("reset did not wake and restore the diagnostic body");
    }

    std::cout << "contact stabilization passed; impacts=" << impacts
              << "; settle_steps=" << (steps + 1U) << '\n';
    return EXIT_SUCCESS;
}
