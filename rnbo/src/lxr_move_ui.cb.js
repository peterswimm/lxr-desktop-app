// LXR on Ableton Move: the control surface (RNBO message codebox)
//
// build.py puts src/lxr_seq.cb.js (the ported LXR sequencer) in front of this file;
// together they are the patch's one message codebox. This part turns Move's pads,
// buttons and encoders into the LXR's front panel for that sequencer and draws its
// state on the LEDs.
//
// Step view (default)
//   steps                  the selected track's steps in the shown bar (16 of up to 64).
//                          Tap: on / off. Hold: edit the step (encoders, see below) and
//                          lock parameters: move any parameter in a parameter view while
//                          a step is held and the step plays it with that value (the
//                          parameter goes back when the step is released).
//   encoders, step held    1 velocity, 2 probability, 3 note, 4 flam (off, x2, x3, x4,
//                          x6, x8), 5 micro shift (-7..+7 / 24ths of a step),
//                          6 automation lane for locks (1/2), 7 lane 1 value, 8 lane 2 value
//   Shift + step           track length ends at this step
//   Delete + step          clear the step (off, default values, no locks)
//   < / >                  previous / next bar of the track (the view follows playback
//                          until you do this; Play turns following back on)
//   Shift + < / >          tempo -1 / +1 BPM
// Tracks
//   track buttons          select a track; the top button is the first track of the page
//   + / -                  page the track buttons between tracks 1-4 and 5-7
//   Mute + track           mute / unmute (Mute alone: the selected track)
//   Delete + track         clear the track
//   Shift + Delete + track clear the track's locks in the selected lane
//   Duplicate + track      copy the selected track there
// Pads
//   bottom row             Drum1, Drum2, Drum3, Snare, Cymbal, closed and open hihat
//   Shift + bottom pad     select that track
//   upper three rows       the selected track chromatically (C3-B4, C4 = the sound's pitch)
//   Repeat + bottom pads   roll the held voices; Repeat + pads 9-14 (second row) picks
//                          the roll rate: one shot, 1/1, 1/2, 1/4, 1/8, 1/16
// Pattern view (Session button)
//   steps                  the 16 patterns: tap = play next (at the end of the playing
//                          pattern); hold one and tap others = chain them (song mode)
//   Delete + pattern       clear it
//   Duplicate + pattern    copy the playing pattern there
// Transport
//   Play                   start / stop (Shift + Play: restart)
//   Record                 live recording: pads and external notes go into the pattern,
//                          parameter moves into the playing step's lock lane
//   Shift + Delete         while recording: erase the selected track's steps as they play
//   Capture + encoders 1-3 Euclid generator for the selected track: length, steps, rotation
//
// External MIDI: channels 1-7 play the tracks (note = pitch), channel 10 plays them
// from a General MIDI drum map; both record like the pads.
//
// Inlets  listin1:  [zone, address, value, ...] from the Move input driver,
//                   [100, note, velocity, channel] external MIDI notes,
//                   [101, index, value] a parameter's value (index: LXR parameter
//                   number, or the IDX_* below)
//         in2:      clock: systicks (0.25 ms) since the last call; [metro 1] -> 4
// Outlets listout1: [zone, address, r, g, b] or [zone, address, on] for the LED driver
//         out2..out8: trigger values for engine in2..in8
//         out9..out15: parameter lock values for the engine's auto_<track>
//         listout16: [index, value] to set a parameter (tempo, shuffle, lock release)
//         out17:    MIDI bytes (sequencer MIDI out, when midi_out is on)

const IDX_BPM = 300;
const IDX_SHUFFLE = 301;
const IDX_MIDI_OUT = 302;
const IDX_MIDI_CHAN = 303;     // 303..309 tracks, 310 global
const IDX_MIDI_NOTE = 311;     // 311..317
const NPV = 320;
const NSOUND = 247;            // END_OF_SOUND_PARAMETERS
const NLED = 76;
const FLASH_MS = 60;
const REFRESH_MS = 10;

// input / LED zones
const Z_PAD = 0;
const Z_STEP = 1;
const Z_TRACK = 2;
const Z_FUNC = 3;
const Z_NAV = 4;
const Z_ENC = 6;
const Z_MIDI = 100;
const Z_PARAM = 101;
// function buttons (zone 3)
const F_CAPTURE = 0;
const F_REPEAT = 2;
const F_MUTE = 3;
const F_DELETE = 4;
const F_DUPLICATE = 5;
const F_SHIFT = 7;
const F_RECORD = 8;
const F_PLAY = 9;
const F_SESSION = 10;

@state uiInited = 0;
@state bank = 0;
@state patternView = 0;
@state bar = 0;
@state follow = 1;
@state shiftHeld = 0;
@state delHeld = 0;
@state dupHeld = 0;
@state muteHeld = 0;
@state muteUsed = 0;
@state repeatHeld = 0;
@state captureHeld = 0;
@state rollRateUi = 5;
@state heldMask = 0;           // step buttons held (step view)
@state heldCount = 0;
@state heldEdited = 0;
@state wasOn = new FixedDoubleArray(16);
@state chainFirst = -1;
@state chainLen = 0;
@state CHAIN = new FixedDoubleArray(64);
@state rollPads = 0;
@state usedPatterns = 0;
@state PV = new FixedDoubleArray(320);        // parameter values as last seen
@state expectSet = new FixedDoubleArray(320); // our own parameter writes, not echoed yet
@state expectVal = new FixedDoubleArray(320);
@state lockOrig = new FixedDoubleArray(247);  // values to put back when the step is let go
@state encAcc = new FixedDoubleArray(8);
@state TCOUNT = new FixedDoubleArray(7);
@state ACOUNT = new FixedDoubleArray(7);
@state flash = new FixedDoubleArray(7);
@state clockMs = 0;
@state clockSub = 0;
@state refreshIn = 0;
@state ledTick = 0;
@state CACHE = new FixedDoubleArray(76);
@state COL = new FixedDoubleArray(21);

function uiInit() {
	uiInited = 1;
	if (seqInited == 0) seqInit();
	// track colours: Drum1-3 red, orange, yellow, Snare green, Cymbal cyan,
	// closed hihat blue, open hihat purple
	setCol(0, 1, 0, 0);
	setCol(1, 1, 0.35, 0);
	setCol(2, 1, 0.85, 0);
	setCol(3, 0, 1, 0.1);
	setCol(4, 0, 0.8, 1);
	setCol(5, 0.1, 0.2, 1);
	setCol(6, 0.7, 0, 1);
	for (let i = 0; i < NLED; i++) CACHE[i] = -1;
	for (let i = 0; i < NPV; i++) PV[i] = -1;
	for (let i = 0; i < NSOUND; i++) lockOrig[i] = -1;
	setRollRate(rollRateUi);
	if (isPatternEmpty(0) != 0) demoPattern();
}

// something to play on first start: four on the floor, backbeat, hats
function demoPattern() {
	for (let s = 0; s < 16; s = s + 4) setMainStep(0, 0, s, 1);
	setMainStep(0, 3, 4, 1);
	setMainStep(0, 3, 12, 1);
	for (let s = 0; s < 16; s = s + 2) {
		if (s != 14) setMainStep(0, 5, s, 1);
		if (imod(s, 4) == 2) setStepField(0, 5, s, 0, 70);
	}
	setMainStep(0, 6, 14, 1);
}

function setCol(t, r, g, b) {
	COL[3 * t] = r;
	COL[3 * t + 1] = g;
	COL[3 * t + 2] = b;
}

function clampv(v, lo, hi) {
	return max(lo, min(hi, v));
}

// ---------------------------------------------------------------- sequencer outputs
function emitTrigger(voice, note, vel) {
	let c = TCOUNT[voice] + 1;
	if (c > 999) c = 1;
	TCOUNT[voice] = c;
	let v = c * 16384 + note * 128 + vel;
	if (voice == 0) out2 = v;
	else if (voice == 1) out3 = v;
	else if (voice == 2) out4 = v;
	else if (voice == 3) out5 = v;
	else if (voice == 4) out6 = v;
	else if (voice == 5) out7 = v;
	else if (voice == 6) out8 = v;
	if (voice < 7) flash[voice] = FLASH_MS;
}

function emitAuto(voice, d1, v1, d2, v2) {
	let c = ACOUNT[voice] + 1;
	if (c > 1000000) c = 1;
	ACOUNT[voice] = c;
	let v = c * 4294967296 + d1 + 256 * v1 + 65536 * d2 + 16777216 * v2;
	if (voice == 0) out9 = v;
	else if (voice == 1) out10 = v;
	else if (voice == 2) out11 = v;
	else if (voice == 3) out12 = v;
	else if (voice == 4) out13 = v;
	else if (voice == 5) out14 = v;
	else if (voice == 6) out15 = v;
}

function emitNoteOff(voice) {
}

function emitMidi(status, d1, d2, len) {
	if (PV[IDX_MIDI_OUT] <= 0) return;
	out17 = status;
	if (len >= 1) out17 = d1;
	if (len >= 2) out17 = d2;
}

function emitShuffle(v) {
	writeParam(IDX_SHUFFLE, v);
}

function writeParam(idx, v) {
	expectSet[idx] = 1;
	expectVal[idx] = v;
	listout16 = [idx, v];
}

// ---------------------------------------------------------------- parameters
function paramEvent(idx, v) {
	if (idx < 0 || idx >= NPV) return;
	let echo = 0;
	if (expectSet[idx] != 0 && expectVal[idx] == v) {
		expectSet[idx] = 0;
		echo = 1;
	}
	let old = PV[idx];
	PV[idx] = v;
	if (idx < NSOUND) {
		if (echo != 0) return;
		if (heldCount > 0 && patternView == 0) {
			if (lockOrig[idx] < 0 && old >= 0) lockOrig[idx] = old;
			heldEdited = 1;
		}
		recordAutomation(activeTrack, autoCode(idx), v);
		return;
	}
	if (idx == IDX_BPM) {
		setBpm(max(1, v));
	} else if (idx == IDX_SHUFFLE) {
		setShuffle(fr(v / 127));
		setPatShuffle(activePattern, v);
	} else if (idx >= IDX_MIDI_CHAN && idx <= IDX_MIDI_CHAN + 7) {
		let ch = clampv(v, 1, 16) - 1;
		let t = idx - IDX_MIDI_CHAN;
		if (t < 7 && midiChan[t] != ch) voiceNoteOff(t);
		midiChan[t] = ch;
	} else if (idx >= IDX_MIDI_NOTE && idx < IDX_MIDI_NOTE + 7) {
		midiNoteOverride[idx - IDX_MIDI_NOTE] = v;
	}
}

// put back the parameters that were moved to lock the held step
function releaseLocks() {
	for (let i = 0; i < NSOUND; i++) {
		if (lockOrig[i] >= 0) {
			let v = lockOrig[i];
			lockOrig[i] = -1;
			PV[i] = v;
			writeParam(i, v);
		}
	}
}

// ---------------------------------------------------------------- playing
function hit(t, note, vel) {
	voiceNoteOn(t, note, vel);
	addNote(t, vel, note);
}

// General MIDI drum notes -> track (-1: not mapped)
function gmTrack(note) {
	if (note == 35 || note == 36) return 0;
	if (note == 41 || note == 43 || note == 45) return 1;
	if (note == 47 || note == 48 || note == 50) return 2;
	if (note >= 37 && note <= 40) return 3;
	if (note == 49 || note == 51 || note == 52 || note == 55 || note == 57 || note == 59) return 4;
	if (note == 42 || note == 44) return 5;
	if (note == 46) return 6;
	return -1;
}

function selectTrack(t) {
	activeTrack = t;
	bank = t >= 4 ? 1 : 0;
}

function stopRolls() {
	for (let v = 0; v < 7; v++) {
		if (bit(rollPads, v) != 0) setRoll(v, 0);
	}
	rollPads = 0;
}

// ---------------------------------------------------------------- step view
function stepNr(i) {
	return bar * 16 + i;
}

function clearStep(t, s) {
	setMainStep(shownPattern, t, s, 0);
	setStepRaw(shownPattern, t, s, 127, 100, SEQ_DEFAULT_NOTE, 0, 7, NO_AUTOMATION, 0, NO_AUTOMATION, 0);
}

function stepPress(i) {
	let s = stepNr(i);
	if (delHeld != 0) {
		clearStep(activeTrack, s);
		return;
	}
	if (shiftHeld != 0) {
		setTrackLength(activeTrack, s + 1);
		return;
	}
	heldMask = withBit(heldMask, i, 1);
	heldCount = heldCount + 1;
	if (heldCount == 1) {
		heldEdited = 0;
		armAutomationStep(s, activeTrack, 1);
	}
	wasOn[i] = isMainStepActive(activeTrack, s, shownPattern);
	if (wasOn[i] == 0) setMainStep(shownPattern, activeTrack, s, 1);
}

function stepRelease(i) {
	if (bit(heldMask, i) == 0) return;
	heldMask = withBit(heldMask, i, 0);
	heldCount = heldCount - 1;
	if (wasOn[i] != 0 && heldEdited == 0) setMainStep(shownPattern, activeTrack, stepNr(i), 0);
	if (heldCount <= 0) {
		heldCount = 0;
		armAutomationStep(0, 0, 0);
		releaseLocks();
	}
}

function releaseAllSteps() {
	for (let i = 0; i < 16; i++) stepRelease(i);
}

// encoder steps, with small ranges moving once every few detents
function encSteps(e, d, div) {
	encAcc[e] = encAcc[e] + d;
	let n = trunc(encAcc[e] / div);
	encAcc[e] = encAcc[e] - n * div;
	return n;
}

function editHeldSteps(e, d) {
	if (e == 5) {
		let n = encSteps(e, d, 3);
		if (n != 0) setActiveAutomationTrack(clampv(activeAutomTrack + n, 0, 1));
		heldEdited = 1;
		return;
	}
	let n = d;
	if (e == 3 || e == 4) n = encSteps(e, d, 3);
	if (n == 0) return;
	heldEdited = 1;
	for (let i = 0; i < 16; i++) {
		if (bit(heldMask, i) == 0) continue;
		let s = stepNr(i);
		if (e == 0) setStepField(shownPattern, activeTrack, s, 0, clampv(stepField(shownPattern, activeTrack, s, 0) + n, 0, 127));
		else if (e == 1) setStepField(shownPattern, activeTrack, s, 1, clampv(stepField(shownPattern, activeTrack, s, 1) + n, 0, 100));
		else if (e == 2) setStepField(shownPattern, activeTrack, s, 2, clampv(stepField(shownPattern, activeTrack, s, 2) + n, 0, 127));
		else if (e == 3) setStepField(shownPattern, activeTrack, s, 3, clampv(stepField(shownPattern, activeTrack, s, 3) + n, 0, 5));
		else if (e == 4) setStepField(shownPattern, activeTrack, s, 4, clampv(stepField(shownPattern, activeTrack, s, 4) + n, 0, 14));
		else if (e == 6 || e == 7) {
			let f = 6 + 2 * (e - 6);
			if (stepField(shownPattern, activeTrack, s, f - 1) != NO_AUTOMATION) {
				setStepField(shownPattern, activeTrack, s, f, clampv(stepField(shownPattern, activeTrack, s, f) + n, 0, 127));
			}
		}
	}
}

function editEuclid(e, d) {
	let t = activeTrack;
	if (e == 0) {
		let n = encSteps(e, d, 3);
		if (n == 0) return;
		euklidSetLength(t, clampv(euLen[t] + n, 1, 16));
		euklidSetSteps(t, euSteps[t], shownPattern);
	} else if (e == 1) {
		let n = encSteps(e, d, 3);
		if (n != 0) euklidSetSteps(t, clampv(euSteps[t] + n, 1, 16), shownPattern);
	} else if (e == 2) {
		let n = encSteps(e, d, 3);
		if (n != 0) euklidSetRotation(t, clampv(euRot[t] + n, 0, 15), shownPattern);
	}
}

// ---------------------------------------------------------------- pattern view
function patternPress(p) {
	if (delHeld != 0) {
		clearPattern(p);
		return;
	}
	if (dupHeld != 0) {
		copyPattern(activePattern, p);
		return;
	}
	if (chainFirst < 0) {
		chainFirst = p;
		chainLen = 1;
		CHAIN[0] = p;
	} else if (chainLen < 63) {
		CHAIN[chainLen] = p;
		chainLen = chainLen + 1;
	}
}

function patternRelease(p) {
	if (p != chainFirst) return;
	if (chainLen <= 1) {
		setSongModeEnabled(0);
		setNextPattern(p);
	} else {
		for (let i = 0; i < MAX_SONG_LENGTH; i++) {
			let v = SONG_END;
			if (i < chainLen) v = CHAIN[i];
			else if (i == chainLen) v = SONG_LOOP;
			setSongEntry(i, v);
		}
		setSongModeEnabled(1);
		if (running == 0) setNextPattern(CHAIN[0]);
	}
	chainFirst = -1;
	chainLen = 0;
}

function inChain(song: list, p) {
	if (songModeEnabled == 0) return 0;
	for (let i = 0; i < MAX_SONG_LENGTH; i++) {
		if (song[i] >= NUM_PATTERNS) return 0;
		if (song[i] == p) return 1;
	}
	return 0;
}

// ---------------------------------------------------------------- clock
function in2(v: number) {
	if (uiInited == 0) uiInit();
	seqClock(v);
	shownPattern = activePattern;
	clockSub = clockSub + v;
	while (clockSub >= 4) {
		clockSub = clockSub - 4;
		clockMs = clockMs + 1;
		for (let t = 0; t < 7; t++) {
			if (flash[t] > 0) flash[t] = flash[t] - 1;
		}
	}
	refreshIn = refreshIn - v / 4;
	if (refreshIn <= 0) {
		refreshIn = refreshIn + REFRESH_MS;
		refresh();
	}
}

// ---------------------------------------------------------------- LEDs
// sends an LED only when it changed; cache slots: pads 0-31, steps 32-47, tracks 48-51,
// function buttons 52-63, nav 64-67, encoders 68-75
function ledSlot(zone, addr) {
	if (zone == Z_STEP) return 32 + addr;
	if (zone == Z_TRACK) return 48 + addr;
	if (zone == Z_FUNC) return 52 + addr;
	if (zone == Z_NAV) return 64 + addr;
	if (zone == Z_ENC) return 68 + addr;
	return addr;
}

function led(zone, addr, r, g, b) {
	let k = ledSlot(zone, addr);
	let rr = clampv(r, 0, 1);
	let gg = clampv(g, 0, 1);
	let bb = clampv(b, 0, 1);
	let key = floor(rr * 255) * 65536 + floor(gg * 255) * 256 + floor(bb * 255);
	if (CACHE[k] != key) {
		CACHE[k] = key;
		listout1 = [zone, addr, rr, gg, bb];
	}
}

function ledTrack(zone, addr, t, level) {
	led(zone, addr, COL[3 * t] * level, COL[3 * t + 1] * level, COL[3 * t + 2] * level);
}

// a track colour mixed towards white
function ledTrackWhite(zone, addr, t, level, white) {
	led(zone, addr, (COL[3 * t] * (1 - white) + white) * level, (COL[3 * t + 1] * (1 - white) + white) * level,
		(COL[3 * t + 2] * (1 - white) + white) * level);
}

// black and white LEDs
function ledBw(zone, addr, on) {
	let k = ledSlot(zone, addr);
	if (CACHE[k] != on) {
		CACHE[k] = on;
		listout1 = [zone, addr, on];
	}
}

function refreshPads() {
	for (let p = 0; p < 7; p++) {
		let level = p == activeTrack ? 0.5 : 0.12;
		if (bit(mutedTracks, p) != 0) level = 0.02;
		if (flash[p] > 0 || bit(rollPads, p) != 0) ledTrackWhite(Z_PAD, p, p, 1, 0.4);
		else ledTrack(Z_PAD, p, p, level);
	}
	led(Z_PAD, 7, 0, 0, 0);
	for (let p = 8; p < 32; p++) {
		if (repeatHeld != 0 && p >= 8 && p <= 13) {
			if (p - 8 == rollRateUi) led(Z_PAD, p, 1, 1, 1);
			else led(Z_PAD, p, 0.15, 0.15, 0.15);
			continue;
		}
		let n = imod(p - 8, 12);
		let level = 0.15;
		if (n == 0) level = 0.6;
		else if (n == 1 || n == 3 || n == 6 || n == 8 || n == 10) level = 0.03;
		ledTrack(Z_PAD, p, activeTrack, level);
	}
}

function refreshSteps() {
	let t = activeTrack;
	let d = trackData(shownPattern, t);
	let len = d[0];
	let play = -1;
	if (running != 0 && shownPattern == activePattern) play = stepIndex[t];
	for (let i = 0; i < 16; i++) {
		let s = stepNr(i);
		if (bit(heldMask, i) != 0) {
			led(Z_STEP, i, 1, 1, 1);
		} else if (s >= len) {
			led(Z_STEP, i, 0, 0, 0);
		} else if (s == play) {
			led(Z_STEP, i, 0.8, 0.8, 0.8);
		} else if (mainStepOf(d, s) != 0) {
			let a = d[5 + s];
			let b = d[69 + s];
			let level = 0.1 + 0.9 * field(a, 0, 7) / 127;
			let white = 0;
			if (field(b, 0, 8) != NO_AUTOMATION || field(b, 16, 8) != NO_AUTOMATION) white = 0.35;
			if (field(a, 8, 8) < 100) level = level * 0.5;
			ledTrackWhite(Z_STEP, i, t, level, white);
		} else {
			led(Z_STEP, i, 0.02, 0.02, 0.02);
		}
	}
}

function refreshPatterns() {
	// which patterns have steps: one pattern per refresh
	let q = imod(ledTick, NUM_PATTERNS);
	usedPatterns = withBit(usedPatterns, q, 1 - isPatternEmpty(q));
	let song = seqSettings(0);
	for (let p = 0; p < 16; p++) {
		if (p == activePattern) led(Z_STEP, p, 0, 1, 0);
		else if (p == pendingPattern && imod(floor(clockMs / 150), 2) == 0) led(Z_STEP, p, 1, 0.5, 0);
		else if (inChain(song, p) != 0) led(Z_STEP, p, 0, 0.2, 0.6);
		else if (bit(usedPatterns, p) != 0) led(Z_STEP, p, 0.12, 0.12, 0.12);
		else led(Z_STEP, p, 0, 0, 0);
	}
}

// encoder LEDs: the held step's values, the Euclid settings, or which bar is shown
function refreshEncoders() {
	let t = activeTrack;
	if (heldCount > 0 && patternView == 0) {
		let s = -1;
		for (let i = 0; i < 16; i++) {
			if (s < 0 && bit(heldMask, i) != 0) s = stepNr(i);
		}
		ledTrack(Z_ENC, 0, t, 0.05 + 0.95 * stepField(shownPattern, t, s, 0) / 127);
		ledTrack(Z_ENC, 1, t, 0.05 + 0.95 * stepField(shownPattern, t, s, 1) / 100);
		ledTrack(Z_ENC, 2, t, 0.05 + 0.95 * stepField(shownPattern, t, s, 2) / 127);
		ledTrack(Z_ENC, 3, t, 0.05 + 0.95 * stepField(shownPattern, t, s, 3) / 5);
		let sh = stepField(shownPattern, t, s, 4) - 7;
		if (sh < 0) led(Z_ENC, 4, 0, 0, -sh / 7);
		else if (sh > 0) led(Z_ENC, 4, sh / 7, 0, 0);
		else led(Z_ENC, 4, 0.3, 0.3, 0.3);
		if (activeAutomTrack == 0) ledTrack(Z_ENC, 5, t, 1);
		else led(Z_ENC, 5, 1, 1, 1);
		for (let k = 0; k < 2; k++) {
			if (stepField(shownPattern, t, s, 5 + 2 * k) == NO_AUTOMATION) led(Z_ENC, 6 + k, 0, 0, 0);
			else led(Z_ENC, 6 + k, 0.05 + 0.95 * stepField(shownPattern, t, s, 6 + 2 * k) / 127, 0.05, 0.6);
		}
		return;
	}
	if (captureHeld != 0) {
		ledTrack(Z_ENC, 0, t, euLen[t] / 16);
		ledTrack(Z_ENC, 1, t, euSteps[t] / 16);
		ledTrack(Z_ENC, 2, t, 0.05 + euRot[t] / 16);
		for (let k = 3; k < 8; k++) led(Z_ENC, k, 0, 0, 0);
		return;
	}
	let len = trackLen(shownPattern, t);
	for (let k = 0; k < 4; k++) {
		if (patternView != 0 || k * 16 >= len) led(Z_ENC, k, 0, 0, 0);
		else if (k == bar) ledTrack(Z_ENC, k, t, 1);
		else ledTrack(Z_ENC, k, t, 0.1);
	}
	for (let k = 4; k < 8; k++) led(Z_ENC, k, 0, 0, 0);
}

function refresh() {
	ledTick = ledTick + 1;
	// resend one LED per refresh in case something else drew over it
	CACHE[imod(ledTick, NLED)] = -1;

	if (follow != 0 && running != 0 && heldCount == 0 && stepIndex[activeTrack] >= 0) {
		bar = floor(stepIndex[activeTrack] / 16);
	}
	refreshPads();
	if (patternView != 0) refreshPatterns();
	else refreshSteps();
	refreshEncoders();

	for (let k = 0; k < 4; k++) {
		let t = bank * 4 + 3 - k;
		if (t > 6) {
			led(Z_TRACK, k, 0, 0, 0);
		} else {
			let level = t == activeTrack ? 1 : 0.15;
			if (bit(mutedTracks, t) != 0) level = level * 0.1;
			if (flash[t] > 0 && t != activeTrack) level = max(level, 0.4);
			ledTrack(Z_TRACK, k, t, level);
		}
	}

	if (running != 0) led(Z_FUNC, F_PLAY, 0, 1, 0);
	else led(Z_FUNC, F_PLAY, 0.15, 0.15, 0.15);
	if (recordActive != 0) led(Z_FUNC, F_RECORD, 1, 0, 0);
	else led(Z_FUNC, F_RECORD, 0.15, 0.15, 0.15);
	ledBw(Z_FUNC, F_MUTE, muteHeld != 0 || bit(mutedTracks, activeTrack) != 0 ? 1 : 0);
	ledBw(Z_FUNC, F_DELETE, delHeld != 0 || eraseActive != 0 ? 1 : 0);
	ledBw(Z_FUNC, F_SHIFT, shiftHeld);
	ledBw(Z_FUNC, F_DUPLICATE, dupHeld);
	ledBw(Z_FUNC, F_REPEAT, repeatHeld);
	ledBw(Z_FUNC, F_CAPTURE, captureHeld);
	ledBw(Z_FUNC, F_SESSION, patternView);
	ledBw(Z_NAV, 0, bank == 0 ? 1 : 0);
	ledBw(Z_NAV, 1, bank == 1 ? 1 : 0);
	ledBw(Z_NAV, 2, patternView == 0 && bar > 0 ? 1 : 0);
	ledBw(Z_NAV, 3, patternView == 0 && bar < 3 ? 1 : 0);
}

function updateErase() {
	let on = 0;
	if (shiftHeld != 0 && delHeld != 0 && recordActive != 0 && running != 0) on = 1;
	setErasingMode(on);
}

// ---------------------------------------------------------------- input
if (uiInited == 0) uiInit();

let m = listin1;
if (m.length < 3) return;
let zone = m[0];
let addr = m[1];
let val = m[2];

if (zone == Z_PARAM) {
	paramEvent(addr, val);
} else if (zone == Z_PAD) {
	if (addr < 7) {
		if (val > 0) {
			if (shiftHeld != 0) selectTrack(addr);
			else if (repeatHeld != 0) {
				rollPads = withBit(rollPads, addr, 1);
				setRoll(addr, 1);
			} else hit(addr, SEQ_DEFAULT_NOTE, val);
		} else if (bit(rollPads, addr) != 0) {
			rollPads = withBit(rollPads, addr, 0);
			setRoll(addr, 0);
		}
	} else if (addr >= 8 && val > 0) {
		if (repeatHeld != 0 && addr <= 13) {
			rollRateUi = addr - 8;
			setRollRate(rollRateUi);
		} else {
			hit(activeTrack, 48 + addr - 8, val);
		}
	}
} else if (zone == Z_STEP) {
	if (patternView != 0) {
		if (val > 0) patternPress(addr);
		else patternRelease(addr);
	} else if (val > 0) {
		stepPress(addr);
	} else {
		stepRelease(addr);
	}
} else if (zone == Z_TRACK) {
	let trk = bank * 4 + 3 - addr;
	if (val > 0 && trk <= 6) {
		if (delHeld != 0 && shiftHeld != 0) {
			clearAutomation(trk, shownPattern, activeAutomTrack);
		} else if (delHeld != 0) {
			clearTrack(trk, shownPattern);
		} else if (dupHeld != 0) {
			if (trk != activeTrack) copyTrack(activeTrack, trk, shownPattern);
		} else if (muteHeld != 0) {
			setMute(trk, 1 - bit(mutedTracks, trk));
			muteUsed = 1;
		} else {
			releaseAllSteps();
			activeTrack = trk;
		}
	}
} else if (zone == Z_FUNC) {
	let down = val > 0 ? 1 : 0;
	if (addr == F_SHIFT) {
		shiftHeld = down;
		updateErase();
	} else if (addr == F_DELETE) {
		delHeld = down;
		updateErase();
	} else if (addr == F_DUPLICATE) {
		dupHeld = down;
	} else if (addr == F_CAPTURE) {
		captureHeld = down;
	} else if (addr == F_REPEAT) {
		repeatHeld = down;
		if (down == 0) stopRolls();
	} else if (addr == F_MUTE) {
		if (down != 0) {
			muteHeld = 1;
			muteUsed = 0;
		} else {
			if (muteHeld != 0 && muteUsed == 0) setMute(activeTrack, 1 - bit(mutedTracks, activeTrack));
			muteHeld = 0;
		}
	} else if (addr == F_PLAY && down != 0) {
		if (shiftHeld != 0) {
			runStop(0);
			runStop(1);
			follow = 1;
		} else if (running != 0) {
			runStop(0);
		} else {
			runStop(1);
			follow = 1;
		}
		updateErase();
	} else if (addr == F_RECORD && down != 0) {
		setRecordingMode(1 - recordActive);
		updateErase();
	} else if (addr == F_SESSION && down != 0) {
		releaseAllSteps();
		patternView = 1 - patternView;
		chainFirst = -1;
		chainLen = 0;
	}
} else if (zone == Z_NAV) {
	if (val > 0) {
		if (addr == 0) {
			if (bank == 0) {
				bank = 1;
				activeTrack = min(6, activeTrack + 4);
			}
		} else if (addr == 1) {
			if (bank == 1) {
				bank = 0;
				activeTrack = activeTrack - 4;
			}
		} else if (addr == 2 || addr == 3) {
			let dir = addr == 2 ? -1 : 1;
			if (shiftHeld != 0) {
				let b = clampv(seqTempo + dir, 20, 300);
				setBpm(b);
				writeParam(IDX_BPM, b);
			} else if (patternView == 0) {
				releaseAllSteps();
				bar = clampv(bar + dir, 0, 3);
				follow = 0;
			}
		}
	}
} else if (zone == Z_ENC && m.length >= 4 && addr < 8) {
	let d = m[3];
	if (d != 0) {
		if (heldCount > 0 && patternView == 0) editHeldSteps(addr, d);
		else if (captureHeld != 0) editEuclid(addr, d);
	}
} else if (zone == Z_MIDI && m.length >= 4) {
	let chan = m[3];
	// (MidiParser.c ignores notes for muted tracks)
	if (val > 0 && chan != 16) {
		if (chan >= 1 && chan <= 7) {
			if (bit(mutedTracks, chan - 1) == 0) hit(chan - 1, addr, val);
		} else if (chan == 10) {
			let gm = gmTrack(addr);
			if (gm >= 0 && bit(mutedTracks, gm) == 0) hit(gm, SEQ_DEFAULT_NOTE, val);
		}
	}
}
