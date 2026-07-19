#include "physics/ShipAssemblyBuilder.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <queue>
#include <unordered_map>

namespace aeronautics::physics {
namespace {

struct GridPositionHash final {
    [[nodiscard]] std::size_t operator()(const GridPosition& position) const noexcept {
        const auto x = static_cast<std::uint32_t>(position.x);
        const auto y = static_cast<std::uint32_t>(position.y);
        const auto z = static_cast<std::uint32_t>(position.z);
        std::size_t seed = static_cast<std::size_t>(x) * 0x9e3779b1U;
        seed ^= static_cast<std::size_t>(y) * 0x85ebca77U;
        seed ^= static_cast<std::size_t>(z) * 0xc2b2ae3dU;
        return seed;
    }
};

constexpr std::array<GridPosition, 6> neighborOffsets{{
    {1, 0, 0},
    {-1, 0, 0},
    {0, 1, 0},
    {0, -1, 0},
    {0, 0, 1},
    {0, 0, -1},
}};

[[nodiscard]] GridPosition add(
    const GridPosition& left,
    const GridPosition& right) noexcept {
    return {left.x + right.x, left.y + right.y, left.z + right.z};
}

[[nodiscard]] GridPosition subtract(
    const GridPosition& left,
    const GridPosition& right) noexcept {
    return {left.x - right.x, left.y - right.y, left.z - right.z};
}

[[nodiscard]] int inclusiveSpan(int minimum, int maximum) noexcept {
    return maximum - minimum + 1;
}

}  // namespace

ShipAssemblyBuilder::ShipAssemblyBuilder(ShipAssemblyConfig config) noexcept
    : mConfig(config) {}

ShipAssemblyResult ShipAssemblyBuilder::build(
    std::span<const ShipBlock> candidates,
    GridPosition selectedCorePosition) const {
    ShipAssemblyResult result{};

    std::unordered_map<GridPosition, std::size_t, GridPositionHash> positions;
    positions.reserve(candidates.size());
    for (std::size_t index = 0; index < candidates.size(); ++index) {
        const ShipBlock& block = candidates[index];
        if (!std::isfinite(block.massKilograms) || block.massKilograms <= 0.0) {
            result.status = ShipAssemblyStatus::invalidMass;
            return result;
        }
        const auto [unused, inserted] = positions.emplace(block.position, index);
        static_cast<void>(unused);
        if (!inserted) {
            result.status = ShipAssemblyStatus::duplicatePosition;
            return result;
        }
    }

    const auto selected = positions.find(selectedCorePosition);
    if (selected == positions.end()) {
        result.status = ShipAssemblyStatus::coreNotFound;
        return result;
    }
    if (candidates[selected->second].kind != ShipBlockKind::shipCore) {
        result.status = ShipAssemblyStatus::selectedBlockIsNotCore;
        return result;
    }

    std::queue<GridPosition> pending;
    std::unordered_map<GridPosition, bool, GridPositionHash> visited;
    visited.reserve(std::min(candidates.size(), mConfig.maximumBlockCount));
    pending.push(selectedCorePosition);
    visited.emplace(selectedCorePosition, true);

    std::vector<const ShipBlock*> connected;
    connected.reserve(std::min(candidates.size(), mConfig.maximumBlockCount));
    while (!pending.empty()) {
        const GridPosition position = pending.front();
        pending.pop();
        connected.push_back(&candidates[positions.at(position)]);

        if (connected.size() > mConfig.maximumBlockCount) {
            result.status = ShipAssemblyStatus::blockLimitExceeded;
            return result;
        }

        for (const GridPosition& offset : neighborOffsets) {
            const GridPosition neighbor = add(position, offset);
            if (!positions.contains(neighbor) || visited.contains(neighbor)) {
                continue;
            }
            visited.emplace(neighbor, true);
            pending.push(neighbor);
        }
    }

    ShipAssemblySnapshot& snapshot = result.snapshot;
    snapshot.coreWorldPosition = selectedCorePosition;
    snapshot.blocks.reserve(connected.size());

    GridPosition minimum{
        std::numeric_limits<int>::max(),
        std::numeric_limits<int>::max(),
        std::numeric_limits<int>::max(),
    };
    GridPosition maximum{
        std::numeric_limits<int>::lowest(),
        std::numeric_limits<int>::lowest(),
        std::numeric_limits<int>::lowest(),
    };

    std::size_t coreCount = 0;
    Vector3d weightedCenter{};
    for (const ShipBlock* block : connected) {
        const GridPosition local = subtract(block->position, selectedCorePosition);
        minimum.x = std::min(minimum.x, local.x);
        minimum.y = std::min(minimum.y, local.y);
        minimum.z = std::min(minimum.z, local.z);
        maximum.x = std::max(maximum.x, local.x);
        maximum.y = std::max(maximum.y, local.y);
        maximum.z = std::max(maximum.z, local.z);

        snapshot.totalMassKilograms += block->massKilograms;
        weightedCenter.x += (static_cast<double>(local.x) + 0.5) * block->massKilograms;
        weightedCenter.y += (static_cast<double>(local.y) + 0.5) * block->massKilograms;
        weightedCenter.z += (static_cast<double>(local.z) + 0.5) * block->massKilograms;

        if (block->kind == ShipBlockKind::shipCore) ++coreCount;
        if (block->kind == ShipBlockKind::helm) ++snapshot.helmCount;
        if (block->kind == ShipBlockKind::aeroEngine) ++snapshot.engineCount;
        snapshot.blocks.push_back({local, block->kind, block->massKilograms});
    }

    snapshot.localBounds = {minimum, maximum};
    if (
        inclusiveSpan(minimum.x, maximum.x) > mConfig.maximumSpanBlocks ||
        inclusiveSpan(minimum.y, maximum.y) > mConfig.maximumSpanBlocks ||
        inclusiveSpan(minimum.z, maximum.z) > mConfig.maximumSpanBlocks) {
        result.status = ShipAssemblyStatus::boundsLimitExceeded;
        return result;
    }
    if (coreCount != 1U) {
        result.status = ShipAssemblyStatus::multipleConnectedCores;
        return result;
    }
    if (snapshot.helmCount == 0U) {
        result.status = ShipAssemblyStatus::missingHelm;
        return result;
    }
    if (snapshot.engineCount == 0U) {
        result.status = ShipAssemblyStatus::missingEngine;
        return result;
    }

    const double inverseMass = 1.0 / snapshot.totalMassKilograms;
    snapshot.centerOfMassLocalMeters = {
        weightedCenter.x * inverseMass,
        weightedCenter.y * inverseMass,
        weightedCenter.z * inverseMass,
    };

    for (const AssembledBlock& block : snapshot.blocks) {
        const double x =
            static_cast<double>(block.localPosition.x) + 0.5 -
            snapshot.centerOfMassLocalMeters.x;
        const double y =
            static_cast<double>(block.localPosition.y) + 0.5 -
            snapshot.centerOfMassLocalMeters.y;
        const double z =
            static_cast<double>(block.localPosition.z) + 0.5 -
            snapshot.centerOfMassLocalMeters.z;
        const double intrinsicCubeInertia = block.massKilograms / 6.0;
        snapshot.inertiaDiagonalKilogramMetersSquared.x +=
            block.massKilograms * (y * y + z * z) + intrinsicCubeInertia;
        snapshot.inertiaDiagonalKilogramMetersSquared.y +=
            block.massKilograms * (x * x + z * z) + intrinsicCubeInertia;
        snapshot.inertiaDiagonalKilogramMetersSquared.z +=
            block.massKilograms * (x * x + y * y) + intrinsicCubeInertia;
    }

    std::sort(
        snapshot.blocks.begin(),
        snapshot.blocks.end(),
        [](const AssembledBlock& left, const AssembledBlock& right) {
            return left.localPosition < right.localPosition;
        });

    result.status = ShipAssemblyStatus::ready;
    return result;
}

std::string_view ShipAssemblyBuilder::architectureMarker() noexcept {
    return "ship_assembly=core_seeded_six_face_flood_fill_mass_inertia_bounds_v1";
}

std::string_view ShipAssemblyBuilder::statusName(
    ShipAssemblyStatus status) noexcept {
    switch (status) {
        case ShipAssemblyStatus::ready: return "ready";
        case ShipAssemblyStatus::coreNotFound: return "core_not_found";
        case ShipAssemblyStatus::selectedBlockIsNotCore:
            return "selected_block_is_not_core";
        case ShipAssemblyStatus::duplicatePosition: return "duplicate_position";
        case ShipAssemblyStatus::invalidMass: return "invalid_mass";
        case ShipAssemblyStatus::blockLimitExceeded:
            return "block_limit_exceeded";
        case ShipAssemblyStatus::boundsLimitExceeded:
            return "bounds_limit_exceeded";
        case ShipAssemblyStatus::multipleConnectedCores:
            return "multiple_connected_cores";
        case ShipAssemblyStatus::missingHelm: return "missing_helm";
        case ShipAssemblyStatus::missingEngine: return "missing_engine";
    }
    return "unknown";
}

}  // namespace aeronautics::physics
