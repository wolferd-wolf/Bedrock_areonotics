#pragma once

#include <compare>
#include <cstddef>
#include <span>
#include <string_view>
#include <vector>

namespace aeronautics::physics {

struct GridPosition final {
    int x{};
    int y{};
    int z{};

    auto operator<=>(const GridPosition&) const = default;
};

enum class ShipBlockKind {
    hull,
    shipCore,
    helm,
    aeroEngine,
    otherSolid,
};

struct ShipBlock final {
    GridPosition position{};
    ShipBlockKind kind{ShipBlockKind::otherSolid};
    double massKilograms{1.0};
};

struct Vector3d final {
    double x{};
    double y{};
    double z{};
};

struct GridBounds final {
    GridPosition minimum{};
    GridPosition maximum{};
};

struct AssembledBlock final {
    GridPosition localPosition{};
    ShipBlockKind kind{ShipBlockKind::otherSolid};
    double massKilograms{1.0};

    auto operator<=>(const AssembledBlock&) const = default;
};

struct ShipAssemblySnapshot final {
    GridPosition coreWorldPosition{};
    GridBounds localBounds{};
    Vector3d centerOfMassLocalMeters{};
    Vector3d inertiaDiagonalKilogramMetersSquared{};
    double totalMassKilograms{};
    std::size_t helmCount{};
    std::size_t engineCount{};
    std::vector<AssembledBlock> blocks{};
};

enum class ShipAssemblyStatus {
    ready,
    coreNotFound,
    selectedBlockIsNotCore,
    duplicatePosition,
    invalidMass,
    blockLimitExceeded,
    boundsLimitExceeded,
    multipleConnectedCores,
    missingHelm,
    missingEngine,
};

struct ShipAssemblyResult final {
    ShipAssemblyStatus status{ShipAssemblyStatus::coreNotFound};
    ShipAssemblySnapshot snapshot{};

    [[nodiscard]] bool ready() const noexcept {
        return status == ShipAssemblyStatus::ready;
    }
};

struct ShipAssemblyConfig final {
    std::size_t maximumBlockCount{2048};
    int maximumSpanBlocks{64};
};

class ShipAssemblyBuilder final {
public:
    ShipAssemblyBuilder() = default;
    explicit ShipAssemblyBuilder(ShipAssemblyConfig config) noexcept;

    [[nodiscard]] ShipAssemblyResult build(
        std::span<const ShipBlock> candidates,
        GridPosition selectedCorePosition) const;

    [[nodiscard]] static std::string_view architectureMarker() noexcept;
    [[nodiscard]] static std::string_view statusName(
        ShipAssemblyStatus status) noexcept;

private:
    ShipAssemblyConfig mConfig{};
};

}  // namespace aeronautics::physics
