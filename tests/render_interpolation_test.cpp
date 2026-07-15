#include "render/RenderInterpolation.hpp"

#include <cstdint>
#include <iostream>

namespace {

[[nodiscard]] bool require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        return false;
    }
    return true;
}

}  // namespace

int main() {
    using aeronautics::render::RenderInterpolation;

    bool ok = true;
    ok &= require(
        RenderInterpolation::alphaPartsPerMillion(1'000, 0) == 0,
        "missing physics timestamp must produce alpha zero");
    ok &= require(
        RenderInterpolation::alphaPartsPerMillion(1'000, 2'000) == 0,
        "render time before physics time must clamp to zero");
    ok &= require(
        RenderInterpolation::alphaPartsPerMillion(25'000'000, 0) == 0,
        "zero timestamp remains invalid even at 25 ms");
    ok &= require(
        RenderInterpolation::alphaPartsPerMillion(125'000'000, 100'000'000) ==
            500'000,
        "25 ms into a 50 ms step must produce alpha 0.5");
    ok &= require(
        RenderInterpolation::alphaPartsPerMillion(150'000'000, 100'000'000) ==
            1'000'000,
        "50 ms must produce alpha one");
    ok &= require(
        RenderInterpolation::alphaPartsPerMillion(250'000'000, 100'000'000) ==
            1'000'000,
        "late render frames must clamp alpha to one");
    ok &= require(
        RenderInterpolation::interpolateMicrometers(
            10'000'000,
            8'000'000,
            500'000) == 9'000'000,
        "half-step falling interpolation is incorrect");
    ok &= require(
        RenderInterpolation::interpolateMicrometers(
            0,
            1'000'000,
            2'000'000) == 1'000'000,
        "interpolation alpha must clamp to one");

    const auto sample = RenderInterpolation::sample(
        4'000'000,
        2'000'000,
        225'000'000,
        200'000'000);
    ok &= require(
        sample.positionYMicrometers == 3'000'000,
        "sampled transform position is incorrect");
    ok &= require(
        sample.alphaPartsPerMillion == 500'000,
        "sampled transform alpha is incorrect");

    return ok ? 0 : 1;
}
