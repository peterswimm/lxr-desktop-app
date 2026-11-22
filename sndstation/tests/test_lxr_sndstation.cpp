// The app's engine bridge, without a screen or a device: commands reach the
// instrument, the view shows what the sequencer holds, saving through the audio
// thread's capture writes what saveState() writes, a session survives a round trip
// through its file, and every SOUND page names a real parameter.
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include "Engine.hpp"
#include "Pages.hpp"
#include "Session.hpp"

using namespace lxrapp;

namespace {

int failures = 0;

#define CHECK(cond)                                                              \
	do {                                                                         \
		if (!(cond)) {                                                           \
			std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);          \
			failures++;                                                          \
		}                                                                        \
	} while (0)

// audio in the device's blocks; returns the peak. Never less than 1024 frames, so a
// view (published every 8 ms, 384 frames at 48 kHz) has always been published since.
float run(Engine& e, int frames = 4800) {
	if (frames < 1024) frames = 1024;
	std::vector<float> buf(2 * 256);
	float peak = 0;
	for (int done = 0; done < frames; done += 256) {
		e.render(buf.data(), 256);
		for (float x : buf) peak = std::fmax(peak, std::fabs(x));
	}
	e.view();
	return peak;
}

void testLockfree() {
	Ring<int, 4> r;
	CHECK(r.push(1) && r.push(2) && r.push(3) && r.push(4));
	CHECK(!r.push(5));
	int x = 0;
	CHECK(r.pop(x) && x == 1);
	CHECK(r.push(5));
	for (int want : {2, 3, 4, 5}) CHECK(r.pop(x) && x == want);
	CHECK(!r.pop(x));

	TripleBuffer<int> tb;
	CHECK(!tb.read());
	tb.back() = 7;
	tb.publish();
	tb.back() = 8;
	tb.publish();
	CHECK(tb.read() && tb.front() == 8);   // the newest wins
	CHECK(!tb.read() && tb.front() == 8);  // and stays until the next
}

void testPages() {
	lxr::LxrInstrument in;
	Pages pages;
	std::string missing;
	CHECK(pages.resolve(in, &missing));
	if (!missing.empty()) std::printf("  missing: %s\n", missing.c_str());
	for (const Voice& v : pages.voices())
		for (const Page& p : v.pages) CHECK(!p.knobs.empty() && p.knobs.size() <= 8);
	for (int t = 0; t <= 4; t++) CHECK(pages.fxPage(t).knobs.size() <= 8);
	// every sound parameter is on some page
	int covered = 0;
	for (int i = 0; i < lxr::kNumSoundParams; i++) {
		bool found = false;
		for (const Voice& v : pages.voices())
			for (const Page& p : v.pages)
				for (const Knob& k : p.knobs) found = found || k.name == lxr::kSoundParams[i].name;
		for (int t = 0; t <= 4; t++)
			for (const Knob& k : pages.fxPage(t).knobs) found = found || k.name == lxr::kSoundParams[i].name;
		if (found) covered++;
		else std::printf("  not on a page: %s\n", lxr::kSoundParams[i].name);
	}
	CHECK(covered == lxr::kNumSoundParams);
}

void testSteps() {
	Engine e;
	applyAppSettings(e.instrument());
	e.setSampleRate(48000);
	run(e, 256);
	CHECK(!e.current().step[2][5].on);
	e.stepEdit(2, 5, lxr::kIdStepOn, 1);
	e.stepEdit(2, 5, lxr::kIdStepVelocity, 90);
	e.stepEdit(2, 5, lxr::kIdStepProbability, 50);
	e.stepEdit(2, 5, lxr::kIdStepShift, -3);
	e.stepEdit(2, 5, lxr::kIdStepFlam, 2);
	e.stepEdit(2, 5, lxr::kIdLock1Dest, 40);   // a target past the first voice's
	e.stepEdit(2, 5, lxr::kIdLock1Value, 99);
	run(e, 256);
	const StepView& s = e.current().step[2][5];
	CHECK(s.on && s.velocity == 90 && s.probability == 50 && s.shift == -3 && s.flam == 2);
	CHECK(s.value1 == 99 && s.lock2 == 0);
	// the view's lock target is the step editor's own reading of it
	CHECK(s.lock1 == static_cast<int>(e.reader().value(lxr::kIdLock1Dest)));
	CHECK(s.lock1 != 0);

	// Every target reads back as the step editor reads a step it is moved to. The FX
	// bus's P1-P4 are listed once per voice and stored as one code, so they read back
	// as the first of them; lockTargetOf() says which that is.
	int mismatches = 0, folded = 0;
	for (int i = 0; i < lxr::kNumModTargets; i++) {
		e.stepEdit(1, 0, lxr::kIdLock2Dest, i);
		e.stepEdit(1, 0, lxr::kIdEditStep, 1);   // select the step again: the editor re-reads it
		run(e, 256);
		const int view = e.current().step[1][0].lock2;
		if (view != static_cast<int>(e.reader().value(lxr::kIdLock2Dest)) || view != lockTargetOf(i)) {
			mismatches++;
			std::printf("  target %d: view %d, editor %g\n", i, view, e.reader().value(lxr::kIdLock2Dest));
		}
		if (lockTargetOf(i) != i) folded++;
	}
	CHECK(mismatches == 0);
	CHECK(folded == 4 * 5);   // FX P1-P4 of the five voices after the first

	e.stepEdit(4, 0, lxr::kIdTrackLength, 32);
	run(e, 256);
	CHECK(e.current().length[4] == 32);
	// the Euclid generator rewrites the track at its own length, as the LXR's does
	e.stepEdit(4, 0, lxr::kIdEuclidSteps, 5);
	run(e, 256);
	CHECK(e.current().euclid[4][1] == 5);
	CHECK(e.current().length[4] == e.current().euclid[4][0]);
	int on = 0;
	for (int st = 0; st < 16; st++) on += e.current().step[4][st].on;
	CHECK(on == 5);   // the Euclid generator wrote five steps
	CHECK((e.current().used & 1) != 0);
}

void testPlay() {
	Engine e;
	demoPattern(e.instrument());
	applyAppSettings(e.instrument());
	e.setSampleRate(48000);
	CHECK(run(e, 4800) == 0.f);   // stopped: silence
	e.param(lxr::kIdPlay, 1);
	const float peak = run(e, 48000);
	CHECK(peak > 0.05f);
	CHECK(e.current().running);
	CHECK(e.current().triggers[0] > 0);   // the kick played
	e.param(lxr::kIdPlay, 0);
	run(e, 256);
	CHECK(!e.current().running);

	// a played voice, and a MIDI note on channel 10 (GM drums: 38 is the snare)
	const int before = e.current().triggers[3];
	e.audition(3, 127);
	run(e, 512);
	CHECK(e.current().triggers[3] == before + 1);
	e.midiIn(0x99, 38, 100);
	run(e, 512);
	CHECK(e.current().triggers[3] == before + 2);

	// pattern change while stopped is at once
	e.param(lxr::kIdPattern, 3);
	run(e, 256);
	CHECK(e.current().pattern == 2);
}

// Two seconds of the demo pattern, as a fingerprint of the output's bits: printed, not
// checked, since a C library's sin() or exp() may round differently on another system.
// The same number from two compilers on one architecture says the engine's float32
// emulation survived both (no fused multiply-adds, no fast math).
void printFingerprint() {
	Engine e;
	demoPattern(e.instrument());
	applyAppSettings(e.instrument());
	e.setSampleRate(48000);
	e.param(lxr::kIdPlay, 1);
	std::vector<float> buf(2 * 256);
	uint64_t h = 1469598103934665603ull;   // FNV-1a
	for (int done = 0; done < 2 * 48000; done += 256) {
		e.render(buf.data(), 256);
		for (float x : buf) {
			uint32_t bits;
			std::memcpy(&bits, &x, sizeof bits);
			for (int b = 0; b < 4; b++) h = (h ^ ((bits >> (8 * b)) & 0xff)) * 1099511628211ull;
		}
	}
	std::printf("demo pattern, 2 s at 48 kHz: fingerprint %016llx\n", static_cast<unsigned long long>(h));
}

void testCopy() {
	Engine e;
	demoPattern(e.instrument());
	applyAppSettings(e.instrument());
	e.setSampleRate(48000);
	e.param(lxr::kIdPattern, 2);   // play pattern 2, then copy pattern 1's kick there as Drum 3
	e.post({Cmd::CopyTrack, 0, 0, 2});
	run(e, 256);
	CHECK(e.current().pattern == 1);
	CHECK(e.current().step[2][0].on && e.current().step[2][4].on && !e.current().step[2][1].on);
	e.post({Cmd::ClearTrack, 2, 1});
	run(e, 256);
	CHECK(!e.current().step[2][0].on);
	e.post({Cmd::CopyPattern, 0, 1});
	run(e, 256);
	CHECK(e.current().step[0][0].on && e.current().step[5][0].on);
	e.post({Cmd::ClearPattern, 1});
	run(e, 256);
	CHECK(!e.current().step[0][0].on);
	CHECK((e.current().used & 2) == 0);
}

void testSave() {
	Engine e;
	demoPattern(e.instrument());
	applyAppSettings(e.instrument());
	e.setSampleRate(48000);
	e.param(8, 77);               // Drum1 coarse
	e.param(lxr::kIdBpm, 133);
	e.stepEdit(3, 9, lxr::kIdStepOn, 1);
	run(e, 256);
	CHECK(e.requestSave());
	CHECK(!e.requestSave());      // one at a time
	std::string text;
	CHECK(!e.takeSave(text));     // not captured until the audio thread runs
	run(e, 256);
	CHECK(e.takeSave(text));
	CHECK(text == e.instrument().saveState());

	const std::string path = (std::filesystem::temp_directory_path() / "lxr-sndstation-test.lxr").string();
	CHECK(writeSession(path, text));
	lxr::LxrInstrument back;
	CHECK(loadSession(path, back));
	CHECK(back.value(8) == 77 && back.value(lxr::kIdBpm) == 133);
	CHECK(back.seq().isMainStepActive(3, 9, 0) == 1);
	CHECK(back.saveState() == text);
	std::filesystem::remove(path);
	lxr::LxrInstrument none;
	CHECK(!loadSession(path, none));
}

void testSwap() {
	Engine e;
	applyAppSettings(e.instrument());
	e.setSampleRate(48000);
	run(e, 256);
	CHECK(!e.current().step[0][0].on);
	e.replace(freshInstrument());
	run(e, 256);
	e.collect();
	CHECK(e.current().step[0][0].on);   // the demo pattern's kick
	CHECK(e.reader().value(lxr::kIdSeqMode) == 1);
	e.param(lxr::kIdPlay, 1);
	CHECK(run(e, 24000) > 0.05f);
}

} // namespace

int main() {
	testLockfree();
	testPages();
	testSteps();
	testPlay();
	testCopy();
	testSave();
	testSwap();
	printFingerprint();
	if (failures) {
		std::printf("%d failure(s)\n", failures);
		return 1;
	}
	std::printf("all passed\n");
	return 0;
}
