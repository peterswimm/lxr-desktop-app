// Debug helper: finds the first block where the RNBO engine's state differs from
// lxr_ref's. Usage: node test/statediff.mjs <scenario script from test/out/tmp> [blocks]
import { execFileSync } from 'child_process';
import { readFileSync, writeFileSync } from 'fs';
import { dirname, join } from 'path';
import { fileURLToPath } from 'url';
import { instantiate } from './codebox.mjs';

const HERE = dirname(fileURLToPath(import.meta.url));
const META = JSON.parse(readFileSync(join(HERE, '../generated/params.json'), 'utf8'));
const byPar = Object.fromEntries(META.params.map((p) => [p.par, p.name]));
const VOICES = ['d1', 'd2', 'd3', 'sn', 'cy', 'hh'];
const maxBlocks = +(process.argv[3] || 1e9);

// one dump after every block
const lines = [];
let blocks = 0;
for (const l of readFileSync(process.argv[2], 'utf8').split('\n')) {
	if (l.startsWith('render')) {
		for (let k = 0; k < +l.split(' ')[1] && blocks < maxBlocks; k++, blocks++) lines.push('render 1', 'dumpall');
	} else if (l.trim()) lines.push(l);
}
const script = join(HERE, 'out/tmp/statediff.txt');
writeFileSync(script, lines.join('\n') + '\n');
const ref = execFileSync(join(HERE, 'ref/build/lxr_ref'), [script, '/dev/null'], { maxBuffer: 1 << 30 }).toString().split('\n');

const { dsp } = instantiate(readFileSync(join(HERE, '../generated/lxr_engine.codebox.js'), 'utf8'), 44140);
const g = (n) => dsp.get(n);
const f9 = (x) => Number(x).toPrecision(9);
let count = 0;
let blk = 0;
for (const l of lines) {
	const [c, a, b, d] = l.split(' ');
	if (c === 'param') dsp.set(byPar[a], +b);
	else if (c === 'lfodest' || c === 'velodest')
		dsp.set(VOICES[a] + (c === 'lfodest' ? '_lfo_dest' : '_vel_dest'), META.mod_targets.indexOf(+b));
	else if (c === 'trig') dsp.input(+a + 2, ++count * 16384 + +d * 128 + +b);
	else if (c === 'bpm') dsp.set('bpm', +a);
	else if (c === 'render') for (let i = 0; i < 64; i++) dsp.perform();
	else if (c === 'dumpall') {
		const out = [];
		for (let o = 0; o < 15; o++)
			out.push(`o${o}`, g('oPhase')[o], g('oInc')[o], g('oWave')[o], g('oTbl')[o], f9(g('oFreq')[o]), f9(g('oPitch')[o]), f9(g('oFm')[o]), g('oOut')[o]);
		for (let v = 0; v < 6; v++)
			out.push(`v${v}`, 'ae', g('aeValue')[v], g('aeState')[v], g('aeRepCnt')[v], g('aeAttack')[v], v === 5 ? g('aeDecay')[v] : g('aeDecay')[v],
				'f', f9(g('svF')[v]), f9(g('svQ')[v]), f9(g('svDrive')[v]), f9(g('svS1')[v]), f9(g('svS2')[v]), g('fType')[v],
				't', g('trPhase')[v], g('trWave')[v], f9(g('trVol')[v]), f9(g('trPitch')[v]),
				'l', g('lPhase')[v], g('lInc')[v], g('lWave')[v], g('lRetrig')[v], 'm', g('lDest')[v], f9(g('lAmount')[v]), f9(g('vmAmount')[v]));
		out.push('fx', g('fxType'), g('fxRouting'), f9(g('fxDryWet')), ...[0, 1, 2, 3].map((i) => f9(g('fxP')[i])));
		const r = ref[blk].trim().split(/\s+/);
		const names = [];
		let tag = '';
		const FIELDS = { o: ['phase', 'inc', 'wave', 'tbl', 'freq', 'pitch', 'fm', 'out'] };
		for (let i = 0; i < out.length; i++) {
			const x = String(out[i]);
			const y = r[i];
			if (/^[a-z]+\d*$/.test(x)) tag = x;
			const nx = Number(x);
			const ny = Number(y);
			const same = x === y || (!Number.isNaN(nx) && Math.abs(nx - ny) <= 1e-7 * Math.max(1, Math.abs(ny)));
			if (!same) names.push(`${tag}[${i}] ref ${y} rnbo ${x}`);
		}
		if (names.length) {
			console.log(`block ${blk}:`, names.slice(0, 12).join('; '));
			process.exit(1);
		}
		blk++;
	}
}
console.log(`no state difference in ${blk} blocks`);
