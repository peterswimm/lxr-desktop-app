// The session on disk: the whole instrument (kit, 16 patterns, settings) as
// LxrInstrument::saveState() text, in ~/.config/lxr/session.lxr.
#pragma once

#include <memory>
#include <string>

#include "LxrInstrument.hpp"

namespace lxrapp {

// What the app always plays with, whatever a file says: its own clock, stopped, Aux
// mixed into Main (the handheld has one stereo output).
void applyAppSettings(lxr::LxrInstrument& in);

// Something to play on a first start: four on the floor, a backbeat and hats, in
// pattern 1 (the RNBO port's demoPattern).
void demoPattern(lxr::LxrInstrument& in);

// A fresh instrument: the init kit, empty patterns but for the demo, the app's settings.
std::unique_ptr<lxr::LxrInstrument> freshInstrument();

// Read a session into `in`. False when there is no file or it is not a session.
bool loadSession(const std::string& path, lxr::LxrInstrument& in, std::string* why = nullptr);
// Write `text` to `path` through a temporary file, so a crash mid-write keeps the old one.
bool writeSession(const std::string& path, const std::string& text, std::string* why = nullptr);

} // namespace lxrapp
