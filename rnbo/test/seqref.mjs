// Checks the sequencer port (src/lxr_seq.cb.js) against the firmware's sequencer
// compiled for this machine (ref/seq_driver.c): every scenario script runs through
// both, and the printed events (voice triggers, automation, MIDI, note offs, pattern
// dumps, playback state) must be identical.
//   node rnbo/test/seqref.mjs [filter]
import { execFileSync } from 'child_process';
import { mkdirSync, readFileSync, writeFileSync } from 'fs';
import { dirname, join } from 'path';
import { fileURLToPath } from 'url';
import { instantiateMessage } from './codebox.mjs';

const HERE = dirname(fileURLToPath(import.meta.url));
const SEQ = readFileSync(join(HERE, '../src/lxr_seq.cb.js'), 'utf8');
const OUT = join(HERE, 'out');
const REF = join(HERE, 'ref/build/seq_ref');
const filter = process.argv[2] || '';

// the port's outputs, logged in the reference's event format
const SHIM = `
function emitTrigger(v, n, vel) { listout2 = [1, systick, v, n, vel]; }
function emitNoteOff(v) { listout2 = [2, systick, v]; }
function emitAuto(v, d1, v1, d2, v2) { listout2 = [3, systick, v, d1, v1, d2, v2]; }
function emitMidi(st, d1, d2, len) { listout2 = [4, systick, st, d1, d2, len]; }
function emitShuffle(v) { }
`;

// automationNode.c: the engine keeps these nodes; here they turn emitAuto into the
// firmware's ccHandler calls (original values are all 0 in seq_ref)
class AutoNodes {
	constructor() {
		this.dest = Array.from({ length: 7 }, () => [255, 255]);
	}
	cc(tick, dest, val) {
		return dest > 127 ? `C ${tick} 244 ${dest - 128} ${val} 0` : `C ${tick} 176 ${dest} ${val} 0`;
	}
	apply(tick, v, d1, v1, d2, v2) {
		const out = [];
		const n = this.dest[v];
		[d1, d2].forEach((d, k) => {
			if (n[k] !== 255) out.push(this.cc(tick, n[k], 0));
			n[k] = d;
		});
		[v1, v2].forEach((val, k) => {
			if (n[k] !== 255) out.push(this.cc(tick, n[k], val));
		});
		return out;
	}
}

function runPort(script, quirk = 1) {
	const { box } = instantiateMessage(SEQ + SHIM);
	const nodes = new AutoNodes();
	const lines = [];
	const c = (name, ...a) => box.call(name, ...a);
	const flush = () => {
		for (const [, e] of box.take()) {
			const [kind, tick, ...r] = e;
			if (kind === 1) lines.push(`N ${tick} ${r[0]} ${r[1]} ${r[2]}`);
			else if (kind === 2) lines.push(`F ${tick} ${r[0]}`);
			else if (kind === 3) lines.push(...nodes.apply(tick, ...r));
			else if (kind === 4) lines.push(`M ${tick} ${r.join(' ')}`);
		}
	};
	c('seqInit');
	box.setv('firmwareDeltaTQuirk', quirk);
	for (let raw of script.split('\n')) {
		raw = raw.replace(/#.*/, '').trim();
		if (!raw) continue;
		const [cmd, ...s] = raw.split(/\s+/);
		const a = s.map(Number);
		switch (cmd) {
			case 'bpm': c('setBpm', a[0]); break;
			case 'shuffle': c('setShuffle', Math.fround(a[0] / 127)); break;
			case 'patshuffle': c('setPatShuffle', a[0], a[1]); break;
			case 'step': c('setStepRaw', ...a); break;
			case 'main': c('setMainStep', ...a); break;
			case 'len': c('setTrackLengthOf', ...a); break;
			case 'run': c('runStop', a[0]); break;
			case 'mute': c('setMute', a[0], a[1]); break;
			case 'next': c('setNextPattern', a[0]); break;
			case 'song': for (let i = 0; i < 64; i++) c('setSongEntry', i, i < a.length ? a[i] : 64); break;
			case 'songmode': c('setSongModeEnabled', a[0]); break;
			case 'rollrate': c('setRollRate', a[0]); break;
			case 'roll': c('setRoll', a[0], a[1]); break;
			case 'rec': c('setRecordingMode', a[0]); break;
			case 'erase': c('setErasingMode', a[0]); break;
			case 'track': box.setv('activeTrack', a[0]); break;
			case 'shown': box.setv('shownPattern', a[0]); break;
			case 'note': c('voiceNoteOn', a[0], a[2], a[1]); c('addNote', a[0], a[1], a[2]); break;
			case 'autotrack': c('setActiveAutomationTrack', a[0]); break;
			case 'arm': c('armAutomationStep', ...a); break;
			case 'autorec': c('recordAutomation', box.get('activeTrack'), a[0], a[1]); break;
			case 'euklid':
				c('euklidSetLength', a[0], a[1]);
				c('euklidSetRotation', a[0], a[3], a[4]);
				c('euklidSetSteps', a[0], a[2], a[4]);
				break;
			case 'cleartrack': c('clearTrack', ...a); break;
			case 'clearpattern': c('clearPattern', a[0]); break;
			case 'clearautom': c('clearAutomation', ...a); break;
			case 'copytrack': c('copyTrack', ...a); break;
			case 'copypattern': c('copyPattern', ...a); break;
			case 'tracklen': c('setTrackLength', ...a); break;
			case 'midichan': box.setv(`midiChan[${a[0]}]`, a[1]); break;
			case 'ticks': c('seqClock', a[0]); break;
			case 'dump': {
				flush();
				const d = c('trackData', a[0], a[1]);
				const f = (w, sh, bits) => Math.floor(w / 2 ** sh) % 2 ** bits;
				const steps = [];
				for (let i = 0; i < 64; i++) {
					const A = d[5 + i], B = d[69 + i];
					steps.push([f(A, 0, 8), f(A, 8, 8), f(A, 16, 8), f(A, 24, 4), f(A, 28, 4),
						f(B, 0, 8), f(B, 8, 8), f(B, 16, 8), f(B, 24, 8)].join(','));
				}
				lines.push(`D ${a[0]} ${a[1]} len ${d[0]} main ${d.slice(1, 5).join(' ')} steps ${steps.join(' ')}`);
				break;
			}
			case 'state': {
				flush();
				const idx = Array.from(box.get('stepIndex'));
				lines.push(`S ${box.get('systick')} pat ${box.get('activePattern')} ${box.get('pendingPattern')} cnt ${box.get('prescaleCounter')} master ${box.get('masterStepCnt')} song ${box.get('songPos')} steps ${idx.join(' ')}`);
				break;
			}
			default: throw new Error('unknown command ' + cmd);
		}
		flush();
	}
	return lines;
}

// ---------------------------------------------------------------- scenarios
function rng(seed) {
	let s = seed >>> 0;
	return (n) => {
		s = (Math.imul(s, 1103515245) + 12345) >>> 0;
		return Math.floor((s / 4294967296) * n);
	};
}

const scenarios = [];
const scenario = (name, fn) => scenarios.push({ name, fn });

const stepLine = (p, t, s, o = {}) =>
	`step ${p} ${t} ${s} ${o.vol ?? 127} ${o.prob ?? 100} ${o.note ?? 60} ${o.flam ?? 0} ${o.shift ?? 7} ${o.p1n ?? 255} ${o.p1v ?? 0} ${o.p2n ?? 255} ${o.p2v ?? 0}`;

scenario('basic', () => [
	'main 0 0 0 1', 'main 0 0 4 1', 'main 0 0 8 1', 'main 0 0 12 1',
	'main 0 3 4 1', 'main 0 3 12 1',
	...[0, 2, 4, 6, 8, 10, 12, 14].map((s) => `main 0 5 ${s} 1`),
	stepLine(0, 5, 2, { vol: 64, note: 72 }),
	'run 1', 'ticks 6000', 'state', 'run 0', 'ticks 100', 'state',
]);

scenario('flam-shift', () => {
	const l = [];
	for (let f = 0; f <= 5; f++) {
		l.push(`main 0 ${f} ${2 * f} 1`, stepLine(0, f, 2 * f, { flam: f, vol: 100 + f }));
	}
	for (let sh = 0; sh <= 14; sh++) {
		l.push(`main 0 6 ${sh} 1`, stepLine(0, 6, sh, { shift: sh, flam: sh % 6, note: 40 + sh }));
	}
	l.push('len 0 6 15');
	l.push('bpm 97', 'run 1', 'ticks 9000', 'state');
	return l;
});

scenario('shift-wrap', () => [
	'len 0 0 4', 'main 0 0 0 1', stepLine(0, 0, 0, { shift: 0 }),
	'main 0 0 3 1', stepLine(0, 0, 3, { shift: 14, flam: 2 }),
	'len 0 1 5', 'main 0 1 4 1', stepLine(0, 1, 4, { shift: 10, flam: 5 }), 'main 0 1 0 1', stepLine(0, 1, 0, { shift: 2 }),
	'run 1', 'ticks 7000', 'state',
]);

for (const sh of [0, 20, 64, 127]) {
	for (const bpm of [90, 120, 173]) {
		scenario(`shuffle-${sh}-${bpm}`, () => [
			...Array.from({ length: 16 }, (_, s) => `main 0 5 ${s} 1`),
			...Array.from({ length: 16 }, (_, s) => stepLine(0, 5, s, { flam: s % 6, shift: (s * 5) % 15 })),
			'main 0 0 0 1', 'main 0 0 7 1',
			`bpm ${bpm}`, `shuffle ${sh}`, 'run 1', 'ticks 8000', 'state',
		]);
	}
}

scenario('probability', () => {
	const l = [];
	for (let t = 0; t < 7; t++) {
		for (let s = 0; s < 16; s++) {
			l.push(`main 0 ${t} ${s} 1`, stepLine(0, t, s, { prob: (t * 17 + s * 7) % 101, shift: (s + t) % 15, flam: t % 6 }));
		}
	}
	l.push('run 1', 'ticks 16000', 'state');
	return l;
});

scenario('polymeter-64', () => {
	const l = [];
	const lens = [64, 3, 16, 33, 7, 1, 48];
	lens.forEach((n, t) => {
		l.push(`len 0 ${t} ${n}`);
		for (let s = 0; s < n; s += 1 + (t % 3)) l.push(`main 0 ${t} ${s} 1`, stepLine(0, t, s, { note: (s + 30 + t) % 128, vol: (s * 9) % 128 }));
	});
	l.push('bpm 150', 'run 1', 'ticks 30000', 'state');
	return l;
});

scenario('mutes-stop-start', () => [
	...Array.from({ length: 7 }, (_, t) => `main 0 ${t} ${t * 2} 1`),
	...Array.from({ length: 7 }, (_, t) => `main 0 ${t} ${t * 2 + 1} 1`),
	'run 1', 'ticks 1500', 'mute 2 1', 'mute 4 1', 'ticks 2000', 'mute 2 0', 'ticks 700',
	'mute 7 1', 'ticks 1200', 'run 0', 'ticks 333', 'run 1', 'ticks 2500', 'bpm 61', 'ticks 3000',
	'run 0', 'run 1', 'ticks 2000', 'state',
]);

scenario('patterns', () => {
	const l = [];
	for (let p = 0; p < 4; p++) {
		l.push(`patshuffle ${p} ${p * 30}`);
		for (let t = 0; t < 7; t++) l.push(`main ${p} ${t} ${(p + t) % 16} 1`, `len ${p} ${t} ${8 + p * 2}`);
	}
	l.push('next 2', 'state', 'run 1', 'ticks 1000', 'next 1', 'ticks 5000', 'state', 'next 3', 'ticks 300', 'next 0',
		'ticks 6000', 'state', 'run 0', 'next 3', 'state', 'run 1', 'ticks 3000', 'state');
	return l;
});

scenario('song', () => {
	const l = [];
	for (let p = 0; p < 4; p++) {
		for (let t = 0; t < 3; t++) l.push(`main ${p} ${t} ${p * 3 + t} 1`, `len ${p} ${t} ${4 + p}`);
	}
	l.push('song 1 3 0 2 65', 'songmode 1', 'run 1', 'ticks 20000', 'state');
	l.push('run 0', 'song 2 1 64', 'songmode 1', 'run 1', 'ticks 20000', 'state', 'run 1', 'ticks 3000', 'state');
	return l;
});

scenario('roll', () => {
	const l = ['main 0 0 0 1', stepLine(0, 0, 3, { note: 50 })];
	for (let r = 0; r <= 5; r++) {
		l.push(`rollrate ${r}`, 'roll 2 1', 'ticks 300', 'roll 2 0');
	}
	// (not between start and the first step: the firmware reads step -1 there, which
	// is outside the pattern data)
	l.push('run 1', 'ticks 30');
	for (let r = 0; r <= 5; r++) {
		l.push(`rollrate ${r}`, `roll ${r} 1`, 'ticks 4000', `roll ${r} 0`, 'ticks 100');
	}
	l.push('rec 1', 'rollrate 3', 'roll 6 1', 'ticks 5000', 'roll 6 0', 'dump 0 6', 'state');
	return l;
});

scenario('record', () => {
	// (a note between start and the first step makes the firmware write outside the pattern)
	const l = ['main 0 0 0 1', 'len 0 3 12', 'run 1', 'ticks 10', 'rec 1', 'note 3 100 62', 'ticks 10'];
	for (let i = 0; i < 40; i++) l.push(`ticks ${37 + (i * 53) % 211}`, `note ${i % 7} ${(i * 13) % 128} ${(i * 7 + 30) % 128}`);
	l.push('ticks 4000', 'rec 0', 'note 1 90 61', 'ticks 2000');
	for (let t = 0; t < 7; t++) l.push(`dump 0 ${t}`);
	l.push('state');
	return l;
});

scenario('automation', () => {
	const l = [];
	for (let s = 0; s < 16; s += 2) {
		l.push(`main 0 1 ${s} 1`, stepLine(0, 1, s, { p1n: [255, 0, 5, 127, 128, 200, 255, 60][s / 2], p1v: s * 8, p2n: [9, 255, 255, 140, 9, 12, 130, 255][s / 2], p2v: 127 - s }));
	}
	l.push('main 0 4 1 1', stepLine(0, 4, 1, { p1n: 77, p1v: 3, flam: 5 }));
	l.push('run 1', 'ticks 9000');
	l.push('track 1', 'rec 1', 'autotrack 1', 'autorec 33 99', 'ticks 700', 'autorec 150 12', 'ticks 400', 'rec 0');
	l.push('arm 5 2 1', 'autotrack 0', 'autorec 21 64', 'autorec 21 70', 'arm 5 2 0', 'autorec 40 1');
	l.push('main 0 2 5 1', 'ticks 4000', 'dump 0 1', 'dump 0 2', 'clearautom 1 0 1', 'dump 0 1', 'state');
	return l;
});

scenario('erase', () => [
	...Array.from({ length: 16 }, (_, s) => `main 0 3 ${s} 1`),
	'main 0 0 0 1', 'track 3', 'run 1', 'ticks 800', 'erase 1', 'ticks 1200', 'erase 0', 'ticks 2000', 'dump 0 3', 'state',
]);

scenario('euklid', () => {
	const l = [];
	let k = 0;
	for (let len = 1; len <= 16; len++) {
		for (let steps = 1; steps <= len; steps += 1 + (len > 8 ? 2 : 0)) {
			const t = k % 7;
			l.push(`euklid ${t} ${len} ${steps} ${(len * steps) % 17} ${k % 3}`, `dump ${k % 3} ${t}`);
			k++;
		}
	}
	return l;
});

scenario('edit-ops', () => {
	const l = [];
	for (let s = 0; s < 64; s += 3) l.push(`main 0 2 ${s} 1`, stepLine(0, 2, s, { vol: s, prob: s % 101, note: s + 20, flam: s % 6, shift: s % 15, p1n: s, p1v: s, p2n: 64 + s, p2v: 127 - s }));
	l.push('len 0 2 50', 'patshuffle 0 77', 'copytrack 2 5 0', 'copypattern 0 3', 'dump 0 5', 'dump 3 2', 'cleartrack 2 0', 'dump 0 2',
		'shown 3', 'tracklen 4 9', 'dump 3 4', 'clearpattern 3', 'dump 3 2', 'next 3', 'state');
	return l;
});

scenario('midi-channels', () => [
	'midichan 0 9', 'midichan 3 9', 'midichan 7 15', 'main 0 0 0 1', 'main 0 3 0 1', 'main 0 3 2 1', 'main 1 6 1 1',
	'run 1', 'ticks 3000', 'next 1', 'ticks 9000', 'mute 3 1', 'ticks 1000', 'state',
]);

for (let k = 1; k <= 8; k++) {
	scenario(`fuzz-${k}`, () => {
		const r = rng(k * 7919);
		const l = [`bpm ${40 + r(200)}`, `shuffle ${r(3) ? 0 : r(128)}`];
		for (let p = 0; p < 3; p++) {
			l.push(`patshuffle ${p} ${r(128)}`);
			for (let t = 0; t < 7; t++) {
				const len = 1 + r(r(2) ? 16 : 64);
				l.push(`len ${p} ${t} ${len}`);
				for (let s = 0; s < len; s++) {
					if (r(3) === 0) {
						l.push(`main ${p} ${t} ${s} 1`);
						l.push(stepLine(p, t, s, {
							vol: r(256), prob: r(4) ? 100 : r(101), note: r(128), flam: r(4) ? 0 : r(6), shift: r(3) ? 7 : r(15),
							p1n: r(3) ? 255 : r(255), p1v: r(128), p2n: r(4) ? 255 : r(255), p2v: r(128),
						}));
					}
				}
			}
		}
		l.push(`song ${r(3)} ${r(3)} ${r(3)} ${r(2) ? 65 : 64}`);
		// the first step comes up to one 96 ppq tick after a start; until then step -1 is
		// current, which the firmware reads (roll on track 1) and writes (recording)
		// outside the pattern data, so leave that gap alone
		// (the same goes for recording automation while stopped)
		l.push('run 1', 'ticks 200');
		let running = 1;
		for (let i = 0; i < 60; i++) {
			const c = r(16);
			if (c === 0) l.push(`mute ${r(8)} ${r(2)}`);
			else if (c === 1) l.push(`next ${r(3)}`);
			else if (c === 2) l.push(`songmode ${r(2)}`);
			else if (c === 3) l.push(`roll ${1 + r(6)} ${r(2)}`, `rollrate ${r(6)}`);
			else if (c === 4) l.push(`rec ${r(2)}`);
			else if (c === 5) l.push(`note ${r(7)} ${r(128)} ${r(128)}`);
			else if (c === 6 && running) l.push(`track ${r(7)}`, `autorec ${r(255)} ${r(128)}`);
			else if (c === 7) l.push(`bpm ${30 + r(250)}`);
			else if (c === 8) l.push(`shuffle ${r(128)}`);
			else if (c === 9 && r(4) === 0) {
				running = r(2);
				l.push(`run ${running}`, 'ticks 200');
			}
			else if (c === 10) l.push(`main ${r(3)} ${r(7)} ${r(16)} ${r(2)}`);
			l.push(`ticks ${1 + r(1500)}`);
		}
		for (let t = 0; t < 7; t++) l.push(`dump 0 ${t}`);
		l.push('state');
		return l;
	});
}

// ---------------------------------------------------------------- run
execFileSync('python3', [join(HERE, 'ref/build.py')], { stdio: ['ignore', 'ignore', 'inherit'] });
mkdirSync(OUT, { recursive: true });
let failed = 0;
for (const s of scenarios) {
	if (filter && !s.name.includes(filter)) continue;
	const script = s.fn().join('\n') + '\n';
	const file = join(OUT, `seq-${s.name}.txt`);
	writeFileSync(file, script);
	const ref = execFileSync(REF, [file], { maxBuffer: 1 << 28 }).toString().trim().split('\n').filter(Boolean);
	const port = runPort(script);
	const n = Math.max(ref.length, port.length);
	let first = -1;
	for (let i = 0; i < n; i++) {
		if (ref[i] !== port[i]) {
			first = i;
			break;
		}
	}
	const triggers = ref.filter((x) => x.startsWith('N ')).length;
	if (first < 0) {
		console.log(`ok   ${s.name.padEnd(18)} ${String(ref.length).padStart(6)} events, ${triggers} triggers`);
	} else {
		failed++;
		console.log(`FAIL ${s.name.padEnd(18)} first difference at event ${first}:`);
		for (let i = Math.max(0, first - 3); i <= Math.min(n - 1, first + 3); i++) {
			console.log(`   ${i === first ? '>' : ' '} ref:  ${(ref[i] || '(none)').slice(0, 150)}`);
			console.log(`   ${i === first ? '>' : ' '} port: ${(port[i] || '(none)').slice(0, 150)}`);
		}
	}
}
// the stall the port fixes: shuffle 127 at 90 BPM keeps playing
{
	const script = scenarios.find((s) => s.name === 'shuffle-127-90').fn().join('\n');
	const n = runPort(script, 0).filter((x) => x.startsWith('N ')).length;
	if (n < 30) {
		console.log(`FAIL deltaT -1 stall still there (${n} triggers)`);
		failed++;
	} else {
		console.log(`ok   no deltaT stall    ${n} triggers with shuffle 127 at 90 BPM`);
	}
}
if (failed) {
	console.log(`${failed} scenario(s) differ`);
	process.exit(1);
}
console.log('sequencer matches the firmware');
