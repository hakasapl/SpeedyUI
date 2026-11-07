#pragma once

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <string>
#include <string_view>
#include <unordered_set>

namespace SpeedyUI {

using FileSet = std::unordered_set<std::string>;

/**
 * @brief Lower-cases a filename, since the game and the INI may spell it differently
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
 * @brief Reduces a movie URL such as "Interface/HUDMenu.swf" to its lower-case filename
 *
 * @param url Movie URL as reported by Scaleform
 * @return Lower-case filename with its extension
 */
inline auto fileNameOf(std::string_view url) -> std::string
{
    const auto lastSlash = url.find_last_of("/\\");
    return toLower(lastSlash == std::string_view::npos ? url : url.substr(lastSlash + 1));
}

/**
 * @brief Parses a comma-separated list of filenames from the INI
 *
 * @param list Raw INI value
 * @return Lower-case filenames with surrounding whitespace removed
 */
inline auto parseFileList(std::string_view list) -> FileSet
{
    constexpr std::string_view WHITESPACE = " \t\r\n";

    FileSet files;
    std::size_t start = 0;
    while (start <= list.size()) {
        const auto comma = list.find(',', start);
        auto entry = list.substr(start, comma == std::string_view::npos ? std::string_view::npos : comma - start);

        const auto first = entry.find_first_not_of(WHITESPACE);
        if (first != std::string_view::npos) {
            entry = entry.substr(first, entry.find_last_not_of(WHITESPACE) - first + 1);
            files.insert(toLower(entry));
        }

        if (comma == std::string_view::npos) {
            break;
        }
        start = comma + 1;
    }
    return files;
}

/**
 * @brief Decides whether a movie should be sped up
 *
 * An empty allowlist includes every movie, otherwise only listed movies are included. The
 * blocklist is applied after the allowlist, so a file in both lists is excluded.
 *
 * @param fileName Lower-case filename, as returned by fileNameOf
 * @param allowlist Files allowed by the INI
 * @param blocklist Files blocked by the INI
 * @return True if the movie should be sped up
 */
inline auto isIncluded(const std::string& fileName,
                       const FileSet& allowlist,
                       const FileSet& blocklist) -> bool
{
    if (!allowlist.empty() && !allowlist.contains(fileName)) {
        return false;
    }
    return !blocklist.contains(fileName);
}

} // namespace SpeedyUI
