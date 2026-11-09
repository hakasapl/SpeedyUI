#pragma once

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace SpeedyUI {

using PathList = std::vector<std::string>; /**< Normalized path patterns read from the INI */

/**
 * @brief Lower-cases text, since the game and the INI may spell names differently
 *
 * @param text Text to convert
 * @return Lower-case copy of the text
 */
inline auto toLower(std::string_view text) -> std::string
{
    std::string lowered(text);
    std::ranges::transform(lowered, lowered.begin(), [](unsigned char character) -> char {
        return static_cast<char>(std::tolower(character));
    });
    return lowered;
}

/**
 * @brief Lower-cases a path and writes its separators one way: single forward slashes
 *
 * @param path Path as written by the game or the INI
 * @return Lower-case path with "/" between folders
 */
inline auto normalizeSlashes(std::string_view path) -> std::string
{
    std::string normalized;
    normalized.reserve(path.size());
    for (const char character : path) {
        const char next = character == '\\'
                              ? '/'
                              : static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
        if (next == '/' && !normalized.empty() && normalized.back() == '/') {
            continue;
        }
        normalized.push_back(next);
    }
    return normalized;
}

/**
 * @brief The path of a movie relative to the Interface folder, the form the INI lists use
 *
 * The game loads menus from Data/Interface: BSScaleformManager builds "Interface/<menu>.swf", or
 * "Interface/Exported/<menu>.gfx". So "Interface/HUDMenu.swf" becomes "hudmenu.swf" and
 * "Interface/StartMenuRedesigned/title.swf" becomes "startmenuredesigned/title.swf". A movie from
 * anywhere else gets a path starting with "../", which no INI entry can match.
 *
 * @param url Movie URL as reported by Scaleform
 * @return Lower-case path relative to Data/Interface
 */
inline auto moviePath(std::string_view url) -> std::string
{
    auto path = normalizeSlashes(url);
    while (path.starts_with("./") || path.starts_with('/')) {
        path.erase(0, path.starts_with('/') ? 1 : 2);
    }

    constexpr std::string_view DATA_FOLDER = "data/";
    constexpr std::string_view INTERFACE_FOLDER = "interface/";
    if (path.starts_with(DATA_FOLDER)) {
        path.erase(0, DATA_FOLDER.size());
    }
    if (path.starts_with(INTERFACE_FOLDER)) {
        return path.substr(INTERFACE_FOLDER.size());
    }
    return "../" + path;
}

/**
 * @brief Whether a normalized INI entry is written relative to Data/Interface
 *
 * A file directly in Data/Interface is just its name, and one in a subfolder starts with the
 * folder: "hudmenu.swf", "mymenu/main.swf". Writing the path from Data or from the drive instead,
 * such as "Interface/hudmenu.swf", "Data/Interface/hudmenu.swf" or "C:/.../hudmenu.swf", is not
 * accepted, and neither is stepping into "." or "..".
 *
 * @param path Entry after normalizeSlashes
 */
inline auto isInterfacePath(std::string_view path) -> bool
{
    if (path.empty() || path.starts_with('/') || path.contains(':')) {
        return false;
    }

    std::size_t start = 0;
    for (bool first = true; start <= path.size(); first = false) {
        const auto slash = path.find('/', start);
        const auto segment = path.substr(start, slash == std::string_view::npos ? std::string_view::npos : slash - start);
        if (segment == "." || segment == ".." || (first && (segment == "data" || segment == "interface"))) {
            return false;
        }
        if (slash == std::string_view::npos) {
            break;
        }
        start = slash + 1;
    }
    return true;
}

/**
 * @brief Matches text against a pattern in which * is any run of characters and ? is one character
 *
 * * also matches across folders, so "mymenu/*" covers every file below Interface/mymenu.
 * Both arguments are expected to be normalized already, so the comparison is case-sensitive.
 *
 * @param pattern Pattern that may contain * and ?
 * @param text Text to test
 * @return True if the whole text matches the whole pattern
 */
inline auto wildcardMatch(std::string_view pattern, std::string_view text) -> bool
{
    std::size_t patternPos = 0;
    std::size_t textPos = 0;
    std::size_t starPos = std::string_view::npos; /**< Pattern position of the last * seen */
    std::size_t starEnd = 0;                      /**< Text position that last * currently extends to */

    while (textPos < text.size()) {
        if (patternPos < pattern.size() && pattern[patternPos] == '*') {
            starPos = patternPos++;
            starEnd = textPos;
        } else if (patternPos < pattern.size()
                   && (pattern[patternPos] == '?' || pattern[patternPos] == text[textPos])) {
            ++patternPos;
            ++textPos;
        } else if (starPos != std::string_view::npos) {
            // Let the last * take one more character and match the rest of the pattern from there.
            // Backtracking only to the last * is enough, since an earlier one can never do better.
            patternPos = starPos + 1;
            textPos = ++starEnd;
        } else {
            return false;
        }
    }

    // Anything left in the pattern has to be *, which matches the empty rest of the text
    return std::ranges::all_of(pattern.substr(patternPos), [](char character) { return character == '*'; });
}

/**
 * @brief An INI list, split into the entries that can be used and the ones that can't
 */
struct ParsedPathList {
    PathList paths; /**< Normalized entries relative to Data/Interface */
    std::vector<std::string> rejected; /**< Entries as written that are not relative to Data/Interface */
};

/**
 * @brief Parses a comma-separated list of paths relative to Data/Interface from the INI
 *
 * @param list Raw INI value
 * @return Normalized entries, with surrounding whitespace and empty entries removed, and the entries
 *         isInterfacePath rejects
 */
inline auto parsePathList(std::string_view list) -> ParsedPathList
{
    constexpr std::string_view WHITESPACE = " \t\r\n";

    ParsedPathList parsed;
    std::size_t start = 0;
    while (start <= list.size()) {
        const auto comma = list.find(',', start);
        auto entry = list.substr(start, comma == std::string_view::npos ? std::string_view::npos : comma - start);

        const auto first = entry.find_first_not_of(WHITESPACE);
        if (first != std::string_view::npos) {
            entry = entry.substr(first, entry.find_last_not_of(WHITESPACE) - first + 1);
            auto path = normalizeSlashes(entry);
            if (isInterfacePath(path)) {
                parsed.paths.push_back(std::move(path));
            } else {
                parsed.rejected.emplace_back(entry);
            }
        }

        if (comma == std::string_view::npos) {
            break;
        }
        start = comma + 1;
    }
    return parsed;
}

/**
 * @brief Checks a movie path against every pattern in a list
 *
 * @param path Movie path, as returned by moviePath
 * @param patterns Normalized patterns, as returned by parsePathList
 * @return True if any pattern matches the path
 */
inline auto matchesAny(std::string_view path, const PathList& patterns) -> bool
{
    // Entries name files in Data/Interface, so a movie from anywhere else matches none of them,
    // not even a pattern that is all wildcards
    if (path.starts_with("../")) {
        return false;
    }
    return std::ranges::any_of(patterns, [path](const std::string& pattern) { return wildcardMatch(pattern, path); });
}

/**
 * @brief Decides whether a movie is included, so SpeedyUI changes it
 *
 * An empty allowlist includes every movie, otherwise only movies matching an allowlist entry are
 * included. The blocklist is applied after the allowlist, so a movie matching both is excluded.
 *
 * @param path Movie path, as returned by moviePath
 * @param allowlist Patterns allowed by the INI
 * @param blocklist Patterns blocked by the INI
 * @return True if the movie is included
 */
inline auto isIncluded(std::string_view path,
                       const PathList& allowlist,
                       const PathList& blocklist) -> bool
{
    if (!allowlist.empty() && !matchesAny(path, allowlist)) {
        return false;
    }
    return !matchesAny(path, blocklist);
}

} // namespace SpeedyUI
