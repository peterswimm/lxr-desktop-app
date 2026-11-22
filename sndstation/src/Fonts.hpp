#pragma once
#include <string>

namespace lxrapp {

// The app's typeface is Open Sans (fonts/), and only Open Sans: it is named through
// sndstation's style for all three of the SDK's faces (sans, bold and mono), so set
// it before the Window opens, when the fonts are loaded.
//
// The SDK falls back to the platform's own face (JetBrains Mono) for a style font
// that will not open, so the files are opened here first. When they are not found,
// or do not open, this says why and returns false, and the app does not start
// rather than draw in another face.
//
// Where it looks, in order:
//   $LXR_FONT_DIR                           when set, the only place looked
//   <bin>/../share/lxr-sndstation/fonts     installed
//   <bin>/fonts                             a folder beside the binary
//   the source tree's fonts/                a build tree, run in place
bool useOpenSans(std::string* dir, std::string* why);

} // namespace lxrapp
