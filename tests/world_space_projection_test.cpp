#include "render/WorldSpaceProjection.hpp"

#include <cassert>
#include <cmath>

int main() {
    using namespace aeronautics::render;

    const Vec3f camera{0.0F, 0.0F, 0.0F};
    const Vec3f target{0.0F, 0.0F, -1.0F};
    const Vec3f anchor{0.0F, 0.0F, -5.0F};

    assert(validCameraSample(camera, target));
    assert(!validCameraSample(camera, camera));

    Mat4f mvp{};
    assert(buildWorldSpaceMvp(camera, target, anchor, 16.0F / 9.0F, 0.0F, 0.0F, mvp));
    const auto centered = transformPoint(mvp, {0.0F, 0.0F, 0.0F, 1.0F});
    assert(centered[3] > 0.0F);
    assert(std::abs(centered[0] / centered[3]) < 0.001F);
    assert(std::abs(centered[1] / centered[3]) < 0.001F);
    assert(centered[2] / centered[3] > -1.0F);
    assert(centered[2] / centered[3] < 1.0F);

    Mat4f movedCameraMvp{};
    assert(buildWorldSpaceMvp(
        {1.0F, 0.0F, 0.0F},
        {1.0F, 0.0F, -1.0F},
        anchor,
        16.0F / 9.0F,
        0.0F,
        0.0F,
        movedCameraMvp));
    const auto moved = transformPoint(movedCameraMvp, {0.0F, 0.0F, 0.0F, 1.0F});
    assert(moved[3] > 0.0F);
    assert(moved[0] / moved[3] < 0.0F);

    Mat4f invalid{};
    assert(!buildWorldSpaceMvp(camera, camera, anchor, 1.0F, 0.0F, 0.0F, invalid));
    assert(!buildWorldSpaceMvp(camera, target, anchor, 0.0F, 0.0F, 0.0F, invalid));
    return 0;
}
