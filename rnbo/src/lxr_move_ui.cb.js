// LXR on Ableton Move: the control surface (RNBO message codebox)
//
// Turns Move's pads and buttons into a drum machine for the LXR engine and draws its
// state on the LEDs.
//
//   bottom pad row      play Drum1, Drum2, Drum3, Snare, Cymbal, closed and open hihat
//                       (Shift + pad selects that track)
//   upper three rows    play the selected track chromatically (C3 .. B4, C4 = unchanged)
//   steps               toggle the selected track's steps, Shift + step: accent,
//                       Delete + step: clear
//   track buttons       select a track; the top button is the first track of the page.
//                       Mute + track: mute / unmute, Delete + track: clear the track,
//                       Mute alone: mute / unmute the selected track
//   + / -               page between tracks 1-4 and 5-7
//   < / >               shorten / lengthen the selected track (1-16 steps),
//                       Shift + < / >: tempo -1 / +1 BPM
//   Play                start / stop the sequencer
//   Record              live recording: pads played while the sequencer runs are
//                       written to the pattern (velocity >= 100 records an accent)
//
// External MIDI: channels 1-7 play the tracks (note = pitch), channel 10 plays them
// from a General MIDI drum map.
//
// The pattern itself lives in the engine's seq_* parameters (so it is saved with the
// presets); this code keeps a mirror of it that the engine refreshes through the
// status inlets, and sends the whole new value of a parameter after every edit.
//
// Inlets  listin1:   [zone, address, value, ...] from the Move input driver, and
//                    [100, note, velocity, channel] for external MIDI notes
//         in2..in8:  step bits of tracks 1-7          (engine out5..out11)
//         in9..in15: accent bits of tracks 1-7        (engine out12..out18)
//         in16:      running + 2 * activity bits + 256 * bpm (engine out19)
//         in17:      lengths - 1 of tracks 1-4, 4 bits each  (engine out20)
//         in18:      lengths - 1 of tracks 5-7       (engine out21)
//         in19:      last played step; its arrival (every TICK_MS) refreshes the LEDs
// Outlets listout1:  [zone, address, r, g, b] or [zone, address, on] for the LED driver
//         out2..out8: trigger values for engine in2..in8
//         out9:      sequencer control for engine in1 (running + 2 * mute bits)
//         out10..out16: new seq_<track>_steps values, out17..out23: seq_<track>_accents,
//         out24..out30: seq_<track>_length, out31: bpm

const TICK_MS = 10;       // interval of the status snapshots
const HOLD = 50;          // status ticks to wait for the engine to confirm an edit
const NLED = 68;

// input / LED zones
const Z_PAD = 0;
const Z_STEP = 1;
const Z_TRACK = 2;
const Z_FUNC = 3;
const Z_NAV = 4;
const Z_MIDI = 100;
// function buttons (zone 3)
const F_MUTE = 3;
const F_DELETE = 4;
const F_SHIFT = 7;
const F_RECORD = 8;
const F_PLAY = 9;

@state inited = 0;
@state sel = 0;           // selected track 0-6
@state bank = 0;          // track button page: 0 = tracks 1-4, 1 = tracks 5-7
@state running = 0;
@state mutes = 0;
@state recording = 0;
@state shift = 0;
@state del = 0;
@state muteHeld = 0;
@state muteUsed = 0;
@state STEPS = new FixedDoubleArray(7);
@state ACC = new FixedDoubleArray(7);
@state LEN = new FixedDoubleArray(7);
@state PEND = new FixedDoubleArray(22);   // edits not yet confirmed: steps, accents, lengths, bpm
@state TCOUNT = new FixedDoubleArray(7);
@state bpm = 120;
@state engRun = 0;
@state active = 0;
@state lastStep = 0;
@state stepTicks = 0;
@state tick = 0;
@state CACHE = new FixedDoubleArray(68);
@state COL = new FixedDoubleArray(21);

function init() {
	inited = 1;
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
	for (let t = 0; t < 7; t++) LEN[t] = 16;
}

function setCol(t, r, g, b) {
	COL[3 * t] = r;
	COL[3 * t + 1] = g;
	COL[3 * t + 2] = b;
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

// ---------------------------------------------------------------- output to the engine
function trigger(t, note, vel) {
	let c = TCOUNT[t] + 1;
	if (c > 999) c = 1;
	TCOUNT[t] = c;
	let v = c * 16384 + note * 128 + vel;
	if (t == 0) out2 = v;
	else if (t == 1) out3 = v;
	else if (t == 2) out4 = v;
	else if (t == 3) out5 = v;
	else if (t == 4) out6 = v;
	else if (t == 5) out7 = v;
	else if (t == 6) out8 = v;
}

function sendControl() {
	out9 = running + 2 * mutes;
}

function sendSteps(t) {
	PEND[t] = HOLD;
	let v = STEPS[t];
	if (t == 0) out10 = v;
	else if (t == 1) out11 = v;
	else if (t == 2) out12 = v;
	else if (t == 3) out13 = v;
	else if (t == 4) out14 = v;
	else if (t == 5) out15 = v;
	else if (t == 6) out16 = v;
}

function sendAccents(t) {
	PEND[7 + t] = HOLD;
	let v = ACC[t];
	if (t == 0) out17 = v;
	else if (t == 1) out18 = v;
	else if (t == 2) out19 = v;
	else if (t == 3) out20 = v;
	else if (t == 4) out21 = v;
	else if (t == 5) out22 = v;
	else if (t == 6) out23 = v;
}

function sendLength(t) {
	PEND[14 + t] = HOLD;
	let v = LEN[t];
	if (t == 0) out24 = v;
	else if (t == 1) out25 = v;
	else if (t == 2) out26 = v;
	else if (t == 3) out27 = v;
	else if (t == 4) out28 = v;
	else if (t == 5) out29 = v;
	else if (t == 6) out30 = v;
}

function sendBpm() {
	PEND[21] = HOLD;
	out31 = bpm;
}

// ---------------------------------------------------------------- editing
function toggleStep(t, i) {
	if (bit(STEPS[t], i) != 0) {
		STEPS[t] = withBit(STEPS[t], i, 0);
		if (bit(ACC[t], i) != 0) {
			ACC[t] = withBit(ACC[t], i, 0);
			sendAccents(t);
		}
	} else {
		STEPS[t] = withBit(STEPS[t], i, 1);
	}
	sendSteps(t);
}

function toggleAccent(t, i) {
	let on = 1 - bit(ACC[t], i);
	ACC[t] = withBit(ACC[t], i, on);
	sendAccents(t);
	// an accent needs a step to sit on
	if (on != 0 && bit(STEPS[t], i) == 0) {
		STEPS[t] = withBit(STEPS[t], i, 1);
		sendSteps(t);
	}
}

function clearStep(t, i) {
	if (bit(STEPS[t], i) != 0) {
		STEPS[t] = withBit(STEPS[t], i, 0);
		sendSteps(t);
	}
	if (bit(ACC[t], i) != 0) {
		ACC[t] = withBit(ACC[t], i, 0);
		sendAccents(t);
	}
}

function clearTrack(t) {
	STEPS[t] = 0;
	ACC[t] = 0;
	sendSteps(t);
	sendAccents(t);
}

function setLength(t, n) {
	let len = max(1, min(16, n));
	if (len != LEN[t]) {
		LEN[t] = len;
		sendLength(t);
	}
}

function setBpm(n) {
	let b = max(20, min(300, n));
	if (b != bpm) {
		bpm = b;
		sendBpm();
	}
}

function toggleMute(t) {
	mutes = withBit(mutes, t, 1 - bit(mutes, t));
	sendControl();
}

function selectTrack(t) {
	sel = t;
	bank = t >= 4 ? 1 : 0;
}

// play a voice; while recording, also write it into the pattern
function hit(t, note, vel) {
	trigger(t, note, vel);
	if (recording == 0 || running == 0 || engRun == 0) return;
	let len = LEN[t];
	let pos = imod(lastStep, len);
	// a hit in the second half of a step belongs to the next one
	if (stepTicks * TICK_MS > 7500 / bpm) pos = imod(lastStep + 1, len);
	if (bit(STEPS[t], pos) == 0) {
		STEPS[t] = withBit(STEPS[t], pos, 1);
		sendSteps(t);
	}
	if (vel >= 100 && bit(ACC[t], pos) == 0) {
		ACC[t] = withBit(ACC[t], pos, 1);
		sendAccents(t);
	}
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

// ---------------------------------------------------------------- status from the engine
// a status value replaces the mirror unless an edit of it is still on its way
function confirm(k, current, v) {
	if (PEND[k] > 0) {
		if (v == current) PEND[k] = 0;
		else PEND[k] = PEND[k] - 1;
		return 0;
	}
	return 1;
}

function stSteps(t, v) {
	if (confirm(t, STEPS[t], v) != 0) STEPS[t] = v;
}

function stAccents(t, v) {
	if (confirm(7 + t, ACC[t], v) != 0) ACC[t] = v;
}

function stLength(t, n) {
	if (confirm(14 + t, LEN[t], n) != 0) LEN[t] = n;
}

function in2(v: number) { stSteps(0, v); }
function in3(v: number) { stSteps(1, v); }
function in4(v: number) { stSteps(2, v); }
function in5(v: number) { stSteps(3, v); }
function in6(v: number) { stSteps(4, v); }
function in7(v: number) { stSteps(5, v); }
function in8(v: number) { stSteps(6, v); }
function in9(v: number) { stAccents(0, v); }
function in10(v: number) { stAccents(1, v); }
function in11(v: number) { stAccents(2, v); }
function in12(v: number) { stAccents(3, v); }
function in13(v: number) { stAccents(4, v); }
function in14(v: number) { stAccents(5, v); }
function in15(v: number) { stAccents(6, v); }

function in16(v: number) {
	engRun = imod(v, 2);
	active = imod(floor(v / 2), 128);
	let b = floor(v / 256);
	if (confirm(21, bpm, b) != 0) bpm = b;
}

function in17(v: number) {
	for (let t = 0; t < 4; t++) stLength(t, imod(floor(v / pow(16, t)), 16) + 1);
}

function in18(v: number) {
	for (let t = 0; t < 3; t++) stLength(4 + t, imod(floor(v / pow(16, t)), 16) + 1);
}

function in19(v: number) {
	if (v != lastStep) {
		lastStep = v;
		stepTicks = 0;
	} else {
		stepTicks = stepTicks + 1;
	}
	refresh();
}

// ---------------------------------------------------------------- LEDs
// sends an LED only when its colour changed; cache slots: pads 0-31, steps 32-47,
// tracks 48-51, function buttons 52-63, nav 64-67
function led(zone, addr, r, g, b) {
	let k = addr;
	if (zone == Z_STEP) k = 32 + addr;
	else if (zone == Z_TRACK) k = 48 + addr;
	else if (zone == Z_FUNC) k = 52 + addr;
	else if (zone == Z_NAV) k = 64 + addr;
	let rr = max(0, min(1, r));
	let gg = max(0, min(1, g));
	let bb = max(0, min(1, b));
	let key = floor(rr * 255) * 65536 + floor(gg * 255) * 256 + floor(bb * 255);
	if (CACHE[k] != key) {
		CACHE[k] = key;
		listout1 = [zone, addr, rr, gg, bb];
	}
}

function ledTrack(zone, addr, t, level) {
	led(zone, addr, COL[3 * t] * level, COL[3 * t + 1] * level, COL[3 * t + 2] * level);
}

// black and white LEDs
function ledBw(zone, addr, on) {
	let k = 52 + addr;
	if (zone == Z_NAV) k = 64 + addr;
	if (CACHE[k] != on) {
		CACHE[k] = on;
		listout1 = [zone, addr, on];
	}
}

function refresh() {
	if (inited == 0) init();
	tick = tick + 1;
	// resend one LED per tick in case something else drew over it
	CACHE[imod(tick, NLED)] = -1;

	// voice pads: dim, brighter when selected, flash when the voice plays
	for (let p = 0; p < 7; p++) {
		let level = p == sel ? 0.5 : 0.12;
		if (bit(mutes, p) != 0) level = 0.02;
		if (bit(active, p) != 0) {
			led(Z_PAD, p, COL[3 * p] + 0.4, COL[3 * p + 1] + 0.4, COL[3 * p + 2] + 0.4);
		} else {
			ledTrack(Z_PAD, p, p, level);
		}
	}
	led(Z_PAD, 7, 0, 0, 0);
	// keyboard for the selected track: C brightest, black keys darkest
	for (let p = 8; p < 32; p++) {
		let n = imod(p - 8, 12);
		let level = 0.15;
		if (n == 0) level = 0.6;
		else if (n == 1 || n == 3 || n == 6 || n == 8 || n == 10) level = 0.03;
		ledTrack(Z_PAD, p, sel, level);
	}

	// steps of the selected track, playhead in white
	let len = LEN[sel];
	let play = -1;
	if (engRun != 0) play = imod(lastStep, len);
	for (let i = 0; i < 16; i++) {
		if (i >= len) led(Z_STEP, i, 0, 0, 0);
		else if (i == play) led(Z_STEP, i, 0.8, 0.8, 0.8);
		else if (bit(STEPS[sel], i) != 0) ledTrack(Z_STEP, i, sel, bit(ACC[sel], i) != 0 ? 1 : 0.25);
		else led(Z_STEP, i, 0.02, 0.02, 0.02);
	}

	// track buttons, top to bottom: first to last track of the page
	for (let k = 0; k < 4; k++) {
		let t = bank * 4 + 3 - k;
		if (t > 6) {
			led(Z_TRACK, k, 0, 0, 0);
		} else {
			let level = t == sel ? 1 : 0.15;
			if (bit(mutes, t) != 0) level = level * 0.1;
			ledTrack(Z_TRACK, k, t, level);
		}
	}

	if (running != 0) led(Z_FUNC, F_PLAY, 0, 1, 0);
	else led(Z_FUNC, F_PLAY, 0.15, 0.15, 0.15);
	if (recording != 0) led(Z_FUNC, F_RECORD, 1, 0, 0);
	else led(Z_FUNC, F_RECORD, 0.15, 0.15, 0.15);
	ledBw(Z_FUNC, F_MUTE, muteHeld != 0 || bit(mutes, sel) != 0 ? 1 : 0);
	ledBw(Z_FUNC, F_DELETE, del);
	ledBw(Z_FUNC, F_SHIFT, shift);
	ledBw(Z_NAV, 0, bank == 0 ? 1 : 0);
	ledBw(Z_NAV, 1, bank == 1 ? 1 : 0);
	ledBw(Z_NAV, 2, 1);
	ledBw(Z_NAV, 3, 1);
}

// ---------------------------------------------------------------- input
if (inited == 0) init();

let m = listin1;
if (m.length < 3) return;
let zone = m[0];
let addr = m[1];
let val = m[2];

if (zone == Z_PAD) {
	if (val > 0) {
		if (addr < 7) {
			if (shift != 0) selectTrack(addr);
			else hit(addr, 60, val);
		} else if (addr >= 8) {
			hit(sel, 48 + addr - 8, val);
		}
	}
} else if (zone == Z_STEP) {
	if (val > 0) {
		if (del != 0) clearStep(sel, addr);
		else if (shift != 0) toggleAccent(sel, addr);
		else toggleStep(sel, addr);
	}
} else if (zone == Z_TRACK) {
	let trk = bank * 4 + 3 - addr;
	if (val > 0 && trk <= 6) {
		if (del != 0) {
			clearTrack(trk);
		} else if (muteHeld != 0) {
			toggleMute(trk);
			muteUsed = 1;
		} else {
			sel = trk;
		}
	}
} else if (zone == Z_FUNC) {
	if (addr == F_SHIFT) {
		shift = val > 0 ? 1 : 0;
	} else if (addr == F_DELETE) {
		del = val > 0 ? 1 : 0;
	} else if (addr == F_MUTE) {
		if (val > 0) {
			muteHeld = 1;
			muteUsed = 0;
		} else {
			if (muteHeld != 0 && muteUsed == 0) toggleMute(sel);
			muteHeld = 0;
		}
	} else if (addr == F_PLAY && val > 0) {
		running = 1 - running;
		sendControl();
	} else if (addr == F_RECORD && val > 0) {
		recording = 1 - recording;
	}
} else if (zone == Z_NAV) {
	if (val > 0) {
		if (addr == 0) {
			// next page: keep the same button lit
			if (bank == 0) {
				bank = 1;
				sel = min(6, sel + 4);
			}
		} else if (addr == 1) {
			if (bank == 1) {
				bank = 0;
				sel = sel - 4;
			}
		} else if (addr == 2) {
			if (shift != 0) setBpm(bpm - 1);
			else setLength(sel, LEN[sel] - 1);
		} else if (addr == 3) {
			if (shift != 0) setBpm(bpm + 1);
			else setLength(sel, LEN[sel] + 1);
		}
	}
} else if (zone == Z_MIDI && m.length >= 4) {
	let chan = m[3];
	if (val > 0 && chan != 16) {
		if (chan >= 1 && chan <= 7) {
			hit(chan - 1, addr, val);
		} else if (chan == 10) {
			let gm = gmTrack(addr);
			if (gm >= 0) hit(gm, 60, val);
		}
	}
}
