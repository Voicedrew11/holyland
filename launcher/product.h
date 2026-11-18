// What the player sees the port called (docs/port/LAUNCHER.md): the launcher's window and messages, the
// AppImage's folders. One place, so that a new name is one edit here and in packaging/package.env. The
// project's own names (kf4, the data folder holyland/) are not the player's and stay.
#pragma once

namespace launcher {

constexpr const char *kProductName = "Darker Reality";  // in windows and messages
constexpr const char *kProductId = "darker-reality";    // the program, the AppImage's share/<id>/, the log prefix

} // namespace launcher
