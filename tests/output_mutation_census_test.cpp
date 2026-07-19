#include "render/OutputMutationCensus.hpp"

#include <array>
#include <cstdint>
#include <cstdlib>
#include <iostream>

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
    using aeronautics::render::qwordDifferenceMask;

    constexpr std::array<std::uint64_t, 8> baseline{
        0, 1, 2, 3, 4, 5, 6, 7};
    constexpr auto unchanged = baseline;
    constexpr std::array<std::uint64_t, 8> changed{
        9, 1, 2, 8, 4, 5, 6, 10};

    static_assert(qwordDifferenceMask(baseline, unchanged) == 0);
    static_assert(qwordDifferenceMask(baseline, changed) == 0x89);

    require(
        qwordDifferenceMask(baseline, unchanged) == 0,
        "unchanged qwords must produce an empty mask");
    require(
        qwordDifferenceMask(baseline, changed) == 0x89,
        "changed qwords must map to their exact bit positions");

    std::cout
        << "output mutation census passed; unchanged=0x0; changed=0x89\n";
    return EXIT_SUCCESS;
}
