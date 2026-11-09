#include "PCH.h"

#include "Timeline.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <memory>
#include <mutex>
#include <span>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace SpeedyUI {
namespace {

    using ExecuteTagList = RE::GFxTimelineDef::ExecuteTagList;
    using PlaceFlags = RE::GFxPlaceFlags;
    using PlaceFlagSet = REX::EnumSet<PlaceFlags, std::uint8_t>;
    using ChangeType = RE::GFxPlaceObjectData::ChangeType;
    using CodePattern = std::span<const std::uint8_t>;

    constexpr std::size_t PLACE_DATA_BUFFER_SIZE = 0x100; /**< Room for GFxPlaceObjectData, plus slack */
    constexpr std::int32_t MAX_DEPTH = 1 << 20;

    static_assert(sizeof(RE::GFxPlaceObjectData) <= PLACE_DATA_BUFFER_SIZE);

    // Scaleform is built without RTTI, so its tag classes are told apart by the code their
    // vtables point to. Slots, after GASExecuteTag and GFxPlaceObjectBase:
    constexpr std::size_t EXECUTE_WITH_PRIORITY_SLOT = 2;
    constexpr std::size_t IS_REMOVE_TAG_SLOT = 3;
    constexpr std::size_t IS_ACTION_TAG_SLOT = 4;
    constexpr std::size_t IS_INIT_IMPORT_TAG_SLOT = 5;
    constexpr std::size_t ADD_TO_SNAPSHOT_SLOT = 6;
    constexpr std::size_t GET_PLACE_OBJECT_DATA_SLOT = 8;
    constexpr std::size_t GET_PLACE_FLAGS_SLOT = 10;

    /** GASExecuteTag::ExecuteWithPriority, which only forwards: mov rax, [rcx]; jmp [rax+8] */
    constexpr std::array<std::uint8_t, 7> FORWARD_TO_EXECUTE = {0x48, 0x8B, 0x01, 0x48, 0xFF, 0x60, 0x08};
    constexpr std::array<std::uint8_t, 3> RETURN_FALSE = {0x32, 0xC0, 0xC3}; /**< xor al, al; ret */
    constexpr std::array<std::uint8_t, 3> RETURN_TRUE = {0xB0, 0x01, 0xC3}; /**< mov al, 1; ret */

    /** GFxRemoveObject(2) handing its depth on: mov r8, rdx; movzx edx, word [rcx+depth offset] */
    constexpr std::array<std::uint8_t, 6> REMOVE_PASSES_DEPTH = {0x4C, 0x8B, 0xC2, 0x0F, 0xB7, 0x51};

    // The start of GetPlaceFlags for each PlaceObject class. The same source compiles differently
    // on SE 1.5.97 than on AE (1.6 and 1.7, whose code is identical), so some have two forms.
    /** GFxPlaceObjectUnpacked: movzx eax, byte [rcx+67h]; mov [rdx], al */
    constexpr std::array<std::uint8_t, 6> UNPACKED_PLACE_FLAGS = {0x0F, 0xB6, 0x41, 0x67, 0x88, 0x02};
    /** GFxPlaceObject2(a): movzx eax, byte [rcx+8]; and al, 5Fh; mov [rdx], al */
    constexpr std::array<std::uint8_t, 8> PLACE_OBJECT2_PLACE_FLAGS = {0x0F, 0xB6, 0x41, 0x08, 0x24, 0x5F, 0x88, 0x02};
    /** GFxPlaceObject on AE: cmp byte [rcx+8], 0; mov eax, 7 */
    constexpr std::array<std::uint8_t, 9> PLACE_OBJECT_PLACE_FLAGS = {0x80, 0x79, 0x08, 0x00, 0xB8, 0x07, 0x00, 0x00, 0x00};
    /** GFxPlaceObject on SE: movzx eax, byte [rcx+8]; neg al; mov rax, rdx; sbb cl, cl; and cl, 8; or cl, 7 */
    constexpr std::array<std::uint8_t, 17> PLACE_OBJECT_PLACE_FLAGS_SE
        = {0x0F, 0xB6, 0x41, 0x08, 0xF6, 0xD8, 0x48, 0x8B, 0xC2, 0x1A, 0xC9, 0x80, 0xE1, 0x08, 0x80, 0xC9, 0x07};
    /** GFxPlaceObject3 on AE: movsx r9, byte [rcx+8]; mov rax, r9; and r9b, 5Fh */
    constexpr std::array<std::uint8_t, 12> PLACE_OBJECT3_PLACE_FLAGS
        = {0x4C, 0x0F, 0xBE, 0x49, 0x08, 0x49, 0x8B, 0xC1, 0x41, 0x80, 0xE1, 0x5F};
    /** GFxPlaceObject3 on SE: movzx r9d, byte [rcx+8]; lea r8, [rcx+8]; movzx eax, r9b; mov ecx, 1; and al, 80h */
    constexpr std::array<std::uint8_t, 20> PLACE_OBJECT3_PLACE_FLAGS_SE = {0x44, 0x0F, 0xB6, 0x49, 0x08, 0x4C, 0x8D,
                                                                           0x41, 0x08, 0x41, 0x0F, 0xB6, 0xC1, 0xB9,
                                                                           0x01, 0x00, 0x00, 0x00, 0x24, 0x80};

    constexpr std::array<CodePattern, 6> PLACE_FLAGS_PATTERNS = {
        UNPACKED_PLACE_FLAGS,
        PLACE_OBJECT2_PLACE_FLAGS,
        PLACE_OBJECT_PLACE_FLAGS,
        PLACE_OBJECT_PLACE_FLAGS_SE,
        PLACE_OBJECT3_PLACE_FLAGS,
        PLACE_OBJECT3_PLACE_FLAGS_SE,
    };

    /**
     * @brief Address ranges of the game executable, for reading its vtables and code safely
     */
    class Image {
    public:
        static auto get() -> const Image&
        {
            static const Image image;
            return image;
        }

        [[nodiscard]] auto contains(std::uintptr_t address,
                                    std::size_t size) const -> bool
        {
            return within(address, size, m_base, m_end);
        }

        [[nodiscard]] auto isCode(std::uintptr_t address,
                                  std::size_t size) const -> bool
        {
            return within(address, size, m_codeBegin, m_codeEnd);
        }

    private:
        Image()
        {
            const auto& module = REL::Module::get();
            m_base = module.base();
            m_end = m_base;
            for (std::size_t i = 0; i < REL::Segment::total; ++i) {
                const auto segment = module.segment(static_cast<REL::Segment::Name>(i));
                if (segment.size() != 0) {
                    m_end = std::max(m_end, segment.address() + segment.size());
                }
            }

            const auto code = module.segment(REL::Segment::textx);
            m_codeBegin = code.address();
            m_codeEnd = code.address() + code.size();
        }

        static auto within(std::uintptr_t address,
                           std::size_t size,
                           std::uintptr_t begin,
                           std::uintptr_t end) -> bool
        {
            return address >= begin && size <= end - begin && address - begin <= (end - begin) - size;
        }

        std::uintptr_t m_base = 0;
        std::uintptr_t m_end = 0;
        std::uintptr_t m_codeBegin = 0;
        std::uintptr_t m_codeEnd = 0;
    };

    auto readPointer(std::uintptr_t address) -> std::uintptr_t
    {
        std::uintptr_t value = 0;
        std::memcpy(&value, reinterpret_cast<const void*>(address), sizeof(value));
        return value;
    }

    /**
     * @brief Function in a vtable slot, or 0 if the slot lies outside the game executable
     *
     * Slots past the end of a short vtable read whatever follows it, which is harmless as long as
     * the result is only compared against code and never called.
     */
    auto vtableSlot(std::uintptr_t vtable,
                    std::size_t slot) -> std::uintptr_t
    {
        const auto address = vtable + (slot * sizeof(std::uintptr_t));
        return Image::get().contains(address, sizeof(std::uintptr_t)) ? readPointer(address) : 0;
    }

    auto slotStartsWith(std::uintptr_t vtable,
                        std::size_t slot,
                        CodePattern code) -> bool
    {
        const auto function = vtableSlot(vtable, slot);
        return Image::get().isCode(function, code.size())
            && std::memcmp(reinterpret_cast<const void*>(function), code.data(), code.size()) == 0;
    }

    enum class TagKind : std::uint8_t {
        OTHER,
        PLACE,
        REMOVE,
    };

    struct TagClass {
        TagKind kind = TagKind::OTHER;
        std::uint8_t depthOffset = 0; /**< Offset of a remove tag's 16-bit depth */
    };

    auto classifyVTable(std::uintptr_t vtable) -> TagClass
    {
        TagClass tagClass;

        // Place and remove tags keep GASExecuteTag's forwarder and are neither action nor
        // init-import tags. DoAction, for one, replaces the forwarder.
        if (!slotStartsWith(vtable, EXECUTE_WITH_PRIORITY_SLOT, FORWARD_TO_EXECUTE)
            || !slotStartsWith(vtable, IS_ACTION_TAG_SLOT, RETURN_FALSE)
            || !slotStartsWith(vtable, IS_INIT_IMPORT_TAG_SLOT, RETURN_FALSE)) {
            return tagClass;
        }

        if (slotStartsWith(vtable, IS_REMOVE_TAG_SLOT, RETURN_TRUE)) {
            const auto function = vtableSlot(vtable, ADD_TO_SNAPSHOT_SLOT);
            if (slotStartsWith(vtable, ADD_TO_SNAPSHOT_SLOT, REMOVE_PASSES_DEPTH)
                && Image::get().isCode(function, REMOVE_PASSES_DEPTH.size() + 1)) {
                const auto offset = *reinterpret_cast<const std::uint8_t*>(function + REMOVE_PASSES_DEPTH.size());
                if (offset >= sizeof(std::uintptr_t) && offset + sizeof(std::uint16_t) <= sizeof(RE::GFxRemoveObject)) {
                    tagClass.kind = TagKind::REMOVE;
                    tagClass.depthOffset = offset;
                }
            }
            return tagClass;
        }

        if (slotStartsWith(vtable, IS_REMOVE_TAG_SLOT, RETURN_FALSE)
            && std::ranges::any_of(PLACE_FLAGS_PATTERNS, [vtable](CodePattern pattern) {
                   return slotStartsWith(vtable, GET_PLACE_FLAGS_SLOT, pattern);
               })) {
            tagClass.kind = TagKind::PLACE;
        }
        return tagClass;
    }

    // NOLINTBEGIN(cppcoreguidelines-avoid-non-const-global-variables)
    std::mutex s_mutex;
    std::unordered_map<std::uintptr_t, TagClass> s_tagClasses; /**< Tag class by vtable, under s_mutex */
    // NOLINTEND(cppcoreguidelines-avoid-non-const-global-variables)

    auto classifyTag(const RE::GASExecuteTag* tag) -> TagClass
    {
        const auto vtable = readPointer(reinterpret_cast<std::uintptr_t>(tag));
        if (const auto found = s_tagClasses.find(vtable); found != s_tagClasses.end()) {
            return found->second;
        }
        const auto tagClass = classifyVTable(vtable);
        s_tagClasses.emplace(vtable, tagClass);
        return tagClass;
    }

    /**
     * @brief Decodes a PlaceObject tag of any flavour with the game's own reader
     */
    class DecodedPlace {
    public:
        explicit DecodedPlace(const RE::GASExecuteTag* tag)
            : m_data(std::construct_at(reinterpret_cast<RE::GFxPlaceObjectData*>(m_buffer.data())))
        {
            using GetPlaceObjectData = void (*)(const RE::GASExecuteTag*, RE::GFxPlaceObjectData*);

            const auto vtable = readPointer(reinterpret_cast<std::uintptr_t>(tag));
            const auto function = readPointer(vtable + (GET_PLACE_OBJECT_DATA_SLOT * sizeof(std::uintptr_t)));
            reinterpret_cast<GetPlaceObjectData>(function)(tag, m_data);
        }

        ~DecodedPlace() { std::destroy_at(m_data); }

        DecodedPlace(const DecodedPlace&) = delete;
        DecodedPlace(DecodedPlace&&) = delete;
        auto operator=(const DecodedPlace&) -> DecodedPlace& = delete;
        auto operator=(DecodedPlace&&) -> DecodedPlace& = delete;

        [[nodiscard]] auto data() const -> const RE::GFxPlaceObjectData& { return *m_data; }

    private:
        alignas(16) std::array<std::byte, PLACE_DATA_BUFFER_SIZE> m_buffer {};
        RE::GFxPlaceObjectData* m_data;
    };

    auto isPlausible(const RE::GFxPlaceObjectData& data) -> bool
    {
        if (data.depth < -MAX_DEPTH || data.depth > MAX_DEPTH) {
            return false;
        }
        const auto transform = toTransform(data.matrix);
        const auto color = toColor(data.colorTransform);
        const std::array matrixTerms {transform.a, transform.b, transform.c, transform.d, transform.tx, transform.ty};
        return std::ranges::all_of(matrixTerms, [](float term) { return std::isfinite(term); })
            && std::ranges::all_of(color, [](float term) { return std::isfinite(term); });
    }

    /**
     * @brief State of the object at one depth, as the timeline leaves it
     */
    struct DepthState {
        Transform2D matrix;
        ColorTransform color = IDENTITY_COLOR_TRANSFORM;
        std::uint32_t character = 0; /**< Resource ID of what was placed */
        std::uint32_t createdFrame = 0; /**< Frame that added or replaced it */
    };

    using Stage = std::unordered_map<std::int32_t, DepthState>;

    auto characterOf(const RE::GFxPlaceObjectData& data) -> std::uint32_t
    {
        std::uint32_t id = 0;
        std::memcpy(&id, &data.characterId, sizeof(id));
        return id;
    }

    /**
     * @brief Tracks what the tags of one frame do to the objects on the timeline
     */
    class FrameSimulation {
    public:
        /**
         * @param frame The frame being run
         * @param interpolate Whether moves in this frame become motions
         * @param wrapping The first frame running again after the last, as playing past the end does
         */
        FrameSimulation(Stage& stage,
                        std::vector<Motion>& motions,
                        std::uint32_t frame,
                        bool interpolate,
                        bool wrapping)
            : m_stage(stage)
            , m_motions(motions)
            , m_frame(frame)
            , m_interpolate(interpolate)
            , m_wrapping(wrapping)
        {
            // Starting over drops every object a later frame made, unless this frame makes it again
            if (m_wrapping) {
                for (const auto& [depth, state] : m_stage) {
                    if (state.createdFrame != 0) {
                        m_dropped.insert(depth);
                    }
                }
            }
        }

        FrameSimulation(const FrameSimulation&) = delete;
        FrameSimulation(FrameSimulation&&) = delete;
        auto operator=(const FrameSimulation&) -> FrameSimulation& = delete;
        auto operator=(FrameSimulation&&) -> FrameSimulation& = delete;

        ~FrameSimulation()
        {
            for (const auto depth : m_dropped) {
                m_stage.erase(depth);
                rebuild(depth);
            }
            std::erase_if(m_motions, [](const Motion& motion) { return !motion.moves && !motion.recolors; });
        }

        /**
         * @brief Applies a decoded PlaceObject tag the way the game runs it
         *
         * What a tag does is told by its change type, not its Move flag: the game's decoders for
         * PlaceObject2 and PlaceObject3 put the flag only in the change type, and unpacked tags
         * always add. The game runs packed tags by change type and unpacked ones as adds.
         */
        auto place(const RE::GFxPlaceObjectData& data) -> void
        {
            const PlaceFlagSet flags = data.placeFlags;
            const bool hasMatrix = flags.any(PlaceFlags::kHasMatrix);
            const bool hasColor = flags.any(PlaceFlags::kHasColorTransform);
            const std::int32_t depth = data.depth;
            const auto change = data.changeType.get();
            const auto existing = m_stage.find(depth);

            if (change == ChangeType::kAddDisplayObject || change == ChangeType::kReplaceDisplayObject) {
                if (!flags.any(PlaceFlags::kHasCharacter)) {
                    return; // Adding nothing does nothing in the game either
                }
                const auto character = characterOf(data);

                // Starting over, GFxSprite::AddDisplayObject finds the object the first frame made
                // still there and moves it back into place instead of making a new one. A color
                // transform the tag leaves out is reset.
                if (m_wrapping && change == ChangeType::kAddDisplayObject && existing != m_stage.end()
                    && existing->second.character == character && existing->second.createdFrame == 0) {
                    DepthState state = existing->second;
                    if (hasMatrix) {
                        state.matrix = toTransform(data.matrix);
                    }
                    state.color = hasColor ? toColor(data.colorTransform) : IDENTITY_COLOR_TRANSFORM;
                    move(depth, state);
                    return;
                }

                // Adds or replaces an object, which appears at once. A replacement keeps what the
                // tag does not set.
                DepthState state;
                if (change == ChangeType::kReplaceDisplayObject && existing != m_stage.end()) {
                    state = existing->second;
                }
                if (hasMatrix) {
                    state.matrix = toTransform(data.matrix);
                }
                if (hasColor) {
                    state.color = toColor(data.colorTransform);
                }
                state.character = character;
                state.createdFrame = m_frame;
                m_stage.insert_or_assign(depth, state);
                rebuild(depth);
                return;
            }

            if (change != ChangeType::kModifyDisplayObject || existing == m_stage.end()) {
                return;
            }
            DepthState state = existing->second;
            if (hasMatrix) {
                state.matrix = toTransform(data.matrix);
            }
            if (hasColor) {
                state.color = toColor(data.colorTransform);
            }
            move(depth, state);
        }

        auto remove(std::int32_t depth) -> void
        {
            m_stage.erase(depth);
            rebuild(depth);
        }

    private:
        /**
         * @brief Moves or recolors the object at a depth, easing into it if this frame interpolates
         */
        auto move(std::int32_t depth,
                  const DepthState& next) -> void
        {
            auto& state = m_stage.at(depth);
            const DepthState before = state;
            state = next;
            if (!m_interpolate || m_rebuilt.contains(depth)) {
                return;
            }

            auto motion = std::ranges::find(m_motions, depth, &Motion::depth);
            if (motion == m_motions.end()) {
                Motion added;
                added.depth = depth;
                added.fromMatrix = before.matrix;
                added.fromColor = before.color;
                m_motions.push_back(added);
                motion = std::prev(m_motions.end());
            }
            motion->toMatrix = state.matrix;
            motion->toColor = state.color;
            motion->moves = motion->fromMatrix != motion->toMatrix;
            motion->recolors = motion->fromColor != motion->toColor;
        }

        /**
         * @brief Marks a depth whose object changed outright this frame, so it is not eased into
         */
        auto rebuild(std::int32_t depth) -> void
        {
            m_rebuilt.insert(depth);
            m_dropped.erase(depth);
            std::erase_if(m_motions, [depth](const Motion& motion) { return motion.depth == depth; });
        }

        Stage& m_stage;
        std::vector<Motion>& m_motions;
        std::unordered_set<std::int32_t> m_rebuilt;
        std::unordered_set<std::int32_t> m_dropped; /**< Objects a wrap drops unless the frame makes them again */
        std::uint32_t m_frame;
        bool m_interpolate;
        bool m_wrapping;
    };

    /**
     * @brief Runs one frame's tags through a simulation
     *
     * @param onlyPlaces Cleared if the frame holds anything but PlaceObject and RemoveObject tags
     * @return False if a tag could not be understood
     */
    auto runFrame(const ExecuteTagList& list,
                  FrameSimulation& simulation,
                  bool& onlyPlaces) -> bool
    {
        if (list.size != 0 && list.data == nullptr) {
            return false;
        }
        for (std::uint32_t i = 0; i < list.size; ++i) {
            const auto* tag = list.data[i];
            if (tag == nullptr) {
                continue;
            }

            const auto tagClass = classifyTag(tag);
            switch (tagClass.kind) {
            case TagKind::PLACE: {
                const DecodedPlace place(tag);
                if (!isPlausible(place.data())) {
                    return false;
                }
                simulation.place(place.data());
                break;
            }
            case TagKind::REMOVE: {
                std::uint16_t depth = 0;
                std::memcpy(&depth, reinterpret_cast<const std::byte*>(tag) + tagClass.depthOffset, sizeof(depth));
                simulation.remove(depth);
                break;
            }
            case TagKind::OTHER:
                onlyPlaces = false;
                break;
            }
        }
        return true;
    }

    /**
     * @brief Drops every motion that is not part of a tween
     *
     * A tween moves an object over several frames in a row, so its motions have neighbours. A
     * motion with none is a snap, where the animation means the object to jump. The transitions
     * are a cycle when the timeline starts over: motions[0] follows the last frame's.
     */
    void dropSnaps(std::vector<std::vector<Motion>>& motions)
    {
        const auto count = motions.size();
        std::vector<std::unordered_set<std::int32_t>> moving(count);
        for (std::size_t frame = 0; frame < count; ++frame) {
            for (const auto& motion : motions[frame]) {
                moving[frame].insert(motion.depth);
            }
        }
        for (std::size_t frame = 0; frame < count; ++frame) {
            const auto& before = moving[(frame + count - 1) % count];
            const auto& after = moving[(frame + 1) % count];
            std::erase_if(motions[frame], [&](const Motion& motion) {
                return !before.contains(motion.depth) && !after.contains(motion.depth);
            });
        }
    }

    /** Shorter timelines that start over only flip between two frames, which is not a tween */
    constexpr std::uint32_t MIN_WRAPPING_FRAMES = 3;

} // namespace

auto planTimeline(const ExecuteTagList* frames,
                  std::uint32_t frameCount,
                  TimelinePlan& plan) -> bool
{
    const std::scoped_lock lock(s_mutex);

    plan.motions.assign(frameCount, {});
    plan.motionCount = 0;

    Stage stage;
    bool lastOnlyPlaces = true;
    for (std::uint32_t frame = 0; frame < frameCount; ++frame) {
        bool onlyPlaces = true;
        FrameSimulation simulation(stage, plan.motions[frame], frame, frame > 0, false);
        if (!runFrame(frames[frame], simulation, onlyPlaces)) {
            return false;
        }
        lastOnlyPlaces = onlyPlaces;
    }

    // Playing past the last frame starts the timeline over. A last frame with nothing but objects
    // in it can't jump or stop anywhere else, so playing on from it is sure to run the first frame
    // again, from the stage the last frame left.
    if (frameCount >= MIN_WRAPPING_FRAMES && lastOnlyPlaces) {
        bool onlyPlaces = true;
        FrameSimulation simulation(stage, plan.motions[0], 0, true, true);
        if (!runFrame(frames[0], simulation, onlyPlaces)) {
            return false;
        }
    }

    dropSnaps(plan.motions);
    for (const auto& motions : plan.motions) {
        plan.motionCount += motions.size();
    }
    return true;
}

} // namespace SpeedyUI
