#include "LxrInstrument.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>

namespace lxr {

namespace {

const char* const kVoiceModule[] = {"Drum1", "Drum2", "Drum3", "Snare", "Cymbal", "Hihat"};
const char* const kTrackNames[] = {"Drum1", "Drum2", "Drum3", "Snare", "Cymbal", "Closed Hihat", "Open Hihat"};
const char* const kFlamNames[] = {"Off", "x2", "x3", "x4", "x6", "x8"};

std::string moduleOf(const std::string& name) {
	static const char* const prefixes[] = {"d1_", "d2_", "d3_", "sn_", "cy_", "hh_"};
	for (int k = 0; k < 6; k++)
		if (name.rfind(prefixes[k], 0) == 0) return kVoiceModule[k];
	if (name.rfind("fx_", 0) == 0) return "FX";
	return "Global";
}

// the firmware's float precision for the tick length (seq_calcDeltaT)
double tickLength(double bpm) {
	float t = static_cast<float>(std::trunc(60000.0 / bpm));
	t = t / 96.f;
	t = t * 4.f;
	return t;
}

} // namespace

struct LxrInstrument::Seq : LxrSeq {
	LxrInstrument* in;
	explicit Seq(LxrInstrument* i) : in(i) {}
	void emitTrigger(double v, double n, double vel) override { in->trigger(v, n, vel); }
	void emitAuto(double v, double d1, double v1, double d2, double v2) override { in->lock(v, d1, v1, d2, v2); }
	void emitNoteOff(double) override {}
	void emitMidi(double st, double d1, double d2, double len) override { in->midi(st, d1, d2, len); }
	void emitShuffle(double v) override { in->shuffleLoaded(v); }
};

LxrSeq& LxrInstrument::seq() { return *seq_; }

LxrInstrument::LxrInstrument() : engine_(new LxrEngine()), seq_(new Seq(this)) {
	buildParams();
	for (size_t i = 0; i < defs_.size(); i++) {
		if (defs_[i].id >= idIndex_.size()) idIndex_.resize(defs_[i].id + 1, -1);
		idIndex_[defs_[i].id] = static_cast<int>(i);
	}
	values_.reset(new std::atomic<double>[defs_.size()]);
	for (int k = 0; k < 7; k++) engineAuto_[k] = -1;
	for (int i = 0; i < LxrEngine::kNumParams; i++) {
		std::string n = LxrEngine::params()[i].name;
		if (n == "bpm") engineBpm_ = i;
		for (int k = 0; k < 7; k++)
			if (n == std::string("auto_") + kTracks[k]) engineAuto_[k] = i;
	}
	seq_->seqInit();
	engine_->sampleRate = sampleRate_;
	engine_->dspsetup();
	for (size_t i = 0; i < defs_.size(); i++) values_[i].store(defs_[i].def, std::memory_order_relaxed);
	// the step editor shows the pattern rather than setting it: its defaults are what the
	// empty pattern holds
	for (size_t i = 0; i < defs_.size(); i++)
		if (defs_[i].id < kIdStepOn || defs_[i].id > kIdEuclidRotation) apply(defs_[i].id, defs_[i].def, false);
	refreshEditor(false);
	for (size_t i = 0; i < defs_.size(); i++)
		if (defs_[i].id >= kIdStepOn && defs_[i].id <= kIdEuclidRotation)
			defs_[i].def = values_[i].load(std::memory_order_relaxed);
	changes_.clear();
}

LxrInstrument::~LxrInstrument() = default;

void LxrInstrument::buildParams() {
	defs_.clear();
	for (int i = 0; i < kNumSoundParams; i++) {
		const SoundParam& p = kSoundParams[i];
		ParamDef d;
		d.id = static_cast<uint32_t>(p.par);
		d.name = p.name;
		d.display = p.display;
		d.module = moduleOf(p.name);
		d.min = p.min;
		d.max = p.max;
		d.def = p.def;
		if (p.enumIndex >= 0)
			for (int k = 0; k <= p.max; k++) d.enumTexts.push_back(kEnums[p.enumIndex][k]);
		d.stepped = true;
		d.automatable = true;
		defs_.push_back(d);
		int e = -1;
		for (int k = 0; k < LxrEngine::kNumParams; k++)
			if (!strcmp(LxrEngine::params()[k].name, p.name)) e = k;
		engineIndex_.push_back(e);
	}
	auto add = [&](uint32_t id, const char* name, const char* display, const char* module, double lo, double hi,
	               double def, std::vector<std::string> texts = {}, bool automatable = true) {
		defs_.push_back({id, name, display, module, lo, hi, def, std::move(texts), true, automatable});
		engineIndex_.push_back(-1);
	};
	add(kIdBpm, "bpm", "Tempo (own clock)", "Sequencer", 20, 300, 120);
	add(kIdSeqMode, "seq_mode", "Sequencer Clock", "Sequencer", 0, 2, 0, {"Host transport", "Own clock (Play)", "Off"});
	add(kIdPlay, "play", "Play (own clock)", "Sequencer", 0, 1, 0, {"Stop", "Play"});
	add(kIdPattern, "pattern", "Pattern", "Sequencer", 1, 16, 1);
	add(kIdShuffle, "shuffle", "Shuffle", "Sequencer", 0, 127, 0);
	add(kIdRecord, "record", "Record", "Sequencer", 0, 1, 0, {"Off", "On"});
	add(kIdNoteMap, "note_map", "Note Input", "Global", 0, 1, 0, {"GM drum notes", "Channels 1-7, GM on 10"});
	add(kIdOutputs, "outputs", "Outputs", "Global", 0, 1, 0, {"Aux also in Main", "Main and Aux separate"});
	add(kIdMidiOut, "midi_out", "Sequencer MIDI Out", "Sequencer", 0, 1, 0, {"Off", "On"});
	for (int t = 0; t < 7; t++) {
		std::string n = std::string("mute_") + kTracks[t];
		std::string disp = std::string("Mute ") + kTrackNames[t];
		add(kIdMute0 + t, n.c_str(), disp.c_str(), "Sequencer", 0, 1, 0, {"Off", "Muted"});
	}
	std::vector<std::string> tracks(kTrackNames, kTrackNames + 7);
	std::vector<std::string> targets;
	for (int i = 0; i < kNumModTargets; i++) targets.push_back(kModTargetNames[i]);
	add(kIdEditTrack, "edit_track", "Edit Track", "Sequencer/Step", 0, 6, 0, tracks, false);
	add(kIdEditStep, "edit_step", "Edit Step", "Sequencer/Step", 1, 64, 1, {}, false);
	add(kIdStepOn, "step_on", "Step On", "Sequencer/Step", 0, 1, 0, {"Off", "On"}, false);
	add(kIdStepVelocity, "step_velocity", "Step Velocity", "Sequencer/Step", 0, 127, 127, {}, false);
	add(kIdStepProbability, "step_probability", "Step Probability", "Sequencer/Step", 0, 100, 100, {}, false);
	add(kIdStepNote, "step_note", "Step Note", "Sequencer/Step", 0, 127, 60, {}, false);
	add(kIdStepFlam, "step_flam", "Step Flam", "Sequencer/Step", 0, 5, 0, std::vector<std::string>(kFlamNames, kFlamNames + 6), false);
	add(kIdStepShift, "step_shift", "Step Shift", "Sequencer/Step", -7, 7, 0, {}, false);
	add(kIdLock1Dest, "lock1_dest", "Step Lock 1 Target", "Sequencer/Step", 0, kNumModTargets - 1, 0, targets, false);
	add(kIdLock1Value, "lock1_value", "Step Lock 1 Value", "Sequencer/Step", 0, 127, 0, {}, false);
	add(kIdLock2Dest, "lock2_dest", "Step Lock 2 Target", "Sequencer/Step", 0, kNumModTargets - 1, 0, targets, false);
	add(kIdLock2Value, "lock2_value", "Step Lock 2 Value", "Sequencer/Step", 0, 127, 0, {}, false);
	add(kIdTrackLength, "track_length", "Track Length", "Sequencer/Track", 1, 64, 16, {}, false);
	add(kIdEuclidLength, "euclid_length", "Euclid Length", "Sequencer/Track", 1, 16, 16, {}, false);
	add(kIdEuclidSteps, "euclid_steps", "Euclid Steps", "Sequencer/Track", 1, 16, 4, {}, false);
	add(kIdEuclidRotation, "euclid_rotation", "Euclid Rotation", "Sequencer/Track", 0, 15, 0, {}, false);
}

int LxrInstrument::indexOf(uint32_t id) const {
	return id < idIndex_.size() ? idIndex_[id] : -1;
}

double LxrInstrument::value(uint32_t id) const {
	int i = indexOf(id);
	return i < 0 ? 0 : values_[i].load(std::memory_order_relaxed);
}

std::string LxrInstrument::valueText(uint32_t id, double v) const {
	int i = indexOf(id);
	if (i < 0) return "";
	const ParamDef& d = defs_[i];
	long k = std::lround(v);
	if (!d.enumTexts.empty()) {
		long j = k - static_cast<long>(d.min);
		if (j >= 0 && j < static_cast<long>(d.enumTexts.size())) return d.enumTexts[j];
	}
	if (id == kIdStepShift && k > 0) return "+" + std::to_string(k);
	return std::to_string(k);
}

bool LxrInstrument::textValue(uint32_t id, const std::string& text, double& v) const {
	int i = indexOf(id);
	if (i < 0) return false;
	const ParamDef& d = defs_[i];
	for (size_t k = 0; k < d.enumTexts.size(); k++) {
		if (d.enumTexts[k] == text) {
			v = d.min + static_cast<double>(k);
			return true;
		}
	}
	char* end = nullptr;
	double x = strtod(text.c_str(), &end);
	if (end == text.c_str()) return false;
	v = std::fmin(d.max, std::fmax(d.min, std::round(x)));
	return true;
}

void LxrInstrument::setSampleRate(double sr) {
	sampleRate_ = sr;
	engine_->sampleRate = sr;
	engine_->dspsetup();
	blockPos_ = 0;
}

void LxrInstrument::reset() {
	seq_->runStop(0);
	hostRunning_ = false;
	playing_ = false;
	if (value(kIdSeqMode) == 1 && value(kIdPlay) != 0) apply(kIdPlay, 1, false);
}

void LxrInstrument::store(uint32_t id, double v, bool notify) {
	int i = indexOf(id);
	if (i < 0) return;
	values_[i].store(v, std::memory_order_relaxed);
	if (notify) changes_.push_back({id, v});
}

std::vector<std::pair<uint32_t, double>> LxrInstrument::takeChanges() {
	std::vector<std::pair<uint32_t, double>> c;
	c.swap(changes_);
	return c;
}

void LxrInstrument::setParam(uint32_t id, double v) {
	int i = indexOf(id);
	if (i < 0) return;
	const ParamDef& d = defs_[i];
	double x = std::fmin(d.max, std::fmax(d.min, std::round(v)));
	values_[i].store(x, std::memory_order_relaxed);
	apply(id, x, true);
}

void LxrInstrument::apply(uint32_t id, double v, bool fromHost) {
	int i = indexOf(id);
	if (i >= 0 && engineIndex_[i] >= 0) {
		*engine_->param(engineIndex_[i]) = v;
		return;
	}
	LxrSeq& s = *seq_;
	switch (id) {
	case kIdBpm:
		if (value(kIdSeqMode) != 0) {
			s.setBpm(v);
			*engine_->param(engineBpm_) = v;
		}
		break;
	case kIdSeqMode:
		s.runStop(0);
		hostRunning_ = false;
		playing_ = false;
		if (v == 1 && value(kIdPlay) != 0) apply(kIdPlay, 1, false);
		if (v != 0) {
			s.setBpm(value(kIdBpm));
			*engine_->param(engineBpm_) = value(kIdBpm);
		}
		break;
	case kIdPlay:
		if (value(kIdSeqMode) == 1) {
			if (v != 0 && !playing_) {
				s.runStop(0);
				s.runStop(1);
			} else if (v == 0 && playing_) {
				s.runStop(0);
			}
			playing_ = v != 0;
		}
		break;
	case kIdPattern:
		s.setNextPattern(v - 1);
		if (s.running == 0) refreshEditor(fromHost);
		break;
	case kIdShuffle:
		s.setShuffle(static_cast<float>(v) / 127.f);
		s.setPatShuffle(s.activePattern, v);
		break;
	case kIdRecord:
		s.setRecordingMode(v);
		break;
	default:
		if (id >= kIdMute0 && id < kIdMute0 + 7) s.setMute(id - kIdMute0, v);
		else if (id >= kIdEditTrack && id <= kIdEuclidRotation) editStep(id, v);
		break;
	}
}

// ---------------------------------------------------------------- step editor
double LxrInstrument::lockCode(double target) const {
	int i = static_cast<int>(target);
	if (i <= 0 || i >= kNumModTargets) return 255;
	return seq_->autoCode(kModTargetPar[i]);
}

double LxrInstrument::lockTarget(double code) const {
	if (code == 255) return 0;
	for (int i = 1; i < kNumModTargets; i++)
		if (lockCode(i) == code) return i;
	return 0;
}

void LxrInstrument::refreshEditor(bool notify) {
	LxrSeq& s = *seq_;
	double p = s.activePattern;
	double t = value(kIdEditTrack);
	double st = value(kIdEditStep) - 1;
	store(kIdStepOn, s.isMainStepActive(t, st, p), notify);
	store(kIdStepVelocity, s.stepField(p, t, st, 0), notify);
	store(kIdStepProbability, s.stepField(p, t, st, 1), notify);
	store(kIdStepNote, s.stepField(p, t, st, 2), notify);
	store(kIdStepFlam, s.stepField(p, t, st, 3), notify);
	store(kIdStepShift, s.stepField(p, t, st, 4) - 7, notify);
	store(kIdLock1Dest, lockTarget(s.stepField(p, t, st, 5)), notify);
	store(kIdLock1Value, s.stepField(p, t, st, 6), notify);
	store(kIdLock2Dest, lockTarget(s.stepField(p, t, st, 7)), notify);
	store(kIdLock2Value, s.stepField(p, t, st, 8), notify);
	store(kIdTrackLength, s.trackLen(p, t), notify);
	store(kIdEuclidLength, s.euLen[t], notify);
	store(kIdEuclidSteps, s.euSteps[t], notify);
	store(kIdEuclidRotation, s.euRot[t], notify);
	lastPattern_ = p;
}

void LxrInstrument::editStep(uint32_t id, double v) {
	LxrSeq& s = *seq_;
	double p = s.activePattern;
	double t = value(kIdEditTrack);
	double st = value(kIdEditStep) - 1;
	switch (id) {
	case kIdEditTrack:
	case kIdEditStep:
		refreshEditor(true);
		return;
	case kIdStepOn: s.setMainStep(p, t, st, v); break;
	case kIdStepVelocity: s.setStepField(p, t, st, 0, v); break;
	case kIdStepProbability: s.setStepField(p, t, st, 1, v); break;
	case kIdStepNote: s.setStepField(p, t, st, 2, v); break;
	case kIdStepFlam: s.setStepField(p, t, st, 3, v); break;
	case kIdStepShift: s.setStepField(p, t, st, 4, v + 7); break;
	case kIdLock1Dest: s.setStepField(p, t, st, 5, lockCode(v)); break;
	case kIdLock1Value: s.setStepField(p, t, st, 6, v); break;
	case kIdLock2Dest: s.setStepField(p, t, st, 7, lockCode(v)); break;
	case kIdLock2Value: s.setStepField(p, t, st, 8, v); break;
	case kIdTrackLength: s.setTrackLengthOf(p, t, v); break;
	// the LXR's Euclid page: a new length regenerates with the current number of steps
	case kIdEuclidLength:
		s.euklidSetLength(t, v);
		s.euklidSetSteps(t, s.euSteps[t], p);
		refreshEditor(true);
		break;
	case kIdEuclidSteps:
		s.euklidSetSteps(t, v, p);
		refreshEditor(true);
		break;
	case kIdEuclidRotation:
		s.euklidSetRotation(t, v, p);
		refreshEditor(true);
		break;
	default: break;
	}
}

// ---------------------------------------------------------------- sequencer outputs
void LxrInstrument::trigger(double voice, double note, double vel) {
	int t = static_cast<int>(voice);
	if (t < 0 || t > 6) return;
	double c = trigCount_[t] + 1;
	if (c > 999) c = 1;
	trigCount_[t] = c;
	double code = c * 16384 + note * 128 + vel;
	double* ins[] = {&engine_->in2, &engine_->in3, &engine_->in4, &engine_->in5, &engine_->in6, &engine_->in7, &engine_->in8};
	*ins[t] = code;
}

void LxrInstrument::lock(double voice, double d1, double v1, double d2, double v2) {
	int t = static_cast<int>(voice);
	if (t < 0 || t > 6 || engineAuto_[t] < 0) return;
	double c = autoCount_[t] + 1;
	if (c > 1000000) c = 1;
	autoCount_[t] = c;
	*engine_->param(engineAuto_[t]) = c * 4294967296.0 + d1 + 256 * v1 + 65536 * d2 + 16777216 * v2;
}

void LxrInstrument::midi(double status, double d1, double d2, double len) {
	if (value(kIdMidiOut) == 0) return;
	int st = static_cast<int>(status);
	int kind = st & 0xf0;
	if (kind != 0x80 && kind != 0x90 && kind != 0xc0) return;
	// a sequencer step sends its note on twice, from the voice and from the step
	// (sequencer.c seq_triggerVoice): keep the step's, which has the MIDI note override
	if (kind == 0x90 && !midiOut_.empty()) {
		const MidiEvent& last = midiOut_.back();
		if (last.data[0] == st && last.frame == renderFrame_) midiOut_.pop_back();
	}
	MidiEvent e;
	e.frame = renderFrame_;
	e.data[0] = static_cast<uint8_t>(st);
	e.data[1] = static_cast<uint8_t>(d1);
	e.data[2] = static_cast<uint8_t>(d2);
	e.size = static_cast<uint8_t>(1 + len);
	midiOut_.push_back(e);
}

void LxrInstrument::shuffleLoaded(double v) { store(kIdShuffle, v, true); }

// ---------------------------------------------------------------- notes
int LxrInstrument::gmTrack(int note) const {
	if (note == 35 || note == 36) return 0;
	if (note == 41 || note == 43 || note == 45) return 1;
	if (note == 47 || note == 48 || note == 50) return 2;
	if (note >= 37 && note <= 40) return 3;
	if (note == 49 || note == 51 || note == 52 || note == 55 || note == 57 || note == 59) return 4;
	if (note == 42 || note == 44) return 5;
	if (note == 46) return 6;
	return -1;
}

void LxrInstrument::note(int channel, int key, int velocity) {
	if (velocity <= 0) return;
	int t = -1;
	double pitch = LxrSeq::SEQ_DEFAULT_NOTE;
	if (value(kIdNoteMap) == 1 && channel >= 0 && channel < 7) {
		t = channel;
		pitch = key;
	} else if (value(kIdNoteMap) == 0 || channel == 9) {
		t = gmTrack(key);
	}
	if (t < 0 || seq_->bit(seq_->mutedTracks, t) != 0) return;
	// MidiParser.c: play, then record
	seq_->voiceNoteOn(t, pitch, velocity);
	seq_->addNote(t, velocity, pitch);
}

// ---------------------------------------------------------------- clock
void LxrInstrument::setTransport(const Transport& t) {
	transport_ = t;
	// the song position where the next 64-sample block starts
	hostBeats_ = t.beats + (static_cast<double>((64 - blockPos_) & 63) / sampleRate_) * t.tempo / 60.0;
}

// The firmware starts a step on the 96 ppq grid of its own clock. Following a host, the
// sequencer's time is the host's song position: one 96 ppq tick per 1/96 beat, so it
// stays on the host's grid; a start (or a jump) lines the next step up with the host's
// next 16th. A 16th the host passed during the last block (late) is played at once: a
// start or a loop lands within a block, and the sequencer only sees it at the next one.
void LxrInstrument::align(double beats, double late) {
	LxrSeq& s = *seq_;
	s.runStop(0);
	s.runStop(1);
	double ticks = beats * 96;
	double tick = tickLength(s.seqTempo);
	double target = std::floor(ticks / 24 + 1e-9) * 24;
	if (ticks - target > late * 96 + 1e-9) target += 24;
	double n = target / 24;
	if (n > 0) {
		for (int t = 0; t < 7; t++) {
			double len = s.trackLen(s.activePattern, t);
			if (len == 0) len = 16;
			s.stepIndex[t] = s.imod(n - 1, len);
		}
		s.masterStepCnt = s.imod(n, 256);
	}
	if (target >= ticks) {
		s.lastTick = s.systick;
		s.deltaT = (target - ticks) * tick;
	} else {
		// the step is due now; its clock started on the grid, so the next ticks stay there
		s.lastTick = s.systick - (ticks - target) * tick;
		s.deltaT = 0;
	}
}

void LxrInstrument::clockBlock() {
	LxrSeq& s = *seq_;
	double mode = value(kIdSeqMode);
	double advance = 64.0 * 4000.0 / sampleRate_;
	if (mode == 0) {
		bool play = transport_.valid && transport_.playing;
		double tempo = std::fmin(300, std::fmax(20, transport_.tempo));
		double bpm = std::round(tempo);
		if (play) {
			s.setBpm(bpm);
			*engine_->param(engineBpm_) = bpm;
			double beatsPerBlock = 64.0 / sampleRate_ * tempo / 60.0;
			if (!hostRunning_) {
				align(hostBeats_, beatsPerBlock);
				hostRunning_ = true;
			} else {
				// a loop or a jump: line up again
				double expected = hostBeatsLast_ + beatsPerBlock;
				if (std::fabs(hostBeats_ - expected) > 2.0 / 96.0) align(hostBeats_, beatsPerBlock);
			}
			advance = beatsPerBlock * 96 * tickLength(bpm);
			hostBeatsLast_ = hostBeats_;
			hostBeats_ += beatsPerBlock;
		} else if (hostRunning_) {
			s.runStop(0);
			hostRunning_ = false;
		}
	}
	clock_ += advance;
	double n = std::floor(clock_) - s.systick;
	if (n > 0) s.seqClock(n);
	if (s.activePattern != lastPattern_) {
		store(kIdPattern, s.activePattern + 1, true);
		refreshEditor(true);
	}
}

void LxrInstrument::render(float* mainL, float* mainR, float* auxL, float* auxR, uint32_t frames) {
	bool separate = value(kIdOutputs) != 0;
	midiOut_.clear();
	LxrEngine& e = *engine_;
	for (uint32_t i = 0; i < frames; i++) {
		renderFrame_ = i;
		if (blockPos_ == 0) clockBlock();
		e.perform();
		float m0 = static_cast<float>(e.out1), m1 = static_cast<float>(e.out2);
		float a0 = static_cast<float>(e.out3), a1 = static_cast<float>(e.out4);
		if (separate) {
			mainL[i] = m0;
			mainR[i] = m1;
		} else {
			mainL[i] = m0 + a0;
			mainR[i] = m1 + a1;
		}
		if (auxL) auxL[i] = a0;
		if (auxR) auxR[i] = a1;
		blockPos_ = (blockPos_ + 1) & 63;
	}
}

// ---------------------------------------------------------------- state
static void putStore(std::ostringstream& o, const char* tag, const cbx::ListStore& st) {
	for (size_t k = 0; k < st.slots.size(); k++) {
		o << tag << ' ' << k << ' ' << st.slots[k].size();
		char buf[32];
		for (double x : st.slots[k]) {
			snprintf(buf, sizeof buf, " %.17g", x);
			o << buf;
		}
		o << '\n';
	}
}

std::string LxrInstrument::saveState() {
	std::ostringstream o;
	o << "LXR 1\n";
	char buf[64];
	for (size_t i = 0; i < defs_.size(); i++) {
		if (!defs_[i].automatable && defs_[i].id != kIdEditTrack && defs_[i].id != kIdEditStep) continue;
		snprintf(buf, sizeof buf, "p %u %.17g\n", defs_[i].id, values_[i].load(std::memory_order_relaxed));
		o << buf;
	}
	putStore(o, "pat", seq_->PAT);
	putStore(o, "set", seq_->SETS);
	for (int t = 0; t < 7; t++) o << "eu " << t << ' ' << seq_->euLen[t] << ' ' << seq_->euSteps[t] << ' ' << seq_->euRot[t] << '\n';
	return o.str();
}

bool LxrInstrument::loadState(const std::string& text) {
	std::istringstream in(text);
	std::string line;
	if (!std::getline(in, line) || line.rfind("LXR ", 0) != 0) return false;
	LxrSeq& s = *seq_;
	s.runStop(0);
	hostRunning_ = false;
	playing_ = false;
	std::vector<std::pair<uint32_t, double>> params;
	while (std::getline(in, line)) {
		std::istringstream l(line);
		std::string tag;
		l >> tag;
		if (tag == "p") {
			uint32_t id;
			double v;
			l >> id >> v;
			params.push_back({id, v});
		} else if (tag == "pat" || tag == "set") {
			size_t slot, n;
			l >> slot >> n;
			cbx::List list;
			for (size_t k = 0; k < n; k++) {
				double x;
				l >> x;
				list.push(x);
			}
			(tag == "pat" ? s.PAT : s.SETS).store(static_cast<double>(slot), list);
		} else if (tag == "eu") {
			int t;
			double a, b, c;
			l >> t >> a >> b >> c;
			if (t >= 0 && t < 7) {
				s.euLen[t] = a;
				s.euSteps[t] = b;
				s.euRot[t] = c;
			}
		}
	}
	// the pattern first: selecting it loads its shuffle from the settings just restored
	// (the saved Shuffle is that same value, so it isn't applied again); the clock last
	for (auto& p : params)
		if (p.first == kIdPattern) setParam(p.first, p.second);
	for (auto& p : params)
		if (p.first != kIdSeqMode && p.first != kIdPlay && p.first != kIdPattern && p.first != kIdShuffle)
			setParam(p.first, p.second);
	for (auto& p : params)
		if (p.first == kIdSeqMode || p.first == kIdPlay) setParam(p.first, p.second);
	refreshEditor(false);
	changes_.clear();
	return true;
}

} // namespace lxr
