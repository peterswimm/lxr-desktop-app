// lxr_native: runs the generated C++ engine and sequencer from the same scripts the
// firmware references take, for rnbo/test/run.mjs --native and seqref.mjs --native.
//
//   lxr_native engine <script> <out.raw>
//       sr <rate> | set <param> <value> | in <inlet> <value> | render <blocks>
//       writes int16 frames, 4 channels: LXR output 2 L/R, output 1 L/R (like lxr_ref)
//   lxr_native seq <script>
//       the seq_ref command language (rnbo/test/ref/seq_driver.c), same output lines
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "LxrEngine.hpp"
#include "LxrSeq.hpp"

using namespace lxr;

// JavaScript's ToInt16, as the JS runner stores its frames
static int16_t toInt16(double x) {
	if (!std::isfinite(x)) return 0;
	long long v = static_cast<long long>(std::trunc(x));
	return static_cast<int16_t>(static_cast<uint16_t>(v & 0xffff));
}

static int runEngine(const char* scriptPath, const char* outPath) {
	auto* e = new LxrEngine();
	std::map<std::string, int> byName;
	for (int i = 0; i < LxrEngine::kNumParams; i++) byName[LxrEngine::params()[i].name] = i;
	std::ifstream in(scriptPath);
	std::vector<int16_t> frames;
	bool setup = false;
	std::string line;
	while (std::getline(in, line)) {
		std::istringstream ss(line);
		std::string cmd;
		if (!(ss >> cmd)) continue;
		if (cmd == "sr") {
			ss >> e->sampleRate;
			e->dspsetup();
			setup = true;
			continue;
		}
		if (!setup) {
			e->dspsetup();
			setup = true;
		}
		if (cmd == "set") {
			std::string name;
			double v;
			ss >> name >> v;
			auto it = byName.find(name);
			if (it == byName.end()) {
				fprintf(stderr, "no parameter %s\n", name.c_str());
				return 1;
			}
			*e->param(it->second) = v;
		} else if (cmd == "in") {
			int k;
			double v;
			ss >> k >> v;
			double* ins[] = {&e->in1, &e->in2, &e->in3, &e->in4, &e->in5, &e->in6, &e->in7, &e->in8};
			*ins[k - 1] = v;
		} else if (cmd == "render") {
			long blocks;
			ss >> blocks;
			for (long i = 0; i < blocks * 64; i++) {
				e->perform();
				frames.push_back(toInt16(e->out3 * 32768));
				frames.push_back(toInt16(e->out4 * 32768));
				frames.push_back(toInt16(e->out1 * 32768));
				frames.push_back(toInt16(e->out2 * 32768));
			}
		} else {
			fprintf(stderr, "unknown command %s\n", cmd.c_str());
			return 1;
		}
	}
	FILE* f = fopen(outPath, "wb");
	fwrite(frames.data(), sizeof(int16_t), frames.size(), f);
	fclose(f);
	delete e;
	return 0;
}

// the sequencer, printing what it does in seq_ref's format
struct PrintingSeq : LxrSeq {
	double nodes[14];
	PrintingSeq() {
		for (double& n : nodes) n = 255;
	}
	void emitTrigger(double v, double n, double vel) override { printf("N %.0f %.0f %.0f %.0f\n", systick, v, n, vel); }
	void emitNoteOff(double v) override { printf("F %.0f %.0f\n", systick, v); }
	void emitMidi(double st, double d1, double d2, double len) override {
		printf("M %.0f %.0f %.0f %.0f %.0f\n", systick, st, d1, d2, len);
	}
	void emitShuffle(double) override {}
	// automationNode.c, as the engine does it; seq_ref's original values are all 0
	void cc(double dest, double val) {
		if (dest > 127) printf("C %.0f 244 %.0f %.0f 0\n", systick, dest - 128, val);
		else printf("C %.0f 176 %.0f %.0f 0\n", systick, dest, val);
	}
	void emitAuto(double v, double d1, double v1, double d2, double v2) override {
		int t = static_cast<int>(v);
		double d[2] = {d1, d2}, val[2] = {v1, v2};
		for (int k = 0; k < 2; k++) {
			if (nodes[2 * t + k] != 255) cc(nodes[2 * t + k], 0);
			nodes[2 * t + k] = d[k];
		}
		for (int k = 0; k < 2; k++) {
			if (nodes[2 * t + k] != 255) cc(nodes[2 * t + k], val[k]);
		}
	}
};

static double field(double w, int sh, int bits) {
	return std::fmod(std::floor(w / std::pow(2.0, sh)), std::pow(2.0, bits));
}

static int runSeq(const char* scriptPath) {
	auto* s = new PrintingSeq();
	s->seqInit();
	s->firmwareDeltaTQuirk = 1;
	std::ifstream in(scriptPath);
	std::string line;
	while (std::getline(in, line)) {
		auto hash = line.find('#');
		if (hash != std::string::npos) line.resize(hash);
		std::istringstream ss(line);
		std::string cmd;
		if (!(ss >> cmd)) continue;
		std::vector<double> a;
		double x;
		while (ss >> x) a.push_back(x);
		auto A = [&](size_t i) { return i < a.size() ? a[i] : 0.0; };
		if (cmd == "bpm") s->setBpm(A(0));
		else if (cmd == "shuffle") s->setShuffle(static_cast<double>(static_cast<float>(A(0)) / 127.f));
		else if (cmd == "patshuffle") s->setPatShuffle(A(0), A(1));
		else if (cmd == "step") s->setStepRaw(A(0), A(1), A(2), A(3), A(4), A(5), A(6), A(7), A(8), A(9), A(10), A(11));
		else if (cmd == "main") s->setMainStep(A(0), A(1), A(2), A(3));
		else if (cmd == "len") s->setTrackLengthOf(A(0), A(1), A(2));
		else if (cmd == "run") s->runStop(A(0));
		else if (cmd == "mute") s->setMute(A(0), A(1));
		else if (cmd == "next") s->setNextPattern(A(0));
		else if (cmd == "song") {
			for (int i = 0; i < 64; i++) s->setSongEntry(i, i < static_cast<int>(a.size()) ? a[i] : 64);
		} else if (cmd == "songmode") s->setSongModeEnabled(A(0));
		else if (cmd == "rollrate") s->setRollRate(A(0));
		else if (cmd == "roll") s->setRoll(A(0), A(1));
		else if (cmd == "rec") s->setRecordingMode(A(0));
		else if (cmd == "erase") s->setErasingMode(A(0));
		else if (cmd == "track") s->activeTrack = A(0);
		else if (cmd == "shown") s->shownPattern = A(0);
		else if (cmd == "note") {
			s->voiceNoteOn(A(0), A(2), A(1));
			s->addNote(A(0), A(1), A(2));
		} else if (cmd == "autotrack") s->setActiveAutomationTrack(A(0));
		else if (cmd == "arm") s->armAutomationStep(A(0), A(1), A(2));
		else if (cmd == "autorec") s->recordAutomation(s->activeTrack, A(0), A(1));
		else if (cmd == "euklid") {
			s->euklidSetLength(A(0), A(1));
			s->euklidSetRotation(A(0), A(3), A(4));
			s->euklidSetSteps(A(0), A(2), A(4));
		} else if (cmd == "cleartrack") s->clearTrack(A(0), A(1));
		else if (cmd == "clearpattern") s->clearPattern(A(0));
		else if (cmd == "clearautom") s->clearAutomation(A(0), A(1), A(2));
		else if (cmd == "copytrack") s->copyTrack(A(0), A(1), A(2));
		else if (cmd == "copypattern") s->copyPattern(A(0), A(1));
		else if (cmd == "tracklen") s->setTrackLength(A(0), A(1));
		else if (cmd == "midichan") s->midiChan[A(0)] = A(1);
		else if (cmd == "ticks") s->seqClock(A(0));
		else if (cmd == "dump") {
			auto d = s->trackData(A(0), A(1));
			printf("D %.0f %.0f len %.0f main %.0f %.0f %.0f %.0f steps", A(0), A(1), d[0], d[1], d[2], d[3], d[4]);
			for (int i = 0; i < 64; i++) {
				double w = d[5 + i], b = d[69 + i];
				printf("%s%.0f,%.0f,%.0f,%.0f,%.0f,%.0f,%.0f,%.0f,%.0f", " ", field(w, 0, 8), field(w, 8, 8),
				       field(w, 16, 8), field(w, 24, 4), field(w, 28, 4), field(b, 0, 8), field(b, 8, 8),
				       field(b, 16, 8), field(b, 24, 8));
			}
			printf("\n");
		} else if (cmd == "state") {
			printf("S %.0f pat %.0f %.0f cnt %.0f master %.0f song %.0f steps", s->systick, s->activePattern,
			       s->pendingPattern, s->prescaleCounter, s->masterStepCnt, s->songPos);
			for (int t = 0; t < 7; t++) printf(" %.0f", s->stepIndex[t]);
			printf("\n");
		} else {
			fprintf(stderr, "unknown command %s\n", cmd.c_str());
			return 1;
		}
	}
	delete s;
	return 0;
}

int main(int argc, char** argv) {
	if (argc >= 4 && !strcmp(argv[1], "engine")) return runEngine(argv[2], argv[3]);
	if (argc >= 3 && !strcmp(argv[1], "seq")) return runSeq(argv[2]);
	fprintf(stderr, "usage: lxr_native engine <script> <out.raw> | seq <script>\n");
	return 2;
}
