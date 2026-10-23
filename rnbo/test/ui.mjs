// Tests the Move control surface together with the engine, wired like the RNBO patch:
// UI outlets -> engine inlets / [set seq_*], engine status -> [snapshot~ 10] -> UI.
//   node rnbo/test/ui.mjs
import assert from 'assert/strict';
import { readFileSync } from 'fs';
import { dirname, join } from 'path';
import { fileURLToPath } from 'url';
import { instantiate, instantiateMessage } from './codebox.mjs';

const HERE = dirname(fileURLToPath(import.meta.url));
const ENGINE = readFileSync(join(HERE, '../generated/lxr_engine.codebox.js'), 'utf8');
const UI = readFileSync(join(HERE, '../src/lxr_move_ui.cb.js'), 'utf8');
const TRACKS = JSON.parse(readFileSync(join(HERE, '../generated/params.json'), 'utf8')).seq_tracks;
const SR = 44100;
const SNAP = Math.round(SR * 0.01); // [snapshot~ 10]

const PAD = 0, STEP = 1, TRACK = 2, FUNC = 3, NAV = 4, MIDI = 100;
const MUTE = 3, DELETE = 4, SHIFT = 7, RECORD = 8, PLAY = 9;

class Rig {
	constructor() {
		this.dsp = instantiate(ENGINE, SR).dsp;
		this.ui = instantiateMessage(UI).box;
		this.leds = new Map();
		this.ledMsgs = [];
		this.sample = 0;
	}

	flush() {
		for (const [n, v] of this.ui.take()) {
			if (n === 1) {
				this.checkLed(v);
				this.leds.set(`${v[0]},${v[1]}`, v.slice(2));
				this.ledMsgs.push(v);
			} else if (n >= 2 && n <= 8) this.dsp.input(n, v);
			else if (n === 9) this.dsp.input(1, v);
			else if (n >= 10 && n <= 16) this.dsp.set(`seq_${TRACKS[n - 10]}_steps`, v);
			else if (n >= 17 && n <= 23) this.dsp.set(`seq_${TRACKS[n - 17]}_accents`, v);
			else if (n >= 24 && n <= 30) this.dsp.set(`seq_${TRACKS[n - 24]}_length`, v);
			else if (n === 31) this.dsp.set('bpm', v);
			else throw new Error(`unexpected outlet ${n}`);
		}
	}

	checkLed(v) {
		const [zone, addr] = v;
		const bw = zone === NAV || (zone === FUNC && addr !== RECORD && addr !== PLAY);
		if (bw) {
			assert.equal(v.length, 3, `b/w LED ${v}`);
			assert.ok(v[2] === 0 || v[2] === 1, `b/w LED ${v}`);
		} else {
			assert.equal(v.length, 5, `RGB LED ${v}`);
			for (const c of v.slice(2)) assert.ok(c >= 0 && c <= 1, `RGB LED ${v}`);
		}
		const count = [32, 16, 4, 12, 4][zone];
		assert.ok(addr >= 0 && addr < count, `LED address ${v}`);
	}

	press(zone, addr, val = 127) {
		this.ui.input(1, [zone, addr, val]);
		this.flush();
	}

	tap(zone, addr) {
		this.press(zone, addr, 127);
		this.press(zone, addr, 0);
	}

	// with a modifier button held
	withHeld(addr, fn) {
		this.press(FUNC, addr, 127);
		fn();
		this.press(FUNC, addr, 0);
	}

	run(ms) {
		const n = Math.round(ms * SR / 1000);
		for (let i = 0; i < n; i++) {
			this.dsp.perform();
			if (++this.sample % SNAP === 0) {
				const o = this.dsp.output();
				for (let k = 0; k < 18; k++) this.ui.input(2 + k, o[4 + k]);
				this.flush();
			}
		}
	}

	// runs until the engine reports a new step, then returns it
	nextStep() {
		const s = this.ui.get('lastStep');
		for (let i = 0; i < 1000 && this.ui.get('lastStep') === s; i++) this.run(1);
		return this.ui.get('lastStep');
	}

	led(zone, addr) {
		return this.leds.get(`${zone},${addr}`);
	}

	param(name) {
		return this.dsp.get(name);
	}

	trigCount(t) {
		return this.dsp.get('tCount')[t];
	}
}

const isWhite = (c) => c[0] > 0.5 && c[1] > 0.5 && c[2] > 0.5;
const isRed = (c) => c[0] > 0.2 && c[1] === 0 && c[2] === 0;
const isDim = (c) => Math.max(...c) <= 0.02;
const isOff = (c) => Math.max(...c) === 0;

function test(name, fn) {
	fn(new Rig());
	console.log('ok  ', name);
}

test('initial LEDs show the demo kick pattern on track 1', (r) => {
	r.run(50);
	for (let i = 0; i < 16; i++) {
		const c = r.led(STEP, i);
		if (i % 4 === 0) assert.ok(isRed(c), `step ${i} ${c}`);
		else assert.ok(isDim(c), `step ${i} ${c}`);
	}
	assert.deepEqual(r.led(TRACK, 3), [1, 0, 0], 'top track button = track 1, selected');
	assert.deepEqual(r.led(NAV, 0), [1], '+ lit on page 1');
	assert.deepEqual(r.led(NAV, 1), [0], '- dark on page 1');
	assert.deepEqual(r.led(FUNC, PLAY), [0.15, 0.15, 0.15]);
});

test('steps toggle the selected track and the engine confirms', (r) => {
	r.run(30);
	r.tap(STEP, 2);
	assert.equal(r.param('seq_d1_steps'), 0x1115);
	r.run(30);
	assert.ok(isRed(r.led(STEP, 2)));
	assert.equal(r.ui.get('PEND')[0], 0, 'edit confirmed');
	r.tap(STEP, 0);
	assert.equal(r.param('seq_d1_steps'), 0x1114);
});

test('a stale status does not undo an edit', (r) => {
	r.run(30);
	r.tap(STEP, 1);
	r.ui.input(2, 0x1111); // status from before the edit
	assert.equal(r.ui.get('STEPS')[0], 0x1113);
	r.run(30);
	assert.equal(r.ui.get('STEPS')[0], 0x1113);
	assert.equal(r.param('seq_d1_steps'), 0x1113);
});

test('Shift + step sets accents, Delete + step clears', (r) => {
	r.run(30);
	r.withHeld(SHIFT, () => r.tap(STEP, 6));
	assert.equal(r.param('seq_d1_accents'), 1 << 6);
	assert.equal(r.param('seq_d1_steps'), 0x1111 | (1 << 6), 'accent adds the step');
	r.run(30);
	assert.deepEqual(r.led(STEP, 6), [1, 0, 0], 'accented step at full brightness');
	r.withHeld(DELETE, () => r.tap(STEP, 6));
	assert.equal(r.param('seq_d1_accents'), 0);
	assert.equal(r.param('seq_d1_steps'), 0x1111);
});

test('track buttons select tracks, + / - page to tracks 5-7', (r) => {
	r.run(30);
	r.tap(TRACK, 0); // bottom button: track 4 (snare)
	assert.equal(r.ui.get('sel'), 3);
	r.run(30);
	assert.ok(r.led(STEP, 4)[1] > 0.2, 'snare steps in green');
	r.tap(STEP, 0);
	assert.equal(r.param('seq_sn_steps'), 0x1011);

	r.tap(NAV, 0); // +
	assert.equal(r.ui.get('bank'), 1);
	assert.equal(r.ui.get('sel'), 6, 'selection moves along with the page');
	r.run(30);
	assert.deepEqual(r.led(NAV, 0), [0]);
	assert.deepEqual(r.led(NAV, 1), [1]);
	assert.ok(isOff(r.led(TRACK, 0)), 'no track 8');
	r.tap(TRACK, 3); // top button on page 2: track 5 (cymbal)
	assert.equal(r.ui.get('sel'), 4);
	r.tap(STEP, 3);
	assert.equal(r.param('seq_cy_steps'), 8);
	r.tap(TRACK, 1); // open hihat
	r.tap(STEP, 15);
	assert.equal(r.param('seq_oh_steps'), 0x8000);
	r.tap(TRACK, 0); // nothing there
	assert.equal(r.ui.get('sel'), 6);

	r.tap(NAV, 1); // -
	assert.equal(r.ui.get('bank'), 0);
	assert.equal(r.ui.get('sel'), 2);
	r.tap(NAV, 1); // already on page 1
	assert.equal(r.ui.get('sel'), 2);
});

test('Shift + pad selects a track without playing it', (r) => {
	r.run(30);
	r.withHeld(SHIFT, () => r.tap(PAD, 5));
	assert.equal(r.ui.get('sel'), 5);
	assert.equal(r.ui.get('bank'), 1);
	r.run(10);
	assert.equal(r.trigCount(5), 0);
});

test('pads play the voices, upper rows play the selected track chromatically', (r) => {
	r.run(10);
	r.press(PAD, 3, 90);
	r.press(PAD, 3, 0);
	r.run(5);
	assert.equal(r.trigCount(3), 1);
	assert.equal(r.dsp.get('tVel')[3], 90);
	assert.equal(r.dsp.get('tNote')[3], 60);
	r.tap(TRACK, 2); // track 2
	r.press(PAD, 8 + 7, 100); // G3
	r.run(5);
	assert.equal(r.trigCount(1), 1);
	assert.equal(r.dsp.get('tNote')[1], 55);
	r.run(20);
	assert.ok(r.led(PAD, 20)[0] > r.led(PAD, 21)[0], 'C brighter than C#');
	// retriggering with the same velocity still triggers
	r.press(PAD, 3, 90);
	r.run(5);
	r.press(PAD, 3, 90);
	r.run(5);
	assert.equal(r.trigCount(3), 3);
});

test('Play runs the sequencer, playhead and pad flashes follow it', (r) => {
	r.run(30);
	r.tap(FUNC, PLAY);
	r.run(1);
	assert.equal(r.dsp.get('sqRun'), 1);
	r.run(2009); // one bar at 120 BPM takes 2 s
	assert.equal(r.trigCount(0), 4 + 1);
	assert.deepEqual(r.led(FUNC, PLAY), [0, 1, 0]);
	const seen = new Set();
	let flashes = 0;
	for (let k = 0; k < 40; k++) {
		r.run(10);
		for (let i = 0; i < 16; i++) if (isWhite(r.led(STEP, i))) seen.add(i);
		if (r.led(PAD, 5).reduce((x, y) => x + y) > 1.5) flashes++;
	}
	assert.ok(seen.size >= 3, `playhead moves ${[...seen]}`);
	assert.ok(flashes > 0, 'closed hihat pad flashes');
	r.tap(FUNC, PLAY);
	r.run(1);
	assert.equal(r.dsp.get('sqRun'), 0);
	r.run(30);
	for (let i = 0; i < 16; i++) assert.ok(!isWhite(r.led(STEP, i)), 'no playhead when stopped');
});

test('Mute + track mutes, Mute alone mutes the selected track', (r) => {
	r.run(30);
	r.withHeld(MUTE, () => r.tap(TRACK, 0));
	assert.equal(r.ui.get('mutes'), 8);
	r.run(1);
	assert.equal(r.dsp.get('sqMutes'), 8);
	r.tap(FUNC, MUTE); // track 1 selected
	r.run(1);
	assert.equal(r.dsp.get('sqMutes'), 9);
	r.tap(FUNC, PLAY);
	r.run(600);
	assert.equal(r.trigCount(0), 0);
	assert.equal(r.trigCount(3), 0);
	assert.ok(r.trigCount(5) > 0);
	r.tap(FUNC, MUTE);
	r.run(600);
	assert.ok(r.trigCount(0) > 0, 'unmuted');
	assert.equal(r.trigCount(3), 0);
});

test('Delete + track clears the track', (r) => {
	r.run(30);
	r.withHeld(DELETE, () => r.tap(TRACK, 2)); // track 2 is empty anyway
	r.withHeld(DELETE, () => r.tap(TRACK, 3));
	assert.equal(r.param('seq_d1_steps'), 0);
	assert.equal(r.param('seq_sn_steps'), 0x1010);
	assert.equal(r.ui.get('sel'), 0, 'selection unchanged');
});

test('< / > change the length, Shift + < / > the tempo', (r) => {
	r.run(30);
	r.tap(NAV, 2);
	r.tap(NAV, 2);
	assert.equal(r.param('seq_d1_length'), 14);
	r.run(30);
	assert.ok(isOff(r.led(STEP, 15)) && isOff(r.led(STEP, 14)));
	assert.ok(isDim(r.led(STEP, 13)) && !isOff(r.led(STEP, 13)));
	for (let i = 0; i < 5; i++) r.tap(NAV, 3);
	assert.equal(r.param('seq_d1_length'), 16);
	r.withHeld(SHIFT, () => {
		r.tap(NAV, 3);
		r.tap(NAV, 3);
	});
	assert.equal(r.param('bpm'), 122);
	r.run(30);
	assert.equal(r.ui.get('bpm'), 122);
	r.withHeld(SHIFT, () => r.tap(NAV, 2));
	assert.equal(r.param('bpm'), 121);
});

test('tempo set elsewhere (parameter view, preset) reaches the UI', (r) => {
	r.dsp.set('bpm', 90);
	r.dsp.set('seq_d2_steps', 0xff);
	r.run(30);
	assert.equal(r.ui.get('bpm'), 90);
	assert.equal(r.ui.get('STEPS')[1], 0xff);
	r.withHeld(SHIFT, () => r.tap(NAV, 3));
	assert.equal(r.param('bpm'), 91);
});

test('live recording quantizes to the nearest step', (r) => {
	r.withHeld(DELETE, () => r.tap(TRACK, 0));
	r.tap(FUNC, RECORD);
	r.tap(FUNC, PLAY);
	r.run(300);
	// early in a step: recorded on that step
	let s = r.nextStep();
	r.run(20);
	r.press(PAD, 3, 80);
	const early = s % 16;
	// late in a step: recorded on the next one, with an accent for a hard hit
	s = r.nextStep();
	r.run(80);
	r.press(PAD, 3, 120);
	const late = (s + 1) % 16;
	r.run(30);
	assert.equal(r.param('seq_sn_steps'), (1 << early) | (1 << late), `steps ${early}, ${late}`);
	assert.equal(r.param('seq_sn_accents'), 1 << late);
	// not while recording is off
	r.tap(FUNC, RECORD);
	r.press(PAD, 4, 100);
	r.run(30);
	assert.equal(r.param('seq_cy_steps'), 0);
	assert.deepEqual(r.led(FUNC, RECORD), [0.15, 0.15, 0.15]);
});

test('external MIDI: channels 1-7 and the GM drum map on channel 10', (r) => {
	r.run(10);
	r.ui.input(1, [MIDI, 38, 100, 10]);
	r.ui.input(1, [MIDI, 72, 90, 2]);
	r.ui.input(1, [MIDI, 70, 100, 16]); // Move's own pads
	r.ui.input(1, [MIDI, 60, 0, 1]); // note off
	r.ui.input(1, [MIDI, 60, 100, 9]);
	r.flush();
	r.run(5);
	assert.equal(r.trigCount(3), 1);
	assert.equal(r.trigCount(1), 1);
	assert.equal(r.dsp.get('tNote')[1], 72);
	for (const t of [0, 2, 4, 5, 6]) assert.equal(r.trigCount(t), 0);
});

test('LEDs are only sent when they change', (r) => {
	r.run(1000);
	r.ledMsgs = [];
	r.run(1000); // 100 status ticks, nothing playing
	assert.ok(r.ledMsgs.length <= 100, `${r.ledMsgs.length} LED messages`);
});

console.log('UI tests passed');
