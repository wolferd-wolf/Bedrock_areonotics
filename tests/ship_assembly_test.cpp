#include "physics/ShipAssemblyBuilder.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <string_view>
#include <vector>

namespace {

[[noreturn]] void fail(const char* message) {
    std::cerr << message << '\n';
    std::exit(EXIT_FAILURE);
}

void require(bool condition, const char* message) {
    if (!condition) fail(message);
}

bool near(double left, double right, double tolerance = 1.0e-9) {
    return std::abs(left - right) <= tolerance;
}

}  // namespace

int main() {
    using namespace aeronautics::physics;

    require(
        ShipAssemblyBuilder::architectureMarker() ==
            std::string_view{
                "ship_assembly=core_seeded_six_face_flood_fill_mass_inertia_bounds_v1"},
        "ship assembly architecture marker changed unexpectedly");

    const GridPosition core{10, 70, -4};
    std::vector<ShipBlock> blocks{
        {core, ShipBlockKind::shipCore, 8.0},
        {{11, 70, -4}, ShipBlockKind::helm, 2.0},
        {{9, 70, -4}, ShipBlockKind::aeroEngine, 4.0},
        {{10, 71, -4}, ShipBlockKind::hull, 1.0},
        {{100, 100, 100}, ShipBlockKind::hull, 500.0},
    };

    ShipAssemblyBuilder builder;
    const ShipAssemblyResult assembled = builder.build(blocks, core);
    require(assembled.ready(), "valid connected ship did not assemble");
    require(assembled.snapshot.blocks.size() == 4U,
        "disconnected block was included in the ship");
    require(near(assembled.snapshot.totalMassKilograms, 15.0),
        "total connected mass was calculated incorrectly");
    require(assembled.snapshot.helmCount == 1U,
        "helm count was calculated incorrectly");
    require(assembled.snapshot.engineCount == 1U,
        "engine count was calculated incorrectly");
    require(assembled.snapshot.localBounds.minimum == GridPosition{-1, 0, 0},
        "minimum local bounds were calculated incorrectly");
    require(assembled.snapshot.localBounds.maximum == GridPosition{1, 1, 0},
        "maximum local bounds were calculated incorrectly");
    require(near(assembled.snapshot.centerOfMassLocalMeters.x, 5.5 / 15.0),
        "center of mass X was calculated incorrectly");
    require(near(assembled.snapshot.centerOfMassLocalMeters.y, 8.5 / 15.0),
        "center of mass Y was calculated incorrectly");
    require(near(assembled.snapshot.centerOfMassLocalMeters.z, 0.5),
        "center of mass Z was calculated incorrectly");
    require(assembled.snapshot.inertiaDiagonalKilogramMetersSquared.x > 0.0,
        "inertia X must be positive");
    require(assembled.snapshot.inertiaDiagonalKilogramMetersSquared.y > 0.0,
        "inertia Y must be positive");
    require(assembled.snapshot.inertiaDiagonalKilogramMetersSquared.z > 0.0,
        "inertia Z must be positive");

    std::reverse(blocks.begin(), blocks.end());
    const ShipAssemblyResult reversed = builder.build(blocks, core);
    require(reversed.ready(), "reordered input did not assemble");
    require(reversed.snapshot.blocks == assembled.snapshot.blocks,
        "assembly block order was not deterministic");
    require(near(
        reversed.snapshot.centerOfMassLocalMeters.x,
        assembled.snapshot.centerOfMassLocalMeters.x),
        "center of mass changed with input order");

    std::vector<ShipBlock> missingEngine{
        {core, ShipBlockKind::shipCore, 8.0},
        {{11, 70, -4}, ShipBlockKind::helm, 2.0},
    };
    require(
        builder.build(missingEngine, core).status ==
            ShipAssemblyStatus::missingEngine,
        "ship without an engine was accepted");

    std::vector<ShipBlock> duplicate{
        {core, ShipBlockKind::shipCore, 8.0},
        {core, ShipBlockKind::helm, 2.0},
    };
    require(
        builder.build(duplicate, core).status ==
            ShipAssemblyStatus::duplicatePosition,
        "duplicate block position was accepted");

    const ShipAssemblyBuilder bounded({3U, 64});
    require(
        bounded.build(
            std::vector<ShipBlock>{
                {core, ShipBlockKind::shipCore, 8.0},
                {{11, 70, -4}, ShipBlockKind::helm, 2.0},
                {{9, 70, -4}, ShipBlockKind::aeroEngine, 4.0},
                {{10, 71, -4}, ShipBlockKind::hull, 1.0},
            },
            core).status == ShipAssemblyStatus::blockLimitExceeded,
        "block count safety limit was not enforced");

    const ShipAssemblyBuilder spanBounded({2048U, 2});
    require(
        spanBounded.build(
            std::vector<ShipBlock>{
                {core, ShipBlockKind::shipCore, 8.0},
                {{11, 70, -4}, ShipBlockKind::helm, 2.0},
                {{12, 70, -4}, ShipBlockKind::aeroEngine, 4.0},
            },
            core).status == ShipAssemblyStatus::boundsLimitExceeded,
        "ship span safety limit was not enforced");

    std::cout
        << "ship assembly passed; connected_blocks=4; deterministic=true; "
        << "mass_inertia=true; bounded=true\n";
    return EXIT_SUCCESS;
}
