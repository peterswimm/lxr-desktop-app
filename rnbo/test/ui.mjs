// Tests the Move codebox (sequencer + control surface) together with the engine, wired
// like the RNBO patch (tools/patch.py): input driver lists in, LED lists out, triggers
// through [sig~] into the engine, parameter locks into auto_<track>, every [param]
// reporting its value to the codebox and settable from it through [route].
//   node rnbo/test/ui.mjs
import assert from 'assert/strict';
import { readFileSync } from 'fs';
import { dirname, join } from 'path';
import { fileURLToPath } from 'url';
import { instantiate, instantiateMessage } from './codebox.mjs';

const HERE = dirname(fileURLToPath(import.meta.url));
const ENGINE = readFileSync(join(HERE, '../generated/lxr_engine.codebox.js'), 'utf8');
const MOVE = readFileSync(join(HERE, '../generated/lxr_move.codebox.js'), 'utf8');
const META = JSON.parse(readFileSync(join(HERE, '../generated/params.json'), 'utf8'));
const TRACKS = META.tracks;
const SR = 44100;

const PAD = 0, STEP = 1, TRACK = 2, FUNC = 3, NAV = 4, ENC = 6, MIDI = 100;
const CAPTURE = 0, REPEAT = 2, MUTE = 3, DELETE = 4, DUPLICATE = 5, SHIFT = 7, RECORD = 8, PLAY = 9, SESSION = 10;

// the patch's [param]s: name -> { idx, value, engine attribute } (tools/patch.py all_params)
function makeParams() {
	const p = {};
	for (const q of META.params) p[q.name] = { idx: q.par, value: q.default, attr: q.name };
	p.bpm = { idx: 300, value: 120, attr: 'bpm' };
	p.shuffle = { idx: 301, value: 0 };
	p.midi_out = { idx: 302, value: 0 };
	TRACKS.forEach((t, k) => {
		p[`midi_chan_${t}`] = { idx: 303 + k, value: k + 1 };
		p[`midi_note_${t}`] = { idx: 311 + k, value: 0 };
	});
	p.midi_chan_global = { idx: 310, value: 8 };
	return p;
}

class Rig {
	constructor() {
		this.dsp = instantiate(ENGINE, SR).dsp;
		this.ui = instantiateMessage(MOVE).box;
		this.params = makeParams();
		this.byIdx = Object.fromEntries(Object.entries(this.params).map(([n, q]) => [q.idx, n]));
		this.leds = new Map();
		this.ledMsgs = [];
		this.midi = [];
		this.sample = 0;
		this.msAcc = 0;
		// [param] sends its initial value at load
		for (const n of Object.keys(this.params)) this.setParam(n, this.params[n].value);
	}

	// a [param] gets a value (from a parameter view, a preset or the codebox)
	setParam(name, v) {
		const q = this.params[name];
		q.value = v;
		if (q.attr) this.dsp.set(q.attr, v);
		this.ui.input(1, [101, q.idx, v]);
		this.flush();
	}

	flush() {
		for (const [n, v] of this.ui.take()) {
			if (n === 1) {
				this.checkLed(v);
				this.leds.set(`${v[0]},${v[1]}`, v.slice(2));
				this.ledMsgs.push(v);
			} else if (n >= 2 && n <= 8) this.dsp.input(n, v);
			else if (n >= 9 && n <= 15) this.dsp.set(`auto_${TRACKS[n - 9]}`, v);
			else if (n === 16) this.setParam(this.byIdx[v[0]], v[1]);
			else if (n === 17) this.midi.push(v);
			else throw new Error(`unexpected outlet ${n}`);
		}
	}

	checkLed(v) {
		const [zone, addr] = v;
		const bw = zone === NAV || (zone === FUNC && addr !== RECORD && addr !== PLAY && addr !== 1);
		if (bw) assert.ok(v.length === 3 && (v[2] === 0 || v[2] === 1), `b/w LED ${v}`);
		else {
			assert.equal(v.length, 5, `RGB LED ${v}`);
			for (const c of v.slice(2)) assert.ok(c >= 0 && c <= 1, `RGB LED ${v}`);
		}
		assert.ok(addr >= 0 && addr < [32, 16, 4, 12, 4, 0, 8][zone], `LED address ${v}`);
	}

	press(zone, addr, val = 127) {
		this.ui.input(1, [zone, addr, val]);
		this.flush();
	}

	release(zone, addr) {
		this.press(zone, addr, 0);
	}

	tap(zone, addr) {
		this.press(zone, addr);
		this.release(zone, addr);
	}

	held(zone, addr, fn) {
		this.press(zone, addr);
		fn();
		this.release(zone, addr);
	}

	turn(enc, delta) {
		this.ui.input(1, [ENC, enc, 127, delta]);
		this.flush();
	}

	// runs the engine and the [metro 1] clock (4 systicks per ms)
	run(ms) {
		const n = Math.round(ms * SR / 1000);
		for (let i = 0; i < n; i++) {
			this.dsp.perform();
			this.sample++;
			this.msAcc += 1000 / SR;
			if (this.msAcc >= 1) {
				this.msAcc -= 1;
				this.ui.input(2, 4);
				this.flush();
			}
		}
	}

	led(zone, addr) {
		return this.leds.get(`${zone},${addr}`);
	}

	get(name) {
		return this.ui.get(name);
	}

	call(name, ...a) {
		const r = this.ui.call(name, ...a);
		this.flush();
		return r;
	}

	step(t, s, p = this.get('activePattern')) {
		return this.call('isMainStepActive', t, s, p);
	}

	field(t, s, f, p = this.get('activePattern')) {
		return this.call('stepField', p, t, s, f);
	}

	trigCount(t) {
		return this.dsp.get('tCount')[t];
	}
}

const isDim = (c) => Math.max(...c) <= 0.02;
const isOff = (c) => Math.max(...c) === 0;
const lockCode = (name) => (makeParams()[name].idx < 127 ? makeParams()[name].idx + 1 : makeParams()[name].idx);

function test(name, fn) {
	fn(new Rig());
	console.log('ok  ', name);
}

test('starts with a demo beat in pattern 1 and lights it', (r) => {
	r.run(30);
	for (let s = 0; s < 16; s++) assert.equal(r.step(0, s), s % 4 === 0 ? 1 : 0);
	assert.equal(r.step(3, 4), 1);
	for (let i = 0; i < 16; i++) {
		const c = r.led(STEP, i);
		if (i % 4 === 0) assert.ok(c[0] > 0.5 && c[1] === 0, `step ${i} ${c}`);
		else assert.ok(isDim(c), `step ${i} ${c}`);
	}
	assert.deepEqual(r.led(TRACK, 3), [1, 0, 0], 'top track button = track 1, selected');
	assert.deepEqual(r.led(NAV, 0), [1]);
	assert.deepEqual(r.led(ENC, 0), [1, 0, 0], 'encoder 1 LED: bar 1 shown');
	assert.ok(isOff(r.led(ENC, 1)), 'no bar 2 at length 16');
});

test('tap toggles a step, hold keeps it and edits it with the encoders', (r) => {
	r.run(20);
	r.tap(STEP, 2);
	assert.equal(r.step(0, 2), 1);
	r.tap(STEP, 2);
	assert.equal(r.step(0, 2), 0);
	// hold an on step and edit: stays on
	r.press(STEP, 4);
	r.turn(0, -27);   // velocity 127 -> 100
	r.turn(1, -40);   // probability 100 -> 60
	r.turn(2, 12);    // note 60 -> 72
	r.turn(3, 6);     // flam +2 (one per 3 detents) -> x3
	r.turn(4, -9);    // shift 7 -> 4 (-3)
	r.run(20);
	assert.ok(r.led(ENC, 0)[0] > 0.7, 'encoder LEDs show the step');
	r.release(STEP, 4);
	assert.equal(r.step(0, 4), 1);
	assert.deepEqual([0, 1, 2, 3, 4].map((f) => r.field(0, 4, f)), [100, 60, 72, 2, 4]);
	// hold several steps: the edit goes to all of them
	r.press(STEP, 6);
	r.press(STEP, 7);
	r.turn(0, -100);
	r.release(STEP, 6);
	r.release(STEP, 7);
	assert.equal(r.field(0, 6, 0), 27);
	assert.equal(r.field(0, 7, 0), 27);
	assert.equal(r.step(0, 6), 1, 'held off steps come on');
});

test('parameter locks: move a parameter while a step is held', (r) => {
	r.run(20);
	const before = r.params.d1_decay.value;
	r.press(STEP, 8);
	r.setParam('d1_decay', 5);        // a parameter view encoder
	r.setParam('d1_decay', 9);
	r.release(STEP, 8);
	assert.equal(r.params.d1_decay.value, before, 'the parameter goes back on release');
	assert.equal(r.field(0, 8, 5), lockCode('d1_decay'));
	assert.equal(r.field(0, 8, 6), 9);
	assert.equal(r.step(0, 8), 1, 'a locked step stays on');
	// lane 2 through encoder 6
	r.press(STEP, 12);
	r.turn(5, 3);
	r.setParam('d1_cutoff', 20);
	r.turn(7, 5);
	r.release(STEP, 12);
	assert.equal(r.field(0, 12, 7), lockCode('d1_cutoff'));
	assert.equal(r.field(0, 12, 8), 25);
	// playback applies the lock for the step and restores it with the next trigger
	r.tap(FUNC, PLAY);
	const P = () => r.dsp.get('P');
	let seenLock = false;
	let restored = false;
	for (let ms = 0; ms < 2200; ms += 5) {
		r.run(5);
		const d = P()[50];
		if (d === 9) seenLock = true;
		if (seenLock && d === before) restored = true;
	}
	assert.ok(seenLock, 'decay locked to 9 on step 9');
	assert.ok(restored, 'and back afterwards');
	assert.equal(r.dsp.get('O')[50], before, 'the parameter value itself never changed');
});

test('track buttons, + / - pages, mutes, clear and copy', (r) => {
	r.run(20);
	r.tap(TRACK, 0);
	assert.equal(r.get('activeTrack'), 3);
	r.tap(NAV, 0);
	assert.equal(r.get('activeTrack'), 6, 'selection moves with the page');
	r.tap(TRACK, 3);
	assert.equal(r.get('activeTrack'), 4);
	r.tap(TRACK, 0); // no track 8
	assert.equal(r.get('activeTrack'), 4);
	r.tap(NAV, 1);
	assert.equal(r.get('activeTrack'), 0);
	r.held(FUNC, MUTE, () => r.tap(TRACK, 0));
	assert.equal(r.get('mutedTracks'), 8);
	r.tap(FUNC, MUTE);
	assert.equal(r.get('mutedTracks'), 9);
	r.held(FUNC, DUPLICATE, () => r.tap(TRACK, 2));
	for (let s = 0; s < 16; s++) assert.equal(r.step(1, s), r.step(0, s), 'track 1 copied to track 2');
	r.held(FUNC, DELETE, () => r.tap(TRACK, 3));
	for (let s = 0; s < 16; s++) assert.equal(r.step(0, s), 0, 'track 1 cleared');
});

test('bars: Shift + step sets the length, < / > move between bars', (r) => {
	r.run(20);
	r.tap(NAV, 3);
	assert.equal(r.get('bar'), 1);
	r.held(FUNC, SHIFT, () => r.tap(STEP, 15)); // length 32
	assert.equal(r.call('trackLen', 0, 0), 32);
	r.tap(STEP, 0); // step 17
	assert.equal(r.step(0, 16), 1);
	r.run(20);
	assert.ok(r.led(ENC, 1)[0] > 0.9 && r.led(ENC, 0)[0] < 0.2, 'bar 2 lit');
	r.tap(NAV, 2);
	assert.equal(r.get('bar'), 0);
	assert.equal(r.get('follow'), 0);
	r.tap(FUNC, PLAY);
	assert.equal(r.get('follow'), 1, 'Play follows the playhead again');
	r.run(2200);
	assert.equal(r.get('bar'), 1, 'the view follows into bar 2');
});

test('Play runs the ported sequencer into the engine', (r) => {
	r.run(20);
	r.call('setBpm', 120);
	r.tap(FUNC, PLAY);
	r.run(2010);
	assert.equal(r.trigCount(0), 5, 'kick on every beat (4 per 2 s bar + the next downbeat)');
	assert.equal(r.trigCount(3), 2, 'snare on 2 and 4');
	assert.ok(r.trigCount(5) >= 7, 'hats');
	assert.deepEqual(r.led(FUNC, PLAY), [0, 1, 0]);
	r.tap(FUNC, PLAY);
	assert.equal(r.get('running'), 0);
	const n = r.trigCount(0);
	r.run(1000);
	assert.equal(r.trigCount(0), n, 'stopped');
});

test('Shift + < / > change the tempo parameter', (r) => {
	r.run(20);
	r.held(FUNC, SHIFT, () => {
		r.tap(NAV, 3);
		r.tap(NAV, 3);
	});
	assert.equal(r.params.bpm.value, 122);
	assert.equal(r.dsp.get('bpm'), 122, 'the engine (synced LFOs) gets it too');
	assert.equal(r.get('seqTempo'), 122);
	r.setParam('bpm', 90); // a parameter view or preset
	assert.equal(r.get('seqTempo'), 90);
});

test('live recording', (r) => {
	r.run(20);
	r.held(FUNC, DELETE, () => r.tap(TRACK, 0)); // empty snare track
	r.tap(FUNC, RECORD);
	r.tap(FUNC, PLAY);
	r.run(300);
	r.press(PAD, 3, 90);
	r.release(PAD, 3);
	r.run(10);
	const rec = [];
	for (let s = 0; s < 16; s++) if (r.step(3, s)) rec.push(s);
	assert.equal(rec.length, 1, `one recorded step ${rec}`);
	assert.equal(r.field(3, rec[0], 0), 90, 'with its velocity');
	r.tap(FUNC, RECORD);
	r.press(PAD, 3, 90);
	r.run(10);
	assert.equal([...Array(16).keys()].filter((s) => r.step(3, s)).length, 1, 'not when recording is off');
});

test('pattern view: switch, chain, copy, clear', (r) => {
	r.run(20);
	r.tap(FUNC, SESSION);
	assert.equal(r.get('patternView'), 1);
	r.held(FUNC, DUPLICATE, () => r.tap(STEP, 2));
	assert.equal(r.step(0, 0, 2), 1, 'pattern 1 copied to 3');
	r.tap(STEP, 2); // stopped: switches now
	assert.equal(r.get('activePattern'), 2);
	r.run(20);
	assert.deepEqual(r.led(STEP, 2), [0, 1, 0]);
	// chain 3 -> 1 -> 3 ...
	r.press(STEP, 2);
	r.tap(STEP, 0);
	r.release(STEP, 2);
	assert.equal(r.get('songModeEnabled'), 1);
	r.tap(FUNC, PLAY);
	const seen = [];
	for (let i = 0; i < 100; i++) {
		r.run(100);
		const p = r.get('activePattern');
		if (seen[seen.length - 1] !== p) seen.push(p);
	}
	assert.deepEqual(seen.slice(0, 4), [2, 0, 2, 0], `chain plays ${seen}`);
	r.tap(STEP, 1); // single pattern again
	assert.equal(r.get('songModeEnabled'), 0);
	r.held(FUNC, DELETE, () => r.tap(STEP, 2));
	assert.equal(r.call('isPatternEmpty', 2), 1);
});

test('shuffle is kept per pattern', (r) => {
	r.run(20);
	r.setParam('shuffle', 50);
	assert.equal(r.call('patShuffle', 0), 50);
	r.call('setNextPattern', 1);
	assert.equal(r.params.shuffle.value, 0, 'pattern 2 has its own shuffle');
	r.call('setNextPattern', 0);
	assert.equal(r.params.shuffle.value, 50);
	assert.equal(r.get('shuffle'), Math.fround(50 / 127));
});

test('roll on Repeat', (r) => {
	r.run(20);
	r.tap(FUNC, PLAY);
	r.run(50);
	r.press(FUNC, REPEAT);
	r.press(PAD, 8 + 4); // 1/8
	const n = r.trigCount(2);
	r.press(PAD, 2);
	r.run(1000);
	r.release(PAD, 2);
	r.release(FUNC, REPEAT);
	const rolled = r.trigCount(2) - n;
	assert.ok(rolled >= 3 && rolled <= 5, `eighth notes for 1 s at 120 BPM: ${rolled}`);
	const m = r.trigCount(2);
	r.run(500);
	assert.equal(r.trigCount(2), m, 'stops with the pad');
});

test('Euclid generator on Capture + encoders', (r) => {
	r.run(20);
	r.tap(TRACK, 1); // track 3
	r.held(FUNC, CAPTURE, () => {
		r.turn(0, -12); // length 16 -> 12
		r.turn(1, 3);   // steps 4 -> 5
		r.turn(2, 3);   // rotation 1
	});
	assert.equal(r.call('trackLen', 0, 2), 12);
	const on = [...Array(12).keys()].filter((s) => r.step(2, s));
	assert.equal(on.length, 5, `5 of 12: ${on}`);
});

test('external MIDI: channels 1-7, GM drums on 10, muted tracks stay quiet', (r) => {
	r.run(10);
	r.ui.input(1, [MIDI, 38, 100, 10]);
	r.ui.input(1, [MIDI, 72, 90, 2]);
	r.ui.input(1, [MIDI, 70, 100, 16]);
	r.flush();
	r.call('setMute', 0, 1);
	r.ui.input(1, [MIDI, 36, 100, 10]);
	r.flush();
	r.run(5);
	assert.equal(r.trigCount(3), 1);
	assert.equal(r.trigCount(1), 1);
	assert.equal(r.dsp.get('tNote')[1], 72);
	assert.equal(r.trigCount(0), 0);
});

test('MIDI out only when switched on', (r) => {
	r.run(10);
	r.tap(FUNC, PLAY);
	r.run(300);
	assert.equal(r.midi.length, 0);
	r.setParam('midi_out', 1);
	r.run(600);
	assert.ok(r.midi.includes(0xf8), 'clock');
	assert.ok(r.midi.includes(0x90), 'note on, channel 1');
});

test('LEDs are only sent when they change', (r) => {
	r.run(500);
	r.ledMsgs = [];
	r.run(1000); // 100 refreshes, nothing playing
	assert.ok(r.ledMsgs.length <= 100, `${r.ledMsgs.length} LED messages`);
});

console.log('Move UI tests passed');
