// Compares the RNBO engine with the original firmware DSP.
//
//   node rnbo/test/run.mjs [filter] [--wav]
//
// Every scenario runs twice: through lxr_ref (the firmware sources compiled for this
// machine, see ref/build.py) and through the generated codebox~ code (run in Node by
// codebox.mjs). Both start from the factory init kit; the outputs are compared sample
// by sample. --wav also writes both renders to rnbo/test/out/ for listening.
import { execFileSync } from 'child_process';
import { existsSync, mkdirSync, readFileSync, writeFileSync } from 'fs';
import { dirname, join } from 'path';
import { fileURLToPath } from 'url';
import { instantiate } from './codebox.mjs';

const HERE = dirname(fileURLToPath(import.meta.url));
const ROOT = join(HERE, '..');
const SR = 44140;
const N = 64;

const args = process.argv.slice(2);
const wantWav = args.includes('--wav');
const filter = args.find((a) => !a.startsWith('--'));

execFileSync('python3', [join(ROOT, 'tools/build.py')], { stdio: 'inherit' });
execFileSync('python3', [join(HERE, 'ref/build.py')], { stdio: 'inherit' });
const REF = join(HERE, 'ref/build/lxr_ref');
const SRC = readFileSync(join(ROOT, 'generated/lxr_engine.codebox.js'), 'utf8');
const META = JSON.parse(readFileSync(join(ROOT, 'generated/params.json'), 'utf8'));
const PARAMS = Object.fromEntries(META.params.map((p) => [p.name, p]));
const SCAN = META.scan_order;
const BY_PAR = Object.fromEntries(META.params.map((p) => [p.par, p]));
const MOD_TARGETS = PARAMS.d1_lfo_dest.enum.length;
const OUT = join(HERE, 'out');

// the modulatable parameter list baked into the engine must match ParameterArray.c
{
	mkdirSync(join(HERE, 'out/tmp'), { recursive: true });
	const f = join(HERE, 'out/tmp/dumpptr.txt');
	writeFileSync(f, 'dumpptr\n');
	const ptr = execFileSync(REF, [f, '/dev/null']).toString().trim().split('\n').map(Number);
	if (JSON.stringify(ptr) !== JSON.stringify(META.has_ptr)) {
		console.log('FAIL has_ptr list differs from the firmware:', ptr.filter((x) => !META.has_ptr.includes(x)), META.has_ptr.filter((x) => !ptr.includes(x)));
		process.exit(1);
	}
}
const TMP = join(OUT, 'tmp');

// ------------------------------------------------------------------ scenario helpers
const S = [];
// opts.chaotic: the scenario feeds back through a very high gain stage, so 1-LSB float
// differences grow into different (but equally loud) waveforms; it only has to match
// the reference's loudness over time.
function scenario(name, build, opts = {}) {
	const ev = [];
	const api = {
		set(values) {
			ev.push({ set: values });
		},
		trig(slot, vel = 127, note = 60) {
			ev.push({ trig: [slot, vel, note] });
		},
		render(blocks) {
			ev.push({ render: blocks });
		},
		bpm(b) {
			ev.push({ bpm: b });
		},
		hit(slot, blocks = 100, vel = 127, note = 60) {
			ev.push({ trig: [slot, vel, note] });
			ev.push({ render: blocks });
		},
	};
	build(api);
	S.push({ name, events: ev, ...opts });
}

function rng(seed) {
	let s = seed >>> 0;
	return () => {
		s = (Math.imul(s, 1664525) + 1013904223) >>> 0;
		return s / 4294967296;
	};
}

const VOICES = ['d1', 'd2', 'd3', 'sn', 'cy', 'hh'];
const slotOf = (v) => VOICES.indexOf(v);

// ------------------------------------------------------------------ scenarios
scenario('initkit', (s) => {
	for (let slot = 0; slot < 7; slot++) s.hit(slot, 120);
	s.hit(0, 10);
	s.hit(5, 10);
	s.hit(6, 40, 90);
	s.hit(5, 80, 30);
});

for (let slot = 0; slot < 7; slot++) {
	scenario(`voice-${slot}`, (s) => {
		s.hit(slot, 60);
		s.hit(slot, 60, 40);
		s.hit(slot, 60, 127, 48);
	});
}

scenario('notes', (s) => {
	for (const note of [36, 48, 60, 67, 72, 84, 96]) for (const slot of [0, 3, 4, 5]) s.hit(slot, 40, 100, note);
});

for (const v of ['d1', 'd2', 'd3']) {
	scenario(`${v}-waves`, (s) => {
		for (let w = 0; w < 6; w++) {
			for (const mode of [0, 1]) {
				s.set({ [`${v}_wave`]: w, [`${v}_fm_wave`]: (w + 2) % 6, [`${v}_fm_mode`]: mode, [`${v}_fm_amount`]: 90,
					[`${v}_fm_coarse`]: 40 + 7 * w, [`${v}_pwm`]: 20 * w, [`${v}_decay`]: 50, [`${v}_coarse`]: 30 + 10 * w });
				s.hit(slotOf(v), 70);
			}
		}
	});
}

scenario('drum-envelopes', (s) => {
	const cases = [
		{ d1_attack: 0, d1_decay: 60, d1_slope: 0, d1_pitch_decay: 20, d1_pitch_slope: 0, d1_pitch_amount: 127 },
		{ d1_attack: 40, d1_decay: 90, d1_slope: 127, d1_pitch_decay: 80, d1_pitch_slope: 126, d1_pitch_amount: 60 },
		{ d1_attack: 100, d1_decay: 30, d1_slope: 64, d1_pitch_decay: 127, d1_pitch_slope: 64, d1_pitch_amount: 10 },
		{ d1_attack: 5, d1_decay: 127, d1_slope: 20, d1_pitch_decay: 0, d1_pitch_slope: 100, d1_pitch_amount: 90 },
	];
	for (const c of cases) {
		s.set(c);
		s.hit(0, 150);
		s.hit(0, 20, 64);
	}
});

for (const v of VOICES) {
	scenario(`${v}-filters`, (s) => {
		for (let t = 0; t < 8; t++) {
			s.set({ [`${v}_filter_type`]: t, [`${v}_cutoff`]: 30 + 12 * t, [`${v}_reso`]: 15 * t + 10,
				[`${v}_filter_drive`]: 16 * t, [v === 'hh' ? 'hh_decay_closed' : `${v}_decay`]: 60, [`${v}_wave`]: (t % 4) });
			s.hit(Math.min(slotOf(v), 5), 50);
		}
	});
}

for (const v of ['d1', 'sn', 'cy', 'hh']) {
	scenario(`${v}-transients`, (s) => {
		for (let w = 0; w < 14; w++) {
			s.set({ [`${v}_trans_wave`]: w, [`${v}_trans_vol`]: 127 - 5 * w, [`${v}_trans_freq`]: (w * 37) % 128 });
			s.hit(slotOf(v), 45);
		}
	});
}

scenario('snare', (s) => {
	for (const [mix, noise, rep, slope] of [[0, 127, 0, 10], [64, 90, 3, 60], [127, 20, 8, 120], [100, 127, 1, 0]]) {
		s.set({ sn_mix: mix, sn_noise_freq: noise, sn_repeat: rep, sn_slope: slope, sn_attack: rep * 10, sn_decay: 40,
			sn_wave: rep % 6, sn_pitch_amount: 90, sn_pitch_decay: 30 });
		s.hit(3, 120);
	}
});

for (const v of ['cy', 'hh']) {
	scenario(`${v}-metal`, (s) => {
		for (let k = 0; k < 6; k++) {
			s.set({ [`${v}_wave`]: k, [`${v}_mod1_wave`]: (k + 1) % 6, [`${v}_mod2_wave`]: (k + 3) % 6,
				[`${v}_mod1_coarse`]: 20 * k, [`${v}_mod2_coarse`]: 127 - 15 * k, [`${v}_mod1_gain`]: 25 * k, [`${v}_mod2_gain`]: 127 - 20 * k,
				[`${v}_pwm`]: 21 * k, [`${v}_coarse`]: 40 + 10 * k });
			if (v === 'cy') s.set({ cy_repeat: k, cy_decay: 40 });
			else s.set({ hh_decay_closed: 10 + 5 * k, hh_decay_open: 50 + 10 * k });
			s.hit(v === 'cy' ? 4 : 5 + (k % 2), 60);
		}
	});
}

scenario('mix-routing', (s) => {
	for (let r = 0; r < 7; r++) {
		const set = { fx_type: 0, fx_out: r % 6 };
		VOICES.forEach((v, i) => {
			set[`${v}_out`] = (r + i) % 7;
			set[`${v}_pan`] = (r * 17 + i * 23) % 128;
			set[`${v}_volume`] = 127 - r * 9;
			set[`${v}_drive`] = (r * 20 + i * 7) % 128;
			set[`${v}_srate`] = 127 - ((r * 13 + i * 11) % 120);
		});
		set.all_srate = 127 - r * 10;
		s.set(set);
		for (let slot = 0; slot < 7; slot++) s.hit(slot, 8);
		s.render(30);
	}
});

scenario('velocity', (s) => {
	const dests = [2, 22, 7, 129, 140, 195, 210, 228, 1];
	dests.forEach((d, i) => {
		const set = {};
		VOICES.forEach((v, k) => {
			set[`${v}_vel_dest`] = (d + 17 * k) % MOD_TARGETS;
			set[`${v}_vel_amount`] = (40 + 30 * k + 11 * i) % 128;
			set[`${v}_vel_volume`] = (i + k) % 2;
		});
		s.set(set);
		for (let slot = 0; slot < 7; slot++) s.hit(slot, 20, 20 + 15 * slot);
		s.hit(0, 30, 127);
	});
});

scenario('lfos', (s) => {
	for (let w = 0; w < 8; w++) {
		const set = {};
		VOICES.forEach((v, k) => {
			set[`${v}_lfo_wave`] = (w + k) % 8;
			set[`${v}_lfo_rate`] = (30 + 25 * k + 9 * w) % 128;
			set[`${v}_lfo_amount`] = 60 + 8 * k;
			set[`${v}_lfo_dest`] = [22, 60, 98, 135, 173, 211, 6, 45, 100, 150][(w + k) % 10];
			set[`${v}_lfo_retrig`] = (w + k) % 7;
			set[`${v}_lfo_phase`] = (w * 30 + k) % 128;
			set[`${v}_lfo_sync`] = w < 4 ? 0 : (w * 3 + k) % 12;
		});
		s.set(set);
		s.bpm(90 + 20 * w);
		for (let slot = 0; slot < 7; slot++) s.hit(slot, 25);
	}
});

const FX_SETTINGS = [
	{ fx_type: 1, fx_drive_type: 0 },
	{ fx_type: 1, fx_drive_type: 1 },
	{ fx_type: 1, fx_drive_type: 2 },
	{ fx_type: 2, fx_ring_wave: 0 },
	{ fx_type: 2, fx_ring_wave: 3 },
	{ fx_type: 2, fx_ring_wave: 5 },
	{ fx_type: 3, fx_comp_ratio: 3 },
	{ fx_type: 3, fx_comp_ratio: 5 },
	{ fx_type: 4, fx_delay_type: 0 },
	{ fx_type: 4, fx_delay_type: 1 },
];
FX_SETTINGS.forEach((fx, i) => {
	const chaotic = fx.fx_type === 1 && fx.fx_drive_type === 2;
	scenario(`fx-${i}`, (s) => {
		const route = {};
		VOICES.forEach((v) => (route[`${v}_out`] = 6));
		s.set({ ...route, fx_mix: 30 + 7 * i, fx_out: i % 6, fx_p1: 20 + 9 * i, fx_p2: 100 - 7 * i, fx_p3: 64 + 5 * i, fx_p4: 40 + 8 * i, ...fx });
		for (let slot = 0; slot < 7; slot++) s.hit(slot, 30);
		s.set({ fx_p1: 90, fx_p2: 30, fx_mix: 100 });
		for (let slot = 0; slot < 7; slot++) s.hit(slot, 20);
		s.render(150);
	}, { chaotic });
});

// the "clip" (RAT) drive without its feedback path, which is not chaotic
scenario('fx-rat', (s) => {
	const route = {};
	VOICES.forEach((v) => (route[`${v}_out`] = 6));
	s.set({ ...route, fx_type: 1, fx_drive_type: 2, fx_mix: 80, fx_p1: 3, fx_p2: 0, fx_p3: 70, fx_p4: 100 });
	for (let slot = 0; slot < 7; slot++) s.hit(slot, 30);
	s.set({ fx_out: 3, fx_p1: 1, fx_p3: 20 });
	for (let slot = 0; slot < 7; slot++) s.hit(slot, 30);
});

scenario('fx-lfo', (s) => {
	s.set({ d1_out: 6, sn_out: 6, hh_out: 6, fx_type: 4, fx_mix: 60, fx_p1: 70, fx_p2: 80, fx_p3: 90, fx_p4: 70,
		d1_lfo_dest: 36, d1_lfo_amount: 90, d1_lfo_rate: 40, sn_lfo_dest: 151, sn_lfo_amount: 50, sn_lfo_rate: 70 });
	for (let k = 0; k < 6; k++) {
		s.hit(0, 30);
		s.hit(3, 30);
		s.hit(5, 30);
	}
	s.render(200);
});

// At pitch slope 127 the firmware divides by zero (the pitch becomes NaN and the
// oscillator stops); the port deliberately clamps instead, so the fuzz avoids that
// value, including via modulation of a pitch slope.
const PITCH_SLOPE_PARS = new Set(META.params.filter((p) => p.name.endsWith('_pitch_slope')).map((p) => p.par));
const PITCH_SLOPE_TARGETS = new Set(META.mod_targets.map((par, i) => (PITCH_SLOPE_PARS.has(par) ? i : -1)).filter((i) => i >= 0));

for (let seed = 1; seed <= 12; seed++) {
	scenario(`fuzz-${seed}`, (s) => {
		const r = rng(seed * 7919);
		for (let round = 0; round < 6; round++) {
			const set = {};
			for (const p of META.params) {
				if (r() < (round === 0 ? 1 : 0.15)) {
					let v = Math.floor(r() * (p.max + 1));
					if (p.name.endsWith('_pitch_slope')) v = Math.min(v, 126);
					if (p.name.endsWith('_dest') && PITCH_SLOPE_TARGETS.has(v)) v = 0;
					set[p.name] = v;
				}
			}
			s.set(set);
			if (r() < 0.3) s.bpm(60 + Math.floor(r() * 140));
			for (let k = 0; k < 10; k++) {
				const slot = Math.floor(r() * 7);
				s.trig(slot, 1 + Math.floor(r() * 127), 30 + Math.floor(r() * 70));
				if (r() < 0.3) s.trig((slot + 3) % 7, 1 + Math.floor(r() * 127), 60);
				s.render(1 + Math.floor(r() * 40));
			}
		}
	});
}

// extra scenarios from a JSON file: [{ "name": ..., "events": [...] }]
if (process.env.LXR_SCENARIOS) S.push(...JSON.parse(readFileSync(process.env.LXR_SCENARIOS, 'utf8')));

// ------------------------------------------------------------------ runners
function initKitValues() {
	const v = {};
	for (const p of META.params) v[p.name] = p.default;
	return v;
}

// The C engine starts from power-on state; send the init kit first, like the RNBO
// engine does on its first block. Parameters are always sent in scanParams() order.
function cScript(events) {
	const lines = [];
	const emit = (values) => {
		const pars = new Set(Object.keys(values).map((n) => PARAMS[n].par));
		for (const par of SCAN) {
			if (!pars.has(par)) continue;
			const p = BY_PAR[par];
			const val = values[p.name];
			const lfo = p.name.match(/^(\w+)_lfo_dest$/);
			const vel = p.name.match(/^(\w+)_vel_dest$/);
			const target = META.mod_targets[val];
			if (lfo) lines.push(`lfodest ${slotOf(lfo[1])} ${target}`);
			else if (vel) lines.push(`velodest ${slotOf(vel[1])} ${target}`);
			else lines.push(`param ${par} ${val}`);
		}
	};
	emit(initKitValues());
	for (const e of events) {
		if (e.set) emit(e.set);
		else if (e.trig) lines.push(`trig ${e.trig.join(' ')}`);
		else if (e.render) lines.push(`render ${e.render}`);
		else if (e.bpm) lines.push(`bpm ${e.bpm}`);
	}
	return lines.join('\n') + '\n';
}

function runC(name, events) {
	mkdirSync(TMP, { recursive: true });
	const script = join(TMP, `${name}.txt`);
	const raw = join(TMP, `${name}.raw`);
	writeFileSync(script, cScript(events));
	execFileSync(REF, [script, raw]);
	const b = readFileSync(raw);
	return new Int16Array(b.buffer, b.byteOffset, b.length / 2);
}

function runRnbo(events) {
	const { dsp } = instantiate(SRC, SR);
	const frames = [];
	let count = 0;
	const trig = new Array(8).fill(0);
	for (const e of events) {
		if (e.set) for (const [k, v] of Object.entries(e.set)) dsp.set(k, v);
		else if (e.bpm) dsp.set('bpm', e.bpm);
		else if (e.trig) {
			const [slot, vel, note] = e.trig;
			count++;
			trig[slot] = count * 16384 + note * 128 + vel;
			dsp.input(slot + 1, trig[slot]);
		} else if (e.render) {
			for (let i = 0; i < e.render * N; i++) {
				dsp.perform();
				const o = dsp.output();
				// same channel order as lxr_ref: output 2 (out3/out4) then output 1 (out1/out2)
				frames.push(o[2] * 32768, o[3] * 32768, o[0] * 32768, o[1] * 32768);
			}
		}
	}
	return Int16Array.from(frames);
}

function compare(ref, got) {
	const n = Math.min(ref.length, got.length);
	let maxDiff = 0;
	let diffCount = 0;
	let sig = 0;
	let err = 0;
	let firstDiff = -1;
	for (let i = 0; i < n; i++) {
		const d = Math.abs(ref[i] - got[i]);
		if (d > 0) {
			diffCount++;
			if (firstDiff < 0) firstDiff = i;
		}
		maxDiff = Math.max(maxDiff, d);
		sig += ref[i] * ref[i];
		err += d * d;
	}
	const snr = err === 0 ? Infinity : 10 * Math.log10(sig / err);
	return { maxDiff, diffCount, snr, firstDiff, lenOk: ref.length === got.length, n, sig };
}

// RMS per 1024-frame window, in dB
function envelope(x) {
	const out = [];
	for (let i = 0; i + 4096 <= x.length; i += 4096) {
		let e = 0;
		for (let k = i; k < i + 4096; k++) e += x[k] * x[k];
		out.push(10 * Math.log10(e / 4096 + 1));
	}
	return out;
}

function envelopeDiff(ref, got) {
	const a = envelope(ref);
	const b = envelope(got);
	let worst = 0;
	for (let i = 0; i < a.length; i++) if (a[i] > 40) worst = Math.max(worst, Math.abs(a[i] - b[i]));
	return worst;
}

function wav(path, data, channels) {
	const b = Buffer.alloc(44 + data.length * 2);
	b.write('RIFF', 0);
	b.writeUInt32LE(36 + data.length * 2, 4);
	b.write('WAVEfmt ', 8);
	b.writeUInt32LE(16, 16);
	b.writeUInt16LE(1, 20);
	b.writeUInt16LE(channels, 22);
	b.writeUInt32LE(SR, 24);
	b.writeUInt32LE(SR * channels * 2, 28);
	b.writeUInt16LE(channels * 2, 32);
	b.writeUInt16LE(16, 34);
	b.write('data', 36);
	b.writeUInt32LE(data.length * 2, 40);
	for (let i = 0; i < data.length; i++) b.writeInt16LE(data[i], 44 + 2 * i);
	writeFileSync(path, b);
}


let failed = 0;
for (const sc of S) {
	if (filter && !sc.name.includes(filter)) continue;
	const ref = runC(sc.name, sc.events);
	const got = runRnbo(sc.events);
	const c = compare(ref, got);
	const silent = c.sig === 0;
	// The port must follow the firmware closely: tiny differences come from float vs
	// double rounding (a few LSB), anything else is a porting error.
	let ok = c.lenOk && !silent && c.snr > 70;
	let note = '';
	if (sc.chaotic) {
		const e = envelopeDiff(ref, got);
		ok = c.lenOk && !silent && e < 3;
		note = `  (chaotic: loudness within ${e.toFixed(1)} dB)`;
	}
	if (!ok) failed++;
	const where = c.firstDiff < 0 ? '' : ` first@${Math.floor(c.firstDiff / 4)}(ch${c.firstDiff % 4})`;
	console.log(`${ok ? 'ok  ' : 'FAIL'} ${sc.name.padEnd(16)} snr ${c.snr === Infinity ? ' exact' : c.snr.toFixed(1).padStart(6)} dB` +
		`  maxdiff ${String(c.maxDiff).padStart(5)}  diffs ${(100 * c.diffCount / c.n).toFixed(2).padStart(6)}%${where}${silent ? '  SILENT' : ''}${note}`);
	if (wantWav) {
		mkdirSync(OUT, { recursive: true });
		let pos = 0;
		const timeline = sc.events.map((e) => {
			const r = { at: pos, ...e };
			if (e.render) pos += e.render * N;
			return r;
		});
		writeFileSync(join(OUT, `${sc.name}-events.json`), JSON.stringify(timeline));
		wav(join(OUT, `${sc.name}-ref.wav`), ref, 4);
		wav(join(OUT, `${sc.name}-rnbo.wav`), got, 4);
	}
}
console.log(failed ? `${failed} scenario(s) failed` : 'all scenarios match');
process.exit(failed ? 1 : 0);
