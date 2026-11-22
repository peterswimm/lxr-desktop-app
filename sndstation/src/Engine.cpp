#include "Engine.hpp"

#include <algorithm>
#include <array>
#include <cmath>

namespace lxrapp {

namespace {

constexpr int kScratch = 1024;   // frames rendered per LxrInstrument::render() call
constexpr int kSlot = 133;       // LxrSeq::SLOT: a track's words

inline uint32_t word(double w) { return static_cast<uint32_t>(static_cast<uint64_t>(w)); }

// One track of a pattern from the sequencer's store, as the screens want it. A slot
// shorter than a track is one the sequencer has never written: the empty track.
void decodeTrack(const std::vector<double>& d, StepView* steps, int& length, const uint16_t* lockTarget) {
	if (static_cast<int>(d.size()) < kSlot) {
		length = 16;
		for (int s = 0; s < kSteps; s++) steps[s] = StepView{};
		return;
	}
	length = static_cast<int>(d[0]);
	for (int s = 0; s < kSteps; s++) {
		StepView& v = steps[s];
		const uint32_t bits = word(d[1 + s / 16]);
		const uint32_t a = word(d[5 + s]), b = word(d[69 + s]);
		v.on = (bits >> (s % 16)) & 1;
		v.velocity = a & 0x7f;
		v.probability = (a >> 8) & 0xff;
		v.note = (a >> 16) & 0xff;
		v.flam = (a >> 24) & 0xf;
		v.shift = static_cast<int8_t>(((a >> 28) & 0xf) - 7);
		v.lock1 = lockTarget[b & 0xff];
		v.value1 = (b >> 8) & 0xff;
		v.lock2 = lockTarget[(b >> 16) & 0xff];
		v.value2 = (b >> 24) & 0xff;
	}
}

// LxrInstrument::lockCode(): what a step stores for a target
int lockCode(int target) {
	if (target <= 0 || target >= lxr::kNumModTargets) return 255;
	const int par = lxr::kModTargetPar[target];
	return (par < 128 && par != 0) ? par + 1 : par;   // LxrSeq::autoCode
}

// LxrInstrument::lockTarget(), as a table: the first target with each code
const uint16_t* lockTable() {
	static const std::array<uint16_t, 256> table = [] {
		std::array<uint16_t, 256> t{};
		for (int i = lxr::kNumModTargets - 1; i >= 1; i--)
			if (lockCode(i) < 255) t[static_cast<size_t>(lockCode(i))] = static_cast<uint16_t>(i);
		return t;
	}();
	return table.data();
}

bool hasSteps(const std::vector<double>& d) {
	return static_cast<int>(d.size()) >= kSlot && d[1] + d[2] + d[3] + d[4] != 0;
}

} // namespace

int lockTargetOf(int target) {
	const int code = lockCode(target);
	return code < 255 ? lockTable()[code] : 0;
}

Engine::Engine() : in_(new lxr::LxrInstrument()) {
	reader_.store(in_.get(), std::memory_order_release);
	in_->prepareState(image_);
	scratch_.assign(4 * kScratch, 0.f);
	lockTarget_ = lockTable();
	publish();
	views_.read();
}

Engine::~Engine() { collect(); }

bool Engine::post(const Cmd& c) {
	if (cmds_.push(c)) return true;
	dropped_.fetch_add(1, std::memory_order_relaxed);
	return false;
}

bool Engine::requestSave() {
	int idle = 0;
	if (!save_.compare_exchange_strong(idle, 1)) return false;
	if (!post({Cmd::Capture})) {
		save_.store(0);
		return false;
	}
	return true;
}

bool Engine::takeSave(std::string& text) {
	if (save_.load(std::memory_order_acquire) != 2) return false;
	text = lxr::LxrInstrument::formatState(image_);
	save_.store(0, std::memory_order_release);
	return true;
}

void Engine::replace(std::unique_ptr<lxr::LxrInstrument> next) {
	next->setSampleRate(rate_);
	Cmd c{Cmd::Swap};
	c.ptr = next.get();
	if (post(c)) next.release();
}

void Engine::collect() {
	lxr::LxrInstrument* old = nullptr;
	while (retired_.pop(old)) delete old;
}

void Engine::midiIn(uint8_t status, uint8_t d1, uint8_t d2) {
	midiIn_.push({status, d1, d2});
}

void Engine::setSampleRate(int rate) {
	rate_ = rate;
	in_->setSampleRate(rate);
}

void Engine::apply(const Cmd& c) {
	lxr::LxrInstrument& in = *in_;
	lxr::LxrSeq& s = in.seq();
	switch (c.kind) {
	case Cmd::Param:
		in.setParam(c.id, c.v);
		break;
	case Cmd::StepEdit:
		in.setParam(lxr::kIdEditTrack, c.a);
		in.setParam(lxr::kIdEditStep, c.b + 1);
		in.setParam(c.id, c.v);
		break;
	case Cmd::Audition:
		// as a pad on the LXR: play it, and record it while recording (MidiParser.c)
		s.voiceNoteOn(c.a, lxr::LxrSeq::SEQ_DEFAULT_NOTE, c.b);
		if (s.bit(s.mutedTracks, c.a) == 0) s.addNote(c.a, c.b, lxr::LxrSeq::SEQ_DEFAULT_NOTE);
		break;
	case Cmd::ClearTrack:
		s.clearTrack(c.a, c.b);
		break;
	case Cmd::CopyTrack:
		s.storeTrack(s.activePattern, c.c, s.trackData(c.b, c.a));
		break;
	case Cmd::ClearPattern:
		s.clearPattern(c.a);
		break;
	case Cmd::CopyPattern:
		s.copyPattern(c.a, c.b);
		break;
	case Cmd::Capture:
		in.captureState(image_);
		save_.store(2, std::memory_order_release);
		break;
	case Cmd::Swap:
		if (retired_.push(in_.get())) {
			in_.release();
			in_.reset(c.ptr);
			reader_.store(in_.get(), std::memory_order_release);
		}
		// else: nowhere to hand the old one back (eight swaps waiting to be collected),
		// so keep playing it; the new one is lost rather than freed on this thread
		break;
	}
}

void Engine::render(float* out, int frames) {
	Cmd c;
	while (cmds_.pop(c)) apply(c);
	MidiIn m;
	while (midiIn_.pop(m)) {
		const int kind = m.status & 0xf0;
		if (kind == 0x90) in_->note(m.status & 0x0f, m.d1, m.d2);
	}
	in_->dropChanges();

	float* mainL = scratch_.data();
	float* mainR = mainL + kScratch;
	float* auxL = mainR + kScratch;
	float* auxR = auxL + kScratch;
	for (int done = 0; done < frames;) {
		const int n = std::min(kScratch, frames - done);
		in_->render(mainL, mainR, auxL, auxR, static_cast<uint32_t>(n));
		for (const lxr::MidiEvent& e : in_->midiOut()) midiOut_.push({{e.data[0], e.data[1], e.data[2]}, e.size});
		float* o = out + 2 * done;
		for (int i = 0; i < n; i++) {
			o[2 * i] = mainL[i];
			o[2 * i + 1] = mainR[i];
		}
		done += n;
	}
	in_->dropChanges();

	sinceView_ += frames;
	if (sinceView_ * 125 >= rate_) {   // about every 8 ms
		sinceView_ = 0;
		publish();
	}
}

void Engine::publish() {
	lxr::LxrInstrument& in = *in_;
	lxr::LxrSeq& s = in.seq();
	View& v = views_.back();
	v.serial = ++serial_;
	v.pattern = static_cast<int>(s.activePattern);
	v.pending = static_cast<int>(s.pendingPattern);
	v.running = s.running != 0;
	v.muted = 0;
	for (int t = 0; t < kTracks; t++) {
		v.stepIndex[t] = static_cast<int>(s.stepIndex[t]);
		if (s.bit(s.mutedTracks, t) != 0) v.muted |= uint8_t(1u << t);
		v.triggers[t] = in.triggers(t);
		v.euclid[t][0] = static_cast<int>(s.euLen[t]);
		v.euclid[t][1] = static_cast<int>(s.euSteps[t]);
		v.euclid[t][2] = static_cast<int>(s.euRot[t]);
		decodeTrack(s.PAT.slots[static_cast<size_t>(v.pattern * kTracks + t)], v.step[t], v.length[t], lockTarget_);
	}
	v.used = 0;
	for (int p = 0; p < kPatterns; p++)
		for (int t = 0; t < kTracks; t++)
			if (hasSteps(s.PAT.slots[static_cast<size_t>(p * kTracks + t)])) {
				v.used |= uint16_t(1u << p);
				break;
			}
	views_.publish();
}

} // namespace lxrapp
