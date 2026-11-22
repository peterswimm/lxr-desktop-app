// The SOUND screen's pages: the LXR's own menu (Core/Inc/menu/menuPages.h), one voice at
// a time, up to eight parameters a page as on the LXR's two rows of four knobs.
// Parameters are named as in native/generated/LxrParams.hpp; Pages::resolve() turns the
// names into ids once, and test_lxr_sndstation checks every name resolves.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace lxr { class LxrInstrument; }

namespace lxrapp {

struct Knob {
	std::string name;    // LxrParams name, e.g. "d1_coarse"
	const char* label;   // what the knob says, as the LXR's menu does
	uint32_t id = 0;     // resolved
};

struct Page {
	const char* title;
	std::vector<Knob> knobs;   // at most 8
};

struct Voice {
	const char* tab;           // D1 ... HH, FX
	const char* name;
	int track;                 // the track X plays, or -1
	std::vector<Page> pages;
};

// The FX page depends on the FX type, as on the LXR (menuPages.h FX_MODE_PAGE).
constexpr int kFxVoice = 6;

class Pages {
public:
	Pages();
	// Fill in the ids; false (with the missing names in `missing`) when a name is not
	// one of the instrument's parameters.
	bool resolve(const lxr::LxrInstrument& in, std::string* missing = nullptr);
	const std::vector<Voice>& voices() const { return voices_; }
	// the FX voice's first page for an FX type (0 Off ... 4 Delay)
	const Page& fxPage(int type) const { return fx_[type < 0 || type > 4 ? 0 : type]; }

private:
	std::vector<Voice> voices_;
	std::vector<Page> fx_;
};

// The voice a track's sound is on: the hihats share one.
inline int voiceOfTrack(int track) { return track >= 6 ? 5 : track; }

} // namespace lxrapp
