// LXR sequencer for RNBO (message codebox)
//
// A port of the LXR V2 sequencer: Core/Src/Soundengine/Sequencer/sequencer.c and
// EuklidGenerator.c, with the parts of MidiVoiceControl.c it relies on. 7 tracks of up
// to 64 steps (4 bars of 16), each step with velocity, probability, note, flam,
// micro-timing shift and two parameter automation lanes; per-track length, shuffle,
// roll, live recording, mutes, pattern switching at the end of the pattern, song
// (chain) mode, the Euclid generator and MIDI out.
//
// Timing is the firmware's: a 96 ppq clock (24 ticks per 16th) computed in the
// firmware's units (0.25 ms "systicks", float arithmetic in single precision), with
// seq_tick() called CALLS_PER_SYSTICK times per systick as the audio interrupt does.
// seqClock(n) advances n systicks; the patch calls it from a [metro 1] with n = 4.
// test/seqref.mjs checks every trigger, automation and MIDI event against the firmware
// code compiled for the host.
//
// Pattern data lives in liststores with "preset": true, so RNBO presets save it.
// Slot p * 7 + t holds track t of pattern p:
//   [length, main step bits of bars 1-4 (16 bit words), 64 step words A, 64 step words B]
//   A = volume (bit 7: active flag, unused) + 256 * probability + 65536 * note
//       + 2^24 * flam + 2^28 * shift
//   B = automation 1 destination + 256 * value 1 + 65536 * destination 2 + 2^24 * value 2
// An empty slot reads as a cleared track. SETS slot 0 is the song, slot 1 the shuffle
// of each pattern.
//
// The code that includes this file provides the outputs:
//   emitTrigger(voice, note, velocity)       play a voice
//   emitAuto(voice, dest1, val1, dest2, val2)  a sequenced trigger's automation
//   emitNoteOff(voice)                       voiceControl_noteOff (no sound)
//   emitMidi(status, data1, data2, length)   MIDI out
//   emitShuffle(value)                       a pattern change loaded this shuffle
//
// Differences from the firmware: 16 patterns instead of 64; edits to the playing
// pattern are kept when patterns change (the firmware discards unsaved edits); the
// Euclid generator stores length 16 as 16 (the firmware stores 0, which the step code
// then divides by); the 8 s stall described at firmwareDeltaTQuirk is fixed; steps
// that would be read or written at index -1 (before the first step after a start)
// are not; no external clock sync, trigger outputs or kit-per-pattern loading.

const NUM_TRACKS = 7;
const NUM_PATTERNS = 16;
const NUM_STEPS = 64;
const SLOT = 133;
const MAX_SONG_LENGTH = 64;
const SONG_END = 64;
const SONG_LOOP = 65;
const SEQ_DEFAULT_NOTE = 60;
const ROLL_VOLUME = 100;
const NO_AUTOMATION = 255;
const CALLS_PER_SYSTICK = 9;
const MIDI_NOTE_OFF = 128;
const MIDI_NOTE_ON = 144;
const MIDI_PROG_CHANGE = 192;
const MIDI_CLOCK = 248;
const MIDI_START = 250;
const MIDI_STOP = 252;

@state PAT = new liststore({"maxlistsize": 133, "slotcount": 112, "preset": true});
@state SETS = new liststore({"maxlistsize": 64, "slotcount": 2, "preset": true});

@state stepIndex = new FixedDoubleArray(7);   // seq_stepIndex (int8)
@state rndPrev = new FixedDoubleArray(7);     // seq_rndValue
@state rndCur = new FixedDoubleArray(7);
@state rndNext = new FixedDoubleArray(7);
@state lastWasNoteOn = new FixedDoubleArray(7);
@state midiChan = new FixedDoubleArray(8);    // midi_MidiChannels, 0-based; [7] global
@state midiNoteOverride = new FixedDoubleArray(7);
@state midiChanNotes = new FixedDoubleArray(16);
@state euLen = new FixedDoubleArray(7);
@state euSteps = new FixedDoubleArray(7);
@state euRot = new FixedDoubleArray(7);
@state euBuf = new FixedDoubleArray(64);
@state prescaleCounter = 0;
@state masterStepCnt = 0;
@state rollRate = 1;
@state rollState = 0;
@state tempo = 120;
@state lastTick = 0;
@state isStepShuffled = 0;
@state deltaT = 0;
@state activeAutomTrack = 0;
@state shuffle = 0;
@state lastShuffle = 0;
@state armedStep = -1;
@state armedTrack = -1;
@state mutedTracks = 0;
@state songModeEnabled = 0;
@state running = 0;
@state activePattern = 0;
@state pendingPattern = 0;
@state recordActive = 0;
@state eraseActive = 0;
@state loadPending = 0;
@state chokeCurrent = 0;
@state chokeNext = 0;
@state songPos = 0;
@state systick = 0;
@state activeTrack = 0;    // frontParser_activeTrack
@state shownPattern = 0;   // frontParser_shownPattern
@state activeVoices = 0;
@state midiNotesOn = 0;
@state euNextCnt1 = 0;
@state euOriginalLength = 0;
@state rngState = 625341585;
@state seqInited = 0;
// The firmware's seq_tick() takes deltaT == -1 as the external clock's "step now"
// signal and then waits 32000 systicks (8 s). With shuffle the internal clock can land
// on exactly -1 too, and the sequencer stalls. External sync is not ported, so the
// check is off; test/seqref.mjs turns it on to compare with the firmware.
@state firmwareDeltaTQuirk = 0;

// ---------------------------------------------------------------- arithmetic helpers
// nearest IEEE single precision value (the firmware computes timing in float)
function fr(x) {
	let c = x * 536870913;
	return c - (c - x);
}

function imod(a, n) {
	return a - n * floor(a / n);
}

function bit(mask, i) {
	return imod(floor(mask / pow(2, i)), 2);
}

function withBit(mask, i, on) {
	let b = bit(mask, i);
	if (on != 0 && b == 0) return mask + pow(2, i);
	if (on == 0 && b != 0) return mask - pow(2, i);
	return mask;
}

// bits [shift, shift + bits) of a non-negative integer
function field(w, shift, bits) {
	return imod(floor(w / pow(2, shift)), pow(2, bits));
}

// GetRngValue(): the hardware RNG, replaced by an LCG
function rngValue() {
	rngState = imod(rngState * 1664525 + 1013904223, 4294967296);
	let hi = floor(rngState / 65536);
	return imod(rngState, 65536) * 65536 + hi;
}

// ---------------------------------------------------------------- pattern storage
function stepWordA(volume, prob, note, flam, shift) {
	return volume + 256 * prob + 65536 * note + 16777216 * flam + 268435456 * shift;
}

function stepWordB(p1Nr, p1Val, p2Nr, p2Val) {
	return p1Nr + 256 * p1Val + 65536 * p2Nr + 16777216 * p2Val;
}

// seq_clearTrack()
function defaultTrack(): list {
	let d: list = [16, 0, 0, 0, 0];
	for (let s = 0; s < NUM_STEPS; s++) {
		let vol = 127;
		if (s - 8 * floor(s / 8) == 0) vol = 255;
		d.push(stepWordA(vol, 100, SEQ_DEFAULT_NOTE, 0, 7));
	}
	for (let s = 0; s < NUM_STEPS; s++) d.push(stepWordB(NO_AUTOMATION, 0, NO_AUTOMATION, 0));
	return d;
}

function trackData(p, t): list {
	let d = PAT.lookup(p * NUM_TRACKS + t);
	if (d.length < SLOT) return defaultTrack();
	return d;
}

function storeTrack(p, t, d: list) {
	PAT.store(p * NUM_TRACKS + t, d);
}

function trackLen(p, t) {
	return trackData(p, t)[0];
}

function mainStepOf(d: list, s) {
	return bit(d[1 + floor(s / 16)], s - 16 * floor(s / 16));
}

function seqSettings(k): list {
	let d = SETS.lookup(k);
	if (k == 0 && d.length < MAX_SONG_LENGTH) {
		d = [0];
		for (let i = 1; i < MAX_SONG_LENGTH; i++) d.push(SONG_END);
	} else if (k == 1 && d.length < NUM_PATTERNS) {
		d = [];
		for (let i = 0; i < NUM_PATTERNS; i++) d.push(0);
	}
	return d;
}

function songEntry(i) {
	return seqSettings(0)[i];
}

function patShuffle(p) {
	return seqSettings(1)[p];
}

function setPatShuffle(p, v) {
	let d = seqSettings(1);
	d[p] = v;
	SETS.store(1, d);
}

function setSongEntry(i, v) {
	let d = seqSettings(0);
	d[i] = v;
	SETS.store(0, d);
}

// write cleared tracks into empty slots, so lookups find full lists
function seqInit() {
	seqInited = 1;
	for (let i = 0; i < NUM_PATTERNS * NUM_TRACKS; i++) {
		if (PAT.lookup(i).length < SLOT) PAT.store(i, defaultTrack());
	}
	SETS.store(0, seqSettings(0));
	SETS.store(1, seqSettings(1));
	for (let t = 0; t < NUM_TRACKS; t++) {
		euLen[t] = 16;
		euSteps[t] = 4;
		euRot[t] = 0;
		midiChan[t] = t;
	}
	midiChan[7] = 7;
}

// The step the firmware reads for seq_subStepPattern[p][t][s]. With s = -1 (a track
// that has not started) that is the element before: step 63 of the previous track.
// Returns [A, B]; [-1, -1] before the very first element.
function stepWords(p, t, s): list {
	let pp = p;
	let tt = t;
	let ss = s;
	if (ss < 0) {
		ss = 63;
		tt = tt - 1;
		if (tt < 0) {
			tt = NUM_TRACKS - 1;
			pp = pp - 1;
			if (pp < 0) return [stepWordA(127, 100, SEQ_DEFAULT_NOTE, 0, 7), stepWordB(NO_AUTOMATION, 0, NO_AUTOMATION, 0)];
		}
	}
	let d = trackData(pp, tt);
	return [d[5 + ss], d[69 + ss]];
}

function isMainStepActive(t, s, p) {
	if (s < 0) return 0;
	return mainStepOf(trackData(p, t), s);
}

function setMainStep(p, t, s, onOff) {
	if (s < 0) return;
	let d = trackData(p, t);
	let k = 1 + floor(s / 16);
	d[k] = withBit(d[k], s - 16 * floor(s / 16), onOff);
	storeTrack(p, t, d);
}

function toggleMainStep(t, s, p) {
	setMainStep(p, t, s, 1 - isMainStepActive(t, s, p));
}

// set one field of a step: 0 volume (7 bit), 1 prob, 2 note, 3 flam, 4 shift,
// 5 dest 1, 6 value 1, 7 dest 2, 8 value 2
function setStepField(p, t, s, f, v) {
	let d = trackData(p, t);
	let a = d[5 + s];
	let b = d[69 + s];
	let vol = field(a, 0, 8);
	let prob = field(a, 8, 8);
	let note = field(a, 16, 8);
	let flam = field(a, 24, 4);
	let shift = field(a, 28, 4);
	let p1n = field(b, 0, 8);
	let p1v = field(b, 8, 8);
	let p2n = field(b, 16, 8);
	let p2v = field(b, 24, 8);
	if (f == 0) vol = vol - imod(vol, 128) + v;
	else if (f == 1) prob = v;
	else if (f == 2) note = v;
	else if (f == 3) flam = v;
	else if (f == 4) shift = v;
	else if (f == 5) p1n = v;
	else if (f == 6) p1v = v;
	else if (f == 7) p2n = v;
	else if (f == 8) p2v = v;
	d[5 + s] = stepWordA(vol, prob, note, flam, shift);
	d[69 + s] = stepWordB(p1n, p1v, p2n, p2v);
	storeTrack(p, t, d);
}

function stepField(p, t, s, f) {
	let d = trackData(p, t);
	if (f == 0) return field(d[5 + s], 0, 7);
	if (f == 1) return field(d[5 + s], 8, 8);
	if (f == 2) return field(d[5 + s], 16, 8);
	if (f == 3) return field(d[5 + s], 24, 4);
	if (f == 4) return field(d[5 + s], 28, 4);
	return field(d[69 + s], 8 * (f - 5), 8);
}

// all fields at once, raw (volume with its active bit)
function setStepRaw(p, t, s, volume, prob, note, flam, shift, p1Nr, p1Val, p2Nr, p2Val) {
	let d = trackData(p, t);
	d[5 + s] = stepWordA(volume, prob, note, flam, shift);
	d[69 + s] = stepWordB(p1Nr, p1Val, p2Nr, p2Val);
	storeTrack(p, t, d);
}

function setTrackLengthOf(p, t, len) {
	let d = trackData(p, t);
	d[0] = len;
	storeTrack(p, t, d);
}

// ---------------------------------------------------------------- MIDI (MidiVoiceControl.c)
function midiNoteOff(chan) {
	if (chan == 255) {
		for (let i = 0; i < 16; i++) {
			if (bit(midiNotesOn, i) != 0) emitMidi(MIDI_NOTE_OFF + i, midiChanNotes[i], 0, 2);
		}
		midiNotesOn = 0;
		return;
	}
	if (chan < 16 && bit(midiNotesOn, chan) != 0) {
		emitMidi(MIDI_NOTE_OFF + chan, midiChanNotes[chan], 0, 2);
		midiNotesOn = withBit(midiNotesOn, chan, 0);
	}
}

function sendMidiNoteOn(chan, note, vel) {
	emitMidi(MIDI_NOTE_ON + chan, note, vel, 2);
	midiChanNotes[chan] = note;
	midiNotesOn = withBit(midiNotesOn, chan, 1);
}

function voiceNoteOn(voice, note, vel) {
	activeVoices = withBit(activeVoices, voice, 1);
	emitTrigger(voice, note, vel);
	sendMidiNoteOn(midiChan[voice], note, vel);
}

function voiceNoteOff(voice) {
	emitNoteOff(voice);
	if (voice == 255) {
		activeVoices = 0;
		midiNoteOff(255);
		return;
	}
	if (voice < 8 && bit(activeVoices, voice) != 0) {
		activeVoices = withBit(activeVoices, voice, 0);
		midiNoteOff(midiChan[voice]);
	}
}

function sendRealtime(status) {
	emitMidi(status, 0, 0, 0);
}

// ---------------------------------------------------------------- tempo and shuffle
function setBpm(bpm) {
	tempo = bpm;
}

function setShuffle(v) {
	shuffle = v;
}

// seq_calcDeltaT(): systicks until the next 96 ppq tick
function calcDeltaT(bpm, isShuffled) {
	deltaT = fr(fr(trunc(60000 / bpm) / 96) * 4);
	if (shuffle != 0) {
		let shuffleFactor = fr(isShuffled * shuffle);
		let originalDeltaT = deltaT;
		deltaT = fr(deltaT + fr(shuffleFactor * originalDeltaT) * 16);
		deltaT = fr(deltaT - fr(lastShuffle * originalDeltaT) * 16);
		lastShuffle = shuffleFactor;
	}
}

// ---------------------------------------------------------------- playback
function setStepIndexToStart() {
	for (let i = 0; i < NUM_TRACKS; i++) stepIndex[i] = -1;
}

function triggerVoice(voice, vol, note) {
	if (voice > 6) return;
	let w = stepWords(activePattern, voice, stepIndex[voice]);
	let b = w[1];
	emitAuto(voice, field(b, 0, 8), field(b, 8, 8), field(b, 16, 8), field(b, 24, 8));
	voiceNoteOff(voice);
	voiceNoteOn(voice, note, vol);
	let midiNote = note;
	if (midiNoteOverride[voice] != 0) midiNote = midiNoteOverride[voice];
	sendMidiNoteOn(midiChan[voice], midiNote, field(w[0], 0, 7));
}

function determineNextPattern(autoIncrement) {
	if (songModeEnabled != 0) {
		if (autoIncrement != 0) {
			songPos = imod(songPos + 1, 256);
			if (songPos == MAX_SONG_LENGTH) songPos = 0;
			return songEntry(songPos);
		}
		let pos = imod(songPos + 1, 256);
		if (pos == MAX_SONG_LENGTH) pos = 0;
		return songEntry(pos);
	}
	return activePattern;
}

function setPendingPatternActive() {
	if (activePattern != pendingPattern || loadPending != 0) {
		setStepIndexToStart();
		loadPending = 0;
		activePattern = pendingPattern;
		let sh = patShuffle(activePattern);
		setShuffle(fr(sh / 127));
		emitShuffle(sh);
		emitMidi(MIDI_PROG_CHANGE + midiChan[7], activePattern, 0, 1);
		voiceNoteOff(255);
	}
}

function setNextPattern(p) {
	if (p >= NUM_PATTERNS) return;
	pendingPattern = p;
	loadPending = 1;
	if (running == 0) setPendingPatternActive();
}

function getLongestTrack() {
	let id = 0;
	let best = trackLen(activePattern, 0);
	for (let i = 1; i < NUM_TRACKS; i++) {
		let l = trackLen(activePattern, i);
		if (l > best) {
			id = i;
			best = l;
		}
	}
	return id;
}

// seq_nextStep()
function nextStep() {
	if (running == 0) return;
	masterStepCnt = imod(masterStepCnt + 1, 256);

	let longest = getLongestTrack();
	let seqlen = trackLen(activePattern, longest);
	if (seqlen == 0) seqlen = 16;
	let next = stepIndex[longest] + 1;
	let masterStepPos = next;
	if (imod(next, 64) == 0 || next >= seqlen) masterStepPos = 0;

	if (masterStepPos == 0) {
		if (activePattern == pendingPattern) {
			pendingPattern = determineNextPattern(1);
			if (pendingPattern == SONG_END) {
				setRunning(0);
				pendingPattern = songEntry(0);
				if (pendingPattern >= 64) pendingPattern = 0;
				songPos = 0;
				return;
			} else if (pendingPattern == SONG_LOOP) {
				pendingPattern = songEntry(0);
				songPos = 0;
			}
		}
		setPendingPatternActive();
	}

	for (let i = 0; i < NUM_TRACKS; i++) {
		stepIndex[i] = stepIndex[i] + 1;
		let d = trackData(activePattern, i);
		let len = d[0];
		if (len == 0) len = 16;
		if (stepIndex[i] >= len || imod(stepIndex[i], 64) == 0) stepIndex[i] = 0;

		if (bit(mutedTracks, i) == 0 && mainStepOf(d, stepIndex[i]) != 0) {
			if (eraseActive != 0 && i == activeTrack) {
				setMainStep(activePattern, activeTrack, stepIndex[i], 0);
			} else {
				rndPrev[i] = rndCur[i];
				rndCur[i] = rndNext[i];
				rndNext[i] = imod(rngValue(), 100) + 1;
			}
		}

		if (rollRate != 255 && bit(rollState, i) != 0) {
			if (imod(stepIndex[i], rollRate) == 0) {
				let note = field(d[5 + stepIndex[i]], 16, 8);
				triggerVoice(i, ROLL_VOLUME, note);
				addNote(i, ROLL_VOLUME, note);
			}
		}
	}
}

function shiftValue(v) {
	if (v <= 6) return v - 7;
	if (v >= 8 && v <= 14) return v - 7;
	return 0;
}

function flamPattern(flam) {
	if (flam == 1) return 8390656;    // x2 0b100000000000100000000000
	if (flam == 2) return 8421504;    // x3 0b100000001000000010000000
	if (flam == 3) return 8521760;    // x4 0b100000100000100000100000
	if (flam == 4) return 8947848;    // x6 0b100010001000100010001000
	if (flam == 5) return 9586980;    // x8 0b100100100100100100100100
	return 8388608;                   // off 0b100000000000000000000000
}

// seq_getShiftFlamPattern(): the 24 tick pattern a step contributes to the current
// step; stepPos 0 = previous step, 1 = current, 2 = next
function shiftFlamPattern(d: list, track, step, stepPos) {
	if (mainStepOf(d, step) == 0) return 0;
	let a = d[5 + step];
	let prob = field(a, 8, 8);
	let rnd = rndCur[track];
	if (stepPos == 0) rnd = rndPrev[track];
	else if (stepPos == 2) rnd = rndNext[track];
	if (rnd > prob) return 0;

	let pattern = flamPattern(field(a, 24, 4));
	let shift = shiftValue(field(a, 28, 4)) * 3;
	if (shift == 0) {
		if (stepPos == 1) return pattern;
		return 0;
	}
	if (shift > 0) {
		if (stepPos == 0) return imod(pattern * pow(2, 24 - shift), 16777216);
		if (stepPos == 1) return floor(pattern / pow(2, shift));
		return 0;
	}
	shift = -shift;
	if (stepPos == 1) return imod(pattern * pow(2, shift), 16777216);
	if (stepPos == 2) return floor(pattern / pow(2, 24 - shift));
	return 0;
}

// seq_processFlam(): play what falls on tick idx (0-23) of the current step
function processFlam(idx) {
	if (running == 0) return;
	if (idx == 0) {
		chokeCurrent = 0;
		chokeNext = 0;
	}
	for (let track = 0; track < NUM_TRACKS; track++) {
		if (bit(mutedTracks, track) != 0) continue;
		let d = trackData(activePattern, track);
		let length = d[0];
		let currentStep = stepIndex[track];
		let nextStep = imod(currentStep + 1, length);
		let prevStep = length - 1;
		if (currentStep != 0) prevStep = currentStep - 1;

		let currentActive = bit(shiftFlamPattern(d, track, currentStep, 1), 23 - idx);
		let nextActive = bit(shiftFlamPattern(d, track, nextStep, 2), 23 - idx);
		let prevActive = bit(shiftFlamPattern(d, track, prevStep, 0), 23 - idx);

		let play = -1;
		if (nextActive != 0) {
			chokeNext = 1;
			play = nextStep;
		} else if (currentActive != 0 && chokeNext == 0) {
			chokeCurrent = 1;
			play = currentStep;
		} else if (prevActive != 0 && chokeNext == 0 && chokeCurrent == 0) {
			play = prevStep;
		}
		if (play >= 0) {
			lastWasNoteOn[track] = 1;
			let a = d[5 + play];
			triggerVoice(track, field(a, 0, 7), field(a, 16, 8));
		} else if (lastWasNoteOn[track] != 0) {
			lastWasNoteOn[track] = 0;
			voiceNoteOff(track);
		}
	}
}

// one seq_tick() call on the internal clock; returns 0 when it had nothing to do
function seqTickOnce() {
	if (firmwareDeltaTQuirk != 0 && deltaT == -1) {
		deltaT = 32000;
		nextStep();
		return 1;
	}
	let elapsed = systick - lastTick;
	if (elapsed < deltaT) return 0;
	let rest = fr(elapsed - deltaT);
	lastTick = systick;
	calcDeltaT(tempo, isStepShuffled);
	isStepShuffled = 0;
	deltaT = fr(deltaT - rest);
	if (prescaleCounter == 0) nextStep();
	processFlam(prescaleCounter);
	if (imod(prescaleCounter, 4) == 0) sendRealtime(MIDI_CLOCK);
	prescaleCounter = prescaleCounter + 1;
	if (prescaleCounter == 23) isStepShuffled = imod(masterStepCnt, 2);
	if (prescaleCounter >= 24) prescaleCounter = 0;
	return 1;
}

// advance the clock by n systicks (0.25 ms each)
function seqClock(n) {
	if (seqInited == 0) seqInit();
	for (let i = 0; i < n; i++) {
		systick = systick + 1;
		for (let k = 0; k < CALLS_PER_SYSTICK; k++) {
			if (seqTickOnce() == 0) break;
		}
	}
}

// ---------------------------------------------------------------- transport
function setRunning(isRunning) {
	running = isRunning;
	if (running != 0) {
		prescaleCounter = 0;
		sendRealtime(MIDI_START);
	}
	setStepIndexToStart();
}

function resetPosition() {
	lastShuffle = 0;
	masterStepCnt = 0;
	songPos = 255;
	deltaT = 0;
	sendRealtime(MIDI_STOP);
	voiceNoteOff(255);
	setStepIndexToStart();
}

// the front panel's start/stop button
function runStop(on) {
	if (on == 0) resetPosition();
	setRunning(on);
}

function setMute(t, isMuted) {
	if (t == 7) {
		mutedTracks = 0;
	} else if (isMuted != 0) {
		mutedTracks = withBit(mutedTracks, t, 1);
		voiceNoteOff(midiChan[t]);
	} else {
		mutedTracks = withBit(mutedTracks, t, 0);
	}
}

function setSongModeEnabled(on) {
	songModeEnabled = on;
	if (on != 0) songPos = 255;
}

// ---------------------------------------------------------------- roll
function setRoll(voice, on) {
	if (voice >= 7) return;
	if (on != 0) {
		rollState = withBit(rollState, voice, 1);
		if (rollRate == 255 || running == 0) {
			triggerVoice(voice, ROLL_VOLUME, SEQ_DEFAULT_NOTE);
			addNote(voice, ROLL_VOLUME, SEQ_DEFAULT_NOTE);
		}
	} else {
		rollState = withBit(rollState, voice, 0);
	}
}

// 0 one shot, 1 1/1, 2 1/2, 3 1/4, 4 1/8, 5 1/16
function setRollRate(rate) {
	let r = 0;
	if (rate == 0) r = 254;
	else if (rate == 1) r = 15;
	else if (rate == 2) r = 7;
	else if (rate == 3) r = 3;
	else if (rate == 4) r = 1;
	rollRate = r + 1;
}

// ---------------------------------------------------------------- recording
function setRecordingMode(on) {
	recordActive = on;
}

function setErasingMode(on) {
	eraseActive = on;
}

// seq_addNote(): record a played note at the current step, or the next one when more
// than half the step has passed
function addNote(t, vel, note) {
	if (running == 0 || recordActive == 0) return;
	let q = stepIndex[t];
	if (fr(fr(prescaleCounter / 23) * 100) >= 50) q = q + 1;
	if (q < 0) return; // a note before the first step: the firmware writes outside the pattern
	q = imod(q, trackLen(activePattern, t));
	let d = trackData(activePattern, t);
	let a = d[5 + q];
	d[5 + q] = stepWordA(vel + 128, 100, note, field(a, 24, 4), field(a, 28, 4));
	let k = 1 + floor(q / 16);
	d[k] = withBit(d[k], q - 16 * floor(q / 16), 1);
	storeTrack(activePattern, t, d);
}

function setActiveAutomationTrack(lane) {
	activeAutomTrack = lane;
}

function armAutomationStep(step, track, isArmed) {
	if (isArmed != 0) {
		armedStep = step;
		armedTrack = track;
	} else {
		armedStep = -1;
		armedTrack = -1;
	}
}

function isAutomationStepArmed() {
	if (armedStep != -1) return 1;
	return 0;
}

// seq_recordAutomation(): a parameter moved (dest: the firmware's automation code,
// see autoCode()). Recorded into the playing step while recording, and into the held
// step.
function recordAutomation(voice, dest, value) {
	if (recordActive != 0) {
		let step = stepIndex[voice];
		if (isMainStepActive(voice, step, activePattern) != 0) {
			setStepField(activePattern, voice, step, 5 + 2 * activeAutomTrack, dest);
			setStepField(activePattern, voice, step, 6 + 2 * activeAutomTrack, value);
		}
	}
	if (armedStep != -1 && armedTrack != -1) {
		setStepField(activePattern, armedTrack, armedStep, 5 + 2 * activeAutomTrack, dest);
		setStepField(activePattern, armedTrack, armedStep, 6 + 2 * activeAutomTrack, value);
	}
}

// LXR parameter number -> automation code (CortexParser.c: front parameters below 128
// are sent one higher)
function autoCode(par) {
	if (par < 128 && par != 0) return par + 1;
	return par;
}

// ---------------------------------------------------------------- editing
function clearTrack(t, p) {
	storeTrack(p, t, defaultTrack());
}

function clearPattern(p) {
	for (let t = 0; t < NUM_TRACKS; t++) clearTrack(t, p);
}

function clearAutomation(t, p, lane) {
	let d = trackData(p, t);
	for (let s = 0; s < NUM_STEPS; s++) {
		let b = d[69 + s];
		if (lane == 0) d[69 + s] = stepWordB(NO_AUTOMATION, 0, field(b, 16, 8), field(b, 24, 8));
		else d[69 + s] = stepWordB(field(b, 0, 8), field(b, 8, 8), NO_AUTOMATION, 0);
	}
	storeTrack(p, t, d);
}

function copyTrack(src, dst, p) {
	storeTrack(p, dst, trackData(p, src));
}

function copyPattern(src, dst) {
	for (let t = 0; t < NUM_TRACKS; t++) storeTrack(dst, t, trackData(src, t));
	setPatShuffle(dst, patShuffle(src));
}

function isPatternEmpty(p) {
	for (let t = 0; t < NUM_TRACKS; t++) {
		let d = trackData(p, t);
		if (d[1] + d[2] + d[3] + d[4] != 0) return 0;
	}
	return 1;
}

// seq_setTrackLength() / seq_getTrackLength(): on the shown pattern
function setTrackLength(t, len) {
	setTrackLengthOf(shownPattern, t, len);
}

function getTrackLength(t) {
	return trackLen(shownPattern, t);
}

// ---------------------------------------------------------------- Euclid generator
function euObjSize(group) {
	let n = 0;
	let g = group;
	while (g > 0) {
		g = floor(g / 2);
		n = n + 1;
	}
	if (n == 0) return 1;
	return n;
}

// euklid_calcRecursive(), as a loop (it only calls itself at the end); groups are
// uint16 as in the firmware
function euCalc(length0, steps0, group10, group20) {
	let length = length0;
	let steps = steps0;
	let group1 = group10;
	let group2 = group20;
	while (true) {
		let remainder = 0;
		if (steps != 0) remainder = imod(length, steps);
		if (euNextCnt1 == 0) euNextCnt1 = steps;
		let objectCnt1 = euNextCnt1;
		let objSize1 = euObjSize(group1);
		let objSize2 = euObjSize(group2);
		let objCnt2 = trunc((euOriginalLength - objectCnt1 * objSize1) / objSize2);
		euNextCnt1 = imod(objectCnt1 > objCnt2 ? objCnt2 : objectCnt1, 65536);
		if (objCnt2 <= 1) {
			let pos = 0;
			for (let i = 0; i < objectCnt1; i++) {
				for (let j = objSize1 - 1; j >= 0; j--) {
					if (pos < 64 && bit(group1, j) != 0) euBuf[pos] = 1;
					pos = pos + 1;
				}
			}
			for (let i = 0; i < objCnt2; i++) {
				for (let j = objSize2 - 1; j >= 0; j--) {
					if (pos < 64 && bit(group2, j) != 0) euBuf[pos] = 1;
					pos = pos + 1;
				}
			}
			return;
		}
		let originalGroup1 = group1;
		group1 = imod(group1 * pow(2, objSize2), 65536);
		for (let i = 0; i < objSize2; i++) {
			if (bit(group2, i) != 0) group1 = withBit(group1, i, 1);
		}
		if (objCnt2 - objectCnt1 <= 0) group2 = originalGroup1;
		length = steps;
		steps = remainder;
	}
}

function euGenerate(t, p) {
	let length = euLen[t];
	let steps = euSteps[t];
	if (steps == 0) steps = 1;
	euNextCnt1 = 0;
	euOriginalLength = length;
	for (let i = 0; i < 64; i++) euBuf[i] = 0;
	euCalc(length, steps, 1, 0);
	// euklid_rotatePattern()
	let amount = euRot[t];
	if (amount != 0 && length != 1 && imod(amount, length) != 0) {
		amount = imod(amount, length);
		let tmp: list = [];
		for (let i = 0; i < 64; i++) tmp.push(euBuf[i]);
		// (q << amount) | (q >> (length - amount)), then only the first length bits
		for (let i = 0; i < 64; i++) {
			let v = 0;
			if (i >= amount && tmp[i - amount] != 0) v = 1;
			if (i + length - amount < 64 && tmp[i + length - amount] != 0) v = 1;
			if (i >= length) v = 0;
			euBuf[i] = v;
		}
	}
	// euklid_transferPattern()
	let d = trackData(p, t);
	for (let b = 0; b < 4; b++) {
		let w = 0;
		for (let i = 0; i < 16; i++) {
			if (euBuf[16 * b + i] != 0) w = w + pow(2, i);
		}
		d[1 + b] = w;
	}
	d[0] = euLen[t];
	storeTrack(p, t, d);
}

function euklidSetLength(t, v) {
	euLen[t] = max(1, v);
}

function euklidSetSteps(t, v, p) {
	let s = max(1, v);
	if (s > euLen[t]) s = euLen[t];
	euSteps[t] = s;
	euGenerate(t, p);
}

function euklidSetRotation(t, v, p) {
	if (v <= euLen[t]) euRot[t] = v;
	euGenerate(t, p);
}
