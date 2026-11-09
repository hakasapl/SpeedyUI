#pragma once

#include <cstdint>
#include <string_view>

namespace RE {
class GFxMovieDefImpl;
}

namespace SpeedyUI {

/**
 * @brief What a movie's timelines offer for smoothing
 */
struct MovieSummary {
    std::uint32_t timelines = 0; /**< Timelines with more than one frame */
    std::uint32_t tweened = 0; /**< Timelines with at least one tween */
};

/**
 * @brief Hooks GFxMovieRoot::Display so tweens are drawn partway between frames
 *
 * Movies keep their own frame rate and their timelines are never changed, so ActionScript sees
 * exactly the frames it was written for. Each time a menu is drawn, objects that a playing
 * timeline will move or recolor on its next frame are drawn the matching fraction of the way
 * there, and put back as soon as the menu has been drawn. Only menus drawn on the main thread,
 * where menus are updated, are smoothed: a loading screen draws on its own thread, possibly while
 * the main thread updates the same menu, so it is left exactly as the game draws it.
 *
 * @return True if the hook is installed
 */
auto initSmoothing() -> bool;

/**
 * @brief Prepares a movie that is about to become a menu's main movie
 *
 * Records whether its tweens may be smoothed and works out the tweens of the timelines it
 * defines, so the work is not done while the menu is being drawn.
 *
 * @param movieDef Movie about to become level 0 of a menu
 * @param name Movie file name, for the log
 * @param included False if the allowlist or blocklist excluded the movie
 * @return The movie's timelines and how many have tweens
 */
auto prepareMovie(const RE::GFxMovieDefImpl& movieDef,
                  std::string_view name,
                  bool included) -> MovieSummary;

} // namespace SpeedyUI
