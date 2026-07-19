#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace aeronautics::render {

template <std::size_t Count>
[[nodiscard]] constexpr std::uint64_t qwordDifferenceMask(
    const std::array<std::uint64_t, Count>& before,
    const std::array<std::uint64_t, Count>& after) noexcept {
    static_assert(Count <= 64, "qword mutation mask supports at most 64 values");

    std::uint64_t mask = 0;
    for (std::size_t index = 0; index < Count; ++index) {
        if (before[index] != after[index]) {
            mask |= std::uint64_t{1} << index;
        }
    }
    return mask;
}

}  // namespace aeronautics::render
