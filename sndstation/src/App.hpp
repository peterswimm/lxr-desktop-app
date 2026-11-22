// The handheld's screens and controls, on the UI thread.
//
//   SEQ       the playing pattern: 7 tracks, one bar of 16 steps at a time
//   STEP      the step under SEQ's cursor (velocity, probability, note, flam, shift,
//             parameter locks) and its track (length, Euclid, clear, copy)
//   SOUND     a voice's parameters, eight to a page, in the LXR's own pages
//   PATTERNS  the 16 patterns: play next, clear, copy
//   PROJECT   tempo, shuffle, recording, MIDI, saving, and what the audio is doing
//
// Controls, on every screen: D-pad moves, A held + D-pad changes the value (Left/Right
// by one, Up/Down by more), B + A resets or clears, SELECT + Left/Right changes screen,
// START plays and stops, X plays the track's voice, Y mutes the track, MENU held quits.
#pragma once

#include <sndstation/sndstation.h>

#include <cstdint>
#include <map>
#include <string>

#include "Engine.hpp"
#include "Pages.hpp"

namespace lxrapp {

enum class Screen { Seq, Step, Sound, Patterns, Project, Count };

struct AppInfo {
	std::string audio;       // device line for PROJECT
	std::string midi;        // MIDI tier
	std::string file;        // where the session is saved
	float cpu = -1;          // audio thread, share of one core (-1: not measured yet)
	float cpuPeak = -1;      // its worst period, as a share of the period
	int underruns = -1;      // alsa-lib's count (-1: not ALSA)
};

class App {
public:
	explicit App(Engine& engine);

	void event(const sndstation::pad::Event& e, const sndstation::pad::State& st);
	// once a frame, after the events: timers and the newest view
	void frame(uint32_t nowMs);
	void draw(SDL_Renderer* r);

	bool wantsQuit() const { return quit_; }
	// Something changed that a save would keep; cleared by saved().
	bool unsaved() const { return unsaved_; }
	void saved(bool ok);
	// PROJECT's "Save now" was chosen
	bool takeSaveRequest() {
		const bool s = saveAsked_;
		saveAsked_ = false;
		return s;
	}
	// PROJECT's "New session" was confirmed
	bool takeResetRequest() {
		const bool s = resetAsked_;
		resetAsked_ = false;
		return s;
	}
	AppInfo& info() { return info_; }
	void toast(const std::string& text);

	void setScreen(Screen s) { screen_ = s; }
	Screen screen() const { return screen_; }
	static const char* screenName(Screen s);

private:
	// values, with what was just posted standing in until the audio thread has it
	double value(uint32_t id) const;
	void setValue(uint32_t id, double v);
	void nudgeValue(uint32_t id, int delta);
	StepView step(int track, int s) const;
	void setStep(int track, int s, uint32_t id, double v);
	int stepField(const StepView& v, uint32_t id) const;
	int trackField(int track, uint32_t id) const;
	void setTrack(int track, uint32_t id, double v);

	const lxr::ParamDef* def(uint32_t id) const;
	std::string text(uint32_t id, double v) const;
	int coarse(uint32_t id) const;
	void touched() { unsaved_ = true; }

	int cursorTrack() const;
	void togglePlay();
	void toggleMute(int track);
	void audition(int track);
	bool confirm(const std::string& what, const std::string& prompt);

	void seqEvent(const sndstation::pad::Event& e, bool a, bool b);
	void stepEvent(const sndstation::pad::Event& e, bool a, bool b);
	void soundEvent(const sndstation::pad::Event& e, bool a, bool b);
	void patternsEvent(const sndstation::pad::Event& e, bool a, bool b);
	void projectEvent(const sndstation::pad::Event& e, bool a, bool b);
	void releaseA();

	void drawHeader(SDL_Renderer* r);
	void drawFooter(SDL_Renderer* r);
	void drawSeq(SDL_Renderer* r);
	void drawStep(SDL_Renderer* r);
	void drawSound(SDL_Renderer* r);
	void drawPatterns(SDL_Renderer* r);
	void drawProject(SDL_Renderer* r);
	void drawRows(SDL_Renderer* r, int x, int y, int w, const char* title, int first, int count);

	const Page& soundPage() const;

	Engine& engine_;
	Pages pages_;
	Screen screen_ = Screen::Seq;
	AppInfo info_;
	uint32_t now_ = 0;

	// SEQ / STEP cursor
	int track_ = 0, step_ = 0;
	// A held on SEQ: the step it went down on, whether that step was on, and whether
	// the D-pad edited it since (then the release does not switch it off)
	bool aHeld_ = false, aWasOn_ = false, aEdited_ = false;
	int aTrack_ = 0, aStep_ = 0;
	int stepRow_ = 0;         // STEP
	int voice_ = 0, page_ = 0, knob_ = 0;   // SOUND
	int pattern_ = 0;         // PATTERNS cursor
	int copyPattern_ = -1;    // PATTERNS copy
	int copyTrack_ = -1, copyTrackPattern_ = -1;   // STEP copy
	int projectRow_ = 0;      // PROJECT

	struct Pending {
		double v;
		uint32_t at;
	};
	std::map<uint64_t, Pending> pending_;
	int lastTrig_[kTracks] = {};
	uint32_t flashAt_[kTracks] = {};

	std::string toast_;
	uint32_t toastAt_ = 0;
	std::string confirm_;     // armed confirmation, by action
	uint32_t confirmAt_ = 0;

	bool menuHeld_ = false;
	uint32_t menuSince_ = 0;
	bool quit_ = false;
	bool unsaved_ = false;
	bool saveAsked_ = false, resetAsked_ = false;
	uint32_t savedAt_ = 0;
	bool saveFailed_ = false;
};

} // namespace lxrapp
