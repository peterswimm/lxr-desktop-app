// LXR as a native instrument: the ported sound engine and sequencer (generated from
// the RNBO codebox sources) with what a plugin or an app needs around them:
// parameters by stable id, notes in, the sequencer following a host's transport or its
// own clock, a step editor made of parameters, MIDI out and state.
//
// Threading: everything here runs on the audio thread (or on any one thread while the
// audio is stopped), except value(), which reads a thread-safe copy of the parameter
// values for the host.
#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "LxrEngine.hpp"
#include "LxrParams.hpp"
#include "LxrSeq.hpp"

namespace lxr {

// parameter ids: the LXR's own parameter numbers (PAR_*) for the sound parameters, and
// these for the rest
enum ParamId : uint32_t {
	kIdBpm = 1000,
	kIdSeqMode,       // 0 host transport, 1 own clock (Play), 2 off
	kIdPlay,
	kIdPattern,
	kIdShuffle,
	kIdRecord,
	kIdNoteMap,       // 0 GM drum notes on any channel, 1 channels 1-7 (+ GM on 10)
	kIdOutputs,       // 0 Aux mixed into Main as well, 1 separate
	kIdMidiOut,
	kIdMute0 = 1010,  // .. 1016
	kIdEditTrack = 1020,
	kIdEditStep,
	kIdStepOn,
	kIdStepVelocity,
	kIdStepProbability,
	kIdStepNote,
	kIdStepFlam,
	kIdStepShift,
	kIdLock1Dest,
	kIdLock1Value,
	kIdLock2Dest,
	kIdLock2Value,
	kIdTrackLength = 1040,
	kIdEuclidLength,
	kIdEuclidSteps,
	kIdEuclidRotation,
};

struct ParamDef {
	uint32_t id;
	std::string name;     // short identifier (the RNBO parameter name for sound parameters)
	std::string display;  // what a host shows
	std::string module;   // group, e.g. "Drum1" or "Sequencer/Step"
	double min, max, def;
	std::vector<std::string> enumTexts;  // empty: a number
	bool stepped;
	bool automatable;
};

struct Transport {
	bool valid = false;    // the host reports a transport
	bool playing = false;
	double tempo = 120;    // BPM
	double beats = 0;      // song position at the first sample of the next render()
};

struct MidiEvent {
	uint32_t frame;        // within the current render() call
	uint8_t data[3];
	uint8_t size;
};

class LxrInstrument {
public:
	LxrInstrument();
	~LxrInstrument();

	const std::vector<ParamDef>& params() const { return defs_; }
	int indexOf(uint32_t id) const;
	double value(uint32_t id) const;  // any thread
	std::string valueText(uint32_t id, double v) const;
	bool textValue(uint32_t id, const std::string& text, double& v) const;

	void setSampleRate(double sr);
	void reset();

	void setParam(uint32_t id, double v);
	void setTransport(const Transport& t);
	void note(int channel, int key, int velocity);  // channel 0-15; velocity 0 = note off
	void render(float* mainL, float* mainR, float* auxL, float* auxR, uint32_t frames);

	// parameters the instrument changed itself (step editor, pattern shuffle), for the
	// host: taken by the plugin after each call above
	std::vector<std::pair<uint32_t, double>> takeChanges();
	// the sequencer's MIDI out (notes and program changes) of the last render()
	const std::vector<MidiEvent>& midiOut() const { return midiOut_; }

	std::string saveState();
	bool loadState(const std::string& s);

	// for tests
	LxrEngine& engine() { return *engine_; }
	LxrSeq& seq();

private:
	struct Seq;
	friend struct Seq;

	void buildParams();
	void apply(uint32_t id, double v, bool fromHost);
	void store(uint32_t id, double v, bool notify);
	void trigger(double voice, double note, double vel);
	void lock(double voice, double d1, double v1, double d2, double v2);
	void midi(double status, double d1, double d2, double len);
	void shuffleLoaded(double v);
	void clockBlock();
	void align(double beats, double late);
	void refreshEditor(bool notify);
	void editStep(uint32_t id, double v);
	int gmTrack(int key) const;
	double lockCode(double modTarget) const;
	double lockTarget(double code) const;

	std::unique_ptr<LxrEngine> engine_;
	std::unique_ptr<Seq> seq_;
	std::vector<ParamDef> defs_;
	std::vector<int> engineIndex_;  // per param: engine attribute, or -1
	std::vector<int> idIndex_;      // param id -> index
	std::unique_ptr<std::atomic<double>[]> values_;
	std::vector<std::pair<uint32_t, double>> changes_;
	std::vector<MidiEvent> midiOut_;

	double sampleRate_ = 48000;
	int engineAuto_[7];
	int engineBpm_ = -1;
	double trigCount_[7] = {};
	double autoCount_[7] = {};
	uint32_t blockPos_ = 0;          // sample within the engine's 64-sample block
	uint32_t renderFrame_ = 0;       // sample within the current render()
	double clock_ = 0;               // sequencer time in systicks (0.25 ms)
	Transport transport_;
	double hostBeats_ = 0;           // song position where the next block starts
	double hostBeatsLast_ = 0;
	bool hostRunning_ = false;
	bool playing_ = false;
	double lastPattern_ = -1;
};

} // namespace lxr
