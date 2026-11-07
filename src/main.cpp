#include "PCH.h"

#include "Filter.h"

#include <spdlog/common.h>
#include <spdlog/logger.h>
#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/spdlog.h>

#define SI_NO_CONVERSION
#include <SimpleIni.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_set>
#include <utility>

namespace {

constexpr std::size_t BRANCH_SIZE = 5; /**< Bytes of GetFrameRate overwritten by the branch to the hook */
constexpr std::size_t TRAMPOLINE_SIZE = 14; /**< One absolute jump for the branch patched over GetFrameRate */

constexpr const char* CONFIG_PATH = "Data/SKSE/Plugins/" PLUGIN_NAME ".ini";
constexpr const char* CONFIG_SECTION = "General";
constexpr double DEFAULT_TARGET_FPS = 60.0;

/**
 * @brief The complete body of GFxMovieDefImpl::GetFrameRate, identical on 1.5.97, 1.6.1170, 1.7.99 and 1.7.104
 *
 *   mov rax, [rcx+38h]        ; bindTaskData
 *   mov rcx, [rax+18h]        ; movieDataResource
 *   mov rax, [rcx+40h]        ; loadTaskData
 *   movss xmm0, [rax+68h]     ; movieInfo.frameRate
 *   ret
 *
 * The hook replaces this function, so it refuses to install unless these bytes match. That also
 * proves the field chain used in getFrameRateHook is right for the running game.
 */
constexpr std::array<std::uint8_t, 18> GET_FRAME_RATE_CODE
    = {0x48, 0x8B, 0x41, 0x38, 0x48, 0x8B, 0x48, 0x18, 0x48, 0x8B, 0x41, 0x40, 0xF3, 0x0F, 0x10, 0x40, 0x68, 0xC3};

/**
 * @brief Settings read from the INI, never modified after SKSEPluginLoad
 */
struct Config {
    bool enabled = true;
    float targetFPS = static_cast<float>(DEFAULT_TARGET_FPS);
    SpeedyUI::FileSet allowlist;
    SpeedyUI::FileSet blocklist;
};

// The hook is called by the game with no context of its own, so its state has to be global
// NOLINTBEGIN(cppcoreguidelines-avoid-non-const-global-variables)
Config s_config;

std::mutex s_loggedMoviesMutex;
std::unordered_set<std::string> s_loggedMovies; /**< Movies already reported, so each is logged once */
// NOLINTEND(cppcoreguidelines-avoid-non-const-global-variables)

/**
 * @brief Sets up the global log file for the plugin using spdlog
 */
void setupLog()
{
    // Resolve the SKSE log directory (Documents/My Games/.../SKSE)
    auto logsFolder = SKSE::log::log_directory();
    if (!logsFolder) {
        SKSE::stl::report_and_fail("SKSE log_directory not provided, logs disabled.");
    }

    // Create a truncating file sink named after the plugin and make it the default logger
    const auto logFilePath = *logsFolder / (std::string(PLUGIN_NAME) + ".log");
    auto fileLogger = std::make_shared<spdlog::sinks::basic_file_sink_mt>(logFilePath.string(), true);
    auto logger = std::make_shared<spdlog::logger>("log", std::move(fileLogger));

    // Log everything and flush per message so crashes don't lose the tail of the log
    spdlog::set_default_logger(std::move(logger));
    spdlog::set_level(spdlog::level::info);
    spdlog::flush_on(spdlog::level::info);
}

/**
 * @brief Reads the plugin INI, falling back to the defaults for anything missing
 *
 * @return Settings to run with
 */
auto loadConfig() -> Config
{
    Config config;

    CSimpleIniA ini;
    if (ini.LoadFile(CONFIG_PATH) < 0) {
        spdlog::warn("Could not read {}, using defaults", CONFIG_PATH);
        return config;
    }

    config.enabled = ini.GetBoolValue(CONFIG_SECTION, "bEnabled", config.enabled);
    config.allowlist = SpeedyUI::parseFileList(ini.GetValue(CONFIG_SECTION, "sAllowlist", ""));
    config.blocklist = SpeedyUI::parseFileList(ini.GetValue(CONFIG_SECTION, "sBlocklist", ""));

    const auto targetFPS = static_cast<float>(ini.GetDoubleValue(CONFIG_SECTION, "fTargetFPS", DEFAULT_TARGET_FPS));
    if (std::isfinite(targetFPS) && targetFPS > 0.0F) {
        config.targetFPS = targetFPS;
    } else {
        spdlog::warn("Ignoring invalid fTargetFPS {}, using {}", targetFPS, DEFAULT_TARGET_FPS);
    }

    return config;
}

/**
 * @brief Reports what happened to a movie the first time it is seen
 *
 * @param fileName Lower-case movie filename
 * @param headerFPS Frame rate stored in the movie file
 * @param frameRate Frame rate handed to Scaleform
 * @param included False if the allowlist or blocklist excluded the movie
 */
void logMovieOnce(const std::string& fileName,
                  float headerFPS,
                  float frameRate,
                  bool included)
{
    const std::scoped_lock lock(s_loggedMoviesMutex);
    if (!s_loggedMovies.insert(fileName).second) {
        return;
    }

    if (!included) {
        spdlog::info("{}: left at {} FPS (excluded by allowlist or blocklist)", fileName, headerFPS);
    } else if (frameRate == headerFPS) {
        spdlog::info("{}: left at {} FPS (already at or above target)", fileName, headerFPS);
    } else {
        spdlog::info("{}: {} -> {} FPS", fileName, headerFPS, frameRate);
    }
}

/**
 * @brief Replacement for GFxMovieDefImpl::GetFrameRate
 *
 * GFxMovieRoot::SetLevelMovie takes 1 / GetFrameRate() as the movie's time per frame when a movie
 * becomes level 0, and GFxMovieRoot::Advance steps the timeline by that. Movies loaded inside
 * another one never set it, so only a menu's main movie gets here and everything it loads follows
 * it. The delta time is left alone on purpose: it also drives getTimer() and setInterval.
 *
 * @param movieDef Movie being asked for its frame rate
 * @return Frame rate from the movie file, raised to the target if the movie is included
 */
auto getFrameRateHook(const RE::GFxMovieDefImpl* movieDef) -> float
{
    const float headerFPS = movieDef->bindTaskData->movieDataResource->loadTaskData->movieInfo.frameRate;

    const char* url = movieDef->GetFileURL();
    const auto fileName = SpeedyUI::fileNameOf(url != nullptr ? url : "");

    const bool included = SpeedyUI::isIncluded(fileName, s_config.allowlist, s_config.blocklist);
    const float frameRate = included ? std::max(headerFPS, s_config.targetFPS) : headerFPS;

    logMovieOnce(fileName, headerFPS, frameRate, included);
    return frameRate;
}

/**
 * @brief Patches GFxMovieDefImpl::GetFrameRate, unless its code is not what this plugin expects
 */
void installHook()
{
    const REL::Relocation<std::uintptr_t> getFrameRate {RELOCATION_ID(83381, 85326)};

    if (std::memcmp(reinterpret_cast<const void*>(getFrameRate.address()),
                    GET_FRAME_RATE_CODE.data(),
                    GET_FRAME_RATE_CODE.size())
        != 0) {
        spdlog::error("GFxMovieDefImpl::GetFrameRate has unexpected code on this game version, no hook installed");
        return;
    }

    SKSE::AllocTrampoline(TRAMPOLINE_SIZE);
    SKSE::GetTrampoline().write_branch<BRANCH_SIZE>(getFrameRate.address(), getFrameRateHook);
    spdlog::info("Hook installed");
}

} // namespace

//
// CommonLibSSE-NG / SKSE Exports
//

SKSEPluginInfo(.Version = REL::Version {0,
                                        1,
                                        0,
                                        0},
               .Name = PLUGIN_NAME,
               .Author = "hakasapl",
               .StructCompatibility = SKSE::StructCompatibility::Independent,
               .RuntimeCompatibility = SKSE::VersionIndependence::AddressLibrary)

    SKSEPluginLoad(const SKSE::LoadInterface* skse)
{
    SKSE::Init(skse);
    setupLog();

    const auto version = REL::Module::get().version();
    spdlog::info("{} {} loading (runtime {})", PLUGIN_NAME, PLUGIN_VERSION, version.string("."));

    s_config = loadConfig();
    spdlog::info("bEnabled={} fTargetFPS={} allowlist entries={} blocklist entries={}",
                 s_config.enabled,
                 s_config.targetFPS,
                 s_config.allowlist.size(),
                 s_config.blocklist.size());

    if (s_config.enabled) {
        installHook();
    }

    spdlog::info("{} loaded", PLUGIN_NAME);
    return true;
}
