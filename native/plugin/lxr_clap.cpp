// The LXR as a CLAP instrument: notes in (CLAP or MIDI), the LXR's Main and Aux stereo
// outputs, every parameter, state, and optionally the sequencer's notes out.
// clap-wrapper turns the same code into a VST3 (see native/CMakeLists.txt).
#include <clap/clap.h>

#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>

#include "LxrInstrument.hpp"

#ifndef LXR_VERSION
#define LXR_VERSION "0.1.0"
#endif

namespace {

const char* const kFeatures[] = {CLAP_PLUGIN_FEATURE_INSTRUMENT, CLAP_PLUGIN_FEATURE_DRUM_MACHINE,
                                 CLAP_PLUGIN_FEATURE_SYNTHESIZER, CLAP_PLUGIN_FEATURE_STEREO, nullptr};

const clap_plugin_descriptor_t kDescriptor = {
	CLAP_VERSION_INIT,
	"io.github.peterswimm.lxr",
	"LXR",
	"LXR port",
	"https://github.com/peterswimm/lxr-desktop-app",
	"",
	"",
	LXR_VERSION,
	"The LXR V2 drum machine's sound engine and sequencer, ported from its firmware",
	kFeatures,
};

struct Plugin {
	clap_plugin_t plugin;
	const clap_host_t* host;
	const clap_host_params_t* hostParams = nullptr;
	std::unique_ptr<lxr::LxrInstrument> lxr;
	std::mutex lock;  // the instrument's state, between audio and state calls
	bool active = false;
	bool processing = false;
};

Plugin* self(const clap_plugin_t* p) { return static_cast<Plugin*>(p->plugin_data); }

// ---------------------------------------------------------------- events
void sendChanges(Plugin* pl, const clap_output_events_t* out, uint32_t time) {
	if (!out) {
		pl->lxr->takeChanges();
		return;
	}
	for (auto& c : pl->lxr->takeChanges()) {
		clap_event_param_value_t ev = {};
		ev.header.size = sizeof ev;
		ev.header.time = time;
		ev.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
		ev.header.type = CLAP_EVENT_PARAM_VALUE;
		ev.param_id = c.first;
		ev.cookie = nullptr;
		ev.note_id = -1;
		ev.port_index = -1;
		ev.channel = -1;
		ev.key = -1;
		ev.value = c.second;
		out->try_push(out, &ev.header);
	}
}

void handleEvent(Plugin* pl, const clap_event_header_t* h) {
	if (h->space_id != CLAP_CORE_EVENT_SPACE_ID) return;
	switch (h->type) {
	case CLAP_EVENT_NOTE_ON: {
		auto* e = reinterpret_cast<const clap_event_note_t*>(h);
		int vel = static_cast<int>(e->velocity * 127.0 + 0.5);
		pl->lxr->note(e->channel < 0 ? 0 : e->channel, e->key, vel < 1 ? 1 : vel);
		break;
	}
	case CLAP_EVENT_MIDI: {
		auto* e = reinterpret_cast<const clap_event_midi_t*>(h);
		if ((e->data[0] & 0xf0) == 0x90) pl->lxr->note(e->data[0] & 0x0f, e->data[1], e->data[2]);
		break;
	}
	case CLAP_EVENT_PARAM_VALUE: {
		auto* e = reinterpret_cast<const clap_event_param_value_t*>(h);
		pl->lxr->setParam(e->param_id, e->value);
		break;
	}
	default:
		break;
	}
}

// ---------------------------------------------------------------- plugin
bool init(const clap_plugin_t* p) {
	Plugin* pl = self(p);
	pl->hostParams = static_cast<const clap_host_params_t*>(pl->host->get_extension(pl->host, CLAP_EXT_PARAMS));
	return true;
}

void destroy(const clap_plugin_t* p) { delete self(p); }

bool activate(const clap_plugin_t* p, double sr, uint32_t, uint32_t) {
	Plugin* pl = self(p);
	std::lock_guard<std::mutex> g(pl->lock);
	pl->lxr->setSampleRate(sr);
	pl->active = true;
	return true;
}

void deactivate(const clap_plugin_t* p) { self(p)->active = false; }

bool startProcessing(const clap_plugin_t* p) {
	self(p)->processing = true;
	return true;
}

void stopProcessing(const clap_plugin_t* p) { self(p)->processing = false; }

void reset(const clap_plugin_t* p) {
	Plugin* pl = self(p);
	std::lock_guard<std::mutex> g(pl->lock);
	pl->lxr->reset();
}

clap_process_status process(const clap_plugin_t* p, const clap_process_t* pr) {
	Plugin* pl = self(p);
	const uint32_t frames = pr->frames_count;
	float* out[4] = {nullptr, nullptr, nullptr, nullptr};
	if (pr->audio_outputs_count > 0 && pr->audio_outputs[0].channel_count >= 2) {
		out[0] = pr->audio_outputs[0].data32[0];
		out[1] = pr->audio_outputs[0].data32[1];
	}
	if (pr->audio_outputs_count > 1 && pr->audio_outputs[1].channel_count >= 2) {
		out[2] = pr->audio_outputs[1].data32[0];
		out[3] = pr->audio_outputs[1].data32[1];
	}
	std::unique_lock<std::mutex> g(pl->lock, std::try_to_lock);
	if (!g.owns_lock() || !out[0]) {
		for (int c = 0; c < 4; c++)
			if (out[c]) memset(out[c], 0, frames * sizeof(float));
		return CLAP_PROCESS_CONTINUE;
	}

	lxr::Transport t;
	if (pr->transport) {
		const clap_event_transport_t* tr = pr->transport;
		t.valid = (tr->flags & CLAP_TRANSPORT_HAS_BEATS_TIMELINE) && (tr->flags & CLAP_TRANSPORT_HAS_TEMPO);
		t.playing = (tr->flags & CLAP_TRANSPORT_IS_PLAYING) != 0;
		t.tempo = tr->tempo;
		t.beats = static_cast<double>(tr->song_pos_beats) / CLAP_BEATTIME_FACTOR;
	}
	pl->lxr->setTransport(t);

	const clap_input_events_t* in = pr->in_events;
	const uint32_t n = in ? in->size(in) : 0;
	uint32_t ev = 0, pos = 0;
	while (pos < frames) {
		while (ev < n) {
			const clap_event_header_t* h = in->get(in, ev);
			if (h->time > pos) break;
			handleEvent(pl, h);
			ev++;
		}
		uint32_t end = frames;
		if (ev < n) {
			const clap_event_header_t* h = in->get(in, ev);
			if (h->time < end) end = h->time;
		}
		if (end <= pos) end = pos + 1;
		pl->lxr->render(out[0] + pos, out[1] + pos, out[2] ? out[2] + pos : nullptr, out[3] ? out[3] + pos : nullptr,
		                end - pos);
		if (pr->out_events) {
			for (const lxr::MidiEvent& m : pl->lxr->midiOut()) {
				clap_event_midi_t me = {};
				me.header.size = sizeof me;
				me.header.time = pos + m.frame;
				me.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
				me.header.type = CLAP_EVENT_MIDI;
				me.port_index = 0;
				me.data[0] = m.data[0];
				me.data[1] = m.size > 1 ? m.data[1] : 0;
				me.data[2] = m.size > 2 ? m.data[2] : 0;
				pr->out_events->try_push(pr->out_events, &me.header);
			}
		}
		sendChanges(pl, pr->out_events, end - 1);
		pos = end;
	}
	while (ev < n) handleEvent(pl, in->get(in, ev++));
	sendChanges(pl, pr->out_events, frames ? frames - 1 : 0);
	return CLAP_PROCESS_CONTINUE;
}

// ---------------------------------------------------------------- audio and note ports
uint32_t audioPortsCount(const clap_plugin_t*, bool isInput) { return isInput ? 0 : 2; }

bool audioPortsGet(const clap_plugin_t*, uint32_t index, bool isInput, clap_audio_port_info_t* info) {
	if (isInput || index > 1) return false;
	info->id = index;
	snprintf(info->name, sizeof info->name, "%s", index == 0 ? "Main" : "Aux");
	info->flags = index == 0 ? CLAP_AUDIO_PORT_IS_MAIN : 0;
	info->channel_count = 2;
	info->port_type = CLAP_PORT_STEREO;
	info->in_place_pair = CLAP_INVALID_ID;
	return true;
}

const clap_plugin_audio_ports_t kAudioPorts = {audioPortsCount, audioPortsGet};

uint32_t notePortsCount(const clap_plugin_t*, bool) { return 1; }

bool notePortsGet(const clap_plugin_t*, uint32_t index, bool isInput, clap_note_port_info_t* info) {
	if (index != 0) return false;
	info->id = 0;
	info->supported_dialects = isInput ? (CLAP_NOTE_DIALECT_CLAP | CLAP_NOTE_DIALECT_MIDI) : CLAP_NOTE_DIALECT_MIDI;
	info->preferred_dialect = CLAP_NOTE_DIALECT_MIDI;
	snprintf(info->name, sizeof info->name, "%s", isInput ? "Notes" : "Sequencer");
	return true;
}

const clap_plugin_note_ports_t kNotePorts = {notePortsCount, notePortsGet};

// ---------------------------------------------------------------- parameters
uint32_t paramsCount(const clap_plugin_t* p) { return static_cast<uint32_t>(self(p)->lxr->params().size()); }

bool paramsGetInfo(const clap_plugin_t* p, uint32_t index, clap_param_info_t* info) {
	const auto& defs = self(p)->lxr->params();
	if (index >= defs.size()) return false;
	const lxr::ParamDef& d = defs[index];
	memset(info, 0, sizeof *info);
	info->id = d.id;
	info->flags = CLAP_PARAM_IS_STEPPED;
	if (d.automatable) info->flags |= CLAP_PARAM_IS_AUTOMATABLE;
	if (!d.enumTexts.empty()) info->flags |= CLAP_PARAM_IS_ENUM;
	info->cookie = nullptr;
	snprintf(info->name, sizeof info->name, "%s", d.display.c_str());
	snprintf(info->module, sizeof info->module, "%s", d.module.c_str());
	info->min_value = d.min;
	info->max_value = d.max;
	info->default_value = d.def;
	return true;
}

bool paramsGetValue(const clap_plugin_t* p, clap_id id, double* value) {
	Plugin* pl = self(p);
	if (pl->lxr->indexOf(id) < 0) return false;
	*value = pl->lxr->value(id);
	return true;
}

bool paramsValueToText(const clap_plugin_t* p, clap_id id, double value, char* out, uint32_t size) {
	Plugin* pl = self(p);
	if (pl->lxr->indexOf(id) < 0 || size == 0) return false;
	snprintf(out, size, "%s", pl->lxr->valueText(id, value).c_str());
	return true;
}

bool paramsTextToValue(const clap_plugin_t* p, clap_id id, const char* text, double* value) {
	return self(p)->lxr->textValue(id, text, *value);
}

void paramsFlush(const clap_plugin_t* p, const clap_input_events_t* in, const clap_output_events_t* out) {
	Plugin* pl = self(p);
	std::lock_guard<std::mutex> g(pl->lock);
	const uint32_t n = in ? in->size(in) : 0;
	for (uint32_t i = 0; i < n; i++) handleEvent(pl, in->get(in, i));
	sendChanges(pl, out, 0);
}

const clap_plugin_params_t kParams = {paramsCount, paramsGetInfo, paramsGetValue, paramsValueToText,
                                      paramsTextToValue, paramsFlush};

// ---------------------------------------------------------------- state
bool stateSave(const clap_plugin_t* p, const clap_ostream_t* stream) {
	Plugin* pl = self(p);
	std::string s;
	{
		std::lock_guard<std::mutex> g(pl->lock);
		s = pl->lxr->saveState();
	}
	const char* data = s.data();
	size_t left = s.size();
	while (left > 0) {
		int64_t w = stream->write(stream, data, left);
		if (w <= 0) return false;
		data += w;
		left -= static_cast<size_t>(w);
	}
	return true;
}

bool stateLoad(const clap_plugin_t* p, const clap_istream_t* stream) {
	Plugin* pl = self(p);
	std::string s;
	char buf[4096];
	for (;;) {
		int64_t r = stream->read(stream, buf, sizeof buf);
		if (r < 0) return false;
		if (r == 0) break;
		s.append(buf, static_cast<size_t>(r));
	}
	bool ok;
	{
		std::lock_guard<std::mutex> g(pl->lock);
		ok = pl->lxr->loadState(s);
	}
	if (ok && pl->hostParams) pl->hostParams->rescan(pl->host, CLAP_PARAM_RESCAN_VALUES);
	return ok;
}

const clap_plugin_state_t kState = {stateSave, stateLoad};

const void* getExtension(const clap_plugin_t*, const char* id) {
	if (!strcmp(id, CLAP_EXT_AUDIO_PORTS)) return &kAudioPorts;
	if (!strcmp(id, CLAP_EXT_NOTE_PORTS)) return &kNotePorts;
	if (!strcmp(id, CLAP_EXT_PARAMS)) return &kParams;
	if (!strcmp(id, CLAP_EXT_STATE)) return &kState;
	return nullptr;
}

void onMainThread(const clap_plugin_t*) {}

const clap_plugin_t* create(const clap_host_t* host) {
	auto* pl = new Plugin();
	pl->host = host;
	pl->lxr.reset(new lxr::LxrInstrument());
	pl->plugin.desc = &kDescriptor;
	pl->plugin.plugin_data = pl;
	pl->plugin.init = init;
	pl->plugin.destroy = destroy;
	pl->plugin.activate = activate;
	pl->plugin.deactivate = deactivate;
	pl->plugin.start_processing = startProcessing;
	pl->plugin.stop_processing = stopProcessing;
	pl->plugin.reset = reset;
	pl->plugin.process = process;
	pl->plugin.get_extension = getExtension;
	pl->plugin.on_main_thread = onMainThread;
	return &pl->plugin;
}

// ---------------------------------------------------------------- factory
uint32_t factoryCount(const clap_plugin_factory_t*) { return 1; }

const clap_plugin_descriptor_t* factoryDescriptor(const clap_plugin_factory_t*, uint32_t index) {
	return index == 0 ? &kDescriptor : nullptr;
}

const clap_plugin_t* factoryCreate(const clap_plugin_factory_t*, const clap_host_t* host, const char* id) {
	if (!clap_version_is_compatible(host->clap_version) || strcmp(id, kDescriptor.id)) return nullptr;
	return create(host);
}

const clap_plugin_factory_t kFactory = {factoryCount, factoryDescriptor, factoryCreate};

} // namespace

// the three functions the CLAP entry points to (see lxr_clap_entry.cpp)
bool lxr_clap_init(const char*) { return true; }
void lxr_clap_deinit() {}
const void* lxr_clap_get_factory(const char* id) {
	return !strcmp(id, CLAP_PLUGIN_FACTORY_ID) ? &kFactory : nullptr;
}
