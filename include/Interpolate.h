#pragma once

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numbers>

namespace SpeedyUI {

/**
 * @brief 2D affine transform in Scaleform's layout: x' = a*x + c*y + tx, y' = b*x + d*y + ty
 */
struct Transform2D {
    float a = 1.0F;
    float b = 0.0F;
    float c = 0.0F;
    float d = 1.0F;
    float tx = 0.0F;
    float ty = 0.0F;

    friend auto operator==(const Transform2D&, const Transform2D&) -> bool = default;
};

/**
 * @brief Color transform as Scaleform stores it: multiply and add terms for R, G, B and A
 */
using ColorTransform = std::array<float, 8>;

constexpr ColorTransform IDENTITY_COLOR_TRANSFORM = {1.0F, 0.0F, 1.0F, 0.0F, 1.0F, 0.0F, 1.0F, 0.0F};

/**
 * @brief Linear interpolation
 */
inline auto lerp(float from,
                 float to,
                 float t) -> float
{
    return from + ((to - from) * t);
}

namespace detail {

    /**
     * @brief Transform split into scale, shear and rotation so rotating objects keep their size
     *
     * The linear part is rotation(rotation) * [[scaleX, shear], [0, scaleY]].
     */
    struct DecomposedTransform {
        float scaleX = 1.0F;
        float scaleY = 1.0F;
        float shear = 0.0F;
        float rotation = 0.0F;
    };

    inline auto decompose(const Transform2D& transform,
                          DecomposedTransform& out) -> bool
    {
        constexpr float MIN_SCALE = 1e-6F;

        const float scaleX = std::hypot(transform.a, transform.b);
        if (!(scaleX > MIN_SCALE)) {
            return false;
        }

        const float cosR = transform.a / scaleX;
        const float sinR = transform.b / scaleX;
        out.scaleX = scaleX;
        out.rotation = std::atan2(transform.b, transform.a);
        out.shear = (cosR * transform.c) + (sinR * transform.d);
        out.scaleY = (cosR * transform.d) - (sinR * transform.c);
        return true;
    }

} // namespace detail

/**
 * @brief Whether a transform mirrors what it draws
 */
inline auto mirrors(const Transform2D& transform) -> bool
{
    return (transform.a * transform.d) - (transform.b * transform.c) < 0.0F;
}

/**
 * @brief Interpolates between two transforms
 *
 * Translation is interpolated linearly. The rest is interpolated as scale, shear and rotation,
 * taking the shorter way around, so a turning object does not shrink halfway through the turn.
 * Falls back to interpolating each component when a transform has collapsed to zero width, or
 * when one transform mirrors and the other doesn't: a flip has no rotation that gets there, so
 * the object narrows through zero and widens mirrored, as a Flash tween of a flip does.
 *
 * @param from Transform at t = 0
 * @param to Transform at t = 1
 * @param t Position between the two, from 0 to 1
 * @return Interpolated transform
 */
inline auto interpolate(const Transform2D& from,
                        const Transform2D& to,
                        float t) -> Transform2D
{
    Transform2D result;
    result.tx = lerp(from.tx, to.tx, t);
    result.ty = lerp(from.ty, to.ty, t);

    if (from.a == to.a && from.b == to.b && from.c == to.c && from.d == to.d) {
        // Pure movement: keep the linear part bit-for-bit so the object doesn't shimmer
        result.a = from.a;
        result.b = from.b;
        result.c = from.c;
        result.d = from.d;
        return result;
    }

    detail::DecomposedTransform start;
    detail::DecomposedTransform end;
    if (mirrors(from) != mirrors(to) || !detail::decompose(from, start) || !detail::decompose(to, end)) {
        result.a = lerp(from.a, to.a, t);
        result.b = lerp(from.b, to.b, t);
        result.c = lerp(from.c, to.c, t);
        result.d = lerp(from.d, to.d, t);
        return result;
    }

    constexpr auto TWO_PI = static_cast<float>(2.0 * std::numbers::pi);
    const float turn = std::remainder(end.rotation - start.rotation, TWO_PI);
    const float rotation = start.rotation + (turn * t);
    const float scaleX = lerp(start.scaleX, end.scaleX, t);
    const float scaleY = lerp(start.scaleY, end.scaleY, t);
    const float shear = lerp(start.shear, end.shear, t);

    const float cosR = std::cos(rotation);
    const float sinR = std::sin(rotation);
    result.a = cosR * scaleX;
    result.b = sinR * scaleX;
    result.c = (cosR * shear) - (sinR * scaleY);
    result.d = (sinR * shear) + (cosR * scaleY);
    return result;
}

/**
 * @brief Interpolates each term of a color transform
 *
 * @param from Color transform at t = 0
 * @param to Color transform at t = 1
 * @param t Position between the two, from 0 to 1
 * @return Interpolated color transform
 */
inline auto interpolate(const ColorTransform& from,
                        const ColorTransform& to,
                        float t) -> ColorTransform
{
    ColorTransform result {};
    for (std::size_t i = 0; i < result.size(); ++i) {
        result.at(i) = lerp(from.at(i), to.at(i), t);
    }
    return result;
}

} // namespace SpeedyUI
