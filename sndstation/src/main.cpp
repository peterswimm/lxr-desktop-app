// LXR on sndstation: the LXR V2 drum machine's sound engine and sequencer (the same
// port as the CLAP/VST3 plugins, native/core) as a handheld app.
//
//   lxr-sndstation                 run it (in a sndstation session, the launcher
//                                  starts it, and it returns there on quit)
//   lxr-sndstation --session FILE  play and save FILE instead of ~/.config/lxr/session.lxr
//   lxr-sndstation --new           start from the init kit and the demo pattern
//   lxr-sndstation --res WxH       force the window size (development)
//   lxr-sndstation --shots DIR     headless: draw every screen to DIR/lxr-<screen>.bmp, exit
//   lxr-sndstation --rt-...        the audio thread's scheduling (sndstation docs/rt-bench.md)
#include <sndstation/midi.h>
#include <sndstation/sndstation.h>

#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "App.hpp"
#include "Engine.hpp"
#include "Fonts.hpp"
#include "Session.hpp"

using namespace sndstation;
using namespace lxrapp;

namespace {

constexpr uint32_t kAutosaveMs = 30000;

void render(void* user, float* out, int frames) { static_cast<Engine*>(user)->render(out, frames); }
void rateMoved(void* user, int rate) { static_cast<Engine*>(user)->setSampleRate(rate); }

// MIDI in, on the SDK's MIDI thread: notes to the engine. MIDI 1.0 channel voice
// messages arrive as UMP type 2; MIDI 2.0 ones (type 4) carry a 16-bit velocity.
struct MidiSink : midi::Sink {
	Engine* engine;
	explicit MidiSink(Engine* e) : engine(e) {}
	void received(const midi::Port&, const uint32_t* ump, size_t words, uint64_t) override {
		const uint32_t w = ump[0];
		const uint32_t type = w >> 28;
		const uint8_t status = (w >> 16) & 0xff;
		const uint8_t d1 = (w >> 8) & 0x7f;
		if (type == 2) {
			engine->midiIn(status, d1, w & 0x7f);
		} else if (type == 4 && words >= 2 && (status & 0xf0) == 0x90) {
			const uint32_t vel16 = ump[1] >> 16;
			uint8_t vel = static_cast<uint8_t>(vel16 >> 9);
			if (vel16 != 0 && vel == 0) vel = 1;   // MIDI 2.0's quietest note is still a note
			engine->midiIn(status, d1, vel);
		}
	}
};

// The sequencer's MIDI out, when MIDI out is on: sent every millisecond from a thread
// of its own, so the notes are not held to the screen's frame.
class MidiOut {
public:
	explicit MidiOut(Engine& e) : engine_(e) {}
	~MidiOut() { stop(); }
	void follow(bool on) {
		if (on && !thread_.joinable()) {
			ports_ = midi::outputs();
			for (const midi::Port& p : ports_) midi::open(p);
			run_ = true;
			thread_ = std::thread([this] { loop(); });
		} else if (!on && thread_.joinable()) {
			stop();
		}
	}

private:
	void stop() {
		run_ = false;
		if (thread_.joinable()) thread_.join();
		for (const midi::Port& p : ports_) midi::close(p);
		ports_.clear();
	}
	void loop() {
		while (run_) {
			Engine::MidiOut m;
			while (engine_.popMidiOut(m)) {
				const uint32_t ump = 0x20000000u | (uint32_t(m.data[0]) << 16) |
				                     (m.size > 1 ? uint32_t(m.data[1]) << 8 : 0) | (m.size > 2 ? m.data[2] : 0);
				for (const midi::Port& p : ports_) midi::send(p, &ump, 1);
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
	}
	Engine& engine_;
	std::vector<midi::Port> ports_;
	std::atomic<bool> run_{false};
	std::thread thread_;
};

std::string audioLine(const AudioOut& out) {
	if (!out.deviceOpen()) return "no audio device";
	std::string s = out.cardId().empty() ? out.driverName() : out.cardId();
	return s + ", " + std::to_string(out.sampleRate()) + " Hz, " + std::to_string(out.deviceFrames()) + " frames";
}

// Headless: every screen, with the sequencer a little way into pattern 1.
int shots(Engine& engine, App& app, Window& win, const std::string& dir) {
	engine.setSampleRate(48000);
	engine.param(lxr::kIdPlay, 1);
	std::vector<float> buf(2 * 512);
	for (int i = 0; i < 48000 * 3 / 4 / 512; i++) engine.render(buf.data(), 512);
	app.info().audio = "no audio device (headless)";
	app.info().midi = "not started";
	bool ok = true;
	for (int s = 0; s < static_cast<int>(Screen::Count); s++) {
		app.setScreen(static_cast<Screen>(s));
		app.frame(1000);
		app.draw(win.renderer);
		std::string name = App::screenName(static_cast<Screen>(s));
		for (char& c : name) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
		const std::string path = dir + "/lxr-" + name + ".bmp";
		const bool saved = win.saveBMP(path);
		std::printf("%s %s\n", saved ? "shot:" : "could not write", path.c_str());
		ok = ok && saved;
	}
	return ok ? 0 : 1;
}

} // namespace

int main(int argc, char** argv) {
	appcfg::setAppName("lxr");
	WindowOptions wo;
	wo.title = "LXR";
	std::string shotsDir, session;
	bool fresh = false;
	for (int i = 1; i < argc; ++i) {
		if (rt::parseArg(argc, argv, i)) continue;
		if (!std::strcmp(argv[i], "--shots") && i + 1 < argc) shotsDir = argv[++i];
		else if (!std::strcmp(argv[i], "--session") && i + 1 < argc) session = argv[++i];
		else if (!std::strcmp(argv[i], "--new")) fresh = true;
		else if (!std::strcmp(argv[i], "--res") && i + 1 < argc) std::sscanf(argv[++i], "%dx%d", &wo.width, &wo.height);
		else {
			std::fprintf(stderr, "usage: %s [--session FILE] [--new] [--res WxH] [--shots DIR] [--rt-...]\n", argv[0]);
			return 2;
		}
	}
	wo.headless = !shotsDir.empty();
	if (session.empty()) session = appcfg::userPath("session.lxr");

	// Open Sans, before the window loads its fonts; never the platform's face instead
	std::string fontDir, fontWhy;
	if (!useOpenSans(&fontDir, &fontWhy)) {
		std::fprintf(stderr, "lxr: %s\n", fontWhy.c_str());
		return 1;
	}
	std::printf("lxr: Open Sans from %s\n", fontDir.c_str());

	Window win;
	if (!win.open(wo)) return 1;
	pad::setAutoRepeat(pad::DPAD_BITS);

	// The instrument, from the session file (not in a headless run, which draws the
	// demo), before the audio starts: nothing else touches it yet.
	Engine engine;
	std::string why;
	if (wo.headless || fresh || !loadSession(session, engine.instrument(), &why)) {
		if (!wo.headless && !fresh) std::printf("lxr: %s (%s): starting from the init kit\n", session.c_str(), why.c_str());
		demoPattern(engine.instrument());
		applyAppSettings(engine.instrument());
	} else {
		std::printf("lxr: playing %s\n", session.c_str());
	}

	App app(engine);
	app.info().file = session;
	if (wo.headless) return shots(engine, app, win, shotsDir);

	AudioOut out;
	const AudioOut::Settings set = AudioOut::loadSettings();
	rt::applySettings(set.realtimePriority, set.cpu);
	out.open(set, &render, &rateMoved, &engine, true);
	engine.setSampleRate(out.sampleRate());
	out.start();
	app.info().audio = audioLine(out);

	MidiSink sink(&engine);
	midi::listen(&sink);
	app.info().midi = midi::tierName(midi::tier());
	MidiOut midiOut(engine);

	pad::State st;
	uint32_t lastSave = SDL_GetTicks(), lastInfo = 0;
	bool saving = false;
	for (bool running = true; running;) {
		SDL_Event e;
		while (SDL_PollEvent(&e)) {
			if (e.type == SDL_QUIT) running = false;
			pad::Event ev[8];
			const int n = pad::translate(e, st, ev);
			for (int i = 0; i < n; ++i) app.event(ev[i], st);
		}
		const uint32_t now = SDL_GetTicks();
		app.frame(now);
		if (app.wantsQuit()) running = false;

		// saving: the audio thread copies the state, this thread writes it
		if (!saving && (app.takeSaveRequest() || (app.unsaved() && now - lastSave >= kAutosaveMs)))
			saving = engine.requestSave();
		std::string text;
		if (saving && engine.takeSave(text)) {
			saving = false;
			lastSave = now;
			std::string err;
			const bool ok = writeSession(session, text, &err);
			app.saved(ok);
			if (!ok) std::fprintf(stderr, "lxr: could not save %s: %s\n", session.c_str(), err.c_str());
		}
		if (app.takeResetRequest()) {
			engine.replace(freshInstrument());
			app.toast("New session: the init kit");
		}

		midiOut.follow(engine.reader().value(lxr::kIdMidiOut) != 0);
		if (out.service()) app.info().audio = audioLine(out);
		if (now - lastInfo >= 500) {
			lastInfo = now;
			const AudioOut::CpuLoad cpu = out.cpuLoad();
			app.info().cpu = cpu.avg;
			app.info().cpuPeak = cpu.peak;
			app.info().underruns = out.health().underruns;
		}
		app.draw(win.renderer);
		win.present();
		SDL_Delay(16);
	}

	midi::listen(nullptr);
	midiOut.follow(false);
	// The SDK's MIDI thread (ALSA's, on a unit) runs until stop(): left running, it is
	// still joinable when the program ends, and that ends it in std::terminate.
	midi::stop();
	out.close();
	// the audio is closed: the instrument is this thread's again
	engine.collect();
	std::string err;
	if (writeSession(session, engine.instrument().saveState(), &err)) std::printf("lxr: saved %s\n", session.c_str());
	else std::fprintf(stderr, "lxr: could not save %s: %s\n", session.c_str(), err.c_str());
	return 0;   // in a sndstation session, the launcher comes back
}
