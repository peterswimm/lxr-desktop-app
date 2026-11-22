// The controls, as the pad delivers them: presses through App::event, and what reaches
// the instrument. No screen and no device; the audio thread is stood in for by render().
#include <cmath>
#include <cstdio>
#include <vector>

#include "App.hpp"
#include "Engine.hpp"
#include "Session.hpp"

using namespace lxrapp;
namespace pad = sndstation::pad;

namespace {

int failures = 0;

#define CHECK(cond)                                                              \
	do {                                                                         \
		if (!(cond)) {                                                           \
			std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);          \
			failures++;                                                          \
		}                                                                        \
	} while (0)

struct Rig {
	Engine engine;
	App app{engine};
	pad::State st;
	uint32_t now = 1000;

	Rig() {
		applyAppSettings(engine.instrument());
		engine.setSampleRate(48000);
		settle();
	}
	// the audio thread's part, and a frame
	void settle() {
		std::vector<float> buf(2 * 512);
		for (int i = 0; i < 4; i++) engine.render(buf.data(), 512);
		now += 16;
		app.frame(now);
	}
	void down(pad::Button b) {
		st.bits |= pad::bit(b);
		app.event({b, true}, st);
	}
	void up(pad::Button b) {
		st.bits &= ~pad::bit(b);
		app.event({b, false}, st);
	}
	void tap(pad::Button b) {
		down(b);
		up(b);
	}
	// hold `mod`, tap `b`, let go
	void chord(pad::Button mod, pad::Button b) {
		down(mod);
		tap(b);
		up(mod);
	}
	const View& view() { return engine.current(); }
	double value(uint32_t id) { return engine.reader().value(id); }
};

void testSeq() {
	Rig r;
	// A on an empty step switches it on at once
	r.tap(pad::East);
	r.settle();
	CHECK(r.view().step[0][0].on);
	// A tapped on a step that is on switches it off
	r.tap(pad::East);
	r.settle();
	CHECK(!r.view().step[0][0].on);

	// A held + Down: the velocity, eight lower; the release leaves the step on
	r.tap(pad::Right);   // step 2
	r.down(pad::East);
	r.settle();
	r.tap(pad::Down);
	r.settle();
	r.up(pad::East);
	r.settle();
	CHECK(r.view().step[0][1].on && r.view().step[0][1].velocity == 119);
	// ... and a hold that edited a step that was on leaves it on too
	r.down(pad::East);
	r.tap(pad::Left);
	r.settle();
	r.up(pad::East);
	r.settle();
	CHECK(r.view().step[0][1].on && r.view().step[0][1].velocity == 118);

	// B + A clears the step back to the empty step
	r.down(pad::South);
	r.tap(pad::East);
	r.up(pad::South);
	r.settle();
	CHECK(!r.view().step[0][1].on && r.view().step[0][1].velocity == 127);

	// the cursor: down a track, R1 a bar on
	r.tap(pad::Down);
	r.tap(pad::R1);
	r.tap(pad::East);
	r.settle();
	CHECK(r.view().step[1][17].on);

	// Y mutes the cursor's track, X plays it
	r.tap(pad::West);
	r.settle();
	CHECK((r.view().muted & 2) != 0);
	r.tap(pad::West);
	r.settle();
	CHECK(r.view().muted == 0);
	const int before = r.view().triggers[1];
	r.tap(pad::North);
	r.settle();
	CHECK(r.view().triggers[1] == before + 1);

	// START plays and stops; R2 queues the next pattern
	r.tap(pad::Start);
	r.settle();
	CHECK(r.view().running);
	r.tap(pad::R2);
	r.settle();
	CHECK(r.view().pending == 1 && r.view().pattern == 0);
	r.tap(pad::Start);
	r.settle();
	CHECK(!r.view().running);
	CHECK(r.app.unsaved());
}

void testScreens() {
	Rig r;
	CHECK(r.app.screen() == Screen::Seq);
	r.chord(pad::Select, pad::Right);
	CHECK(r.app.screen() == Screen::Step);
	r.chord(pad::Select, pad::Left);
	r.chord(pad::Select, pad::Left);
	CHECK(r.app.screen() == Screen::Project);
}

void testStep() {
	Rig r;
	r.app.setScreen(Screen::Step);
	// Active, then down to Probability: A + Down takes it from 100 by eight
	r.tap(pad::East);
	r.tap(pad::Down);
	r.tap(pad::Down);
	r.down(pad::East);
	r.tap(pad::Down);
	r.up(pad::East);
	r.settle();
	CHECK(r.view().step[0][0].on && r.view().step[0][0].probability == 92);
	// Lock 1 (row 6): A + Right picks the first target
	for (int i = 0; i < 4; i++) r.tap(pad::Down);
	r.down(pad::East);
	r.tap(pad::Right);
	r.up(pad::East);
	r.settle();
	CHECK(r.view().step[0][0].lock1 == 1);
	// back up to Active, then Right: the track column beside it, Length; A + Down, eight off
	for (int i = 0; i < 6; i++) r.tap(pad::Up);
	r.tap(pad::Right);
	r.down(pad::East);
	r.tap(pad::Down);
	r.up(pad::East);
	r.settle();
	CHECK(r.view().length[0] == 8);
	// L2/R2 change the track, L1/R1 the step
	r.tap(pad::R2);
	r.tap(pad::R1);
	r.app.setScreen(Screen::Seq);
	r.tap(pad::East);
	r.settle();
	CHECK(r.view().step[1][1].on);
}

void testSound() {
	Rig r;
	r.app.setScreen(Screen::Sound);
	// Drum 1, OSC, Coarse (31): A + Up is eight more, A + Right one more
	r.down(pad::East);
	r.tap(pad::Up);
	r.tap(pad::Right);
	r.up(pad::East);
	r.settle();
	CHECK(r.value(8) == 40);
	// B + A: back to the default
	r.down(pad::South);
	r.tap(pad::East);
	r.up(pad::South);
	r.settle();
	CHECK(r.value(8) == 31);
	// R1 to Drum 2, R2 to its AEG page, Right to Decay
	r.tap(pad::R1);
	r.tap(pad::R2);
	r.tap(pad::Right);
	const double decay = r.value(52);   // d2_decay
	r.down(pad::East);
	r.tap(pad::Right);
	r.up(pad::East);
	r.settle();
	CHECK(r.value(52) == decay + 1);
	// Right past the page's last knob goes on to the next page: MOD, Pitch Decay
	const double pitchDecay = r.value(71);   // d2_pitch_decay
	r.tap(pad::Right);
	r.tap(pad::Right);
	r.down(pad::East);
	r.tap(pad::Right);
	r.up(pad::East);
	r.settle();
	CHECK(r.value(71) == pitchDecay + 1);
}

void testPatterns() {
	Rig r;
	demoPattern(r.engine.instrument());
	r.settle();
	r.app.setScreen(Screen::Patterns);
	// L1 copies pattern 1; Right to pattern 2; R1 twice pastes (the second confirms)
	r.tap(pad::L1);
	r.tap(pad::Right);
	r.tap(pad::R1);
	r.settle();
	CHECK((r.view().used & 2) == 0);
	r.tap(pad::R1);
	r.settle();
	CHECK((r.view().used & 2) != 0);
	// A plays it (at once, while stopped)
	r.tap(pad::East);
	r.settle();
	CHECK(r.view().pattern == 1 && r.view().step[0][0].on);
	// B + A twice clears it
	r.chord(pad::South, pad::East);
	r.settle();
	CHECK((r.view().used & 2) != 0);
	r.chord(pad::South, pad::East);
	r.settle();
	CHECK((r.view().used & 2) == 0);
}

void testProject() {
	Rig r;
	r.app.setScreen(Screen::Project);
	// Tempo: A + Up ten more
	r.down(pad::East);
	r.tap(pad::Up);
	r.up(pad::East);
	r.settle();
	CHECK(r.value(lxr::kIdBpm) == 130);
	// Record: A switches it
	r.tap(pad::Down);
	r.tap(pad::Down);
	r.tap(pad::East);
	r.settle();
	CHECK(r.value(lxr::kIdRecord) == 1);
	// Save now
	r.tap(pad::Down);
	r.tap(pad::Down);
	r.tap(pad::Down);
	r.tap(pad::East);
	CHECK(r.app.takeSaveRequest());
	// New session wants A twice
	r.tap(pad::Down);
	r.tap(pad::East);
	CHECK(!r.app.takeResetRequest());
	r.tap(pad::East);
	CHECK(r.app.takeResetRequest());
}

void testMenuHold() {
	Rig r;
	r.down(pad::Menu);
	r.settle();
	CHECK(!r.app.wantsQuit());
	r.now += 1600;
	r.app.frame(r.now);
	CHECK(r.app.wantsQuit());
}

} // namespace

int main() {
	testSeq();
	testScreens();
	testStep();
	testSound();
	testPatterns();
	testProject();
	testMenuHold();
	if (failures) {
		std::printf("%d failure(s)\n", failures);
		return 1;
	}
	std::printf("all passed\n");
	return 0;
}
