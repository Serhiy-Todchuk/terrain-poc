#pragma once
#include "Config.h"

#include <filesystem>
#include <string>

// UI settings persisted between runs ("TerrainPOC.ini" next to the exe): the
// layer / rendering / weather toggles, MSAA, vsync, the tunables of the settings
// panel, the HUD / panel visibility, the open panel tab and sections, the window
// size. Not the weather time (live at every start) and not the camera (the
// start view comes from the command line). Plain "key=value" lines, written at
// exit; command-line options override the file; harness runs (--script /
// --screenshot) and --no-settings neither read nor write it.
namespace Settings {

// Reads the file into `cfg` (keys it knows; the others keep their values).
// `report` describes the outcome for the log. False: no file / unreadable.
bool load(const std::filesystem::path& path, AppConfig& cfg, std::string& report);

// Writes the persisted subset of `cfg`.
bool save(const std::filesystem::path& path, const AppConfig& cfg, std::string& report);

} // namespace Settings
