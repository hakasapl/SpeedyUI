#pragma once

#include "Interpolate.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace SpeedyUI {

/**
 * @brief An object a timeline moves or recolors between two consecutive frames
 */
struct Motion {
    std::int32_t depth = 0;
    bool moves = false;
    bool recolors = false;
    Transform2D fromMatrix;
    Transform2D toMatrix;
    ColorTransform fromColor = IDENTITY_COLOR_TRANSFORM;
    ColorTransform toColor = IDENTITY_COLOR_TRANSFORM;
};

/**
 * @brief The tweens of one timeline, as the frames that ease each object from one frame to the next
 */
struct TimelinePlan {
    /**
     * motions[i] leads from frame i - 1 into frame i. motions[0] leads from the last frame back
     * into the first, for a timeline that plays past its end and starts over.
     */
    std::vector<std::vector<Motion>> motions;
    std::size_t motionCount = 0;
};

/**
 * @brief Plays a timeline from its first frame to find what each frame moves or recolors
 *
 * Objects that a frame adds, replaces or removes appear or vanish at once, so only objects already
 * on the stage that a frame moves or recolors become motions. A motion is only kept as part of a
 * tween: the same object must also move into the frame before or after it. A change on its own is
 * the animation snapping, such as a panel jumping into place, which stays a snap. Thread-safe.
 *
 * @param frames The timeline's frames, each a list of execute tags
 * @param frameCount Number of frames
 * @param plan Receives the motions
 * @return False if a tag could not be understood, in which case the timeline has no plan
 */
auto planTimeline(const RE::GFxTimelineDef::ExecuteTagList* frames,
                  std::uint32_t frameCount,
                  TimelinePlan& plan) -> bool;

/**
 * @brief Converts Scaleform's matrix to the transform the plans use
 */
inline auto toTransform(const RE::GMatrix2D& matrix) -> Transform2D
{
    return {.a = matrix.data[0][0],
            .b = matrix.data[1][0],
            .c = matrix.data[0][1],
            .d = matrix.data[1][1],
            .tx = matrix.data[0][2],
            .ty = matrix.data[1][2]};
}

/**
 * @brief Writes a transform back in Scaleform's matrix layout
 */
inline void storeTransform(const Transform2D& transform,
                           RE::GMatrix2D& matrix)
{
    matrix.data[0][0] = transform.a;
    matrix.data[0][1] = transform.c;
    matrix.data[0][2] = transform.tx;
    matrix.data[1][0] = transform.b;
    matrix.data[1][1] = transform.d;
    matrix.data[1][2] = transform.ty;
}

/**
 * @brief Converts Scaleform's color transform to the one the plans use, which has the same layout
 */
inline auto toColor(const RE::GRenderer::Cxform& cxform) -> ColorTransform
{
    static_assert(sizeof(cxform.matrix) == sizeof(ColorTransform));
    ColorTransform color {};
    std::memcpy(color.data(), static_cast<const void*>(cxform.matrix), sizeof(color));
    return color;
}

/**
 * @brief Writes a color transform back in Scaleform's layout
 */
inline void storeColor(const ColorTransform& color,
                       RE::GRenderer::Cxform& cxform)
{
    std::memcpy(static_cast<void*>(cxform.matrix), color.data(), sizeof(color));
}

} // namespace SpeedyUI
