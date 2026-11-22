#include "App.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace lxrapp {

using namespace sndstation;
using gfx::Font;
using theme::su;
using theme::suf;

namespace {

const char* const kTrackTab[kTracks] = {"D1", "D2", "D3", "SN", "CY", "CH", "OH"};
const char* const kTrackName[kTracks] = {"Drum 1", "Drum 2", "Drum 3", "Snare", "Cymbal", "Closed hihat",
                                         "Open hihat"};
const SDL_Color kTrackColor[kTracks] = {{238, 74, 60, 255},  {244, 142, 46, 255}, {236, 200, 52, 255},
                                        {86, 200, 98, 255},  {62, 188, 228, 255}, {96, 124, 242, 255},
                                        {172, 96, 232, 255}};

constexpr uint32_t kPendingMs = 400;     // a posted value stands in this long at most
constexpr uint32_t kFlashMs = 140;
constexpr uint32_t kToastMs = 1800;
constexpr uint32_t kConfirmMs = 2500;
constexpr uint32_t kMenuHoldMs = 1500;   // as MSDJ: hold MENU to quit

// STEP's rows: the step's fields, then its track's
const uint32_t kStepRows[] = {lxr::kIdStepOn,     lxr::kIdStepVelocity, lxr::kIdStepProbability,
                              lxr::kIdStepNote,   lxr::kIdStepFlam,     lxr::kIdStepShift,
                              lxr::kIdLock1Dest,  lxr::kIdLock1Value,   lxr::kIdLock2Dest,
                              lxr::kIdLock2Value};
const char* const kStepLabels[] = {"Active", "Velocity", "Probability", "Note",   "Flam",
                                   "Shift",  "Lock 1",   "Lock 1 value", "Lock 2", "Lock 2 value"};
constexpr int kNumStepRows = 10;
const uint32_t kTrackRows[] = {lxr::kIdTrackLength, lxr::kIdEuclidLength, lxr::kIdEuclidSteps,
                               lxr::kIdEuclidRotation};
const char* const kTrackLabels[] = {"Length", "Euclid length", "Euclid steps", "Euclid rotation"};
constexpr int kNumTrackRows = 4;
const char* const kTrackActions[] = {"Copy track", "Paste track", "Clear track"};
constexpr int kNumTrackActions = 3;

// PROJECT's rows
const uint32_t kProjectParams[] = {lxr::kIdBpm, lxr::kIdShuffle, lxr::kIdRecord, lxr::kIdNoteMap,
                                   lxr::kIdMidiOut};
const char* const kProjectLabels[] = {"Tempo", "Shuffle", "Record", "MIDI note input", "MIDI out"};
constexpr int kNumProjectParams = 5;
const char* const kProjectActions[] = {"Save now", "New session"};
constexpr int kProjectRows = kNumProjectParams + 2;

std::string noteName(int n) {
	static const char* const names[] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
	return std::string(names[((n % 12) + 12) % 12]) + std::to_string(n / 12 - 1);
}

SDL_Color scaled(SDL_Color c, float f) {
	f = std::clamp(f, 0.f, 1.f);
	return {Uint8(c.r * f), Uint8(c.g * f), Uint8(c.b * f), c.a};
}

SDL_Color mix(SDL_Color a, SDL_Color b, float t) {
	t = std::clamp(t, 0.f, 1.f);
	return {Uint8(a.r + (b.r - a.r) * t), Uint8(a.g + (b.g - a.g) * t), Uint8(a.b + (b.b - a.b) * t), 255};
}

uint64_t paramKey(uint32_t id) { return (1ull << 40) | id; }
uint64_t stepKey(int t, int s, uint32_t id) { return (2ull << 40) | (uint64_t(t) << 24) | (uint64_t(s) << 16) | id; }
uint64_t trackKey(int t, uint32_t id) { return (3ull << 40) | (uint64_t(t) << 16) | id; }

// One footer item: a control's print (two for a pair, "L1/R1") and what it does.
std::string item(pad::Button b, const char* what) { return std::string(pad::label(b)) + " " + what; }
std::string item(pad::Button b1, pad::Button b2, const char* what) {
	return std::string(pad::label(b1)) + "/" + pad::label(b2) + " " + what;
}

// `s` cut to `maxW` with "..." where it had to be, on a UTF-8 character boundary
std::string fit(Font f, const std::string& s, int maxW) {
	if (gfx::textWidth(f, s) <= maxW) return s;
	std::string t = s;
	while (!t.empty() && gfx::textWidth(f, t + "...") > maxW) {
		while (!t.empty() && (static_cast<unsigned char>(t.back()) & 0xC0) == 0x80) t.pop_back();
		if (!t.empty()) t.pop_back();
	}
	return t + "...";
}

} // namespace

const char* App::screenName(Screen s) {
	switch (s) {
	case Screen::Seq: return "SEQ";
	case Screen::Step: return "STEP";
	case Screen::Sound: return "SOUND";
	case Screen::Patterns: return "PATTERN";
	case Screen::Project: return "PROJECT";
	default: return "";
	}
}

App::App(Engine& engine) : engine_(engine) {
	std::string missing;
	if (!pages_.resolve(engine_.reader(), &missing))
		std::fprintf(stderr, "lxr: pages name parameters the instrument does not have: %s\n", missing.c_str());
	const View& v = engine_.current();
	for (int t = 0; t < kTracks; t++) lastTrig_[t] = v.triggers[t];
}

// ---------------------------------------------------------------- values
const lxr::ParamDef* App::def(uint32_t id) const {
	const lxr::LxrInstrument& in = engine_.reader();
	const int i = in.indexOf(id);
	return i < 0 ? nullptr : &in.params()[static_cast<size_t>(i)];
}

double App::value(uint32_t id) const {
	auto it = pending_.find(paramKey(id));
	if (it != pending_.end()) return it->second.v;
	return engine_.reader().value(id);
}

void App::setValue(uint32_t id, double v) {
	const lxr::ParamDef* d = def(id);
	if (!d) return;
	v = std::clamp(std::round(v), d->min, d->max);
	pending_[paramKey(id)] = {v, now_};
	engine_.param(id, v);
	if (id != lxr::kIdPlay && id != lxr::kIdPattern) touched();
}

void App::nudgeValue(uint32_t id, int delta) { setValue(id, value(id) + delta); }

std::string App::text(uint32_t id, double v) const {
	if (id == lxr::kIdLock1Dest || id == lxr::kIdLock2Dest) {
		// the FX bus's parameters are the bus's, whichever voice lists them
		std::string t = engine_.reader().valueText(id, v);
		const size_t k = t.find("FxP");
		return k == std::string::npos ? t : "FX P" + t.substr(k + 3);
	}
	if (id == lxr::kIdStepNote) return noteName(static_cast<int>(v)) + "  (" + std::to_string(int(v)) + ")";
	if (id == lxr::kIdStepProbability) return std::to_string(int(v)) + "%";
	if (id == lxr::kIdBpm) return std::to_string(int(v)) + " BPM";
	return engine_.reader().valueText(id, v);
}

int App::coarse(uint32_t id) const {
	const lxr::ParamDef* d = def(id);
	if (!d) return 1;
	const double range = d->max - d->min;
	if (range > 150) return 10;
	if (range > 20) return 8;
	return 1;
}

int App::stepField(const StepView& v, uint32_t id) const {
	switch (id) {
	case lxr::kIdStepOn: return v.on;
	case lxr::kIdStepVelocity: return v.velocity;
	case lxr::kIdStepProbability: return v.probability;
	case lxr::kIdStepNote: return v.note;
	case lxr::kIdStepFlam: return v.flam;
	case lxr::kIdStepShift: return v.shift;
	case lxr::kIdLock1Dest: return v.lock1;
	case lxr::kIdLock1Value: return v.value1;
	case lxr::kIdLock2Dest: return v.lock2;
	case lxr::kIdLock2Value: return v.value2;
	default: return 0;
	}
}

StepView App::step(int t, int s) const {
	StepView v = engine_.current().step[t][s];
	for (uint32_t id : kStepRows) {
		auto it = pending_.find(stepKey(t, s, id));
		if (it == pending_.end()) continue;
		const int x = static_cast<int>(it->second.v);
		switch (id) {
		case lxr::kIdStepOn: v.on = x != 0; break;
		case lxr::kIdStepVelocity: v.velocity = uint8_t(x); break;
		case lxr::kIdStepProbability: v.probability = uint8_t(x); break;
		case lxr::kIdStepNote: v.note = uint8_t(x); break;
		case lxr::kIdStepFlam: v.flam = uint8_t(x); break;
		case lxr::kIdStepShift: v.shift = int8_t(x); break;
		case lxr::kIdLock1Dest: v.lock1 = uint16_t(x); break;
		case lxr::kIdLock1Value: v.value1 = uint8_t(x); break;
		case lxr::kIdLock2Dest: v.lock2 = uint16_t(x); break;
		case lxr::kIdLock2Value: v.value2 = uint8_t(x); break;
		default: break;
		}
	}
	return v;
}

void App::setStep(int t, int s, uint32_t id, double v) {
	const lxr::ParamDef* d = def(id);
	if (!d) return;
	v = std::clamp(std::round(v), d->min, d->max);
	pending_[stepKey(t, s, id)] = {v, now_};
	engine_.stepEdit(t, s, id, v);
	touched();
}

int App::trackField(int t, uint32_t id) const {
	auto it = pending_.find(trackKey(t, id));
	if (it != pending_.end()) return static_cast<int>(it->second.v);
	const View& v = engine_.current();
	switch (id) {
	case lxr::kIdTrackLength: return v.length[t];
	case lxr::kIdEuclidLength: return v.euclid[t][0];
	case lxr::kIdEuclidSteps: return v.euclid[t][1];
	case lxr::kIdEuclidRotation: return v.euclid[t][2];
	default: return 0;
	}
}

void App::setTrack(int t, uint32_t id, double v) {
	const lxr::ParamDef* d = def(id);
	if (!d) return;
	v = std::clamp(std::round(v), d->min, d->max);
	pending_[trackKey(t, id)] = {v, now_};
	// the step editor works on its track: any step of it will do
	engine_.stepEdit(t, step_, id, v);
	touched();
}

// ---------------------------------------------------------------- actions
int App::cursorTrack() const {
	if (screen_ == Screen::Sound) {
		const int t = pages_.voices()[static_cast<size_t>(voice_)].track;
		return t < 0 ? track_ : t;
	}
	return track_;
}

void App::togglePlay() {
	const bool running = engine_.current().running;
	setValue(lxr::kIdPlay, running ? 0 : 1);
}

void App::toggleMute(int t) {
	const uint32_t id = lxr::kIdMute0 + static_cast<uint32_t>(t);
	const bool muted = value(id) != 0;
	setValue(id, muted ? 0 : 1);
	toast(std::string(kTrackName[t]) + (muted ? " plays" : " muted"));
}

void App::audition(int t) { engine_.audition(t, 127); }

void App::toast(const std::string& text) {
	toast_ = text;
	toastAt_ = now_;
}

// Destructive actions want the same press twice.
bool App::confirm(const std::string& what, const std::string& prompt) {
	if (confirm_ == what && now_ - confirmAt_ < kConfirmMs) {
		confirm_.clear();
		return true;
	}
	confirm_ = what;
	confirmAt_ = now_;
	toast(prompt);
	return false;
}

void App::saved(bool ok) {
	saveFailed_ = !ok;
	if (ok) {
		unsaved_ = false;
		savedAt_ = now_;
	}
}

const Page& App::soundPage() const {
	const Voice& v = pages_.voices()[static_cast<size_t>(voice_)];
	if (voice_ == kFxVoice && page_ == 0) return pages_.fxPage(static_cast<int>(value(v.pages[0].knobs[0].id)));
	return v.pages[static_cast<size_t>(page_)];
}

// ---------------------------------------------------------------- input
void App::event(const pad::Event& e, const pad::State& st) {
	using namespace pad;
	if (e.b == Menu) {
		if (!e.repeat) {
			menuHeld_ = e.pressed;
			menuSince_ = now_;
		}
		return;
	}
	if (!e.pressed) {
		if (e.b == East) releaseA();
		return;
	}
	const bool a = st.down(East) && e.b != East;
	const bool b = st.down(South);
	if (st.down(Select) && (e.b == Left || e.b == Right)) {
		const int n = static_cast<int>(Screen::Count);
		screen_ = static_cast<Screen>((static_cast<int>(screen_) + (e.b == Right ? 1 : n - 1)) % n);
		return;
	}
	if (e.repeat && (e.b == Start || e.b == North || e.b == West)) return;
	if (e.b == Start) {
		togglePlay();
		return;
	}
	if (e.b == West) {
		toggleMute(cursorTrack());
		return;
	}
	if (e.b == North) {
		int t = cursorTrack();
		if (screen_ == Screen::Sound && voice_ == 5 && b) t = 6;   // B + X: the open hihat
		audition(t);
		return;
	}
	switch (screen_) {
	case Screen::Seq: seqEvent(e, a, b); break;
	case Screen::Step: stepEvent(e, a, b); break;
	case Screen::Sound: soundEvent(e, a, b); break;
	case Screen::Patterns: patternsEvent(e, a, b); break;
	case Screen::Project: projectEvent(e, a, b); break;
	default: break;
	}
}

void App::releaseA() {
	if (!aHeld_) return;
	aHeld_ = false;
	// a tap on a step that was on switches it off; a hold that edited leaves it
	if (screen_ == Screen::Seq && aWasOn_ && !aEdited_ && aTrack_ == track_ && aStep_ == step_)
		setStep(track_, step_, lxr::kIdStepOn, 0);
}

void App::seqEvent(const pad::Event& e, bool a, bool b) {
	using namespace pad;
	switch (e.b) {
	case East: {
		if (e.repeat) return;
		aHeld_ = true;
		aTrack_ = track_;
		aStep_ = step_;
		aEdited_ = false;
		const StepView v = step(track_, step_);
		aWasOn_ = v.on;
		if (b) {
			// B + A: clear the step, every field back to the empty step's
			const StepView empty;
			for (uint32_t id : kStepRows) setStep(track_, step_, id, stepField(empty, id));
			aEdited_ = true;
		} else if (!v.on) {
			setStep(track_, step_, lxr::kIdStepOn, 1);
		}
		return;
	}
	case Up:
	case Down:
	case Left:
	case Right:
		if (a) {
			const int d = e.b == Up ? 8 : e.b == Down ? -8 : e.b == Right ? 1 : -1;
			const StepView v = step(track_, step_);
			setStep(track_, step_, lxr::kIdStepVelocity, v.velocity + d);
			if (!v.on) setStep(track_, step_, lxr::kIdStepOn, 1);
			aEdited_ = true;
		} else if (e.b == Up) {
			track_ = (track_ + kTracks - 1) % kTracks;
		} else if (e.b == Down) {
			track_ = (track_ + 1) % kTracks;
		} else {
			step_ = (step_ + (e.b == Right ? 1 : kSteps - 1)) % kSteps;
		}
		return;
	case L1:
	case R1:
		step_ = (step_ + (e.b == R1 ? 16 : kSteps - 16)) % kSteps;
		return;
	case L2:
	case R2: {
		const View& v = engine_.current();
		const int next = std::clamp((v.running ? v.pending : v.pattern) + (e.b == R2 ? 1 : -1), 0, kPatterns - 1);
		setValue(lxr::kIdPattern, next + 1);
		return;
	}
	default: return;
	}
}

void App::stepEvent(const pad::Event& e, bool a, bool b) {
	using namespace pad;
	const bool left = stepRow_ < kNumStepRows;
	switch (e.b) {
	case East: {
		if (e.repeat) return;
		if (stepRow_ < kNumStepRows) {
			const uint32_t id = kStepRows[stepRow_];
			if (b) setStep(track_, step_, id, stepField(StepView{}, id));
			else if (id == lxr::kIdStepOn) setStep(track_, step_, id, step(track_, step_).on ? 0 : 1);
			return;
		}
		const int r = stepRow_ - kNumStepRows;
		if (r < kNumTrackRows) {
			if (b) {
				static const int defaults[] = {16, 16, 4, 0};
				setTrack(track_, kTrackRows[r], defaults[r]);
			}
			return;
		}
		const View& v = engine_.current();
		switch (r - kNumTrackRows) {
		case 0:
			copyTrack_ = track_;
			copyTrackPattern_ = v.pattern;
			toast(std::string("Copied ") + kTrackName[track_]);
			break;
		case 1:
			if (copyTrack_ < 0) {
				toast("Copy a track first");
			} else if (confirm("paste", std::string("Press A again to paste over ") + kTrackName[track_])) {
				engine_.post({Cmd::CopyTrack, uint8_t(copyTrack_), uint8_t(copyTrackPattern_), uint8_t(track_)});
				touched();
				toast(std::string("Pasted over ") + kTrackName[track_]);
			}
			break;
		case 2:
			if (confirm("clear-track", std::string("Press A again to clear ") + kTrackName[track_])) {
				engine_.post({Cmd::ClearTrack, uint8_t(track_), uint8_t(v.pattern)});
				touched();
				toast(std::string("Cleared ") + kTrackName[track_]);
			}
			break;
		default: break;
		}
		return;
	}
	case Up:
	case Down:
	case Left:
	case Right:
		if (a) {
			uint32_t id = 0;
			int cur = 0;
			if (stepRow_ < kNumStepRows) {
				id = kStepRows[stepRow_];
				cur = stepField(step(track_, step_), id);
			} else if (stepRow_ - kNumStepRows < kNumTrackRows) {
				id = kTrackRows[stepRow_ - kNumStepRows];
				cur = trackField(track_, id);
			} else {
				return;
			}
			const int big = coarse(id);
			const int d = e.b == Up ? big : e.b == Down ? -big : e.b == Right ? 1 : -1;
			int next = cur + d;
			if (id == lxr::kIdLock1Dest || id == lxr::kIdLock2Dest) {
				// past the targets a step would store as another one (lockTargetOf)
				next = std::clamp(next, 0, lxr::kNumModTargets - 1);
				while (next > 0 && next < lxr::kNumModTargets - 1 && lockTargetOf(next) != next) next += d > 0 ? 1 : -1;
				if (lockTargetOf(next) != next) next = lockTargetOf(next);
			}
			if (stepRow_ < kNumStepRows) setStep(track_, step_, id, next);
			else setTrack(track_, id, next);
		} else if (e.b == Left || e.b == Right) {
			stepRow_ = left ? kNumStepRows + std::min(stepRow_, kNumTrackRows + kNumTrackActions - 1)
			                : std::min(stepRow_ - kNumStepRows, kNumStepRows - 1);
		} else if (left) {
			stepRow_ = (stepRow_ + (e.b == Down ? 1 : kNumStepRows - 1)) % kNumStepRows;
		} else {
			const int n = kNumTrackRows + kNumTrackActions;
			stepRow_ = kNumStepRows + (stepRow_ - kNumStepRows + (e.b == Down ? 1 : n - 1)) % n;
		}
		return;
	case L1:
	case R1:
		step_ = (step_ + (e.b == R1 ? 1 : kSteps - 1)) % kSteps;
		return;
	case L2:
	case R2:
		track_ = (track_ + (e.b == R2 ? 1 : kTracks - 1)) % kTracks;
		return;
	default: return;
	}
}

void App::soundEvent(const pad::Event& e, bool a, bool b) {
	using namespace pad;
	const auto& voices = pages_.voices();
	const int pages = static_cast<int>(voices[static_cast<size_t>(voice_)].pages.size());
	auto clampKnob = [&] {
		const int n = static_cast<int>(soundPage().knobs.size());
		knob_ = std::clamp(knob_, 0, std::max(0, n - 1));
	};
	switch (e.b) {
	case East: {
		if (e.repeat || !b) return;
		const auto& knobs = soundPage().knobs;
		if (knob_ < static_cast<int>(knobs.size())) {
			const lxr::ParamDef* d = def(knobs[static_cast<size_t>(knob_)].id);
			if (d) setValue(d->id, d->def);
		}
		return;
	}
	case Up:
	case Down:
	case Left:
	case Right: {
		const auto& knobs = soundPage().knobs;
		if (a) {
			if (knob_ >= static_cast<int>(knobs.size())) return;
			const uint32_t id = knobs[static_cast<size_t>(knob_)].id;
			const int big = coarse(id);
			nudgeValue(id, e.b == Up ? big : e.b == Down ? -big : e.b == Right ? 1 : -1);
			if (voice_ == kFxVoice && page_ == 0 && knob_ == 0) clampKnob();   // the type changes the page
			return;
		}
		const int n = static_cast<int>(knobs.size());
		if (e.b == Up || e.b == Down) {
			if (knob_ >= 4) knob_ -= 4;
			else if (knob_ + 4 < n) knob_ += 4;
		} else if (e.b == Right) {
			if (knob_ % 4 < 3 && knob_ + 1 < n) {
				knob_++;
			} else {
				page_ = (page_ + 1) % pages;
				knob_ = knob_ >= 4 ? 4 : 0;
				clampKnob();
			}
		} else {
			if (knob_ % 4 > 0) {
				knob_--;
			} else {
				const int row = knob_ >= 4 ? 4 : 0;
				page_ = (page_ + pages - 1) % pages;
				const int m = static_cast<int>(soundPage().knobs.size());
				knob_ = std::min(row + 3, m - 1);
				clampKnob();
			}
		}
		return;
	}
	case L1:
	case R1: {
		const int nv = static_cast<int>(voices.size());
		voice_ = (voice_ + (e.b == R1 ? 1 : nv - 1)) % nv;
		page_ = std::min(page_, static_cast<int>(voices[static_cast<size_t>(voice_)].pages.size()) - 1);
		clampKnob();
		const int t = voices[static_cast<size_t>(voice_)].track;
		if (t >= 0 && voiceOfTrack(track_) != voice_) track_ = t;
		return;
	}
	case L2:
	case R2:
		page_ = (page_ + (e.b == R2 ? 1 : pages - 1)) % pages;
		clampKnob();
		return;
	default: return;
	}
}

void App::patternsEvent(const pad::Event& e, bool a, bool b) {
	using namespace pad;
	(void)a;
	switch (e.b) {
	case East:
		if (e.repeat) return;
		if (b) {
			if (confirm("clear-pattern", "Press " + std::string(label(South)) + " + " + label(East) +
			                                 " again to clear pattern " + std::to_string(pattern_ + 1))) {
				engine_.post({Cmd::ClearPattern, uint8_t(pattern_)});
				touched();
				toast("Cleared pattern " + std::to_string(pattern_ + 1));
			}
		} else {
			setValue(lxr::kIdPattern, pattern_ + 1);
		}
		return;
	case Up: pattern_ = (pattern_ + 12) % 16; return;
	case Down: pattern_ = (pattern_ + 4) % 16; return;
	case Left: pattern_ = (pattern_ / 4) * 4 + (pattern_ + 3) % 4; return;
	case Right: pattern_ = (pattern_ / 4) * 4 + (pattern_ + 1) % 4; return;
	case L1:
		if (e.repeat) return;
		copyPattern_ = pattern_;
		toast("Copied pattern " + std::to_string(pattern_ + 1));
		return;
	case R1:
		if (e.repeat) return;
		if (copyPattern_ < 0) {
			toast("Copy a pattern first");
		} else if (copyPattern_ != pattern_ &&
		           confirm("paste-pattern", "Press " + std::string(label(R1)) + " again to paste over pattern " +
		                                        std::to_string(pattern_ + 1))) {
			engine_.post({Cmd::CopyPattern, uint8_t(copyPattern_), uint8_t(pattern_)});
			touched();
			toast("Pasted over pattern " + std::to_string(pattern_ + 1));
		}
		return;
	default: return;
	}
}

void App::projectEvent(const pad::Event& e, bool a, bool b) {
	using namespace pad;
	switch (e.b) {
	case East:
		if (e.repeat) return;
		if (projectRow_ < kNumProjectParams) {
			const uint32_t id = kProjectParams[projectRow_];
			const lxr::ParamDef* d = def(id);
			if (b && d) setValue(id, d->def);
			else if (d && d->max - d->min == 1) setValue(id, value(id) != 0 ? d->min : d->max);
		} else if (projectRow_ == kNumProjectParams) {
			saveAsked_ = true;
		} else if (confirm("reset", "Press A again: a new session, the init kit and empty patterns")) {
			resetAsked_ = true;
		}
		return;
	case Up:
	case Down:
	case Left:
	case Right:
		if (a && projectRow_ < kNumProjectParams) {
			const uint32_t id = kProjectParams[projectRow_];
			const int big = coarse(id);
			nudgeValue(id, e.b == Up ? big : e.b == Down ? -big : e.b == Right ? 1 : -1);
		} else if (e.b == Up) {
			projectRow_ = (projectRow_ + kProjectRows - 1) % kProjectRows;
		} else if (e.b == Down) {
			projectRow_ = (projectRow_ + 1) % kProjectRows;
		}
		return;
	default: return;
	}
}

// ---------------------------------------------------------------- frame
void App::frame(uint32_t nowMs) {
	now_ = nowMs;
	engine_.view();
	engine_.collect();
	for (auto it = pending_.begin(); it != pending_.end();)
		it = now_ - it->second.at > kPendingMs ? pending_.erase(it) : std::next(it);
	const View& v = engine_.current();
	for (int t = 0; t < kTracks; t++)
		if (v.triggers[t] != lastTrig_[t]) {
			lastTrig_[t] = v.triggers[t];
			flashAt_[t] = now_;
		}
	if (menuHeld_ && now_ - menuSince_ >= kMenuHoldMs) quit_ = true;
	if (aHeld_ && screen_ != Screen::Seq) aHeld_ = false;
}

// ---------------------------------------------------------------- drawing
void App::draw(SDL_Renderer* r) {
	gfx::clear(r, theme::bg0);
	switch (screen_) {
	case Screen::Seq: drawSeq(r); break;
	case Screen::Step: drawStep(r); break;
	case Screen::Sound: drawSound(r); break;
	case Screen::Patterns: drawPatterns(r); break;
	case Screen::Project: drawProject(r); break;
	default: break;
	}
	drawHeader(r);
	drawFooter(r);
}

void App::drawHeader(SDL_Renderer* r) {
	using namespace theme;
	const int h = HUD_H;
	gfx::fillRect(r, {0, 0, SCREEN_W, h}, bg1);
	gfx::hline(r, 0, h - 1, SCREEN_W, line);
	int x = su(16);
	x += gfx::textMid(r, Font::Mark, "LXR", x, h / 2, txt0) + su(18);
	for (int i = 0; i < static_cast<int>(Screen::Count); i++) {
		const Screen s = static_cast<Screen>(i);
		const char* name = screenName(s);
		const int w = gfx::textWidth(Font::LabelBold, name) + su(16);
		const bool on = s == screen_;
		if (on) gfx::fillRect(r, {x, su(10), w, h - su(20)}, accL);
		gfx::textMid(r, Font::LabelBold, name, x + w / 2, h / 2, on ? bg0 : txt2, 0);
		x += w + su(4);
	}

	// transport, right-aligned: play state, tempo, pattern (and the next one), record
	const View& v = engine_.current();
	std::string pat = "P" + std::to_string(v.pattern + 1);
	if (v.running && v.pending != v.pattern) pat += ">" + std::to_string(v.pending + 1);
	std::string right = pat + "  " + std::to_string(int(value(lxr::kIdBpm)));
	int rx = SCREEN_W - su(16);
	rx -= gfx::textMid(r, Font::Value, right, rx, h / 2, txt0, 1);
	if (value(lxr::kIdRecord) != 0) {
		rx -= su(10);
		gfx::disc(r, rx - suf(6), h / 2.f, suf(6), warn);
		rx -= su(14);
	}
	rx -= su(14);
	const int cy = h / 2;
	if (v.running) {
		// a play triangle, from two rects' worth of geometry
		const float s = suf(8);
		SDL_Vertex tri[3] = {{{rx - s, cy - s}, accL, {0, 0}}, {{rx - s, cy + s}, accL, {0, 0}},
		                     {{rx + s * 0.8f, float(cy)}, accL, {0, 0}}};
		gfx::geometry(r, tri, 3);
	} else {
		gfx::fillRect(r, {rx - su(7), cy - su(7), su(14), su(14)}, txt2);
	}
}

void App::drawFooter(SDL_Renderer* r) {
	using namespace theme;
	using namespace pad;
	const int fy = SCREEN_H - FOOTER_H;
	gfx::fillRect(r, {0, fy, SCREEN_W, FOOTER_H}, bg1);
	gfx::hline(r, 0, fy, SCREEN_W, line);
	// most important first: what does not fit on a narrow panel is left off the end
	const std::string a = label(East), b = label(South);
	const std::string edit = a + "+D-pad ";
	const std::string reset = b + "+" + a + " ";
	std::vector<std::string> items = {std::string(label(Select)) + "+L/R screen"};
	switch (screen_) {
	case Screen::Seq:
		items.push_back(item(East, "step"));
		items.push_back(edit + "velocity");
		items.push_back(item(North, "play"));
		items.push_back(item(West, "mute"));
		items.push_back(item(L1, R1, "bar"));
		items.push_back(item(L2, R2, "pattern"));
		break;
	case Screen::Step:
		items.push_back(edit + "value");
		items.push_back(item(L1, R1, "step"));
		items.push_back(item(L2, R2, "track"));
		items.push_back(reset + "default");
		items.push_back(item(North, "play"));
		break;
	case Screen::Sound:
		items.push_back(edit + "value");
		items.push_back(item(L1, R1, "voice"));
		items.push_back(item(L2, R2, "page"));
		items.push_back(item(North, "play"));
		items.push_back(reset + "default");
		break;
	case Screen::Patterns:
		items.push_back(item(East, "play next"));
		items.push_back(item(L1, "copy"));
		items.push_back(item(R1, "paste"));
		items.push_back(reset + "clear");
		break;
	case Screen::Project:
		items.push_back(item(East, "choose"));
		items.push_back(edit + "value");
		items.push_back(reset + "default");
		break;
	default: break;
	}
	std::string s;
	const int maxW = SCREEN_W - su(32);
	for (const std::string& it : items) {
		const std::string next = s.empty() ? it : s + "   " + it;
		if (gfx::textWidth(Font::Small, next) > maxW) break;
		s = next;
	}
	gfx::textMid(r, Font::Small, s, su(16), fy + FOOTER_H / 2, txt1);

	// a toast over the footer's top edge
	std::string msg;
	if (menuHeld_) msg = "Keep holding to quit";
	else if (!toast_.empty() && now_ - toastAt_ < kToastMs) msg = toast_;
	if (!msg.empty()) {
		const int w = gfx::textWidth(Font::Label, msg) + su(32);
		const int h = su(34);
		const SDL_Rect rc{(SCREEN_W - w) / 2, fy - h - su(10), w, h};
		gfx::fillRect(r, rc, bg2);
		gfx::frameRect(r, rc, accL);
		gfx::textMid(r, Font::Label, msg, SCREEN_W / 2, rc.y + h / 2, txt0, 0);
		if (menuHeld_) {
			const float t = std::min(1.f, float(now_ - menuSince_) / kMenuHoldMs);
			gfx::fillRect(r, {rc.x, rc.y + h - su(4), int(rc.w * t), su(4)}, accL);
		}
	}
}

void App::drawSeq(SDL_Renderer* r) {
	using namespace theme;
	const View& v = engine_.current();
	const int top = HUD_H + su(10);
	const int labelW = su(56);
	const int x0 = su(16) + labelW;
	const int gap = su(4), group = su(8);
	const int avail = SCREEN_W - x0 - su(16);
	const int cw = std::max(4, (avail - 15 * gap - 3 * group) / 16);
	const int bar = step_ / 16;

	// the strip above the grid: track, bars (dim past the track's length, ringed where
	// it plays), the track's length
	const int stripY = top + su(12);
	gfx::textMid(r, Font::LabelBold, kTrackName[track_], su(16), stripY, kTrackColor[track_]);
	const int len = v.length[track_];
	int bx = x0 + 4 * (cw + gap) + group;
	gfx::textMid(r, Font::Small, "BAR", bx - su(10), stripY, txt2, 1);
	for (int k = 0; k < 4; k++) {
		const SDL_Rect rc{bx + k * su(30), stripY - su(10), su(26), su(20)};
		const bool inLen = k * 16 < len;
		const bool playing = v.running && v.stepIndex[track_] >= 0 && v.stepIndex[track_] / 16 == k;
		gfx::fillRect(r, rc, k == bar ? accL : bg1);
		if (playing) gfx::frameRect(r, rc, txt0);
		gfx::textMid(r, Font::Small, std::to_string(k + 1), rc.x + rc.w / 2, stripY,
		             k == bar ? bg0 : inLen ? txt1 : txt2, 0);
	}
	gfx::textMid(r, Font::Small, "LEN " + std::to_string(len), SCREEN_W - su(16), stripY, txt1, 1);

	const int gy = top + su(30);
	const int rowH = std::max(su(18), std::min(su(38), (SCREEN_H - FOOTER_H - gy - su(44)) / kTracks));
	const int ch = rowH - su(6);
	for (int t = 0; t < kTracks; t++) {
		const int y = gy + t * rowH;
		const bool muted = (v.muted >> t) & 1;
		const float flash = 1.f - std::min(1.f, float(now_ - flashAt_[t]) / kFlashMs);
		const SDL_Rect lab{su(16), y, labelW - su(8), ch};
		gfx::fillRect(r, lab, mix(bg1, kTrackColor[t], muted ? 0.f : 0.25f + 0.75f * flash));
		if (t == track_) gfx::frameRect(r, lab, txt0);
		gfx::textMid(r, Font::LabelBold, kTrackTab[t], lab.x + lab.w / 2, y + ch / 2, muted ? txt2 : txt0, 0);
		if (muted) gfx::textMid(r, Font::Small, "M", lab.x + lab.w - su(4), y + su(8), warn, 1);

		for (int c = 0; c < 16; c++) {
			const int s = bar * 16 + c;
			const int x = x0 + c * (cw + gap) + (c / 4) * group;
			const SDL_Rect rc{x, y, cw, ch};
			const StepView sv = step(t, s);
			const bool inLen = s < v.length[t];
			if (!inLen) {
				gfx::frameRect(r, rc, bg1);
			} else if (sv.on) {
				const float bright = 0.35f + 0.65f * sv.velocity / 127.f;
				const SDL_Color col = scaled(kTrackColor[t], muted ? bright * 0.45f : bright);
				if (sv.probability < 100) {
					gfx::fillRect(r, rc, bg2);
					const int inset = su(5);
					gfx::fillRect(r, {x + inset, y + inset, cw - 2 * inset, ch - 2 * inset}, col);
				} else {
					gfx::fillRect(r, rc, col);
				}
				if (sv.lock1 || sv.lock2) gfx::fillRect(r, {x + cw - su(7), y + su(2), su(5), su(5)}, txt0);
				if (sv.flam) gfx::fillRect(r, {x + su(2), y + ch - su(5), cw - su(4), su(2)}, bg0);
			} else {
				gfx::fillRect(r, rc, (c % 4 == 0) ? bg2 : bg1);
			}
			if (v.running && v.stepIndex[t] == s) gfx::frameRect(r, rc, txt0);
			if (t == track_ && s == step_) {
				gfx::frameRect(r, {x - su(2), y - su(2), cw + su(4), ch + su(4)}, accL);
				gfx::frameRect(r, {x - su(3), y - su(3), cw + su(6), ch + su(6)}, accL);
			}
		}
	}

	// the step under the cursor, in a line
	const StepView sv = step(track_, step_);
	const int dy = gy + kTracks * rowH + su(14);
	std::string d = "Step " + std::to_string(step_ + 1) + "   ";
	if (!sv.on) {
		d += "off";
	} else {
		d += "vel " + std::to_string(sv.velocity) + "   prob " + std::to_string(sv.probability) + "%   " +
		     noteName(sv.note);
		if (sv.flam) d += "   flam " + text(lxr::kIdStepFlam, sv.flam);
		if (sv.shift) d += "   shift " + text(lxr::kIdStepShift, sv.shift);
		if (sv.lock1) d += "   " + text(lxr::kIdLock1Dest, sv.lock1) + " " + std::to_string(sv.value1);
		if (sv.lock2) d += "   " + text(lxr::kIdLock2Dest, sv.lock2) + " " + std::to_string(sv.value2);
	}
	gfx::textMid(r, Font::Small, d, su(16), dy, txt1);
}

void App::drawRows(SDL_Renderer* r, int x, int y, int w, const char* title, int first, int count) {
	using namespace theme;
	gfx::textMid(r, Font::Small, title, x, y, txt2);
	const int rowH = su(28);
	y += su(14);
	for (int i = 0; i < count; i++) {
		const int row = first + i;
		const int ry = y + i * rowH;
		const bool cur = (screen_ == Screen::Step ? stepRow_ : projectRow_) == row;
		if (cur) gfx::fillRect(r, {x - su(6), ry, w + su(12), rowH - su(2)}, bg2);
		std::string label, val;
		bool action = false;
		if (screen_ == Screen::Step) {
			if (row < kNumStepRows) {
				const uint32_t id = kStepRows[row];
				label = kStepLabels[row];
				const StepView sv = step(track_, step_);
				val = text(id, stepField(sv, id));
				if ((id == lxr::kIdLock1Value && !sv.lock1) || (id == lxr::kIdLock2Value && !sv.lock2)) val = "-";
			} else if (row - kNumStepRows < kNumTrackRows) {
				const int k = row - kNumStepRows;
				label = kTrackLabels[k];
				val = std::to_string(trackField(track_, kTrackRows[k]));
			} else {
				label = kTrackActions[row - kNumStepRows - kNumTrackRows];
				action = true;
				if (row - kNumStepRows - kNumTrackRows == 1 && copyTrack_ >= 0)
					val = std::string(kTrackTab[copyTrack_]) + " of P" + std::to_string(copyTrackPattern_ + 1);
			}
		} else {
			if (row < kNumProjectParams) {
				label = kProjectLabels[row];
				val = text(kProjectParams[row], value(kProjectParams[row]));
			} else {
				label = kProjectActions[row - kNumProjectParams];
				action = true;
			}
		}
		const int cy = ry + rowH / 2 - su(1);
		const int lw = gfx::textMid(r, Font::Label, label, x, cy, action ? accL : txt1);
		if (!val.empty())
			gfx::textMid(r, Font::Value, fit(Font::Value, val, w - lw - su(12)), x + w, cy, cur ? txt0 : txt1, 1);
	}
}

void App::drawStep(SDL_Renderer* r) {
	using namespace theme;
	const int top = HUD_H + su(14);
	const int colW = (SCREEN_W - su(16) * 3) / 2;
	const std::string head = std::string(kTrackName[track_]) + "  -  step " + std::to_string(step_ + 1) + " of " +
	                         std::to_string(engine_.current().length[track_]) + "  -  pattern " +
	                         std::to_string(engine_.current().pattern + 1);
	gfx::textMid(r, Font::LabelBold, head, su(16), top + su(4), kTrackColor[track_]);
	drawRows(r, su(16), top + su(30), colW, "STEP", 0, kNumStepRows);
	drawRows(r, su(16) * 2 + colW, top + su(30), colW, "TRACK", kNumStepRows, kNumTrackRows + kNumTrackActions);
}

void App::drawSound(SDL_Renderer* r) {
	using namespace theme;
	const auto& voices = pages_.voices();
	const Voice& vc = voices[static_cast<size_t>(voice_)];
	int y = HUD_H + su(8);

	// voices
	const int tabW = (SCREEN_W - su(32) - su(4) * (static_cast<int>(voices.size()) - 1)) / static_cast<int>(voices.size());
	for (size_t i = 0; i < voices.size(); i++) {
		const SDL_Rect rc{su(16) + static_cast<int>(i) * (tabW + su(4)), y, tabW, su(26)};
		const bool on = static_cast<int>(i) == voice_;
		const SDL_Color col = voices[i].track >= 0 ? kTrackColor[voices[i].track] : txt1;
		gfx::fillRect(r, rc, on ? col : bg1);
		gfx::textMid(r, Font::LabelBold, voices[i].tab, rc.x + rc.w / 2, y + su(13), on ? bg0 : txt1, 0);
	}
	y += su(34);

	// the voice's pages
	int px = su(16);
	for (size_t i = 0; i < vc.pages.size(); i++) {
		const char* title = (voice_ == kFxVoice && i == 0) ? "FX" : vc.pages[i].title;
		const int w = gfx::textWidth(Font::Small, title) + su(14);
		const bool on = static_cast<int>(i) == page_;
		if (on) gfx::fillRect(r, {px, y, w, su(22)}, bg2);
		gfx::textMid(r, Font::Small, title, px + w / 2, y + su(11), on ? txt0 : txt2, 0);
		px += w + su(4);
	}
	gfx::textMid(r, Font::Small, vc.name, SCREEN_W - su(16), y + su(11), txt2, 1);
	y += su(30);

	// eight knobs, two rows of four, as on the LXR
	const Page& page = soundPage();
	const int cellW = (SCREEN_W - su(32)) / 4;
	const int cellH = (SCREEN_H - FOOTER_H - y - su(8)) / 2;
	const float rad = std::min(cellW, cellH) * 0.27f;
	for (size_t k = 0; k < page.knobs.size() && k < 8; k++) {
		const Knob& kn = page.knobs[k];
		const lxr::ParamDef* d = def(kn.id);
		if (!d) continue;
		const int cx = su(16) + static_cast<int>(k % 4) * cellW + cellW / 2;
		const int cy0 = y + static_cast<int>(k / 4) * cellH;
		const float cy = cy0 + rad + su(10);
		const bool cur = static_cast<int>(k) == knob_;
		if (cur) gfx::fillRect(r, {cx - cellW / 2 + su(3), cy0, cellW - su(6), cellH - su(6)}, bg1);
		const double v = value(kn.id);
		const float t = d->max > d->min ? float((v - d->min) / (d->max - d->min)) : 0.f;
		const SDL_Color col = vc.track >= 0 ? kTrackColor[vc.track] : accL;
		gfx::ringGeo(r, float(cx), cy, rad, rad - suf(5), 225.f, -45.f, bg2, bg2, 48, true);
		if (t > 0.001f)
			gfx::ringGeo(r, float(cx), cy, rad, rad - suf(5), 225.f, gfx::knobAngle(t), col, col, 48, true);
		const float a = gfx::knobAngle(t) * float(M_PI) / 180.f;
		gfx::disc(r, cx + std::cos(a) * (rad - suf(2.5f)), cy - std::sin(a) * (rad - suf(2.5f)), suf(4), txt0);
		std::string val = text(kn.id, v);
		Font vf = Font::Value;
		if (gfx::textWidth(vf, val) > cellW - su(12)) vf = Font::Small;
		gfx::textMid(r, vf, val, cx, int(cy + rad + su(16)), cur ? txt0 : txt1, 0);
		gfx::textMid(r, Font::Small, kn.label, cx, int(cy + rad + su(36)), cur ? accL : txt2, 0);
	}
}

void App::drawPatterns(SDL_Renderer* r) {
	using namespace theme;
	const View& v = engine_.current();
	const int top = HUD_H + su(16);
	const int w = (SCREEN_W - su(32) - su(10) * 3) / 4;
	const int h = std::min(su(76), (SCREEN_H - FOOTER_H - top - su(40) - su(10) * 3) / 4);
	const bool blink = (now_ / 250) % 2 == 0;
	for (int p = 0; p < kPatterns; p++) {
		const int x = su(16) + (p % 4) * (w + su(10));
		const int y = top + (p / 4) * (h + su(10));
		const SDL_Rect rc{x, y, w, h};
		const bool playing = p == v.pattern;
		const bool next = v.running && p == v.pending && v.pending != v.pattern;
		const bool used = (v.used >> p) & 1;
		SDL_Color fill = bg1;
		if (playing) fill = accL;
		else if (next && blink) fill = mix(bg1, accL, 0.5f);
		gfx::fillRect(r, rc, fill);
		if (p == pattern_) {
			gfx::frameRect(r, {x - su(3), y - su(3), w + su(6), h + su(6)}, txt0);
			gfx::frameRect(r, {x - su(2), y - su(2), w + su(4), h + su(4)}, txt0);
		}
		const SDL_Color tc = playing ? bg0 : used ? txt0 : txt2;
		gfx::textMid(r, Font::Title, std::to_string(p + 1), x + su(12), y + h / 2, tc);
		std::string tag = playing ? "playing" : next ? "next" : used ? "" : "empty";
		if (p == copyPattern_) tag += tag.empty() ? "copied" : ", copied";
		gfx::textMid(r, Font::Small, tag, x + w - su(10), y + h - su(14), tc, 1);
	}
	const int ty = top + 4 * (h + su(10)) + su(8);
	gfx::textMid(r, Font::Small,
	             fit(Font::Small, "Chosen while playing, a pattern starts when the playing one ends.", SCREEN_W - su(32)),
	             su(16), ty, txt2);
}

void App::drawProject(SDL_Renderer* r) {
	using namespace theme;
	const int top = HUD_H + su(14);
	const int colW = (SCREEN_W - su(16) * 3) / 2;
	drawRows(r, su(16), top + su(10), colW, "SESSION", 0, kProjectRows);

	const int x = su(16) * 2 + colW;
	int y = top + su(10);
	gfx::textMid(r, Font::Small, "AUDIO", x, y, txt2);
	y += su(24);
	const int maxW = SCREEN_W - x - su(12);
	auto line = [&](const std::string& s, SDL_Color c) {
		gfx::textMid(r, Font::Small, fit(Font::Small, s, maxW), x, y, c);
		y += su(22);
	};
	line(info_.audio, txt1);
	char buf[96];
	if (info_.cpu < 0) std::snprintf(buf, sizeof buf, "audio thread: measuring");
	else std::snprintf(buf, sizeof buf, "audio thread %.0f%% of a core, worst period %.0f%%", info_.cpu * 100.f,
	                   info_.cpuPeak * 100.f);
	line(buf, info_.cpuPeak > 0.7f ? warn : txt1);
	if (info_.underruns >= 0) line("underruns " + std::to_string(info_.underruns), info_.underruns ? warn : txt1);
	line("MIDI: " + info_.midi, txt1);
	y += su(10);
	gfx::textMid(r, Font::Small, "SAVED", x, y, txt2);
	y += su(24);
	if (saveFailed_) line("could not save", warn);
	else if (unsaved_) line("changes not saved yet", txt1);
	else if (savedAt_) line("saved " + std::to_string((now_ - savedAt_) / 1000) + " s ago", txt1);
	else line("nothing changed", txt1);
	std::string file = info_.file;
	if (const char* home = std::getenv("HOME"); home && *home && file.rfind(home, 0) == 0)
		file = "~" + file.substr(std::strlen(home));
	line(file, txt2);
	line("Saved every 30 s, and on quit.", txt2);
	y += su(10);
	line("Hold " + std::string(pad::label(pad::Menu)) + " to quit,", txt2);
	line(std::string("or hold ") + pad::label(pad::Select) + ", then " + pad::label(pad::Start) + ".", txt2);
}

} // namespace lxrapp
