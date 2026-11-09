#include "PCH.h"

#include "Smoothing.h"

#include "Interpolate.h"
#include "Timeline.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace SpeedyUI {
namespace {

    using ExecuteTagList = RE::GFxTimelineDef::ExecuteTagList;
    using ResourceType = RE::GFxResource::ResourceType;

    // Scaleform GFx 3 object layout, which is the same on SE 1.5.97 and on AE 1.6.1170, 1.7.99 and
    // 1.7.104. Read from GFxMovieRoot::Advance and SetLevelMovie, GFxSprite::AdvanceFrame and
    // GFxDisplayList::MoveDisplayObject on each. smooth() checks the back pointers between these
    // objects before it changes anything, and turns smoothing off if they don't line up.
    namespace rootfield {
        constexpr std::size_t LEVEL0_SPRITE = 0x58; /**< GFxSprite* */
        constexpr std::size_t LEVEL0_DEF = 0x60; /**< GFxMovieDefImpl* */
        constexpr std::size_t TIME_REMAINDER = 0x188; /**< float: time since the last frame */
        constexpr std::size_t FRAME_TIME = 0x18C; /**< float: 1 / frame rate */
        constexpr std::size_t PLAY_LIST = 0x2B60; /**< First character Advance steps through */
        constexpr std::size_t ADVANCE_SLOT = 0x25;
        constexpr std::size_t DISPLAY_SLOT = 0x26;
    } // namespace rootfield

    namespace characterfield {
        constexpr std::size_t DEPTH = 0x24; /**< std::int32_t, below -1 once removed */
        constexpr std::size_t PARENT = 0x30; /**< GFxSprite* */
        constexpr std::size_t CXFORM = 0x38; /**< GRenderer::Cxform */
        constexpr std::size_t MATRIX = 0x58; /**< GMatrix2D */
        constexpr std::size_t CXFORM_CHANGED_SLOT = 1; /**< Recomputes the fully-transparent flag */
        constexpr std::size_t CONTINUE_ANIMATION_SLOT = 6;
        constexpr std::size_t ACCEPTS_ANIM_MOVES_SLOT = 7;
    } // namespace characterfield

    namespace ascharacterfield {
        constexpr std::size_t STATE_FLAGS = 0x96; /**< std::uint16_t */
        constexpr std::size_t PLAY_NEXT = 0xB0; /**< Next character in the root's play list */
        constexpr std::size_t FLAGS = 0xE8; /**< std::uint32_t */
        constexpr std::uint16_t SKIPS_ADVANCE = 0x0140; /**< STATE_FLAGS bits Advance and AdvanceFrame skip */
        constexpr std::uint32_t NOT_ADVANCING = 0xC; /**< FLAGS bits AdvanceFrame skips */
    } // namespace ascharacterfield

    namespace spritefield {
        constexpr std::size_t DEF = 0x110; /**< GFxTimelineDef*: a GFxSpriteDef, or the GFxMovieDataDef at level 0 */
        constexpr std::size_t ROOT = 0x118; /**< GFxMovieRoot* */
        constexpr std::size_t CHILDREN = 0x128; /**< GFxCharacter** sorted by depth, then the count */
        constexpr std::size_t PLAY_STATE = 0x148; /**< 0 while playing */
        constexpr std::size_t CURRENT_FRAME = 0x14C;
        constexpr std::size_t LOADING = 0x310; /**< While set and not done, playing stops at its loaded frame count */
        constexpr std::size_t LOADING_DONE = 0x28; /**< std::uint8_t in LOADING */
        constexpr std::size_t LOADING_FRAMES = 0x20; /**< std::uint32_t in LOADING */
    } // namespace spritefield

    constexpr std::uint32_t LOAD_FINISHED = 2; /**< LoadTaskData::loadStatus once the whole file is loaded */
    constexpr std::size_t MAX_PLAY_LIST = 1U << 20U;
    constexpr std::size_t MAX_CHILDREN = 1U << 20U;
    constexpr std::size_t MAX_PLANS = 1U << 14U;
    constexpr float FRAME_TIME_TOLERANCE = 1e-4F;

    template <class T>
    auto field(void* object,
               std::size_t offset) -> T&
    {
        return *reinterpret_cast<T*>(static_cast<std::byte*>(object) + offset);
    }

    auto vtableOf(void* object) -> void*
    {
        return *static_cast<void**>(object);
    }

    template <class R>
    auto callVirtual(void* object,
                     std::size_t slot) -> R
    {
        using Function = R (*)(void*);
        return (*static_cast<Function**>(object))[slot](object);
    }

    /**
     * @brief How a movie may be smoothed, recorded when it becomes a menu's main movie
     */
    struct MovieRecord {
        std::string name;
        bool included = false;
        float frameTime = 0.0F; /**< What SetLevelMovie stores, to confirm the root's layout */
    };

    /**
     * @brief The tweens of one timeline definition
     */
    struct PlanRecord {
        const ExecuteTagList* frames = nullptr; /**< Tells a reused definition address apart */
        std::uint32_t frameCount = 0;
        bool usable = false;
        TimelinePlan plan;
    };

    /**
     * @brief An object drawn partway between frames, and what to put back afterwards
     */
    struct Override {
        void* root = nullptr;
        void* character = nullptr;
        RE::GMatrix2D matrix; /**< As the timeline left it */
        RE::GRenderer::Cxform color;
        RE::GMatrix2D easedMatrix; /**< As this plugin drew it */
        RE::GRenderer::Cxform easedColor;
        bool moved = false;
        bool recolored = false;
    };

    using DisplayFunction = void (*)(void*);

    constexpr std::uint32_t MAX_CONFLICTS_LOGGED = 20;

    // NOLINTBEGIN(cppcoreguidelines-avoid-non-const-global-variables)
    std::mutex s_mutex;
    std::unordered_map<const void*, MovieRecord> s_movies; /**< By GFxMovieDefImpl, under s_mutex */
    std::unordered_map<const void*, PlanRecord> s_plans; /**< By GFxTimelineDef, under s_mutex */
    std::unordered_set<const void*> s_frameTimeWarned; /**< Under s_mutex */
    DisplayFunction s_display = nullptr;
    std::atomic<bool> s_active = false;
    std::uint32_t s_mainThread = 0; /**< The thread menus are updated on, set before s_active */
    std::atomic<bool> s_offThreadLogged = false;
    std::atomic<std::uint32_t> s_conflicts = 0; /**< Eased objects something else changed during Display */
    std::atomic<std::uint32_t> s_busySkips = 0; /**< Frames drawn unsmoothed because another thread held the lock */
    // NOLINTEND(cppcoreguidelines-avoid-non-const-global-variables)

    /**
     * @brief Turns smoothing off for good when the game's objects don't look the way they should
     */
    void disable(std::string_view reason)
    {
        if (s_active.exchange(false)) {
            spdlog::error("Tween smoothing turned off: {}", reason);
        }
    }

    struct Frames {
        const ExecuteTagList* data = nullptr;
        std::uint32_t count = 0;
    };

    /**
     * @brief The frames of a fully loaded timeline definition with more than one frame
     */
    auto framesOf(const RE::GFxTimelineDef* def) -> Frames
    {
        switch (def->GetResourceType()) {
        case ResourceType::kSpriteDef: {
            const auto* sprite = static_cast<const RE::GFxSpriteDef*>(def);
            if (sprite->frameCount >= 2 && sprite->frameLoading == sprite->frameCount
                && sprite->frames.GetSize() == static_cast<std::size_t>(sprite->frameCount)) {
                return {.data = sprite->frames.data.data, .count = static_cast<std::uint32_t>(sprite->frameCount)};
            }
            break;
        }
        case ResourceType::kMovieDataDef: {
            const auto* load = static_cast<const RE::GFxMovieDataDef*>(def)->loadTaskData.get();
            if (load != nullptr && load->loadStatus == LOAD_FINISHED && load->movieInfo.frameCount >= 2
                && load->movieFrames.GetSize() == load->movieInfo.frameCount
                && load->frameLoading == static_cast<std::int32_t>(load->movieInfo.frameCount)) {
                return {.data = load->movieFrames.data.data, .count = load->movieInfo.frameCount};
            }
            break;
        }
        default:
            break;
        }
        return {};
    }

    /**
     * @brief The tweens of a timeline definition, worked out the first time it is asked for
     *
     * @return The plan, or nullptr if the timeline has no tweens or cannot be read
     */
    auto planFor(const RE::GFxTimelineDef* def) -> const TimelinePlan*
    {
        const auto frames = framesOf(def);
        if (frames.data == nullptr) {
            return nullptr;
        }

        if (s_plans.size() >= MAX_PLANS && !s_plans.contains(def)) {
            s_plans.clear(); // Definitions of closed menus pile up otherwise
        }
        auto& record = s_plans[def];
        if (record.frames != frames.data || record.frameCount != frames.count) {
            record.frames = frames.data;
            record.frameCount = frames.count;
            record.usable = planTimeline(frames.data, frames.count, record.plan) && record.plan.motionCount != 0;
            if (!record.usable) {
                record.plan = {};
            }
        }
        return record.usable ? &record.plan : nullptr;
    }

    /**
     * @brief The child of a sprite at a depth, from its display list sorted by depth
     */
    auto findChild(void* sprite,
                   std::int32_t depth) -> void*
    {
        auto** children = field<void**>(sprite, spritefield::CHILDREN);
        const auto count = field<std::size_t>(sprite, spritefield::CHILDREN + sizeof(void*));
        if (children == nullptr || count > MAX_CHILDREN) {
            return nullptr;
        }
        auto** const end = children + count;
        auto** const found = std::lower_bound(children, end, depth, [](void* child, std::int32_t wanted) {
            return field<std::int32_t>(child, characterfield::DEPTH) < wanted;
        });
        return found != end && field<std::int32_t>(*found, characterfield::DEPTH) == depth ? *found : nullptr;
    }

    /**
     * @brief Draws one object the given fraction of the way into the timeline's next frame
     */
    void ease(void* root,
              void* child,
              const Motion& motion,
              float t,
              std::vector<Override>& overrides)
    {
        // As GFxDisplayList::MoveDisplayObject does, leave an object that ActionScript has taken
        // over, unless it was told to keep following the timeline
        if (!callVirtual<bool>(child, characterfield::ACCEPTS_ANIM_MOVES_SLOT)
            && !callVirtual<bool>(child, characterfield::CONTINUE_ANIMATION_SLOT)) {
            return;
        }

        auto& matrix = field<RE::GMatrix2D>(child, characterfield::MATRIX);
        auto& color = field<RE::GRenderer::Cxform>(child, characterfield::CXFORM);

        // Only ease from exactly where the timeline left the object. Anything else means it got
        // there another way, such as a jump or a script, and the plan does not apply to it.
        if ((motion.moves && toTransform(matrix) != motion.fromMatrix)
            || (motion.recolors && toColor(color) != motion.fromColor)) {
            return;
        }

        auto& saved = overrides.emplace_back(Override {.root = root,
                                                       .character = child,
                                                       .matrix = matrix,
                                                       .color = color,
                                                       .moved = motion.moves,
                                                       .recolored = motion.recolors});
        if (motion.moves) {
            storeTransform(interpolate(motion.fromMatrix, motion.toMatrix, t), matrix);
        }
        if (motion.recolors) {
            storeColor(interpolate(motion.fromColor, motion.toColor, t), color);
            callVirtual<void>(child, characterfield::CXFORM_CHANGED_SLOT);
        }
        saved.easedMatrix = matrix;
        saved.easedColor = color;
    }

    /**
     * @brief Eases the objects a sprite's next frame moves, if the sprite will play into it
     *
     * @return False if the sprite's layout is not what this plugin expects
     */
    auto smoothSprite(void* root,
                      void* sprite,
                      float t,
                      std::vector<Override>& overrides) -> bool
    {
        if (field<void*>(sprite, spritefield::ROOT) != root) {
            disable("a sprite in a menu's play list belongs to another menu");
            return false;
        }

        // The same tests GFxSprite::AdvanceFrame makes before it steps to the next frame
        if ((field<std::uint16_t>(sprite, ascharacterfield::STATE_FLAGS) & ascharacterfield::SKIPS_ADVANCE) != 0
            || (field<std::uint32_t>(sprite, ascharacterfield::FLAGS) & ascharacterfield::NOT_ADVANCING) != 0
            || field<std::int32_t>(sprite, characterfield::DEPTH) < -1 || field<std::uint32_t>(sprite, spritefield::PLAY_STATE) != 0) {
            return true;
        }

        const auto* def = field<const RE::GFxTimelineDef*>(sprite, spritefield::DEF);
        const auto* plan = def != nullptr ? planFor(def) : nullptr;
        if (plan == nullptr) {
            return true;
        }

        // A sprite still loading stops at its last loaded frame. Otherwise it plays into the next
        // frame, or past the last one back into the first, whose motions are in motions[0].
        const auto current = static_cast<std::size_t>(field<std::uint32_t>(sprite, spritefield::CURRENT_FRAME));
        const auto frameCount = plan->motions.size();
        if (current >= frameCount) {
            return true;
        }
        if (auto* loading = field<void*>(sprite, spritefield::LOADING);
            loading != nullptr && field<std::uint8_t>(loading, spritefield::LOADING_DONE) == 0
            && current + 1 >= field<std::uint32_t>(loading, spritefield::LOADING_FRAMES)) {
            return true;
        }
        const auto next = (current + 1) % frameCount;

        for (const auto& motion : plan->motions[next]) {
            auto* child = findChild(sprite, motion.depth);
            if (child == nullptr) {
                continue;
            }
            if (field<void*>(child, characterfield::PARENT) != sprite) {
                disable("a sprite's display list holds an object with another parent");
                return false;
            }
            ease(root, child, motion, t, overrides);
        }
        return true;
    }

    /**
     * @brief Eases every tween in a menu that is between two frames
     */
    void smooth(void* root,
                std::vector<Override>& overrides)
    {
        // Never make a draw wait: if another thread is preparing a menu, draw this frame as it is
        const std::unique_lock lock(s_mutex, std::try_to_lock);
        if (!lock.owns_lock()) {
            if (s_busySkips.fetch_add(1, std::memory_order_relaxed) == 0) {
                spdlog::info("A menu was drawn unsmoothed on thread {} while another thread prepared a menu",
                             REX::W32::GetCurrentThreadId());
            }
            return;
        }

        const auto* movie = field<void*>(root, rootfield::LEVEL0_DEF);
        const auto record = s_movies.find(movie);
        if (record == s_movies.end() || !record->second.included) {
            return;
        }

        // SetLevelMovie stored the movie's frame time here, so this confirms where the timing is
        const float frameTime = field<float>(root, rootfield::FRAME_TIME);
        if (!(std::abs(frameTime - record->second.frameTime) <= record->second.frameTime * FRAME_TIME_TOLERANCE)) {
            if (s_frameTimeWarned.insert(movie).second) {
                spdlog::warn("A menu's frame time is {} instead of {}, so it is not smoothed", frameTime, record->second.frameTime);
            }
            return;
        }

        // Advance leaves the time since the last frame here, always less than one frame
        const float elapsed = field<float>(root, rootfield::TIME_REMAINDER);
        if (!(elapsed > 0.0F && elapsed < frameTime)) {
            return;
        }
        const float t = elapsed / frameTime;

        auto* level0 = field<void*>(root, rootfield::LEVEL0_SPRITE);
        if (level0 == nullptr) {
            return;
        }
        if (field<void*>(level0, spritefield::ROOT) != root) {
            disable("a menu's main movie does not point back at the menu");
            return;
        }

        // Every sprite is a GFxSprite, the same class as the main movie
        auto* const spriteVTable = vtableOf(level0);
        std::size_t visited = 0;
        for (auto* entry = field<void*>(root, rootfield::PLAY_LIST); entry != nullptr;
             entry = field<void*>(entry, ascharacterfield::PLAY_NEXT)) {
            if (++visited > MAX_PLAY_LIST) {
                disable("a menu's play list does not end");
                return;
            }
            if (vtableOf(entry) == spriteVTable && !smoothSprite(root, entry, t, overrides)) {
                return;
            }
        }
    }

    /**
     * @brief Logs an eased object that something else changed while its menu was being drawn
     */
    void reportConflict(void* root)
    {
        if (s_conflicts.fetch_add(1, std::memory_order_relaxed) >= MAX_CONFLICTS_LOGGED) {
            return;
        }
        std::string name = "a menu";
        if (const std::unique_lock lock(s_mutex, std::try_to_lock); lock.owns_lock()) {
            if (const auto record = s_movies.find(field<void*>(root, rootfield::LEVEL0_DEF)); record != s_movies.end()) {
                name = record->second.name;
            }
        }
        spdlog::warn("{}: an object changed while it was being drawn (thread {}, main thread {}), so it keeps the game's value",
                     name,
                     REX::W32::GetCurrentThreadId(),
                     s_mainThread);
    }

    /**
     * @brief Puts every eased object back the way the timeline left it
     *
     * An object is only put back if it still holds what this plugin drew. If the game changed it in
     * the meantime, which can only happen if another thread updated the menu during the draw,
     * writing the old value back would undo the game's change.
     */
    void restore(const std::vector<Override>& overrides)
    {
        for (auto it = overrides.rbegin(); it != overrides.rend(); ++it) {
            bool conflict = false;
            if (it->moved) {
                auto& matrix = field<RE::GMatrix2D>(it->character, characterfield::MATRIX);
                if (std::memcmp(&matrix, &it->easedMatrix, sizeof(matrix)) == 0) {
                    matrix = it->matrix;
                } else {
                    conflict = true;
                }
            }
            if (it->recolored) {
                auto& color = field<RE::GRenderer::Cxform>(it->character, characterfield::CXFORM);
                if (std::memcmp(&color, &it->easedColor, sizeof(color)) == 0) {
                    color = it->color;
                    callVirtual<void>(it->character, characterfield::CXFORM_CHANGED_SLOT);
                } else {
                    conflict = true;
                }
            }
            if (conflict) {
                reportConflict(it->root);
            }
        }
    }

    /**
     * @brief Whether this draw runs on the thread that updates menus
     *
     * Menus are advanced and their ActionScript runs on the main thread. While the game loads, a
     * loading screen thread draws menus too, and can do so while the main thread updates the same
     * menu. Easing an object there would race the game's own update of it, so those draws are
     * left exactly as the game makes them.
     */
    auto onMainThread() -> bool
    {
        const auto thread = REX::W32::GetCurrentThreadId();
        if (thread == s_mainThread) {
            return true;
        }
        if (!s_offThreadLogged.exchange(true)) {
            spdlog::info("Menus drawn on thread {}, such as loading screens, are not smoothed (main thread {})",
                         thread,
                         s_mainThread);
        }
        return false;
    }

    /**
     * @brief Replacement for GFxMovieRoot::Display
     *
     * Display draws the menu at once, so the eased positions exist only while it runs. Advance and
     * ActionScript never see them.
     */
    void displayHook(void* root)
    {
        std::vector<Override> overrides;
        if (s_active.load(std::memory_order_relaxed) && onMainThread()) {
            smooth(root, overrides);
        }
        s_display(root);
        restore(overrides);
    }

    /**
     * @brief Plans a timeline definition and counts it, for the log
     */
    void prepareTimeline(const RE::GFxTimelineDef* def,
                         MovieSummary& summary)
    {
        if (framesOf(def).data == nullptr) {
            return;
        }
        ++summary.timelines;
        if (planFor(def) != nullptr) {
            ++summary.tweened;
        }
    }

} // namespace

auto initSmoothing() -> bool
{
    const REL::Relocation<std::uintptr_t> advance {RELOCATION_ID(81705, 83766)};
    const REL::Relocation<std::uintptr_t> display {RELOCATION_ID(81728, 83789)};

    // Scaleform has no RTTI, so the vtable is found as the one that holds both functions in
    // their slots. Every menu's movie is a GFxMovieRoot, so this one vtable covers them all.
    const auto rdata = REL::Module::get().segment(REL::Segment::rdata);
    const auto begin = (rdata.address() + alignof(std::uintptr_t) - 1) & ~(alignof(std::uintptr_t) - 1);
    const auto end = rdata.address() + rdata.size();
    std::vector<std::uintptr_t> vtables;
    for (auto slot = begin + (rootfield::DISPLAY_SLOT * sizeof(std::uintptr_t)); slot + sizeof(std::uintptr_t) <= end;
         slot += sizeof(std::uintptr_t)) {
        if (*reinterpret_cast<const std::uintptr_t*>(slot) == display.address()
            && *reinterpret_cast<const std::uintptr_t*>(slot - sizeof(std::uintptr_t)) == advance.address()) {
            vtables.push_back(slot - (rootfield::DISPLAY_SLOT * sizeof(std::uintptr_t)));
        }
    }
    if (vtables.size() != 1) {
        spdlog::error("Found {} GFxMovieRoot vtables instead of 1, tweens cannot be smoothed", vtables.size());
        return false;
    }

    REL::Relocation<std::uintptr_t> vtable {vtables.front()};
    s_display = reinterpret_cast<DisplayFunction>(vtable.write_vfunc(rootfield::DISPLAY_SLOT, &displayHook));
    s_mainThread = REX::W32::GetCurrentThreadId();
    s_active = true;
    spdlog::info("Tween smoothing ready (GFxMovieRoot vtable at +{:X}, main thread {})",
                 vtables.front() - REL::Module::get().base(),
                 s_mainThread);
    return true;
}

auto prepareMovie(const RE::GFxMovieDefImpl& movieDef,
                  std::string_view name,
                  bool included) -> MovieSummary
{
    MovieSummary summary;
    const auto* bind = movieDef.bindTaskData.get();
    const auto* data = bind != nullptr ? bind->movieDataResource : nullptr;
    const auto* load = data != nullptr ? data->loadTaskData.get() : nullptr;
    if (load == nullptr || !(load->movieInfo.frameRate > 0.0F)) {
        return summary;
    }

    const std::scoped_lock lock(s_mutex);

    // SetLevelMovie stores 1.0 / frame rate, worked out in double and kept as a float
    auto& record = s_movies[&movieDef];
    if (record.name != name) {
        record = MovieRecord {.name = std::string(name)};
    }
    record.included = included;
    record.frameTime = static_cast<float>(1.0 / static_cast<double>(load->movieInfo.frameRate));
    s_frameTimeWarned.erase(&movieDef);
    if (!included) {
        return summary;
    }

    prepareTimeline(data, summary);
    for (const auto& entry : load->resources) {
        const auto& source = entry.second;
        if (source.type != RE::GFxResourceSource::kNormal || source.data.resource == nullptr
            || source.data.resource->GetResourceType() != ResourceType::kSpriteDef) {
            continue;
        }
        const auto* sprite = static_cast<const RE::GFxSpriteDef*>(source.data.resource);
        if (sprite->movieData == data) {
            prepareTimeline(sprite, summary);
        }
    }
    return summary;
}

} // namespace SpeedyUI
