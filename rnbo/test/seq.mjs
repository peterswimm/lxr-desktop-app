// Tests the engine's step sequencer (no firmware reference: the Move sequencer is new).
//   node rnbo/test/seq.mjs
import assert from 'assert/strict';
import { readFileSync } from 'fs';
import { dirname, join } from 'path';
import { fileURLToPath } from 'url';
import { instantiate } from './codebox.mjs';

const HERE = dirname(fileURLToPath(import.meta.url));
const SRC = readFileSync(join(HERE, '../generated/lxr_engine.codebox.js'), 'utf8');
const SR = 44100;
const N = 64;

function engine() {
	return instantiate(SRC, SR).dsp;
}

// runs `blocks` blocks; returns the block indices at which each slot was triggered
function run(dsp, blocks) {
	const hits = [[], [], [], [], [], [], []];
	let prev = Array.from(dsp.get('tCount'));
	for (let b = 0; b < blocks; b++) {
		for (let i = 0; i < N; i++) dsp.perform();
		const c = dsp.get('tCount');
		for (let s = 0; s < 7; s++) if (c[s] !== prev[s]) hits[s].push(b);
		prev = Array.from(c);
	}
	return hits;
}

const stepBlocks = (bpm) => SR * 60 / bpm / 4 / N;

// --- default demo pattern: kick on every beat at 120 bpm
{
	const dsp = engine();
	dsp.input(1, 1); // running
	const hits = run(dsp, Math.ceil(stepBlocks(120) * 15) + 2);
	assert.equal(hits[0].length, 4, `kick hits ${hits[0]}`);
	for (let k = 1; k < hits[0].length; k++) {
		const d = hits[0][k] - hits[0][k - 1];
		assert.ok(Math.abs(d - 4 * stepBlocks(120)) <= 1, `kick spacing ${d}`);
	}
	assert.equal(hits[3].length, 2, `snare hits ${hits[3]}`);
	const status = dsp.output();
	assert.equal(status[4], 0x1111, 'out5 = step bits of track 1');
	assert.equal(status[9], 0x5555, 'out10 = step bits of track 6 (closed hihat)');
	assert.equal(status[16], 0x1111, 'out17 = accent bits of track 6');
	assert.equal(status[18] % 2, 1, 'out19 running bit');
	assert.equal(Math.floor(status[18] / 256), 120, 'out19 bpm');
	assert.equal(status[21], 15, 'out22 = last played step (16th step started)');
	console.log('ok   demo pattern timing');
}

// --- swing: every second 16th is late
{
	const dsp = engine();
	dsp.set('seq_d1_steps', 65535);
	dsp.set('seq_swing', 127);
	dsp.set('bpm', 100);
	dsp.input(1, 1);
	const hits = run(dsp, Math.ceil(stepBlocks(100) * 8));
	const d = hits[0].slice(1).map((x, i) => x - hits[0][i]);
	const long = stepBlocks(100) * 1.5;
	const short = stepBlocks(100) * 0.5;
	assert.ok(Math.abs(d[0] - long) <= 1 && Math.abs(d[1] - short) <= 1, `swing intervals ${d}`);
	console.log('ok   swing');
}

// --- mutes, track length, stop/restart
{
	const dsp = engine();
	dsp.set('seq_d1_steps', 1); // step 1 only
	dsp.set('seq_d1_length', 3); // ... every 3 steps
	dsp.input(1, 1 + 2 * 8); // running, track 4 (snare) muted
	const hits = run(dsp, Math.ceil(stepBlocks(120) * 11));
	assert.equal(hits[3].length, 0, 'muted snare stays quiet');
	assert.equal(hits[0].length, 4, `length-3 kick ${hits[0]}`);
	for (let k = 1; k < 4; k++) assert.ok(Math.abs(hits[0][k] - hits[0][k - 1] - 3 * stepBlocks(120)) <= 1);
	const st = dsp.output();
	assert.equal(st[19] % 16, 2, 'out20 holds length - 1 of track 1');
	dsp.input(1, 0); // stop
	assert.equal(run(dsp, 200)[0].length, 0, 'stopped');
	console.log('ok   mutes, lengths, stop');
}

console.log('sequencer tests passed');
