// Loads the built LXR.clap the way a host does and checks what the validators don't: the
// step editor, the sequencer following the host's transport (start, loop jump, stop),
// its MIDI out, notes in, and state.
//
//   lxr_clap_test path/to/LXR.clap
#include <clap/clap.h>
#include <dlfcn.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

const double kRate = 48000;
const uint32_t kBlock = 300;  // not a multiple of the engine's 64-sample block
int failures = 0;

void check(bool ok, const char* what) {
	printf("%s %s\n", ok ? "ok  " : "FAIL", what);
	if (!ok) failures++;
}

// ---------------------------------------------------------------- host side
const void* hostExtension(const clap_host_t*, const char*) { return nullptr; }
void hostRequest(const clap_host_t*) {}

const clap_host_t kHost = {CLAP_VERSION_INIT, nullptr, "lxr test", "", "", "1", hostExtension,
                           hostRequest, hostRequest, hostRequest};

struct InEvents {
	std::vector<clap_event_param_value_t> params;
	std::vector<clap_event_note_t> notes;
	std::vector<const clap_event_header_t*> list;
	clap_input_events_t in = {this, size, get};
	static uint32_t size(const clap_input_events_t* l) {
		return static_cast<uint32_t>(static_cast<const InEvents*>(l->ctx)->list.size());
	}
	static const clap_event_header_t* get(const clap_input_events_t* l, uint32_t i) {
		return static_cast<const InEvents*>(l->ctx)->list[i];
	}
	void param(clap_id id, double v) {
		clap_event_param_value_t e = {};
		e.header = {sizeof e, 0, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_PARAM_VALUE, 0};
		e.param_id = id;
		e.note_id = e.port_index = e.channel = e.key = -1;
		e.value = v;
		params.push_back(e);
	}
	void note(int key, double velocity, uint32_t time) {
		clap_event_note_t e = {};
		e.header = {sizeof e, time, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_NOTE_ON, 0};
		e.note_id = -1;
		e.port_index = 0;
		e.channel = 0;
		e.key = static_cast<int16_t>(key);
		e.velocity = velocity;
		notes.push_back(e);
	}
	const clap_input_events_t* build() {
		list.clear();
		for (auto& e : params) list.push_back(&e.header);
		for (auto& e : notes) list.push_back(&e.header);
		return &in;
	}
	void clear() {
		params.clear();
		notes.clear();
		list.clear();
	}
};

struct OutEvents {
	struct Midi {
		uint64_t frame;
		uint8_t data[3];
	};
	std::vector<Midi> midi;
	std::vector<std::pair<clap_id, double>> params;
	uint64_t base = 0;
	uint32_t lastTime = 0;
	bool ordered = true;
	clap_output_events_t out = {this, push};
	static bool push(const clap_output_events_t* l, const clap_event_header_t* h) {
		auto* o = static_cast<OutEvents*>(l->ctx);
		if (h->time < o->lastTime) o->ordered = false;
		o->lastTime = h->time;
		if (h->type == CLAP_EVENT_MIDI) {
			auto* m = reinterpret_cast<const clap_event_midi_t*>(h);
			o->midi.push_back({o->base + h->time, {m->data[0], m->data[1], m->data[2]}});
		} else if (h->type == CLAP_EVENT_PARAM_VALUE) {
			auto* p = reinterpret_cast<const clap_event_param_value_t*>(h);
			o->params.push_back({p->param_id, p->value});
		}
		return true;
	}
};

struct Stream {
	std::string data;
	size_t pos = 0;
	clap_ostream_t out = {this, write};
	clap_istream_t in = {this, read};
	static int64_t write(const clap_ostream_t* s, const void* buf, uint64_t size) {
		uint64_t n = size > 1000 ? 1000 : size;  // short writes, as hosts may do
		static_cast<Stream*>(s->ctx)->data.append(static_cast<const char*>(buf), n);
		return static_cast<int64_t>(n);
	}
	static int64_t read(const clap_istream_t* s, void* buf, uint64_t size) {
		auto* st = static_cast<Stream*>(s->ctx);
		uint64_t n = std::min<uint64_t>(size, st->data.size() - st->pos);
		if (n > 777) n = 777;
		memcpy(buf, st->data.data() + st->pos, n);
		st->pos += n;
		return static_cast<int64_t>(n);
	}
};

// ---------------------------------------------------------------- one plugin instance
struct Instance {
	const clap_plugin_t* p = nullptr;
	const clap_plugin_params_t* params = nullptr;
	const clap_plugin_state_t* state = nullptr;
	InEvents in;
	OutEvents out;
	uint64_t frame = 0;
	float buf[4][kBlock];
	double energy = 0;
	bool finite = true;

	explicit Instance(const clap_plugin_factory_t* f) {
		p = f->create_plugin(f, &kHost, "io.github.peterswimm.lxr");
		p->init(p);
		params = static_cast<const clap_plugin_params_t*>(p->get_extension(p, CLAP_EXT_PARAMS));
		state = static_cast<const clap_plugin_state_t*>(p->get_extension(p, CLAP_EXT_STATE));
		p->activate(p, kRate, 1, 4096);
		p->start_processing(p);
	}
	~Instance() {
		p->stop_processing(p);
		p->deactivate(p);
		p->destroy(p);
	}
	double value(clap_id id) {
		double v = -1;
		params->get_value(p, id, &v);
		return v;
	}
	// one block; transport: tempo < 0 for none, else playing from beats
	void process(double tempo, bool playing, double beats) {
		clap_event_transport_t tr = {};
		tr.header = {sizeof tr, 0, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_TRANSPORT, 0};
		tr.flags = CLAP_TRANSPORT_HAS_TEMPO | CLAP_TRANSPORT_HAS_BEATS_TIMELINE | (playing ? CLAP_TRANSPORT_IS_PLAYING : 0);
		tr.tempo = tempo;
		tr.song_pos_beats = static_cast<clap_beattime>(std::llround(beats * CLAP_BEATTIME_FACTOR));
		float* chans[2][2] = {{buf[0], buf[1]}, {buf[2], buf[3]}};
		clap_audio_buffer_t outs[2] = {};
		for (int k = 0; k < 2; k++) {
			outs[k].data32 = chans[k];
			outs[k].channel_count = 2;
		}
		clap_process_t pr = {};
		pr.steady_time = static_cast<int64_t>(frame);
		pr.frames_count = kBlock;
		pr.transport = tempo < 0 ? nullptr : &tr;
		pr.audio_outputs = outs;
		pr.audio_outputs_count = 2;
		pr.in_events = in.build();
		pr.out_events = &out.out;
		out.base = frame;
		out.lastTime = 0;
		p->process(p, &pr);
		in.clear();
		for (int c = 0; c < 4; c++)
			for (uint32_t i = 0; i < kBlock; i++) {
				if (!std::isfinite(buf[c][i])) finite = false;
				energy += static_cast<double>(buf[c][i]) * buf[c][i];
			}
		frame += kBlock;
	}
	std::vector<uint64_t> noteOns(uint64_t from = 0) {
		std::vector<uint64_t> r;
		for (auto& m : out.midi)
			if ((m.data[0] & 0xf0) == 0x90 && m.data[2] > 0 && m.frame >= from) r.push_back(m.frame);
		return r;
	}
};

const double kFramesPerBeat = kRate * 60 / 120;

// within one 64-sample engine block, the resolution of the sequencer's clock
bool near(uint64_t frame, double expected) { return std::fabs(static_cast<double>(frame) - expected) <= 64; }

} // namespace

int main(int argc, char** argv) {
	if (argc < 2) {
		fprintf(stderr, "usage: %s LXR.clap\n", argv[0]);
		return 2;
	}
	void* lib = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
	if (!lib) {
		fprintf(stderr, "%s\n", dlerror());
		return 2;
	}
	auto* entry = static_cast<const clap_plugin_entry_t*>(dlsym(lib, "clap_entry"));
	check(entry && entry->init(argv[1]), "entry");
	auto* factory = static_cast<const clap_plugin_factory_t*>(entry->get_factory(CLAP_PLUGIN_FACTORY_ID));
	check(factory && factory->get_plugin_count(factory) == 1, "factory");

	// parameter ids (see native/core/LxrInstrument.hpp)
	const clap_id kBpm = 1000, kSeqMode = 1001, kPlay = 1002, kMidiOut = 1008, kEditTrack = 1020, kEditStep = 1021, kStepOn = 1022, kStepVelocity = 1023;
	{
		Instance a(factory);
		check(a.params->count(a.p) > 250, "parameters");

		// kick on steps 1, 5, 9 and 13 (each beat), the 5th one quieter
		a.in.param(kMidiOut, 1);
		a.in.param(kEditTrack, 0);
		for (int s : {1, 5, 9, 13}) {
			a.in.param(kEditStep, s);
			a.in.param(kStepOn, 1);
		}
		a.in.param(kStepVelocity, 90);
		a.process(-1, false, 0);
		check(a.value(kStepOn) == 1 && a.value(kStepVelocity) == 90, "step editor");
		check(a.noteOns().empty(), "silent without a transport");

		// the host plays from beat 0 at 120 BPM for 4 beats
		uint64_t start = a.frame;
		double beats = 0;
		while (a.frame < start + 4 * kFramesPerBeat) {
			a.process(120, true, beats);
			beats += kBlock / kFramesPerBeat;
		}
		auto on = a.noteOns(start);
		while (!on.empty() && on.back() - start > 3.9 * kFramesPerBeat) on.pop_back();  // the next bar's
		bool timed = on.size() == 4;
		for (size_t k = 0; timed && k < 4; k++) timed = near(on[k] - start, k * kFramesPerBeat);
		check(timed, "a kick on each beat, in time with the host");
		for (auto f : on) printf("     note on at beat %.3f\n", (f - start) / kFramesPerBeat);
		check(a.energy > 1 && a.finite, "audio");
		bool vel = false;
		for (auto& m : a.out.midi)
			if ((m.data[0] & 0xf0) == 0x90 && m.data[2] == 90) vel = true;
		check(vel, "step velocity in the MIDI out");
		check(a.out.ordered, "output events in order");

		// a loop: from beat 3.5 back to 0
		uint64_t loop = a.frame;
		beats = 0;
		for (int k = 0; k < 4; k++) {
			a.process(120, true, beats);
			beats += kBlock / kFramesPerBeat;
		}
		on = a.noteOns(loop);
		check(on.size() == 1 && near(on[0] - loop, 0), "a host loop restarts the pattern");

		// stopped: no more steps; notes still play the voices
		uint64_t stop = a.frame;
		for (int k = 0; k < 200; k++) a.process(120, false, beats);
		check(a.noteOns(stop).empty(), "the host stops the sequencer");
		a.energy = 0;
		for (int k = 0; k < 100; k++) a.process(120, false, beats);
		double quiet = a.energy;
		a.in.note(38, 1.0, 10);
		for (int k = 0; k < 10; k++) a.process(120, false, beats);
		check(a.energy > quiet + 1, "a note plays the snare");

		// state into a second instance, and back out the same
		Stream st;
		check(a.state->save(a.p, &st.out), "state save");
		Instance b(factory);
		check(b.state->load(b.p, &st.in), "state load");
		Stream st2;
		b.state->save(b.p, &st2.out);
		check(st.data == st2.data, "state round trip");
		check(b.value(kEditStep) == 13 && b.value(kStepOn) == 1 && b.value(kStepVelocity) == 90, "editor restored");
		b.in.param(kEditStep, 5);
		b.process(-1, false, 0);
		check(b.value(kStepOn) == 1 && b.value(kStepVelocity) == 127, "steps restored");
		bool told = false;
		for (auto& c : b.out.params)
			if (c.first == kStepVelocity && c.second == 127) told = true;
		check(told, "the host hears about the editor's new values");

		// its own clock, without a transport
		b.in.param(kBpm, 120);
		b.in.param(kSeqMode, 1);
		b.in.param(kPlay, 1);
		uint64_t own = b.frame;
		while (b.frame < own + 3.5 * kFramesPerBeat) b.process(-1, false, 0);
		on = b.noteOns(own);
		timed = on.size() == 4;
		for (size_t k = 0; timed && k < 4; k++) timed = near(on[k] - own, k * kFramesPerBeat);
		check(timed, "own clock: a kick on each beat at 120 BPM");
		b.in.param(kPlay, 0);
		b.process(-1, false, 0);
		uint64_t stopped = b.frame;
		for (int k = 0; k < 200; k++) b.process(-1, false, 0);
		check(b.noteOns(stopped).empty(), "own clock: stop");
	}
	entry->deinit();
	dlclose(lib);
	printf(failures ? "%d failed\n" : "all passed\n", failures);
	return failures ? 1 : 0;
}
