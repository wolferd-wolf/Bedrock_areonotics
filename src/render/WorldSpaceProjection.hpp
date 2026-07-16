#pragma once

#include <array>
#include <cmath>
#include <cstddef>

namespace aeronautics::render {

struct Vec3f final {
    float x{};
    float y{};
    float z{};
};

struct Mat4f final {
    std::array<float, 16> values{};
};

[[nodiscard]] inline Vec3f operator+(Vec3f lhs, Vec3f rhs) noexcept {
    return {lhs.x + rhs.x, lhs.y + rhs.y, lhs.z + rhs.z};
}

[[nodiscard]] inline Vec3f operator-(Vec3f lhs, Vec3f rhs) noexcept {
    return {lhs.x - rhs.x, lhs.y - rhs.y, lhs.z - rhs.z};
}

[[nodiscard]] inline Vec3f operator*(Vec3f value, float scalar) noexcept {
    return {value.x * scalar, value.y * scalar, value.z * scalar};
}

[[nodiscard]] inline float dot(Vec3f lhs, Vec3f rhs) noexcept {
    return lhs.x * rhs.x + lhs.y * rhs.y + lhs.z * rhs.z;
}

[[nodiscard]] inline Vec3f cross(Vec3f lhs, Vec3f rhs) noexcept {
    return {
        lhs.y * rhs.z - lhs.z * rhs.y,
        lhs.z * rhs.x - lhs.x * rhs.z,
        lhs.x * rhs.y - lhs.y * rhs.x,
    };
}

[[nodiscard]] inline float lengthSquared(Vec3f value) noexcept {
    return dot(value, value);
}

[[nodiscard]] inline Vec3f normalize(Vec3f value) noexcept {
    const float squared = lengthSquared(value);
    if (!std::isfinite(squared) || squared <= 1.0e-12F) {
        return {};
    }
    const float inverseLength = 1.0F / std::sqrt(squared);
    return value * inverseLength;
}

[[nodiscard]] inline bool finiteVector(Vec3f value) noexcept {
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}

[[nodiscard]] inline bool validCameraSample(Vec3f position, Vec3f target) noexcept {
    if (!finiteVector(position) || !finiteVector(target)) {
        return false;
    }
    constexpr float maximumCoordinateMagnitude = 30'000'000.0F;
    const auto coordinateInRange = [](float value) noexcept {
        return std::abs(value) <= maximumCoordinateMagnitude;
    };
    if (!coordinateInRange(position.x) || !coordinateInRange(position.y) ||
        !coordinateInRange(position.z) || !coordinateInRange(target.x) ||
        !coordinateInRange(target.y) || !coordinateInRange(target.z)) {
        return false;
    }
    const float directionSquared = lengthSquared(target - position);
    return std::isfinite(directionSquared) && directionSquared >= 1.0e-6F &&
           directionSquared <= 16'777'216.0F;
}

[[nodiscard]] inline Mat4f identityMatrix() noexcept {
    Mat4f result{};
    result.values[0] = 1.0F;
    result.values[5] = 1.0F;
    result.values[10] = 1.0F;
    result.values[15] = 1.0F;
    return result;
}

[[nodiscard]] inline Mat4f multiply(Mat4f const& lhs, Mat4f const& rhs) noexcept {
    Mat4f result{};
    for (std::size_t column = 0; column < 4; ++column) {
        for (std::size_t row = 0; row < 4; ++row) {
            float value = 0.0F;
            for (std::size_t inner = 0; inner < 4; ++inner) {
                value += lhs.values[inner * 4 + row] * rhs.values[column * 4 + inner];
            }
            result.values[column * 4 + row] = value;
        }
    }
    return result;
}

[[nodiscard]] inline Mat4f translationMatrix(Vec3f translation) noexcept {
    Mat4f result = identityMatrix();
    result.values[12] = translation.x;
    result.values[13] = translation.y;
    result.values[14] = translation.z;
    return result;
}

[[nodiscard]] inline Mat4f rotationXMatrix(float radians) noexcept {
    Mat4f result = identityMatrix();
    const float cosine = std::cos(radians);
    const float sine = std::sin(radians);
    result.values[5] = cosine;
    result.values[6] = sine;
    result.values[9] = -sine;
    result.values[10] = cosine;
    return result;
}

[[nodiscard]] inline Mat4f rotationYMatrix(float radians) noexcept {
    Mat4f result = identityMatrix();
    const float cosine = std::cos(radians);
    const float sine = std::sin(radians);
    result.values[0] = cosine;
    result.values[2] = -sine;
    result.values[8] = sine;
    result.values[10] = cosine;
    return result;
}

[[nodiscard]] inline Mat4f lookAtMatrix(Vec3f eye, Vec3f target) noexcept {
    Vec3f forward = normalize(target - eye);
    Vec3f worldUp{0.0F, 1.0F, 0.0F};
    if (std::abs(dot(forward, worldUp)) > 0.985F) {
        worldUp = {0.0F, 0.0F, 1.0F};
    }
    const Vec3f side = normalize(cross(forward, worldUp));
    const Vec3f up = cross(side, forward);

    Mat4f result = identityMatrix();
    result.values[0] = side.x;
    result.values[4] = side.y;
    result.values[8] = side.z;
    result.values[12] = -dot(side, eye);

    result.values[1] = up.x;
    result.values[5] = up.y;
    result.values[9] = up.z;
    result.values[13] = -dot(up, eye);

    result.values[2] = -forward.x;
    result.values[6] = -forward.y;
    result.values[10] = -forward.z;
    result.values[14] = dot(forward, eye);
    return result;
}

[[nodiscard]] inline Mat4f perspectiveMatrix(
    float fieldOfViewYRadians,
    float aspect,
    float nearPlane,
    float farPlane) noexcept {
    Mat4f result{};
    const float focalLength = 1.0F / std::tan(fieldOfViewYRadians * 0.5F);
    result.values[0] = focalLength / aspect;
    result.values[5] = focalLength;
    result.values[10] = (farPlane + nearPlane) / (nearPlane - farPlane);
    result.values[11] = -1.0F;
    result.values[14] = (2.0F * farPlane * nearPlane) / (nearPlane - farPlane);
    return result;
}

[[nodiscard]] inline bool buildWorldSpaceMvp(
    Vec3f cameraPosition,
    Vec3f cameraTarget,
    Vec3f worldAnchor,
    float aspect,
    float rotationX,
    float rotationY,
    Mat4f& output) noexcept {
    if (!validCameraSample(cameraPosition, cameraTarget) || !finiteVector(worldAnchor) ||
        !std::isfinite(aspect) || aspect <= 0.05F || aspect >= 20.0F) {
        return false;
    }

    constexpr float pi = 3.14159265358979323846F;
    constexpr float fieldOfViewY = 70.0F * pi / 180.0F;
    const Mat4f projection = perspectiveMatrix(fieldOfViewY, aspect, 0.05F, 2048.0F);
    const Mat4f view = lookAtMatrix(cameraPosition, cameraTarget);
    const Mat4f model = multiply(
        translationMatrix(worldAnchor),
        multiply(rotationYMatrix(rotationY), rotationXMatrix(rotationX)));
    output = multiply(projection, multiply(view, model));
    return true;
}

[[nodiscard]] inline std::array<float, 4> transformPoint(
    Mat4f const& matrix,
    std::array<float, 4> point) noexcept {
    std::array<float, 4> result{};
    for (std::size_t row = 0; row < 4; ++row) {
        result[row] = matrix.values[row] * point[0] +
                      matrix.values[4 + row] * point[1] +
                      matrix.values[8 + row] * point[2] +
                      matrix.values[12 + row] * point[3];
    }
    return result;
}

}  // namespace aeronautics::render
