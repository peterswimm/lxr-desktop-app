// The LXR instrument on the audio thread, and what the other threads see of it.
//
// LxrInstrument is not thread-safe: once the audio runs, only the audio thread touches
// it. The UI thread posts commands (post(), and the helpers below) and reads a view of
// the pattern the audio thread publishes about every 8 ms (view()). Parameter values it
// reads directly: LxrInstrument::value() is safe from any thread. The MIDI thread posts
// notes through a ring of its own.
//
// Saving never formats text on the audio thread: it copies the state into a prepared
// image (captureState), and the UI thread formats it. Loading builds a whole new
// instrument on the UI thread and swaps it in; the old one comes back to be freed.
#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "LxrInstrument.hpp"
#include "Lockfree.hpp"

namespace lxrapp {

constexpr int kTracks = 7, kSteps = 64, kPatterns = 16;

// The lock target (an index into lxr::kModTargetNames) a step stores `target` as: the
// first one with the same code. The FX bus's P1-P4 are listed once per voice and stored
// as one, so "Drum2 FxP1" reads back as "Drum1 FxP1". 0 for none.
int lockTargetOf(int target);

struct StepView {
	bool on = false;
	uint8_t velocity = 127, probability = 100, note = 60, flam = 0;
	int8_t shift = 0;
	// parameter locks: index into lxr::kModTargetNames (0: none), and value
	uint16_t lock1 = 0, lock2 = 0;
	uint8_t value1 = 0, value2 = 0;
};

// What the screens draw: the playing pattern, published by the audio thread.
struct View {
	uint32_t serial = 0;
	int pattern = 0, pending = 0;      // 0-based
	bool running = false;
	int stepIndex[kTracks] = {};       // the step each track plays, -1 before its first
	int length[kTracks] = {};
	uint8_t muted = 0;                 // bit per track
	uint16_t used = 0;                 // bit per pattern that holds any step
	int triggers[kTracks] = {};        // LxrInstrument::triggers(), to flash a voice
	int euclid[kTracks][3] = {};       // the Euclid generator's length, steps, rotation
	StepView step[kTracks][kSteps];
};

struct Cmd {
	enum Kind : uint8_t {
		Param,         // id = v
		StepEdit,      // the step editor: track a, step b (0-based), then id = v
		Audition,      // play track a at velocity b (and record it, while recording)
		ClearTrack,    // track a of pattern b
		CopyTrack,     // track a of pattern b over track c of the playing pattern
		ClearPattern,  // pattern a
		CopyPattern,   // pattern a to pattern b
		Capture,       // copy the state for saving
		Swap,          // ptr: an instrument to play from now on
	} kind = Param;
	uint8_t a = 0, b = 0, c = 0;
	uint32_t id = 0;
	double v = 0;
	lxr::LxrInstrument* ptr = nullptr;
};

class Engine {
public:
	Engine();
	~Engine();

	// Only while the audio is not running (setup and tests).
	lxr::LxrInstrument& instrument() { return *in_; }
	// The parameter table and value(), safe from any thread: parameter definitions
	// never change, and value() reads atomics. Reads the instrument now playing.
	const lxr::LxrInstrument& reader() const { return *reader_.load(std::memory_order_acquire); }

	// ── UI thread ──
	bool post(const Cmd& c);
	void param(uint32_t id, double v) { post({Cmd::Param, 0, 0, 0, id, v}); }
	void stepEdit(int track, int step, uint32_t id, double v) {
		post({Cmd::StepEdit, uint8_t(track), uint8_t(step), 0, id, v});
	}
	void audition(int track, int velocity) { post({Cmd::Audition, uint8_t(track), uint8_t(velocity)}); }
	// The newest view, if one arrived since the last call; current() keeps the last one.
	bool view() { return views_.read(); }
	const View& current() const { return views_.front(); }
	// Saving: ask, then poll until the text is there. False while a request is pending.
	bool requestSave();
	bool takeSave(std::string& text);
	// Loading: an instrument made and loaded on this thread replaces the playing one.
	void replace(std::unique_ptr<lxr::LxrInstrument> next);
	// Free instruments the audio thread has let go of; call now and then.
	void collect();
	// Commands that did not fit in the ring (never, unless the audio stalls).
	uint32_t dropped() const { return dropped_.load(std::memory_order_relaxed); }

	// ── MIDI thread ──
	void midiIn(uint8_t status, uint8_t d1, uint8_t d2);

	// ── audio thread ──
	// Fill `out` (interleaved stereo) with `frames` frames.
	void render(float* out, int frames);
	// Sequencer MIDI out, for a sender on another thread (see midiOut()).
	struct MidiOut {
		uint8_t data[3];
		uint8_t size;
	};
	bool popMidiOut(MidiOut& m) { return midiOut_.pop(m); }

	// ── device paused (RateFn) or not yet started ──
	void setSampleRate(int rate);

private:
	void apply(const Cmd& c);
	void publish();

	std::unique_ptr<lxr::LxrInstrument> in_;
	std::atomic<lxr::LxrInstrument*> reader_{nullptr};
	Ring<Cmd, 1024> cmds_;
	struct MidiIn {
		uint8_t status, d1, d2;
	};
	Ring<MidiIn, 512> midiIn_;
	Ring<MidiOut, 512> midiOut_;
	Ring<lxr::LxrInstrument*, 8> retired_;
	TripleBuffer<View> views_;
	std::atomic<uint32_t> dropped_{0};

	lxr::LxrInstrument::StateImage image_;
	std::atomic<int> save_{0};  // 0 idle, 1 asked, 2 captured
	std::vector<float> scratch_;  // mainL, mainR, auxL, auxR
	int rate_ = 48000;
	int sinceView_ = 0;
	uint32_t serial_ = 0;
	const uint16_t* lockTarget_;   // lock code -> kModTargetNames index
};

} // namespace lxrapp
