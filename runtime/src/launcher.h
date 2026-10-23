// Startup launcher: a small native window before the game boots (game files, mods, a few options).
#pragma once
#include <string>

namespace launcher {

// Shows the launcher unless it is switched off (setting show_launcher, Shift at startup forces it) or
// the run is automated (HWDER_LOG / HWDER_AUTOPLAY / HWDER_OFFSCREEN / HWDER_NO_LAUNCHER). Applies the
// saved game paths (HWDER_NSP / HWDER_KEYS) either way. False = the user closed it: exit.
bool run(const std::string& exe_dir);

}  // namespace launcher
