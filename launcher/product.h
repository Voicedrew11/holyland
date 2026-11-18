// The port's name (docs/port/LAUNCHER.md): the launcher's window and messages, the AppImage's folders. A new
// name is an edit here, in packaging/package.env and in the folders of port/platform/data_dir.c. The game's
// program stays kf4.
#pragma once

namespace launcher {

constexpr const char *kProductName = "Darker Reality";  // in windows and messages
constexpr const char *kProductId = "darker-reality";    // the program, the AppImage's share/<id>/, the log prefix

} // namespace launcher
